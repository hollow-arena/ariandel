#pragma once

#include "ariandel_rt.h"

// Global registry and thread-local active arena context
extern ARIANDEL__registry           *arn__registry;
extern THREAD_LOCAL ARIANDEL__arena *arn__tl_arena;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Call once at program entry before any SCOPE or allocation
#define ARIANDEL_INIT() \
    arn__registry = ARIANDEL__create_registry(); \
    for (ARIANDEL__scope_guard arn__guard \
             __attribute__((cleanup(ARIANDEL__cleanup_new_scope))) \
             = { ARIANDEL__create_arena(arn__registry), arn__tl_arena }; \
         (arn__tl_arena = arn__guard.scope, arn__guard.scope != NULL); \
         (ARIANDEL__free_arena(arn__registry, arn__guard.scope), arn__guard.scope = NULL)) {

// Call once at program exit, arenas are freed by their scope cleanup functions;
// this only frees the registry struct itself
#define ARIANDEL_DESTROY() \
    } free(arn__registry)

// ---------------------------------------------------------------------------
// Scope guard - bundles the scope arena with the saved outer arena for cleanup
// ---------------------------------------------------------------------------

typedef struct {
    ARIANDEL__arena *scope;
    ARIANDEL__arena *saved;
} ARIANDEL__scope_guard;

// Called automatically by GCC/Clang on any exit from SCOPE_NEW: return, break, or normal
static inline void ARIANDEL__cleanup_new_scope(ARIANDEL__scope_guard *g) {
    if (g->scope) ARIANDEL__free_arena(arn__registry, g->scope);
    arn__tl_arena = g->saved;
}

// Called automatically by GCC/Clang on any exit from SCOPE, restores without freeing
static inline void ARIANDEL__cleanup_scope(ARIANDEL__scope_guard *g) {
    arn__tl_arena = g->saved;
}

// ---------------------------------------------------------------------------
// Scopes
// ---------------------------------------------------------------------------

// Opens a new scope and creates a fresh arena and sets it as the active arena.
// Cleanup (free + restore) runs automatically on any exit, including return/break.
#define SCOPE_NEW \
    for (ARIANDEL__scope_guard arn__guard \
             __attribute__((cleanup(ARIANDEL__cleanup_new_scope))) \
             = { ARIANDEL__create_arena(arn__registry), arn__tl_arena }; \
         (arn__tl_arena = arn__guard.scope, arn__guard.scope != NULL); \
         (ARIANDEL__free_arena(arn__registry, arn__guard.scope), arn__guard.scope = NULL))

// Opens an existing arena based on the home arena of the specified pointer.
// Does not free on exit, the pointer's arena is owned elsewhere.
#define SCOPE(ptr) \
    for (ARIANDEL__scope_guard arn__guard \
             __attribute__((cleanup(ARIANDEL__cleanup_scope))) \
             = { ARIANDEL__get_arena_from_ptr(arn__registry, (ptr)), arn__tl_arena }; \
         (arn__tl_arena = arn__guard.scope, arn__guard.scope != NULL); \
         arn__guard.scope = NULL)

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------

// Allocate obj_size bytes in the current scope's arena.
// Returns an ARENA_PTR handle.
#define ALLOC(obj_size) \
    ARIANDEL__alloc_arena(arn__registry, arn__tl_arena, obj_size)

// ---------------------------------------------------------------------------
// Dereference
// ---------------------------------------------------------------------------

// Resolve a handle to a typed pointer. Result is valid only until the next
// allocation that triggers a realloc.
#define DEREF(handle, Type) \
    ((Type*)ARIANDEL__deref_ptr(arn__registry, (handle)))

#define COPY(dst, src, size) \
    ARIANDEL__memcpy(arn__registry, dst, src, size)
