/* simrupt: A device that simulates interrupts */

#include <linux/cdev.h>
#include <linux/circ_buf.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#include "game.h"
#include "mcts.h"

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("A device that simulates interrupts");

/* Macro DECLARE_TASKLET_OLD exists for compatibiity.
 * See https://lwn.net/Articles/830964/
 */
#ifndef DECLARE_TASKLET_OLD
#define DECLARE_TASKLET_OLD(arg1, arg2) DECLARE_TASKLET(arg1, arg2, 0L)
#endif

#define DEV_NAME "simrupt"

#define NR_SIMRUPT 1

static int delay = 100; /* time (in ms) to generate an event */

/* Data produced by the simulated device */
static int simrupt_data;

/* Timer to simulate a periodic IRQ */
static struct timer_list timer;

/* Character device stuff */
static int major;
static struct class *simrupt_class;
static struct cdev simrupt_cdev;

/* Game board*/
static char table[N_GRIDS];
#define BOARD_GRIDS 2 * 4 * (N_GRIDS + 1) + 2
static char board_buff[BOARD_GRIDS];
char turn;

/* Data are stored into a kfifo buffer before passing them to the userspace */
static DECLARE_KFIFO_PTR(rx_fifo, unsigned char);

/* NOTE: the usage of kfifo is safe (no need for extra locking), until there is
 * only one concurrent reader and one concurrent writer. Writes are serialized
 * from the interrupt context, readers are serialized using this mutex.
 */
static DEFINE_MUTEX(read_lock);

/* Wait queue to implement blocking I/O from userspace */
static DECLARE_WAIT_QUEUE_HEAD(rx_wait);
static DECLARE_WAIT_QUEUE_HEAD(player_wait);

/* Generate new data from the simulated device */
static inline char *update_simrupt_data(void)
{
    // simrupt_data = max((simrupt_data + 1) % 16, 0);
    // table[simrupt_data] = 'O';
    return table;
}

/* Insert a value into the kfifo buffer */
static void produce_data(void)
{
    /* Implement a kind of circular FIFO here (skip oldest element if kfifo
     * buffer is full).
     */
    unsigned int len = kfifo_in(&rx_fifo, board_buff, sizeof(board_buff));
    if (unlikely(len < sizeof(board_buff)) && printk_ratelimit())
        pr_warn("%s: %zu bytes dropped\n", __func__, sizeof(board_buff) - len);

    pr_debug("simrupt: %s: in %u/%u bytes\n", __func__, len,
             kfifo_len(&rx_fifo));
}

/* Mutex to serialize kfifo writers within the workqueue handler */
static DEFINE_MUTEX(producer_lock);

/* Mutex to serialize fast_buf consumers: we can use a mutex because consumers
 * run in workqueue handler (kernel thread context).
 */
static DEFINE_MUTEX(consumer_lock);

/* AI player task*/

static void Player_I_task(struct work_struct *w)
{
    // if(turn != 'O') return;
    /* This code runs from a kernel thread, so softirqs and hard-irqs must
     * be enabled.
     */
    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    int move;

    if (turn == 'O') {
        move = mcts(table, turn);
        if (move != -1) {
            WRITE_ONCE(table[move], turn);
        }
        WRITE_ONCE(turn, 'X');
    }
    smp_wmb();

    pr_info("------- first ------");
    pr_info("simrupt: [CPU#%d] player I game\n", smp_processor_id());
}

static void Player_II_task(struct work_struct *w)
{
    // if(turn != 'O') return;
    /* This code runs from a kernel thread, so softirqs and hard-irqs must
     * be enabled.
     */
    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    int move;

    if (turn == 'X') {
        move = mcts(table, turn);
        if (move != -1) {
            WRITE_ONCE(table[move], turn);
        }
        WRITE_ONCE(turn, 'O');
    }
    smp_wmb();

    pr_info("------- second ------");
    pr_info("simrupt: [CPU#%d] player I game\n", smp_processor_id());
}

static int draw_board(const char *t)
{
    int i = 0;
    board_buff[i++] = '\n';
    smp_wmb();
    board_buff[i++] = '\n';
    smp_wmb();
    for (int x = 0; x < BOARD_SIZE; x++) {
        for (int y = 0; y < BOARD_SIZE; y++) {
            board_buff[i++] = ' ';
            smp_wmb();
            WRITE_ONCE(board_buff[i++], table[GET_INDEX(x, y)]);
            smp_wmb();
            board_buff[i++] = ' ';
            smp_wmb();
            if (y != BOARD_SIZE - 1) {
                board_buff[i++] = '|';
                smp_wmb();
            }
        }
        board_buff[i++] = '\n';
        smp_wmb();
        for (int y = 0; y < BOARD_SIZE; y++) {
            board_buff[i++] = '-';
            smp_wmb();
            board_buff[i++] = '-';
            smp_wmb();
            board_buff[i++] = '-';
            smp_wmb();
            board_buff[i++] = '-';
            smp_wmb();
        }
        board_buff[i++] = '\n';
        smp_wmb();
    }

    return 0;
}

