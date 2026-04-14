#include "ariandel_rt.h"

// Using naive printf / exit(1) statements for failed allocations for initial PoC
// In practice, error handling should be more holistic and/or graceful

ARIANDEL__registry           *arn__registry = NULL;
THREAD_LOCAL ARIANDEL__arena *arn__tl_arena = NULL;

ARIANDEL__registry* ARIANDEL__create_registry() {
    ARIANDEL__registry *registry = calloc(1, sizeof(ARIANDEL__registry));
    if (!registry) {
        printf("Failed to allocate arena registry in memory");
        exit(1);
    }

    // Assign all 1's to both the top and bottom word, indicating available slots
    atomic_store(&registry->top_word, ~0ULL);
    for (int i = 0; i < 64; i++) atomic_store(&registry->bot_word[i], ~0ULL);

    // Reserve arena slot 0 permanently — ensures 0ULL is never a valid handle,
    // making it a safe ARENA_NULL sentinel
    atomic_fetch_and(&registry->bot_word[0], ~1ULL);

    return registry;
}

// Since registry doesn't allocate anything via pointer in its struct, freeing is simple as "free(registry)" at end of entry point
// May consider a defined destructor if we get to dynamic resizing later

ARIANDEL__arena* ARIANDEL__create_arena(ARIANDEL__registry *registry) {
    uint64_t top_val, bot_val, new_bot_val;
    int top, bot;

    while (1) {
        top_val = atomic_load_explicit(&registry->top_word, memory_order_acquire);
        if (!top_val) {
            printf("Arena pool exhausted");
            exit(1);
        }
        top = __builtin_ctz(top_val);

        bot_val = atomic_load_explicit(&registry->bot_word[top], memory_order_acquire);
        if (!bot_val) {
            // Stale top_word bit — clear it and retry.
            // If a free races here and sets bot_word, it will also restore top_word via fetch_or.
            atomic_fetch_and_explicit(&registry->top_word, ~(1ULL << top), memory_order_acq_rel);
            if (atomic_load_explicit(&registry->bot_word[top], memory_order_acquire) != 0)
                atomic_fetch_or_explicit(&registry->top_word, 1ULL << top, memory_order_release);
            continue;
        }

        bot = __builtin_ctz(bot_val);
        new_bot_val = bot_val & ~(1ULL << bot);

        if (!atomic_compare_exchange_weak_explicit(
                &registry->bot_word[top], &bot_val, new_bot_val,
                memory_order_acq_rel, memory_order_acquire))
            continue;   // Another thread grabbed a slot in this group; retry

        // Slot claimed. If the group is now empty, clear the top_word bit.
        if (!new_bot_val) {
            atomic_fetch_and_explicit(&registry->top_word, ~(1ULL << top), memory_order_acq_rel);
            // A concurrent free may have set bot_word between our CAS and the fetch_and.
            // If so, restore the top_word bit so the group stays visible.
            if (atomic_load_explicit(&registry->bot_word[top], memory_order_acquire) != 0)
                atomic_fetch_or_explicit(&registry->top_word, 1ULL << top, memory_order_release);
        }

        uint16_t id = top * 64 + bot;
        registry->arenas[id] = (ARIANDEL__arena){ .bumper = 0, .capacity = DEF_ARENA_SIZE };
        registry->arenas[id].memory = malloc(sizeof(char) * DEF_ARENA_SIZE);
        if (!registry->arenas[id].memory) {
            printf("Failed to allocate arena memory for id %d", id);
            exit(1);
        }

        return &registry->arenas[id];
    }
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

    if ((size_t)aligned_bumper + (size_t)obj_size > (size_t)arena->capacity) {
        while ((size_t)aligned_bumper + (size_t)obj_size > (size_t)arena->capacity) {
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

    // bot_word first, then top_word — create_arena reads in this order,
    // so a racing acquire always sees a consistent state.
    atomic_fetch_or_explicit(&registry->bot_word[bot_idx], 1ULL << (id % 64), memory_order_release);
    atomic_fetch_or_explicit(&registry->top_word, 1ULL << bot_idx, memory_order_release);
}