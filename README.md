# Ariandel Memory Model — Specification v0.1

## Overview

Ariandel is a deterministic, scope-structured memory management model targeting compiled, statically-typed languages. It provides automatic heap memory reclamation without stop-the-world GC pauses, without manual memory management, without a borrow checker, and without cycle detection. Safety and allocation routing are resolved entirely at compile time via a mechanical desugaring transformation. The runtime overhead is a bump-pointer allocation and an atomic bitmask operation per scope boundary. For any compiled, statically typed language whose compiler implements the desugaring rule, allocation and deallocation performance is equivalent to a handwritten C bump allocator — the fastest allocation strategy available.

---

## Core Invariants

1. **Stack allocation is permitted; returning a stack pointer is not syntactically expressible.** A user may stack-allocate a struct, pass a raw pointer to it into a void function for construction or mutation, and use it freely within the same scope. Stack memory has a stable address and cannot move, so raw pointers to stack variables are safe within their scope. Raw pointers into arena-backed heap memory are not safe — arena backing slabs may be moved by `realloc()` as allocation grows, invalidating any raw pointer into them. Heap objects must always be referenced via handles, which resolve `base + offset` at dereference time and remain valid across realloc. What the language makes mechanically impossible is *returning* a raw pointer from a function — because any function with a pointer return type is subject to the desugaring rule, which pre-allocates the return value in the caller's arena. There is no syntax for "return a pointer to my stack frame" because pointer-returning functions are, by definition, arena-routed. The borrow checker's core rule — no reference may outlive its owner — is enforced by omission of syntax rather than by compiler error.
2. **All heap allocation is arena-backed.** Every heap object lives in exactly one arena. Scope cleanup is a bump-pointer reset — O(1) regardless of how many objects are freed.
3. **Allocation is pre-routed at compile time.** The compiler determines the correct arena for every heap allocation before the program runs. No runtime escape analysis, no runtime promotion, no per-object lifetime tracking.
4. **Everything remaining in a scope's arena at scope exit is dead by construction.** The desugaring rule guarantees that any object meant to outlive a scope was allocated into an outer arena from the start. Scope cleanup requires no per-object inspection.
5. **Handles are stable; raw pointers from DEREF are ephemeral.** An `ARENA_PTR` handle is a plain integer and survives any number of allocations — the underlying arena slab may move via `realloc()`, but the handle resolves correctly at dereference time because it stores an offset, not an address. A raw pointer obtained via DEREF is valid only until the next allocation that triggers a slab resize. The programming rule follows directly: **handles cross function boundaries, raw pointers do not.** Functions receive and pass `ARENA_PTR` handles; DEREF is used at the leaf — field access, array indexing, memcpy source or destination — and discarded immediately. A raw pointer is never stored in a struct, never passed as an argument, and never cached across a call that may allocate.

---

## The Pointer Handle

Every heap reference is a packed `uint64_t` split evenly into two 32-bit fields:

```
 63                32 31                 0
  [    arena_id     |       offset       ]
```

- **`arena_id`** (upper 32 bits): index into the global arena registry. Identifies which arena owns this object. Supports up to ~4.3 billion distinct arenas.
- **`offset`** (lower 32 bits): byte offset from the start of that arena where the object's data begins. Supports up to 4GB per arena.

Dereferencing a handle:

```c
void* deref(uint64_t handle) {
    uint32_t arena_id = (uint32_t)(handle >> 32);
    uint32_t offset   = (uint32_t)(handle & 0xFFFFFFFF);
    return arena_registry[arena_id].base + offset;
}
```

The 32/32 split is symmetric and keeps handle arithmetic simple. The `arena_id` space (~4.3 billion) is large enough to be a practical ceiling for any realistic concurrency model; a process will exhaust physical RAM long before exhausting arena IDs. The 4GB offset ceiling is generous for any single scope's allocations.

All references held by user code are handles. Stack-allocated values may be passed by raw pointer within the same scope (e.g. a struct constructed in-place via a void function) — stack memory has a stable address and cannot move. Raw pointers into arena-backed heap memory are not safe: each arena's backing slab may be moved by `realloc()` as the arena grows, invalidating any raw pointer into it. The handle design is specifically what makes heap references stable across realloc — the `base` address in `deref()` is resolved at dereference time against the arena's current slab address, so a moved slab is transparent to any code holding a handle. Raw pointers into arena memory are therefore not a supported pattern. Raw pointers are never returned from functions — pointer-returning functions are arena-routed by the desugaring rule. There are no invalid handle states: a handle either refers to a live object in its arena or the arena has been reset and the handle is unreachable.

