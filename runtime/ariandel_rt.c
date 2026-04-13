#include "ariandel_rt.h"

// Using naive printf / exit(1) statements for failed allocations for initial PoC
// In practice, error handling should be more holistic and/or graceful

ARIANDEL__registry* ARIANDEL__create_registry() {
    ARIANDEL__registry *registry = calloc(1, sizeof(ARIANDEL__registry));
    if (!registry) {
        printf("Failed to allocate arena registry in memory");
        exit(1);
    }

    // Assign all 1's to both the top and bottom word, indicating available slots
    registry->top_word = ~0ULL;
    for (int i = 0; i < 64; i++) registry->bot_word[i] = ~0ULL;

    // Reserve arena slot 0 permanently — ensures 0ULL is never a valid handle,
    // making it a safe ARENA_NULL sentinel
    registry->bot_word[0] &= ~1ULL;

    return registry;
}

// Since registry doesn't allocate anything via pointer in its struct, freeing is simple as "free(registry)" at end of entry point
// May consider a defined destructor if we get to dynamic resizing later

ARIANDEL__arena* ARIANDEL__create_arena(ARIANDEL__registry *registry) {

    if (!registry->top_word) {
        printf("Arena pool exhausted");
        exit(1);
    }

    uint8_t top = __builtin_ctz(registry->top_word);
    uint8_t bot = __builtin_ctz(registry->bot_word[top]);

    // Set arena as in use
    registry->bot_word[top] &= ~(1ULL << bot);

    // Update top word if all bits are 0 in relevant bot word index
    if (!registry->bot_word[top])
        registry->top_word &= ~(1ULL << top);

    uint16_t id = top * 64 + bot;
    registry->arenas[id] = (ARIANDEL__arena){ .bumper = 0, .capacity = DEF_ARENA_SIZE};
    registry->arenas[id].memory = malloc(sizeof(char) * DEF_ARENA_SIZE);
    if (!registry->arenas[id].memory) {
        free(registry);
        printf("Failed to allocate arena memory for id %d", id);
        exit(1);
    }

    return &registry->arenas[id];
}

void ARIANDEL__resize_arena(ARIANDEL__arena *arena, size_t new_size) {
    uint8_t *new_memory = realloc(arena->memory, new_size);

    // OK not to free other arenas and registry since exit should free everything?
    // Probably ok for PoC but needs attention for finalizers / file handlers
    if (!new_memory) {
        printf("Failed to resize current arena");
        exit(1);
    }

    arena->memory = new_memory;
}

ARENA_PTR ARIANDEL__alloc_arena(ARIANDEL__registry *registry, ARIANDEL__arena *arena, size_t obj_size) {

    uint32_t aligned_bumper = (arena->bumper + 7) & ~7u;

    if (aligned_bumper + obj_size > MAX_ARENA_SIZE) {
        printf("Proposed heap object too large for arena in this PoC");
        exit(1);
    }

    if (arena->capacity - aligned_bumper < obj_size) {
        while (arena->capacity - aligned_bumper < obj_size) {
            // expansion factor of 1.5 for simplicity
            if (arena->capacity >= MAX_ARENA_SIZE / 3 * 2) {
                arena->capacity = MAX_ARENA_SIZE;
                break;
            }
            arena->capacity += arena->capacity >> 1;
        }
        // Error throws on resize call if cannot reallocate properly, no handling needed here
        ARIANDEL__resize_arena(arena, arena->capacity);
    }

    arena->bumper = aligned_bumper + obj_size;
    return (ARIANDEL__get_arena_id(registry, arena) << 32) | aligned_bumper;
}

void ARIANDEL__free_arena(ARIANDEL__registry *registry, ARIANDEL__arena *arena) {
    uint64_t id = ARIANDEL__get_arena_id(registry, arena);
    free(arena->memory);
    arena->memory = NULL;

    uint64_t bot_idx = id / 64;

    // Set top word bit unconditionally
    registry->top_word |= 1ULL << bot_idx;

    // Update bot word bit
    registry->bot_word[bot_idx] |= 1ULL << (id % 64);
}