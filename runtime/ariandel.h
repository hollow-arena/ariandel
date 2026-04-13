#pragma once

#include "ariandel_rt.h"

// Global registry — initialized by ARIANDEL_INIT, used implicitly by all macros
static ARIANDEL__registry *__registry;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Call once at program entry before any SCOPE or allocation
#define ARIANDEL_INIT() \
    __registry = ARIANDEL__create_registry()

// Call once at program exit
#define ARIANDEL_DESTROY() \
    free(__registry)

// ---------------------------------------------------------------------------
// Scopes
// ---------------------------------------------------------------------------

// Opens a scope — acquires an arena, exposes it as __arena for ALLOC calls.
// Nested SCOPE_BEGIN blocks shadow __arena correctly with their own local.
#define SCOPE_BEGIN \
    { \
        ARIANDEL__arena *__arena = ARIANDEL__create_arena(__registry);

// Closes the scope — resets and releases the arena. O(1) regardless of allocations.
#define SCOPE_END \
        ARIANDEL__free_arena(__registry, __arena); \
    }

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------

// Allocate sizeof(Type) bytes in the current scope's arena.
// Returns an ARENA_PTR handle.
#define ALLOC(obj_size) \
    ARIANDEL__alloc_arena(__registry, __arena, obj_size)

// Allocate obj_size bytes in the same arena as an existing handle.
// Use when an object must outlive the current scope by living in an outer arena.
#define ALLOC_INTO(handle, obj_size) \
    ARIANDEL__alloc_arena(__registry, ARIANDEL__get_arena_from_ptr(__registry, (handle)), obj_size)

// ---------------------------------------------------------------------------
// Dereference
// ---------------------------------------------------------------------------

// Resolve a handle to a typed pointer. Result is valid only until the next
// allocation that triggers a realloc — do not cache across allocations.
#define DEREF(handle, Type) \
    ((Type*)ARIANDEL__deref_ptr(__registry, (handle)))