---

## The Arena Pool

The runtime maintains a dynamically extensible pool of arenas indexed by a **two-level availability bitmap**. The pool starts at 4,096 arenas (sufficient for the vast majority of programs) and adds capacity in 4,096-arena increments if all slots are simultaneously occupied.

```c
typedef struct {
    uint8_t* base;       // start of arena memory
    uint32_t bump;       // next free byte offset
    uint32_t capacity;   // total arena size
} Arena;

Arena    arena_pool[MAX_ARENAS];    // grows as needed
uint64_t bitmap_top;                // 1 word  — bit N set = bottom[N] has a free slot
uint64_t bitmap_bottom[64];         // 64 words — each bit = one arena slot (64 × 64 = 4,096)
```

### Two-Level Bitmap

The availability map is a two-level hierarchy. Each bit in `bitmap_top` summarises an entire word in `bitmap_bottom`:

- `bitmap_top` bit N = "`bitmap_bottom[N]` contains at least one free arena slot"
- `bitmap_bottom[N]` bit M = "arena `N×64 + M` is free"

A set bit means *free* throughout. Both levels are initialised to all-ones at startup.

**Acquiring an arena (scope entry):**
```c
uint32_t word = ctz(bitmap_top);                    // which bottom word has a free slot?
uint32_t bit  = ctz(bitmap_bottom[word]);           // which bit in that word?
uint32_t arena_id = word * 64 + bit;               // computed — no search

bitmap_bottom[word] &= ~(1ULL << bit);             // mark slot occupied
if (bitmap_bottom[word] == 0)
    bitmap_top &= ~(1ULL << word);                 // bottom word full — clear top bit

arena_pool[arena_id].bump = 0;                     // reset bump pointer
```

**Releasing an arena (scope exit):**
```c
free(arena_pool[arena_id].base);                   // release backing slab (PoC — production would reset bump and retain slab)
arena_pool[arena_id].base = NULL;

uint32_t word = arena_id / 64;
uint32_t bit  = arena_id % 64;
bitmap_bottom[word] |= (1ULL << bit);              // mark slot free
bitmap_top         |= (1ULL << word);              // bottom word has a free slot again
```

Acquisition is two `ctz` calls regardless of pool size — O(log₆₄ N), which is a fixed constant of at most a handful of levels for any realistic arena count. Practically indistinguishable from O(1).

Arena slot 0 is permanently reserved at registry initialisation (`bot_word[0] &= ~1ULL`). This guarantees that `0ULL` is never a valid handle, making it a safe `ARENA_NULL` sentinel without any extra runtime check.

**Pool expansion:** If `bitmap_top == 0` (all 4,096 slots occupied) and a new arena is needed, the pool appends another 4,096-slot tier. Given that each arena is a separate scope, 4,096 simultaneous scopes represents an extraordinary concurrency load; expansion is expected to be rare in practice. Physical RAM will be the binding constraint long before arena IDs are. *Pool expansion is not yet implemented in this alpha PoC — exhaustion currently exits the process.*

For concurrent execution, acquisition and release use atomic operations (`atomic_fetch_and` / `atomic_fetch_or`) on the bitmap words, giving each concurrent call frame an isolated arena with no shared mutable state between them.

### Arena backing memory

Each arena is backed by a memory slab acquired from the system allocator. Arenas start at a configurable default size (256 bytes in the PoC) and grow via `realloc()` as needed, using a 1.5× growth factor up to the 4GB maximum imposed by the 32-bit offset field. Allocations are 8-byte aligned.

The backing slab is freed at scope exit and reallocated fresh at scope entry. Every scope boundary pays one `malloc` and one `free` on the slab in addition to the bitmap operations. Freeing immediately rather than retaining idle slabs keeps memory footprint honest across concurrent workloads where a freed arena slot may sit unoccupied for an indeterminate period.

---

## Allocation

All allocation is bump allocation into a specific arena:

```c
uint64_t arena_alloc(uint32_t arena_id, uint32_t size) {
    Arena* a = &arena_pool[arena_id];
    uint32_t offset = (a->bump + 7) & ~7u;        // 8-byte align
    if (a->capacity - offset < size) arena_grow(a, size);
    a->bump = offset + size;
    return ((uint64_t)arena_id << 32) | offset;   // packed 32/32 handle
}
```

The bump pointer is aligned to 8 bytes before each allocation. If the slab has insufficient space, the arena grows via `realloc()` using a 1.5× capacity factor until the object fits, up to the 4GB ceiling.

