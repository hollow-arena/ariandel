#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define N 12

static double ms(clock_t a, clock_t b) {
    return (double)(b - a) / CLOCKS_PER_SEC * 1000.0;
}

// Idiomatic C N-Queens — no Ariandel.
// Demonstrates the "collect from recursion" pattern:
//   - Each solution is malloc'd and pushed onto a heap-allocated list
//   - Cleanup requires iterating the entire solution list to free each entry — O(n)
//   - Any missed free leaks silently

typedef struct {
    int  **solutions;
    int    count;
    int    capacity;
} SolutionList;

static void list_push(SolutionList *list, int *board) {
    if (list->count == list->capacity) {
        list->capacity *= 2;
        list->solutions = realloc(list->solutions, sizeof(int*) * list->capacity);
    }
    int *copy = malloc(sizeof(int) * N);
    memcpy(copy, board, sizeof(int) * N);
    list->solutions[list->count++] = copy;
}

static int is_safe(int *board, int row, int col) {
    for (int r = 0; r < row; r++) {
        int c = board[r];
        int diff = c - col;
        if (diff < 0) diff = -diff;
        if (c == col || diff == row - r) return 0;
    }
    return 1;
}

static void solve(SolutionList *list, int *board, int row) {
    if (row == N) {
        list_push(list, board);
        return;
    }
    for (int col = 0; col < N; col++) {
        if (is_safe(board, row, col)) {
            board[row] = col;
            solve(list, board, row + 1);
        }
    }
}

int main() {
    int *board = malloc(sizeof(int) * N);

    SolutionList list;
    list.capacity  = 256;
    list.count     = 0;
    list.solutions = malloc(sizeof(int*) * list.capacity);

    clock_t t0, t1, t2;

    t0 = clock();
    solve(&list, board, 0);
    t1 = clock();

    // Must iterate entire solution list to free — O(n)
    for (int i = 0; i < list.count; i++) free(list.solutions[i]);
    free(list.solutions);
    t2 = clock();

    printf("%d-Queens: %d solutions\n", N, list.count);
    printf("solve:%.3f cleanup:%.3f total:%.3f\n",
        ms(t0, t1), ms(t1, t2), ms(t0, t2));

    free(board);
    return 0;
}
