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

    // Set all bitmap words to all-ones — every slot free
    atomic_store(&registry->word1, (REGISTRY_SIZE)~0ULL);
    for (int i = 0; i < (int)REGISTRY_BITS; i++)
        atomic_store(&registry->word2[i], (REGISTRY_SIZE)~0ULL);
    for (int i = 0; i < (int)(REGISTRY_BITS * REGISTRY_BITS); i++)
        atomic_store(&registry->word3[i], (REGISTRY_SIZE)~0ULL);

    // Reserve arena slot 0 permanently — ensures 0ULL is never a valid handle,
    // making it a safe ARENA_NULL sentinel
    atomic_fetch_and(&registry->word3[0], ~(REGISTRY_SIZE)1);

    return registry;
}

// Since registry doesn't allocate anything via pointer in its struct, freeing is simple as "free(registry)" at end of entry point
// May consider a defined destructor if we get to dynamic resizing later

ARIANDEL__arena* ARIANDEL__create_arena(ARIANDEL__registry *registry) {
    REGISTRY_SIZE top_val, mid_val, bot_val, new_bot_val;
    int top, mid, bot;

    while (1) {
        // Level 1: find a word2 group with free slots
        top_val = atomic_load_explicit(&registry->word1, memory_order_acquire);
        if (!top_val) {
            printf("Arena pool exhausted");
            exit(1);
        }
        top = ARN__CTZ(top_val);

        // Level 2: find a word3 group with free slots
        mid_val = atomic_load_explicit(&registry->word2[top], memory_order_acquire);
        if (!mid_val) {
            // Stale word1 bit — clear it and retry.
            // If a free races here and sets word2, it will also restore word1 via fetch_or.
            atomic_fetch_and_explicit(&registry->word1, ~((REGISTRY_SIZE)1 << top), memory_order_acq_rel);
            if (atomic_load_explicit(&registry->word2[top], memory_order_acquire) != 0)
                atomic_fetch_or_explicit(&registry->word1, (REGISTRY_SIZE)1 << top, memory_order_release);
            continue;
        }
        mid = ARN__CTZ(mid_val);

        int word3_idx = top * (int)REGISTRY_BITS + mid;

        // Level 3: find a free arena slot
        bot_val = atomic_load_explicit(&registry->word3[word3_idx], memory_order_acquire);
        if (!bot_val) {
            // Stale word2 bit — clear it and retry.
            atomic_fetch_and_explicit(&registry->word2[top], ~((REGISTRY_SIZE)1 << mid), memory_order_acq_rel);
            if (atomic_load_explicit(&registry->word3[word3_idx], memory_order_acquire) != 0)
                atomic_fetch_or_explicit(&registry->word2[top], (REGISTRY_SIZE)1 << mid, memory_order_release);
            continue;
        }
        bot = ARN__CTZ(bot_val);
        new_bot_val = bot_val & ~((REGISTRY_SIZE)1 << bot);

        if (!atomic_compare_exchange_weak_explicit(
                &registry->word3[word3_idx], &bot_val, new_bot_val,
                memory_order_acq_rel, memory_order_acquire))
            continue;   // Another thread grabbed a slot in this group; retry

        // Slot claimed. Propagate emptiness up the bitmap if needed.
        if (!new_bot_val) {
            atomic_fetch_and_explicit(&registry->word2[top], ~((REGISTRY_SIZE)1 << mid), memory_order_acq_rel);
            // A concurrent free may have restored word3 between our CAS and the fetch_and.
            if (atomic_load_explicit(&registry->word3[word3_idx], memory_order_acquire) != 0)
                atomic_fetch_or_explicit(&registry->word2[top], (REGISTRY_SIZE)1 << mid, memory_order_release);

            // If word2[top] is now empty, clear the word1 bit
            if (!atomic_load_explicit(&registry->word2[top], memory_order_acquire)) {
                atomic_fetch_and_explicit(&registry->word1, ~((REGISTRY_SIZE)1 << top), memory_order_acq_rel);
                if (atomic_load_explicit(&registry->word2[top], memory_order_acquire) != 0)
                    atomic_fetch_or_explicit(&registry->word1, (REGISTRY_SIZE)1 << top, memory_order_release);
            }
        }

        uint32_t id = (uint32_t)(top * (int)REGISTRY_BITS * (int)REGISTRY_BITS
                                + mid * (int)REGISTRY_BITS
                                + bot);
        registry->arenas[id] = (ARIANDEL__arena){ .bumper = 0, .capacity = DEF_ARENA_SIZE };
        registry->arenas[id].memory = malloc(DEF_ARENA_SIZE);
        if (!registry->arenas[id].memory) {
            printf("Failed to allocate arena memory for id %u", id);
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

    uint64_t aligned_bumper = (arena->bumper + 7) & ~(uint64_t)7;

    if (obj_size > MAX_ARENA_SIZE || aligned_bumper > MAX_ARENA_SIZE - obj_size) {
        printf("Proposed heap object too large for arena");
        exit(1);
    }

    if (aligned_bumper + obj_size > arena->capacity) {
        while (aligned_bumper + obj_size > arena->capacity) {
            // expansion factor of 1.5 for simplicity
            if (arena->capacity >= MAX_ARENA_SIZE / 3 * 2) {
                arena->capacity = MAX_ARENA_SIZE;
                break;
            }
            arena->capacity += arena->capacity >> 1;
        }
        // Error throws on resize call if cannot reallocate properly, no handling needed here
        ARIANDEL__resize_arena(arena, (size_t)arena->capacity);
    }

    arena->bumper = aligned_bumper + (uint64_t)obj_size;
    return (ARIANDEL__get_arena_id(registry, arena) << ARENA_OFFSET_BITS) | aligned_bumper;
}

void ARIANDEL__free_arena(ARIANDEL__registry *registry, ARIANDEL__arena *arena) {
    uint64_t id = ARIANDEL__get_arena_id(registry, arena);
    free(arena->memory);
    arena->memory = NULL;

    uint64_t bot_bit   = id % REGISTRY_BITS;
    uint64_t mid_bit   = (id / REGISTRY_BITS) % REGISTRY_BITS;
    uint64_t top_bit   = id / ((uint64_t)REGISTRY_BITS * REGISTRY_BITS);
    uint64_t word3_idx = top_bit * REGISTRY_BITS + mid_bit;

    // Restore availability bottom-up — create_arena reads top-down,
    // so a racing acquire always sees a consistent state.
    atomic_fetch_or_explicit(&registry->word3[word3_idx], (REGISTRY_SIZE)1 << bot_bit, memory_order_release);
    atomic_fetch_or_explicit(&registry->word2[top_bit],   (REGISTRY_SIZE)1 << mid_bit, memory_order_release);
    atomic_fetch_or_explicit(&registry->word1,            (REGISTRY_SIZE)1 << top_bit, memory_order_release);
}
