#pragma once

#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#if defined(_MSC_VER)
#  define THREAD_LOCAL __declspec(thread)
#else
#  define THREAD_LOCAL __thread
#endif

#define DEF_ARENA_SIZE 256         // 256 bytes = 0.25KB per initial arena memory allocation
#define MAX_ARENA_SIZE UINT32_MAX  // Max size 4GB for PoC
#define ARENA_PTR      uint64_t
#define ARENA_NULL     0ULL        // Null handle sentinel — arena 0 is never user-allocated

typedef struct {
    uint8_t *memory;   // Byte-sized pointer
    uint32_t bumper;   // Can be smaller/larger depending on config settings, 32-bit covers the PoC design
    uint32_t capacity; // in bytes
} ARIANDEL__arena;

// This registry should expand in theory as needed, leaving as a fixed 4096 arena cap for initial proof of concept
// Size is ~64.5KB, could put on stack or heap in theory, will put on heap for PoC
// Later, struct could live on stack with pointer to heap for dynamic-length words when expansion is added
typedef struct {
    _Atomic uint64_t top_word;      // Find the first uint64_t in bottom_word that has an available slot
    _Atomic uint64_t bot_word[64];  // This model supports 64 x 64 = 4,096 arenas at program start
    ARIANDEL__arena  arenas[4096];  // Actual arena structs
} ARIANDEL__registry;

ARIANDEL__registry* ARIANDEL__create_registry();
ARIANDEL__arena*    ARIANDEL__create_arena(ARIANDEL__registry *registry);

static inline uint64_t ARIANDEL__get_arena_id(ARIANDEL__registry *registry, ARIANDEL__arena *arena) {
    return arena - registry->arenas;
}

static inline ARIANDEL__arena* ARIANDEL__get_arena_from_ptr(ARIANDEL__registry *registry, ARENA_PTR ptr) {
    return &registry->arenas[ptr >> 32];
}

void      ARIANDEL__resize_arena(ARIANDEL__arena *arena,       size_t new_size);
ARENA_PTR ARIANDEL__alloc_arena( ARIANDEL__registry *registry, ARIANDEL__arena *arena, size_t obj_size);
void      ARIANDEL__free_arena(  ARIANDEL__registry *registry, ARIANDEL__arena *arena);

static inline void* ARIANDEL__deref_ptr(ARIANDEL__registry *registry, ARENA_PTR ptr) {
    return registry->arenas[ptr >> 32].memory + (ptr & UINT32_MAX);
}

static inline void  ARIANDEL__memcpy(ARIANDEL__registry *registry, ARENA_PTR dst, ARENA_PTR src, size_t size) {
    memcpy(ARIANDEL__deref_ptr(registry, dst), ARIANDEL__deref_ptr(registry, src), size);
}