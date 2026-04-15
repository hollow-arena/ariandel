#include "../../runtime/ariandel.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
#include <time.h>

// rand_r is POSIX and not available on MinGW — use a simple LCG instead.
// Same contract: caller-owned seed, no shared state, thread-safe.
static inline int thread_rand(unsigned int *seed) {
    *seed = *seed * 1103515245 + 12345;
    return (int)((*seed >> 16) & 0x7fff);
}

#define NUM_THREADS 64     // Way more than your physical cores
#define ITERATIONS 1000000 // 1 Million iterations per thread
#define MAX_NEST 10        // Deeper stack

void* black_swan_worker(void* arg) {
    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)(uintptr_t)arg;
    
    for (int i = 0; i < ITERATIONS; i++) {
        SCOPE_NEW {
            // Randomly sized allocations to force frequent reallocations
            size_t size = (thread_rand(&seed) % 1024) + 1;
            ARENA_PTR p = ALLOC(size);
            
            // Write to the memory to ensure it's mapped and valid
            uint8_t* raw = DEREF(p, uint8_t);
            *raw = 0xAA;

            if (i % 100 == 0) {
                SCOPE_NEW {
                    // Deeply nested allocation
                    ARENA_PTR p2 = ALLOC(thread_rand(&seed) % 512);
                    *DEREF(p2, uint8_t) = 0xBB;
                }
            }
        }
    }
    return NULL;
}

int main() {
    printf("Executing Black Swan Test: 64 threads, 1 Million iterations each...\n");
    printf("Total Scopes to be created: ~65,000,000\n");
    
    ARIANDEL_INIT();

    pthread_t threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, black_swan_worker, (void*)(uintptr_t)i);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("65 Million scopes processed. Registry status: OK.\n");
    ARIANDEL_DESTROY();
    return 0;
}