The caller always knows the target `arena_id` — it is either the current scope's arena or, for objects meant to outlive the current scope, the arena of the relevant outer scope read from the container handle's `arena_id` field.

Handle dereference (`base + offset`) adds one integer addition over a raw pointer load. In practice this is dominated by arena contiguity: objects allocated within a scope are physically adjacent in memory, eliminating the cache misses that characterize fragmented heap allocators. When `deref()` is inlined — trivial for any optimizing C compiler — the base pointer is hoisted out of loops automatically, reducing the per-dereference cost to a single add.

---

## The Desugaring Rule

This is Ariandel's core compile-time contribution. The rule is mechanical and applies uniformly across the language:

> **Any function that returns a pointer is transformed into a pre-allocation in the caller's arena followed by a void constructor call writing into that slot.**

```c
// Source
MyStruct* s = MyConstructor(args);

// Desugared
uint64_t s = arena_alloc(current_arena_id, sizeof(MyStruct));
MyConstructor(s, args);   // void — writes into pre-allocated handle
```

This transformation is applied recursively to sub-objects: if `MyStruct` contains a pointer field, that field is also pre-allocated into the appropriate arena before `MyConstructor` runs.

**Consequence:** The allocation routing decision — which arena does this object belong to — is made entirely at compile time by reading the static scope structure and the `arena_id` fields of any container handles in scope. There is no runtime decision, no escape analysis, and no promotion step.

### Cross-scope allocation

When an object is inserted into a container that lives in an outer scope's arena, the target arena is read directly from the container's handle:

```c
// Appending a new object into a list owned by an outer scope
uint64_t target_arena = handle_arena_id(list_handle);   // read from handle
uint64_t new_obj = arena_alloc(target_arena, sizeof(Obj));
list_append(list_handle, new_obj);
```

The compiler emits `target_arena` as a constant or a simple handle field read — never a runtime search.

---

## Scope Exit Protocol

At the exit of scope N (owning `arena_id = N`):

1. Free the backing slab — bulk reclaims all objects in a single `free()` call
2. Set bit N in the availability bitmap — marks the slot free for reuse

That is the entire protocol. There is no per-object inspection, no fingerprint evaluation, no promotion step. Correctness follows from the desugaring rule: nothing in arena N at scope exit was supposed to survive — if it was, the compiler pre-allocated it into an outer arena.

Freeing the slab rather than retaining it is a deliberate design choice. A held but idle arena slab is dead memory for the lifetime of any async operation that outlives the scope — freeing it immediately keeps the memory footprint honest and avoids phantom allocations in concurrent workloads.

---

## Memory Safety Guarantees

| Bug class | How Ariandel eliminates it |
|---|---|
| Dangling stack pointer | Returning a stack pointer is not syntactically expressible — pointer-returning functions are arena-routed by definition |
| Use-after-free | Cross-scope handles always point into the outer arena by the desugaring rule — anything inserted into an outer container is allocated into that container's arena, not the inner scope's. Inner arena resets therefore never invalidate handles visible to outer scopes. |
| Double free | No manual free calls exist |
| Memory leak | Scope exit frees the entire arena slab unconditionally |
| Cycles | Irrelevant — owned references form a DAG by scope structure; no cycle detector needed |

---

## What This Model Does Not Require

- **No borrow checker.** The core borrow checker rule (no reference outlives its owner) is enforced by the syntax itself — returning a stack pointer is not a compiler error, it is simply not a thing the language can express. Stack allocation for local use is fully supported and carries no overhead.
- **No escape analysis.** Allocation routing is determined mechanically by the desugaring rule and static scope structure.
- **No per-object metadata.** Object headers carry no fingerprint, no generation counter, no reference count.
- **No GC pauses.** All reclamation is O(1) at scope exit.
- **No cycle detector.** Owned references cannot form cycles through scope structure.
- **No promotion at runtime.** Compile-time desugaring routes allocations correctly from the start.
- **No generation counter.** Generation counters exist to detect the ABA problem: a pool slot freed and reacquired by a new allocation, with a stale handle from the old occupant still live. Ariandel makes this condition unconstructable. By the time an arena slot is freed, the scope that owned it has exited — this is not a policy but a structural consequence of scoped execution. All cross-scope handles point into outer arenas by the desugaring rule, meaning they live in arenas that are still occupied. There is no execution path in which a live handle refers to a freed-and-reacquired slot. The generation counter problem is solved structurally, not by metadata.

---

## Comparison to Related Work

