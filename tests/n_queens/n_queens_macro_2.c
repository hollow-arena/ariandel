#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../runtime/ariandel.h"

#define N 13

static double ms(clock_t a, clock_t b) {
    return (double)(b - a) / CLOCKS_PER_SEC * 1000.0;
}

typedef struct {
    ARENA_PTR solutions;
    int       count;
    int       capacity;
} SolutionList;

static void list_push(SolutionList *list, ARENA_PTR board) {
    if (list->count == list->capacity) {
        list->capacity *= 2;
        ARENA_PTR new_solutions = ALLOC(sizeof(ARENA_PTR) * list->capacity);
        COPY(new_solutions, list->solutions, list->capacity * sizeof(ARENA_PTR) / 2);
        list->solutions = new_solutions;
    }
    // printf("board handle: %llu arena: %llu offset: %llu\n", 
    // board, board >> 32, board & UINT32_MAX);
    ARENA_PTR copy = ALLOC(sizeof(int) * N);
    // printf("copy handle: %llu arena: %llu offset: %llu\n",
    // copy, copy >> 32, copy & UINT32_MAX);
    COPY(copy, board, sizeof(int) * N);
    DEREF(list->solutions, ARENA_PTR)[list->count++] = copy;
}

static int is_safe(ARENA_PTR board, int row, int col) {
    for (int r = 0; r < row; r++) {
        int c = DEREF(board, int)[r];
        int diff = c - col;
        if (diff < 0) diff = -diff;
        if (c == col || diff == row - r) return 0;
    }
    return 1;
}

static void solve(SolutionList *list, ARENA_PTR board, int row) {
    if (row == N) {
        list_push(list, board);
        return;
    }
    for (int col = 0; col < N; col++) {
        if (is_safe(board, row, col)) {
            DEREF(board, int)[row] = col;
            solve(list, board, row + 1);
        }
    }
}

int main() {
    ARIANDEL_INIT();
    
    clock_t t0, t1, t2;
    SolutionList list;
    SCOPE_NEW {
        ARENA_PTR board = ALLOC(sizeof(int) * N);

        list.capacity  = 256;
        list.count     = 0;
        list.solutions = ALLOC(sizeof(ARENA_PTR) * list.capacity);

        t0 = clock();
        solve(&list, board, 0);
        t1 = clock();
    }
    t2 = clock();

    printf("%d-Queens: %d solutions\n", N, list.count);
    printf("solve:%.3f cleanup:%.3f total:%.3f\n",
        ms(t0, t1), ms(t1, t2), ms(t0, t2));

    ARIANDEL_DESTROY();
    return 0;
}
