/*  userspace_ioctl.c - the process to use ioctl's to control the kernel module
 *
 *  Until now we could have used cat for input and output.  But now
 *  we need to do ioctl's, which require writing our own process.
 */

/* device specifics, such as ioctl numbers and the
 * major device file. */
#include "chardev.h"

#include <ctype.h>
#include <fcntl.h> /* open */
#include <stdbool.h>
#include <stdio.h>     /* standard I/O */
#include <stdlib.h>    /* exit */
#include <sys/ioctl.h> /* ioctl */
#include <termios.h>
#include <unistd.h> /* close */


struct editorConfig {
    int cx, cy;
    struct termios orig_termios;
};

struct editorConfig E;

static char display_board = 0;
static bool stop_game = false;


/* Functions for the ioctl calls */

int ioctl_set_msg(int file_desc, char *message)
{
    int ret_val;

    ret_val = ioctl(file_desc, IOCTL_SET_MSG, message);

    if (ret_val < 0) {
        printf("ioctl_set_msg failed:%d\n", ret_val);
    }

    return ret_val;
}

int ioctl_get_msg(int file_desc)
{
    int ret_val;
    char message[150] = {0};

    /* Warning - this is dangerous because we don't tell
     * the kernel how far it's allowed to write, so it
     * might overflow the buffer. In a real production
     * program, we would have used two ioctls - one to tell
     * the kernel the buffer length and another to give
     * it the buffer to fill
     */
    ret_val = ioctl(file_desc, IOCTL_GET_MSG, message);

    if (ret_val < 0) {
        printf("ioctl_get_msg failed:%d\n", ret_val);
    }
    printf("%s", message);

    return ret_val;
}


void disableRawMode()
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
}

void enableRawMode()
{
    tcgetattr(STDIN_FILENO, &E.orig_termios);
    struct termios raw = E.orig_termios;
    atexit(disableRawMode);
    raw.c_iflag &= ~(IXON);
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

char keyboard_task(int file_desc)
{
    char c;
    if (read(STDIN_FILENO, &c, 1) == 1) {
        switch (c) {
        case (16):
            display_board = display_board == 'p' ? 'n' : 'p';
            ioctl_set_msg(file_desc, &display_board);
            break;
        case (17):
            stop_game = true;
            break;
        }
    }
    return c;
}



/* Main - Call the ioctl functions */
int main(void)
{
    int file_desc, ret_val;

    file_desc = open(DEVICE_PATH, O_RDONLY);
    if (file_desc < 0) {
        printf("Can't open device file: %s, error:%d\n", DEVICE_PATH,
               file_desc);
        exit(EXIT_FAILURE);
    }
    enableRawMode();
    while (!stop_game) {
        keyboard_task(file_desc);
        ret_val = ioctl_get_msg(file_desc);
        if (ret_val)
            goto error;
    }
    disableRawMode();
    close(file_desc);
    return 0;
error:
    close(file_desc);
    exit(EXIT_FAILURE);
}