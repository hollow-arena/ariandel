#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../../runtime/ariandel.h"

#define N 12

static double ms(clock_t a, clock_t b) {
    return (double)(b - a) / CLOCKS_PER_SEC * 1000.0;
}

// Ariandel macro version — same logic as n_queens.c, mechanical transformation applied.
// Demonstrates: ALLOC_INTO routing for collect-from-recursion, O(1) scope cleanup.

typedef struct {
    ARENA_PTR  solutions;
    int        count;
    int        capacity;
} SolutionList;

static void list_push(ARENA_PTR list_h, ARENA_PTR board_h) {
    int       count    = DEREF(list_h, SolutionList)->count;
    int       capacity = DEREF(list_h, SolutionList)->capacity;
    ARENA_PTR sols     = DEREF(list_h, SolutionList)->solutions;

    if (count == capacity) {
        capacity *= 2;
        ARENA_PTR new_sols = ALLOC_INTO(list_h, sizeof(ARENA_PTR) * capacity);
        memcpy(DEREF(new_sols, ARENA_PTR), DEREF(sols, ARENA_PTR), sizeof(ARENA_PTR) * count);
        sols = new_sols;
    }
    ARENA_PTR copy = ALLOC_INTO(list_h, sizeof(int) * N);
    memcpy(DEREF(copy, int), DEREF(board_h, int), sizeof(int) * N);
    DEREF(sols, ARENA_PTR)[count] = copy;

    DEREF(list_h, SolutionList)->count     = count + 1;
    DEREF(list_h, SolutionList)->capacity  = capacity;
    DEREF(list_h, SolutionList)->solutions = sols;
}

static int is_safe(ARENA_PTR board_h, int row, int col) {
    int *board = DEREF(board_h, int);
    for (int r = 0; r < row; r++) {
        int c = board[r];
        int diff = c - col;
        if (diff < 0) diff = -diff;
        if (c == col || diff == row - r) return 0;
    }
    return 1;
}

static void solve(ARENA_PTR list_h, ARENA_PTR board_h, int row) {
    if (row == N) {
        list_push(list_h, board_h);
        return;
    }
    for (int col = 0; col < N; col++) {
        if (is_safe(board_h, row, col)) {
            DEREF(board_h, int)[row] = col;
            solve(list_h, board_h, row + 1);
        }
    }
}

int main() {
    ARIANDEL_INIT();

    clock_t t0, t1, t2;

    t0 = clock();
    SCOPE_BEGIN {
        ARENA_PTR list_h    = ALLOC(sizeof(SolutionList));
        ARENA_PTR solutions = ALLOC_INTO(list_h, sizeof(ARENA_PTR) * 256);
        ARENA_PTR board_h   = ALLOC(sizeof(int) * N);
        SolutionList *list  = DEREF(list_h, SolutionList);
        list->capacity      = 256;
        list->count         = 0;
        list->solutions     = solutions;

        solve(list_h, board_h, 0);
        t1 = clock();

        printf("%d-Queens: %d solutions\n", N, DEREF(list_h, SolutionList)->count);
    } SCOPE_END;
    t2 = clock();

    printf("solve:%.3f cleanup:%.3f total:%.3f\n",
        ms(t0, t1), ms(t1, t2), ms(t0, t2));

    ARIANDEL_DESTROY();
    return 0;
}