/* Workqueue handler: executed by a kernel thread */
static void simrupt_work_func(struct work_struct *w)
{
    int cpu;

    /* This code runs from a kernel thread, so softirqs and hard-irqs must
     * be enabled.
     */
    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    /* Pretend to simulate access to per-CPU data, disabling preemption
     * during the pr_info().
     */
    cpu = get_cpu();
    pr_info("simrupt: [CPU#%d] %s\n", cpu, __func__);
    put_cpu();

    mutex_lock(&producer_lock);
    produce_data();
    mutex_unlock(&producer_lock);

    wake_up_interruptible(&rx_wait);
}

/* Workqueue for asynchronous bottom-half processing */
static struct workqueue_struct *simrupt_workqueue;

/* Work item: holds a pointer to the function that is going to be executed
 * asynchronously.
 */
static DECLARE_WORK(work, simrupt_work_func);
static DECLARE_WORK(player1, Player_I_task);
static DECLARE_WORK(player2, Player_II_task);

/* Tasklet handler.
 *
 * NOTE: different tasklets can run concurrently on different processors, but
 * two of the same type of tasklet cannot run simultaneously. Moreover, a
 * tasklet always runs on the same CPU that schedules it.
 */
static void simrupt_tasklet_func(unsigned long __data)
{
    ktime_t tv_start, tv_end;
    s64 nsecs;

    WARN_ON_ONCE(!in_interrupt());
    WARN_ON_ONCE(!in_softirq());

    tv_start = ktime_get();
    queue_work(simrupt_workqueue, &work);
    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));

    pr_info("simrupt: [CPU#%d] %s in_softirq: %llu usec\n", smp_processor_id(),
            __func__, (unsigned long long) nsecs >> 10);
}

/* AI player task*/
static void play_game(void)
{
    // char turn = 'X';
    // char ai = 'O';
    // int move;
    // pr_info("------- play game ------");
    // while (1) {
    //     mutex_lock(&consumer_lock);
    //     char win = check_win(table);
    //     mutex_unlock(&consumer_lock);
    //     if (win == 'D') {
    //         // draw_board(table);
    //         // printf("It is a draw!\n");
    //         break;
    //     } else if (win != ' ') {
    //         // draw_board(table);
    //         // printf("%c won!\n", win);
    //         break;
    //     }

    //     if (turn == ai) {
    //         mutex_lock(&consumer_lock);
    //         move = mcts(table, ai);
    //         if (move != -1) {
    //             table[move] = ai;
    //             // record_move(move);
    //         }
    //         mutex_unlock(&consumer_lock);
    //         pr_info("------- first ------");

    //     } else {
    //         mutex_lock(&consumer_lock);
    //         move = mcts(table, turn);
    //         if (move != -1) {
    //             table[move] = turn;
    //             // record_move(move);
    //         }
    //         mutex_unlock(&consumer_lock);
    //         pr_info("------- second ------");
    //     }
    //     turn = turn == 'X' ? 'O' : 'X';
    // }
    pr_info("------- enter first ------");
    queue_work(simrupt_workqueue, &player1);
    pr_info("------- enter second ------");
    queue_work(simrupt_workqueue, &player2);
}

/* Tasklet for asynchronous bottom-half processing in softirq context */
static DECLARE_TASKLET_OLD(simrupt_tasklet, simrupt_tasklet_func);

static void process_data(void)
{
    WARN_ON_ONCE(!irqs_disabled());
    pr_info("simrupt: [CPU#%d] produce data\n", smp_processor_id());
    draw_board(update_simrupt_data());
    pr_info("simrupt: [CPU#%d] scheduling tasklet\n", smp_processor_id());
    tasklet_schedule(&simrupt_tasklet);
}

static void timer_handler(struct timer_list *__timer)
{
    ktime_t tv_start, tv_end;
    s64 nsecs;

    pr_info("simrupt: [CPU#%d] enter %s\n", smp_processor_id(), __func__);
    /* We are using a kernel timer to simulate a hard-irq, so we must expect
     * to be in softirq context here.
     */
    WARN_ON_ONCE(!in_softirq());

    /* Disable interrupts for this CPU to simulate real interrupt context */
    local_irq_disable();

    tv_start = ktime_get();
    // process_data();
    mutex_lock(&consumer_lock);
    char win = check_win(table);
    mutex_unlock(&consumer_lock);
    if (win == ' ') {
        play_game();
    }
    process_data();
    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));

    pr_info("simrupt: [CPU#%d] %s in_irq: %llu usec\n", smp_processor_id(),
            __func__, (unsigned long long) nsecs >> 10);
    mod_timer(&timer, jiffies + msecs_to_jiffies(delay));

    local_irq_enable();
}

