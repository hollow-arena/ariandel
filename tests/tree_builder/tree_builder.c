#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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
    struct Node *left;
    struct Node *right;
} Node;

Node* make_node(int value, Node *left, Node *right) {
    Node *n = malloc(sizeof(Node));
    n->value = value;
    n->left  = left;
    n->right = right;
    return n;
}

// Builds a balanced BST from a sorted array
Node* build_tree(int *values, int lo, int hi) {
    if (lo > hi) return NULL;
    int mid = (lo + hi) / 2;
    return make_node(
        values[mid],
        build_tree(values, lo, mid - 1),
        build_tree(values, mid + 1, hi)
    );
}

void print_inorder(Node *node) {
    if (!node) return;
    print_inorder(node->left);
    printf("%d ", node->value);
    print_inorder(node->right);
}

// Must traverse the entire tree to free it — O(n), error-prone
void free_tree(Node *node) {
    if (!node) return;
    free_tree(node->left);
    free_tree(node->right);
    free(node);
}

int main() {
    int *values = malloc(sizeof(int) * BENCH_N);
    for (int i = 0; i < BENCH_N; i++) values[i] = i;

    clock_t t0, t1, t2;

    t0 = clock();
    Node *tree = build_tree(values, 0, BENCH_N - 1);
    t1 = clock();
    free_tree(tree);
    t2 = clock();

    printf("build:%.3f cleanup:%.3f total:%.3f\n",
        ms(t0, t1), ms(t1, t2), ms(t0, t2));

    free(values);
    return 0;
}
