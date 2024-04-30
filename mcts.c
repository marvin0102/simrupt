#include <linux/slab.h>
#include <linux/string.h>

#include "game.h"
#include "mcts.h"
#include "util.h"
#include "wyhash.h"

#define EXPLORATION_FACTOR fixed_sqrt(2 << Q)
#define K (1 << (Q - 1))

struct node {
    int move;
    char player;
    int n_visits;
    Q23_8 score;
    struct node *parent;
    struct node *children[N_GRIDS];
};

static struct node *new_node(int move, char player, struct node *parent)
{
    struct node *node = kzalloc(sizeof(struct node), __GFP_ZERO);
    node->move = move;
    node->player = player;
    node->n_visits = 0;
    node->score = 0;
    node->parent = parent;
    memset(node->children, 0, sizeof(node->children));
    return node;
}

static void free_node(struct node *node)
{
    for (int i = 0; i < N_GRIDS; i++)
        if (node->children[i])
            free_node(node->children[i]);
    kfree(node);
}

Q23_8 fixed_sqrt(Q23_8 x)
{
    if (x <= 1 << Q) /* Assume x is always positive */
        return x;

    Q23_8 z = 0;
    for (Q23_8 m = 1UL << ((31 - __builtin_clz(x)) & ~1UL); m; m >>= 2) {
        int b = z + m;
        z >>= 1;
        if (x >= b)
            x -= b, z += m;
    }
    z = z << Q / 2;
    return z;
}

Q23_8 fixed_div(Q23_8 a, Q23_8 b)
{
    /* pre-multiply by the base (Upscale to Q16 so that the result will be in Q8
     * format) */
    unsigned long temp = (unsigned long) a << Q;
    /* Rounding: mid values are rounded up (down for negative values). */
    /* OR compare most significant bits i.e. if (((temp >> 31) & 1) == ((b >>
     * 15) & 1)) */

    temp += b / 2; /* OR shift 1 bit i.e. temp += (b >> 1); */

    return (Q23_8) (temp / b);
}

Q23_8 fixed_log(int input)
{
    if (!input || input == 1)
        return 0;

    Q23_8 y = input << Q;  // int to Q15_16
    Q23_8 L = 1L << ((31 - __builtin_clz(y))), R = L << 1;
    Q23_8 Llog = (31 - __builtin_clz(y) - Q) << Q, Rlog = Llog + (1 << Q), log;

    for (int i = 1; i < 20; i++) {
        if (y == L)
            return Llog;
        else if (y == R)
            return Rlog;
        log = fixed_div(Llog + Rlog, 2 << Q);

        unsigned long tmp = ((unsigned long) L * (unsigned long) R) >> Q;
        tmp = fixed_sqrt((Q23_8) tmp);

        if (y >= tmp) {
            L = tmp;
            Llog = log;
        } else {
            R = tmp;
            Rlog = log;
        }
    }

    return (Q23_8) log;
}

static inline Q23_8 uct_score(int n_total, int n_visits, Q23_8 score)
{
    if (n_visits == 0)
        return 0xffffffff;
    Q23_8 result = score << Q / (Q23_8) (n_visits << Q);
    unsigned long tmp =
        (unsigned long) EXPLORATION_FACTOR *
        (unsigned long) fixed_sqrt(fixed_log(n_total) / n_visits);
    Q23_8 resultN = tmp >> Q;
    return result + resultN;
}

static struct node *select_move(struct node *node)
{
    struct node *best_node = NULL;
    Q23_8 best_score = 0;
    for (int i = 0; i < N_GRIDS; i++) {
        if (!node->children[i])
            continue;
        Q23_8 score = uct_score(node->n_visits, node->children[i]->n_visits,
                                node->children[i]->score);
        if (score > best_score) {
            best_score = score;
            best_node = node->children[i];
        }
    }
    return best_node;
}

static Q23_8 simulate(char *table, char player)
{
    char current_player = player;
    char temp_table[N_GRIDS];
    memcpy(temp_table, table, N_GRIDS);
    while (1) {
        int *moves = available_moves(temp_table);
        if (moves[0] == -1) {
            kfree(moves);
            break;
        }
        int n_moves = 0;
        while (n_moves < N_GRIDS && moves[n_moves] != -1)
            ++n_moves;
        int move = moves[wyhash64() % n_moves];
        kfree(moves);
        temp_table[move] = current_player;
        char win;
        if ((win = check_win(temp_table)) != ' ')
            return calculate_win_value(win, player);
        current_player ^= 'O' ^ 'X';
    }
    return 0.5;
}

static void backpropagate(struct node *node, Q23_8 score)
{
    while (node) {
        node->n_visits++;
        node->score += score;
        node = node->parent;
        score = 1 - score;
    }
}

static void expand(struct node *node, char *table)
{
    int *moves = available_moves(table);
    int n_moves = 0;
    while (n_moves < N_GRIDS && moves[n_moves] != -1)
        ++n_moves;
    for (int i = 0; i < n_moves; i++) {
        node->children[i] = new_node(moves[i], node->player ^ 'O' ^ 'X', node);
    }
    kfree(moves);
}

int mcts(char *table, char player)
{
    char win;
    struct node *root = new_node(-1, player, NULL);
    for (int i = 0; i < ITERATIONS; i++) {
        struct node *node = root;
        char temp_table[N_GRIDS];
        memcpy(temp_table, table, N_GRIDS);
        while (1) {
            if ((win = check_win(temp_table)) != ' ') {
                Q23_8 score =
                    calculate_win_value(win, node->player ^ 'O' ^ 'X');
                backpropagate(node, score);
                break;
            }
            if (node->n_visits == 0) {
                Q23_8 score = simulate(temp_table, node->player);
                backpropagate(node, score);
                break;
            }
            if (node->children[0] == NULL)
                expand(node, temp_table);
            node = select_move(node);
            temp_table[node->move] = node->player ^ 'O' ^ 'X';
        }
    }
    struct node *best_node = NULL;
    int most_visits = -1;
    for (int i = 0; i < N_GRIDS; i++) {
        if (root->children[i] && root->children[i]->n_visits > most_visits) {
            most_visits = root->children[i]->n_visits;
            best_node = root->children[i];
        }
    }
    int best_move = -1;
    if (best_node)
        best_move = best_node->move;
    free_node(root);
    return best_move;
}