static ssize_t simrupt_read(struct file *file,
                            char __user *buf,
                            size_t count,
                            loff_t *ppos)
{
    unsigned int read;
    int ret;

    pr_debug("simrupt: %s(%p, %zd, %lld)\n", __func__, buf, count, *ppos);

    if (unlikely(!access_ok(buf, count)))
        return -EFAULT;

    if (mutex_lock_interruptible(&read_lock))
        return -ERESTARTSYS;

    do {
        ret = kfifo_to_user(&rx_fifo, buf, count, &read);
        if (unlikely(ret < 0))
            break;
        if (read)
            break;
        if (file->f_flags & O_NONBLOCK) {
            ret = -EAGAIN;
            break;
        }
        ret = wait_event_interruptible(rx_wait, kfifo_len(&rx_fifo));
    } while (ret == 0);
    pr_debug("simrupt: %s: out %u/%u bytes\n", __func__, read,
             kfifo_len(&rx_fifo));

    mutex_unlock(&read_lock);

    return ret ? ret : read;
}

static atomic_t open_cnt;

static int simrupt_open(struct inode *inode, struct file *filp)
{
    pr_debug("simrupt: %s\n", __func__);
    if (atomic_inc_return(&open_cnt) == 1)
        mod_timer(&timer, jiffies + msecs_to_jiffies(delay));
    pr_info("openm current cnt: %d\n", atomic_read(&open_cnt));
    // play_game();

    return 0;
}

static int simrupt_release(struct inode *inode, struct file *filp)
{
    pr_debug("simrupt: %s\n", __func__);
    if (atomic_dec_and_test(&open_cnt) == 0) {
        del_timer_sync(&timer);
        flush_workqueue(simrupt_workqueue);
    }
    pr_info("release, current cnt: %d\n", atomic_read(&open_cnt));

    return 0;
}

static const struct file_operations simrupt_fops = {
    .read = simrupt_read,
    .llseek = no_llseek,
    .open = simrupt_open,
    .release = simrupt_release,
    .owner = THIS_MODULE,
};

static int __init simrupt_init(void)
{
    dev_t dev_id;
    int ret;

    if (kfifo_alloc(&rx_fifo, PAGE_SIZE, GFP_KERNEL) < 0)
        return -ENOMEM;

    /* Register major/minor numbers */
    ret = alloc_chrdev_region(&dev_id, 0, NR_SIMRUPT, DEV_NAME);
    if (ret)
        goto error_alloc;
    major = MAJOR(dev_id);

    /* Add the character device to the system */
    cdev_init(&simrupt_cdev, &simrupt_fops);
    ret = cdev_add(&simrupt_cdev, dev_id, NR_SIMRUPT);
    if (ret) {
        kobject_put(&simrupt_cdev.kobj);
        goto error_region;
    }

    /* Create a class structure */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
    simrupt_class = class_create(THIS_MODULE, DEV_NAME);
#else
    simrupt_class = class_create(DEV_NAME);
#endif
    if (IS_ERR(simrupt_class)) {
        printk(KERN_ERR "error creating simrupt class\n");
        ret = PTR_ERR(simrupt_class);
        goto error_cdev;
    }

    /* Register the device with sysfs */
    device_create(simrupt_class, NULL, MKDEV(major, 0), NULL, DEV_NAME);


    /* Create the workqueue */
    simrupt_workqueue = alloc_workqueue("simruptd", WQ_UNBOUND, WQ_MAX_ACTIVE);
    if (!simrupt_workqueue) {
        device_destroy(simrupt_class, dev_id);
        class_destroy(simrupt_class);
        ret = -ENOMEM;
        goto error_cdev;
    }

    /* Setup the timer */
    timer_setup(&timer, timer_handler, 0);
    atomic_set(&open_cnt, 0);

    /* init game table */
    for (int i = 0; i < N_GRIDS; i++) {
        table[i] = ' ';
    }
    turn = 'O';

    pr_info("simrupt: registered new simrupt device: %d,%d\n", major, 0);
out:
    return ret;
error_cdev:
    cdev_del(&simrupt_cdev);
error_region:
    unregister_chrdev_region(dev_id, NR_SIMRUPT);
error_alloc:
    kfifo_free(&rx_fifo);
    goto out;
}

static void __exit simrupt_exit(void)
{
    dev_t dev_id = MKDEV(major, 0);

    del_timer_sync(&timer);
    tasklet_kill(&simrupt_tasklet);
    flush_workqueue(simrupt_workqueue);
    destroy_workqueue(simrupt_workqueue);
    device_destroy(simrupt_class, dev_id);
    class_destroy(simrupt_class);
    cdev_del(&simrupt_cdev);
    unregister_chrdev_region(dev_id, NR_SIMRUPT);

    kfifo_free(&rx_fifo);
    pr_info("simrupt: unloaded\n");
}

module_init(simrupt_init);
module_exit(simrupt_exit);
