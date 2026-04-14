#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../../runtime/ariandel.h"

#define BENCH_N 1000000

static double ms(clock_t a, clock_t b) {
    return (double)(b - a) / CLOCKS_PER_SEC * 1000.0;
}

// Idiomatic C tree builder — no Ariandel.
// This is the "before" picture: what a programmer naturally writes.
// Key things Ariandel targets:
//   - make_node() returns a raw pointer (malloc inside)
//   - free_tree() must recursively traverse to free — O(n)
//   - Any bug in free_tree leaks memory silently

typedef struct Node {
    int value;
    ARENA_PTR left;
    ARENA_PTR right;
} Node;

ARENA_PTR make_node(int value, ARENA_PTR left, ARENA_PTR right) {
    ARENA_PTR n = ALLOC(sizeof(Node));
    DEREF(n, Node)->value = value;
    DEREF(n, Node)->left  = left;
    DEREF(n, Node)->right = right;
    return n;
}

// Builds a balanced BST from a sorted array
ARENA_PTR build_tree(ARENA_PTR values, int lo, int hi) {
    if (lo > hi) return ARENA_NULL;
    int mid = (lo + hi) / 2;
    return make_node(
        DEREF(values, int)[mid],
        build_tree(values, lo, mid - 1),
        build_tree(values, mid + 1, hi)
    );
}

// void print_inorder(Node *node) {
//     if (!node) return;
//     print_inorder(node->left);
//     printf("%d ", node->value);
//     print_inorder(node->right);
// }

// Must traverse the entire tree to free it — O(n), error-prone
// void free_tree(Node *node) {
//     if (!node) return;
//     free_tree(node->left);
//     free_tree(node->right);
//     free(node);
// }

int main() {
    ARIANDEL_INIT();
    clock_t t0, t1, t2;
    SCOPE_NEW {
        ARENA_PTR values = ALLOC(sizeof(int) * BENCH_N);
        for (int i = 0; i < BENCH_N; i++) DEREF(values, int)[i] = i;

        t0 = clock();
        ARENA_PTR tree = build_tree(values, 0, BENCH_N - 1);
        t1 = clock();
    }

    t2 = clock();

    printf("build:%.3f cleanup:%.3f total:%.3f\n",
        ms(t0, t1), ms(t1, t2), ms(t0, t2));

    ARIANDEL_DESTROY();
    return 0;
}