| Scheme | Pauses | Manual frees | Borrow checker | Overhead per object | Deterministic |
|---|---|---|---|---|---|
| Tracing GC | Yes | No | No | None | No |
| Reference counting | No | No | No | One integer | Partial |
| Rust ownership | No | No | Yes | None | Yes |
| Region inference (Tofte & Talpin) | No | No | No | None | Yes |
| **Ariandel** | **No** | **No** | **No** | **None¹** | **Yes** |

¹ Overhead per object is zero for regular scopes. Context scopes (resource management) write a small inline header at the base of their arena; this is a per-scope cost, not a per-object cost, and affects only scopes explicitly managing external resources.

Ariandel's primary distinction from Tofte & Talpin region inference is that allocation routing is solved by a single mechanical desugaring rule rather than by static region inference over the full program. T&T regions are monolithic — a region lives or dies as a unit with no cross-region allocation routing at the call-site level. Ariandel makes cross-scope routing trivial by embedding `arena_id` in every handle and reading it at the call site.

---

## Empirical Results

The following benchmarks were produced by the PoC implementation against a balanced BST of **1,000,000 nodes**, comparing idiomatic C (`malloc` / recursive `free`) against the Ariandel macro shim backed by the arena runtime. Both programs were compiled with GCC on Windows (MinGW64), no optimisation flags. `deref` is `static inline` and hoisted by the compiler.

| Phase | Idiomatic C | Ariandel | Speedup |
|---|---|---|---|
| Build | 506 ms | 36 ms | **14×** |
| Cleanup | 383 ms | 1 ms | **383×** |
| **Total** | **889 ms** | **37 ms** | **24×** |

**Cleanup** is the headline result and the direct confirmation of the O(1) scope exit claim. Freeing 1,000,000 individually `malloc`'d nodes via recursive traversal costs 383 ms. Resetting an arena's bump pointer costs 1 ms — a single integer write and a `free()` on one slab. The ratio scales linearly with N; the arena reset does not.

**Build** is the result the spec mentions but undersells. Arena contiguity — all nodes physically adjacent in the same slab — eliminates the cache misses and per-allocation overhead that `malloc` incurs at scale. The 14× build speedup is a free consequence of the allocation model, not a separately engineered optimisation.

### 12-Queens — collect from recursion (14,200 solutions)

The second benchmark exercises the cross-scope allocation pattern: deep backtracking recursion collects solutions into an outer scope's arena via `ALLOC_INTO`. Idiomatic C `malloc`s each solution individually and frees them by iterating the full list. Both programs use identical backtracking logic; the only difference is memory management.

| Phase | Idiomatic C | Ariandel | Speedup |
|---|---|---|---|
| Solve | 282 ms | 301 ms | **~1×** |
| Cleanup | 325 ms | 3 ms | **108×** |
| **Total** | **607 ms** | **304 ms** | **~2×** |

Solve time carries a small overhead (~7%) versus idiomatic C. This is an honest consequence of the handle discipline: raw pointer accesses are replaced by `DEREF` calls that compute `base + offset` at each access site. The backtracking loop dereferences the board handle on every placement and every safety check — the overhead is real and expected, not a model failure. Cleanup tells the same story as the tree benchmark: 14,200 individual `free()` calls cost 325 ms; one arena reset costs 3 ms. The O(1) cleanup claim holds regardless of how many objects were collected.

These numbers are unoptimized. No `-O2`, no arena pre-sizing. Production use with compiler optimization could vary results in either direction.

---

## Finalizers and External Resources

Bump pointer reset is a pure memory operation — it does not inspect or invoke any code on the objects it reclaims. This is correct for memory-only objects but insufficient for objects owning external resources (file descriptors, sockets, locks, GPU buffers, database connections). These require explicit cleanup before their arena is reset.

Ariandel handles this via a **context scope** — a scoped construct (the host language determines the syntax; a `with`-style block is the natural expression) that guarantees destructor invocation at scope exit before the arena is released. This is not a language feature of Ariandel itself but a specified behavior the host language should expose.

A context scope acquires its own arena from the pool exactly like any other scope. On exit, the registered destructor(s) are called, then the bump pointer is reset. External resources are always released before memory is reclaimed.

### Finalizer metadata stored in the context arena itself

Because a context arena is short-lived and purpose-built, it stores its own cleanup metadata inline at the base of the arena — before any object data:

```c
typedef struct {
    void     (*destructor)(uint64_t handle);  // destructor function pointer
    uint64_t   resource_handle;               // handle to the managed object
    uint32_t   object_count;                  // number of objects requiring cleanup
} ContextArenaHeader;
```

