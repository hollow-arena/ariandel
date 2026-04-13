#include <stdio.h>
#include <time.h>
#include "../../runtime/ariandel.h"

#define BENCH_N 1000000

static double ms(clock_t a, clock_t b) {
    return (double)(b - a) / CLOCKS_PER_SEC * 1000.0;
}

// Ariandel macro version — same logic as tree_builder.c, mechanical transformation applied.
// Demonstrates: ALLOC_INTO routing, ARENA_NULL sentinel, DEREF field access, O(1) scope cleanup.

typedef struct Node {
    int value;
    ARENA_PTR left;
    ARENA_PTR right;
} Node;

void make_node(ARENA_PTR slot, int value, ARENA_PTR left, ARENA_PTR right) {
    Node *n   = DEREF(slot, Node);
    n->value  = value;
    n->left   = left;
    n->right  = right;
}

// Builds a balanced BST from a sorted array
void build_tree(ARENA_PTR tree, int *values, int lo, int hi) {
    if (lo > hi) return;
    int mid = (lo + hi) / 2;

    ARENA_PTR tmp_l = (lo <= mid - 1) ? ALLOC_INTO(tree, sizeof(Node)) : ARENA_NULL;
    ARENA_PTR tmp_r = (mid + 1 <= hi) ? ALLOC_INTO(tree, sizeof(Node)) : ARENA_NULL;

    build_tree(tmp_l, values, lo, mid - 1);
    build_tree(tmp_r, values, mid + 1, hi);
    make_node(tree, values[mid], tmp_l, tmp_r);
}

void print_inorder(ARENA_PTR node) {
    if (node == ARENA_NULL) return;
    print_inorder(DEREF(node, Node)->left);
    printf("%d ", DEREF(node, Node)->value);
    print_inorder(DEREF(node, Node)->right);
}

int main() {
    ARIANDEL_INIT();

    SCOPE_BEGIN {

        ARENA_PTR values = ALLOC(sizeof(int) * BENCH_N);
        for (int i = 0; i < BENCH_N; i++) DEREF(values, int)[i] = i;

        clock_t t0, t1, t2;

        t0 = clock();
        SCOPE_BEGIN {
            ARENA_PTR tree = ALLOC(sizeof(Node));
            build_tree(tree, DEREF(values, int), 0, BENCH_N - 1);
            t1 = clock();
        } SCOPE_END;
        t2 = clock();

        printf("build:%.3f cleanup:%.3f total:%.3f\n",
            ms(t0, t1), ms(t1, t2), ms(t0, t2));
    } SCOPE_END;

    ARIANDEL_DESTROY();
    return 0;
}
