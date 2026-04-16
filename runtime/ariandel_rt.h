#pragma once

#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#if defined(_MSC_VER)
#  define THREAD_LOCAL __declspec(thread)
#else
#  define THREAD_LOCAL __thread
#endif

#ifndef REGISTRY_SIZE
#define REGISTRY_SIZE uint16_t
#endif

static_assert(
sizeof(REGISTRY_SIZE) == 1 ||
sizeof(REGISTRY_SIZE) == 2 ||
sizeof(REGISTRY_SIZE) == 4 ||
sizeof(REGISTRY_SIZE) == 8,
"Must use a valid uint value for REGISTRY_SIZE");

#define REGISTRY_BITS     (sizeof(REGISTRY_SIZE) * 8)

// log2(REGISTRY_BITS) as a constant expression, __builtin_ctz is not valid in
// constant expressions (struct sizes, static_assert), so use a ternary instead.
#define ARN__LOG2_BITS \
    ((sizeof(REGISTRY_SIZE) == 8) ? 6 : \
     (sizeof(REGISTRY_SIZE) == 4) ? 5 : \
     (sizeof(REGISTRY_SIZE) == 2) ? 4 : 3)

#define ARENA_ID_BITS     (3 * ARN__LOG2_BITS)
#define ARENA_OFFSET_BITS (64 - ARENA_ID_BITS)
#define ARENA_OFFSET_MASK ((1ULL << ARENA_OFFSET_BITS) - 1ULL)

// always use ctzll; smaller types zero-extend to uint64_t correctly.
#define ARN__CTZ(x) __builtin_ctzll((unsigned long long)(x))

#define DEF_ARENA_SIZE 256         // 256 bytes = 0.25KB per initial arena memory allocation
#define MAX_ARENA_SIZE (1ULL << ARENA_OFFSET_BITS)
#define ARENA_PTR      uint64_t
#define ARENA_NULL     0ULL        // Null handle sentinel — arena 0 is never user-allocated

typedef struct {
    uint8_t *memory;   // Byte-sized pointer
    uint64_t bumper;   // Byte offset of next free slot; capped at MAX_ARENA_SIZE
    uint64_t capacity; // in bytes
} ARIANDEL__arena;

// Pool capacity = REGISTRY_BITS³ arenas (4,096 with default uint16_t). Registry lives on the heap.
// Dynamic expansion on exhaustion is not yet implemented, pool size is fixed at compile time.
typedef struct {
    _Atomic REGISTRY_SIZE word1;
    _Atomic REGISTRY_SIZE word2[REGISTRY_BITS];
    _Atomic REGISTRY_SIZE word3[REGISTRY_BITS * REGISTRY_BITS];
    ARIANDEL__arena       arenas[REGISTRY_BITS * REGISTRY_BITS * REGISTRY_BITS];  // Actual arena structs
} ARIANDEL__registry;

ARIANDEL__registry* ARIANDEL__create_registry();
ARIANDEL__arena*    ARIANDEL__create_arena(ARIANDEL__registry *registry);

static inline uint64_t ARIANDEL__get_arena_id(ARIANDEL__registry *registry, ARIANDEL__arena *arena) {
    return arena - registry->arenas;
}

static inline ARIANDEL__arena* ARIANDEL__get_arena_from_ptr(ARIANDEL__registry *registry, ARENA_PTR ptr) {
    return &registry->arenas[ptr >> ARENA_OFFSET_BITS];
}

void      ARIANDEL__resize_arena(ARIANDEL__arena *arena,       size_t new_size);
ARENA_PTR ARIANDEL__alloc_arena( ARIANDEL__registry *registry, ARIANDEL__arena *arena, size_t obj_size);
void      ARIANDEL__free_arena(  ARIANDEL__registry *registry, ARIANDEL__arena *arena);

static inline void* ARIANDEL__deref_ptr(ARIANDEL__registry *registry, ARENA_PTR ptr) {
    return registry->arenas[ptr >> ARENA_OFFSET_BITS].memory + (ptr & ARENA_OFFSET_MASK);
}

static inline void  ARIANDEL__memcpy(ARIANDEL__registry *registry, ARENA_PTR dst, ARENA_PTR src, size_t size) {
    memcpy(ARIANDEL__deref_ptr(registry, dst), ARIANDEL__deref_ptr(registry, src), size);
}