On context scope exit, the runtime reads this header from the arena base, invokes the destructor(s), then resets the bump pointer. The metadata lives and dies entirely within the arena — no external registry, no auxiliary allocation, no pointer chasing. Regular function arenas write no such header and pay no overhead.

The destructor's job is solely to release external resources. It does not free arena memory — that is handled by the bump reset.

The distinction between a context scope and a regular scope is purely compile-time: the compiler knows statically which scopes are context scopes and emits different exit code accordingly — a destructor call followed by a bump reset, versus a bare bump reset. No runtime flag or header inspection is needed to make this determination.

---

## Concurrency

Each concurrent execution unit (coroutine, thread, async task) acquires its own arena slot on entry via the two-level bitmap. Because each unit owns a fully isolated arena, concurrent units require no synchronization for allocation or deallocation.

### Implicit parent arena argument

When a scope spawns an async or concurrent child, the parent's `arena_id` is passed to the child as an implicit runtime argument. This single mechanism enforces two properties simultaneously:

1. **Allocation routing.** The child already holds the parent arena ID and can pre-route allocations that need to outlive its own scope directly into the parent arena via the desugaring rule — no runtime search, no separate mechanism.
2. **Lifetime dependency.** The parent, before resetting its arena, checks the availability bitmap against the set of child arena IDs it spawned. If any child arena is still marked occupied, the parent blocks. The child holding a reference to the parent arena ID makes the dependency relationship concrete and auditable in the runtime.

This is the structured concurrency model: child scopes always exit before their parent, enforced mechanically by a bitmask check against IDs the parent already tracks. No separate lifetime annotation or dependency graph is required.

### Memory ordering

Ariandel defines ordering guarantees across four access patterns, covering the vast majority of concurrent code without requiring explicit synchronization from the programmer:

| Access pattern | Synchronization mechanism |
|---|---|
| Scope writing to its own arena | None — private by construction |
| Parent reading child-written data after join | Acquire/release on bitmap flip — automatic |
| Single child with exclusive handle into parent arena | Ordered by join point — automatic |
| Multiple children reading a shared handle | None — reads do not race |
| Multiple children writing a shared handle | Requires atomic type — host language responsibility |

**The join point as a synchronization barrier.** When a child scope exits, its bitmap bit flips from occupied to free via a C11 `memory_order_release` store. The parent's bitmap check is a `memory_order_acquire` load. This acquire-release pair establishes a happens-before edge by the C11 memory model: everything the child wrote before releasing its arena is guaranteed visible to the parent after the join. No additional annotation is needed — the ordering falls out of the bitmap operations already required by the model.

**Exclusive vs. shared handles.** A handle passed to exactly one concurrent child is exclusive — the child has write access, the parent does not touch it until the join, and ordering is guaranteed automatically. A handle passed to multiple concurrent children simultaneously is shared — reads require no synchronization, but concurrent writes require the object to be declared as an atomic type. The compiler can enforce this statically, since it knows the spawn structure and can see at compile time whether a handle is passed to one child or many.

Ariandel specifies the first four rows of the table. The fifth — concurrent mutation of explicitly shared handles — is delegated to the host language's atomic type system (C11 `_Atomic`, or equivalent). Because rows 1–4 cover the common case, the atomic requirement surfaces only for genuinely shared mutable state, not pervasively.

---

## Open Questions

- **Handle width for large applications.** The standard handle is `uint64_t` (32/32 split), covering 4GB per arena and ~4.3 billion arena IDs — sufficient for over 99% of real-world programs. Applications requiring more space may widen to `uint128_t` with a 64/64 split. The layout, arithmetic, and bitmap logic are identical; only the word width changes.
- **Bitmap expansion atomicity.** Regular bitmap acquisition and release are lock-free via C11 `_Atomic` on individual words. Pool expansion (appending a new 4,096-slot tier) is a multi-step transaction across multiple words and cannot be made atomic with `_Atomic` alone. A mutex is the correct mechanism — expansion is expected to occur at most once or twice in the lifetime of a massive concurrent application, making the lock penalty irrelevant. Physical RAM exhaustion is a far more binding constraint than the cost of a single mutex acquisition.
- **Handle validity after arena reuse.** When a slot is freed and reacquired by a new scope, the backing memory is not zeroed — only the bump pointer resets. A stale handle with the old `arena_id` persisting into the new scope would read uninitialised or foreign data. The model's argument against this is sound: the scope that held the stale handle has exited by the time the arena is released, so no live reference to that handle can exist. This is a design axiom — "a handle cannot outlive the scope that created it" — that the desugaring rule and structured concurrency together enforce. The PoC should stress-test this under concurrent arena reuse to confirm no edge case violates it in practice.
