# Ariandel Memory Model — Specification v0.1

## Overview

Ariandel is a deterministic, scope-structured memory management model targeting compiled, statically-typed languages. It provides automatic heap memory reclamation without stop-the-world GC pauses, without manual memory management, without a borrow checker, and without cycle detection. Every heap object lives in a scope-owned arena; scope exit resets the arena in O(1) regardless of how many objects were allocated. Allocation routing — which arena an object belongs to — is determined statically by scope structure and the `arena_id` embedded in every handle. The runtime overhead is a bump-pointer allocation and an atomic bitmask operation per scope boundary, equivalent in performance to a handwritten C bump allocator.

---

## Core Invariants

1. **Stack allocation is permitted within a scope; raw pointers into arena memory must not cross scope boundaries.** A user may stack-allocate a struct, pass a raw pointer to it into a void function for construction or mutation, and use it freely within the same scope. Stack memory has a stable address and cannot move, so raw pointers to stack variables are safe within their scope. Raw pointers into arena-backed heap memory are not safe across scope boundaries — arena backing slabs may be moved by `realloc()` as allocation grows, invalidating any raw pointer into them. Heap objects must be referenced via `ARENA_PTR` handles, which encode `arena_id + offset` and resolve correctly at dereference time regardless of slab movement. Functions that allocate heap objects should return `ARENA_PTR` handles — integers, not memory addresses — which are safe to pass across scope boundaries. A raw pointer obtained via `DEREF` is valid only within the current scope and must not be stored, returned, or passed to any call that may allocate. This is a programming discipline enforced by convention in the macro PoC; a language targeting Ariandel would enforce it via its type system.
2. **All heap allocation is arena-backed.** Every heap object lives in exactly one arena. Scope cleanup is a bump-pointer reset — O(1) regardless of how many objects are freed.
3. **Allocation routing is determined by scope structure, not by runtime object analysis.** Every `ALLOC` call routes to the active scope's arena — whichever arena was entered most recently via `SCOPE_NEW` or `SCOPE(ptr)`. No escape analysis, no runtime promotion, no per-object lifetime tracking. The programmer controls routing explicitly by controlling which scope is active at the point of allocation. A compiler targeting Ariandel would resolve arena assignment statically from scope structure; the macro PoC achieves the same semantics via the thread-local active arena pointer.
4. **Everything remaining in a scope's arena at scope exit is intended to be dead.** Any object meant to outlive a scope must be allocated into an outer arena from the start — via `SCOPE(ptr)` in the macro PoC, or by statically routing into the correct arena in a compiler implementation. Scope cleanup requires no per-object inspection regardless: the bump pointer resets unconditionally. Correctness of that reset depends on the programmer having routed long-lived objects out of the scope before it exits.
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

Dereferencing a handle (off-implementation for readability):

```c
void* deref(uint64_t handle) {
    uint32_t arena_id = (uint32_t)(handle >> 32);
    uint32_t offset   = (uint32_t)(handle & 0xFFFFFFFF);
    return arena_registry[arena_id].base + offset;
}
```

The 32/32 split is symmetric and keeps handle arithmetic simple. The `arena_id` space (~4.3 billion) is large enough to be a practical ceiling for any realistic concurrency model; a process will exhaust physical RAM long before exhausting arena IDs. The 4GB offset ceiling is generous for any single scope's allocations.

All references held by user code are handles. Stack-allocated values may be passed by raw pointer within the same scope (e.g. a struct constructed in-place via a void function) — stack memory has a stable address and cannot move. Raw pointers into arena-backed heap memory are not safe: each arena's backing slab may be moved by `realloc()` as the arena grows, invalidating any raw pointer into it. The handle design is specifically what makes heap references stable across realloc — the `base` address in `deref()` is resolved at dereference time against the arena's current slab address, so a moved slab is transparent to any code holding a handle. Raw pointers into arena memory are therefore not a supported pattern. Functions that allocate heap objects return `ARENA_PTR` handles — integers, not addresses — so a dangling arena pointer cannot be produced at a return boundary. There are no invalid handle states: a handle either refers to a live object in its arena or the arena has been reset and the handle is unreachable.

---

## The Arena Pool

The runtime maintains a dynamically extensible pool of arenas indexed by a **two-level availability bitmap**. The pool starts at 4,096 arenas (sufficient for the vast majority of programs). Future iterations will add capacity in 4,096-arena increments if all slots are simultaneously occupied.

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

Handle dereference (`base + offset`) adds one integer addition over a raw pointer load. In practice this is dominated by arena contiguity: objects allocated within a scope are physically adjacent in memory, eliminating the cache misses that characterize fragmented heap allocators. A native compiler implementing Ariandel would inline `deref()` and hoist the `base` pointer out of hot loops automatically, reducing the per-dereference cost to a single add. The C macro shim cannot do this — `DEREF` resolves the base pointer on every call, and the overhead is measurable in compute-intensive loops (see Empirical Results).

---

## Cross-Scope Allocation

When an object needs to outlive the current scope, the programmer enters the target arena before allocating. The target arena is identified by any existing handle into it:

```c
// Enter the arena that owns list_handle; allocate new_obj into it
SCOPE(list_handle) {
    ARENA_PTR new_obj = ALLOC(sizeof(Obj));   // routed into list_handle's arena
    list_append(list_handle, new_obj);
}
```

`SCOPE(ptr)` resolves the target arena from the `arena_id` field of the handle — a single integer read, never a runtime search — and sets it as the active arena for the duration of the block without taking ownership. Anything allocated inside routes into that arena via the standard `ALLOC` path. On exit, the active arena is restored; the target arena is not freed.

---

## Scope Exit Protocol

At the exit of scope N (owning `arena_id = N`):

1. Free the backing slab — bulk reclaims all objects in a single `free()` call
2. Set bit N in the availability bitmap — marks the slot free for reuse

That is the entire protocol. There is no per-object inspection, no fingerprint evaluation, no promotion step. Correctness depends on the programmer having routed any long-lived objects into an outer arena before the scope exits — nothing remaining at scope exit was supposed to survive.

Freeing the slab rather than retaining it is a deliberate design choice. A held but idle arena slab is dead memory for the lifetime of any async operation that outlives the scope — freeing it immediately keeps the memory footprint honest and avoids phantom allocations in concurrent workloads.

---

## Memory Safety

| Bug class | How Ariandel eliminates it |
|---|---|
| Dangling stack pointer | Allocating functions return `ARENA_PTR` handles (integers), not raw addresses — a dangling arena pointer cannot be produced at a return boundary |
| Use-after-free | Any object inserted into an outer container must be allocated into that container's arena via `SCOPE(ptr)` — inner arena resets therefore never invalidate handles visible to outer scopes, provided routing discipline is followed. |
| Double free | No manual free calls exist |
| Memory leak | Scope exit frees the entire arena slab unconditionally |
| Cycles | Irrelevant — owned references form a DAG by scope structure; no cycle detector needed |

---

## What This Model Does Not Require

- **No borrow checker.** Lifetime correctness is expressed through scope structure and explicit arena routing rather than compiler-enforced ownership annotations. Stack allocation for local use is fully supported and carries no overhead.
- **No escape analysis.** Allocation routing is determined by which scope is active at the point of allocation — the programmer controls this explicitly via `SCOPE_NEW` and `SCOPE(ptr)`.
- **No per-object metadata.** Object headers carry no fingerprint, no generation counter, no reference count.
- **No GC pauses.** All reclamation is O(1) at scope exit.
- **No cycle detector.** Owned references cannot form cycles through scope structure.
- **No promotion at runtime.** Objects are allocated into the correct arena at the point of allocation; nothing is moved or promoted after the fact.
- **No generation counter.** Generation counters exist to detect the ABA problem: a pool slot freed and reacquired by a new allocation, with a stale handle from the old occupant still live. Ariandel makes this condition unconstructable. By the time an arena slot is freed, the scope that owned it has exited — this is not a policy but a structural consequence of scoped execution. Any handle that outlives its scope was allocated into an outer arena and therefore lives in an arena that is still occupied. There is no execution path in which a live handle refers to a freed-and-reacquired slot.

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

Ariandel's primary distinction from Tofte & Talpin region inference is that cross-scope allocation routing is delegated to the programmer at the call site rather than inferred by whole-program static analysis. T&T regions are monolithic — a region lives or dies as a unit with no cross-region allocation routing at the call-site level. Ariandel expresses cross-scope routing explicitly via `SCOPE(ptr)`, with the target arena identified by the `arena_id` embedded in any existing handle into it.

---

## Empirical Results

The following benchmarks were produced by the PoC implementation compiled with GCC (MinGW64) on Windows, no optimisation flags. `DEREF` is `static inline`. Numbers are representative of repeated runs; do not use results from debugger-attached runs (GDB intercepts heap operations and inflates cleanup times dramatically).

### Tree builder — 1,000,000 nodes

Balanced BST built from a sorted array of 1M integers. Idiomatic C allocates one node per `malloc` and frees via recursive traversal (`free_tree`). Ariandel allocates all nodes into a single `SCOPE_NEW` arena and exits the scope at the end of `main`.

| Phase | Idiomatic C | Ariandel | Ratio |
|---|---|---|---|
| Build | 53 ms | 54 ms | **~1×** |
| Cleanup | 31 ms | 1 ms | **~30×** |
| **Total** | **84 ms** | **55 ms** | **~1.5×** |

**Cleanup** is the direct confirmation of the O(1) scope exit claim. Freeing 1M individually `malloc`'d nodes via recursive traversal costs 31 ms. Resetting an arena's bump pointer costs ~1 ms — a single integer write and a `free()` on one slab. The ratio scales linearly with N; the arena reset does not.

**Build** is within measurement noise between the two implementations. Both complete in approximately 53 ms for 1M nodes against a fresh system allocator. The arena contiguity advantage — all nodes physically adjacent in the same slab — is present but does not dominate here. It widens under heap fragmentation and in long-running processes where the system allocator's per-call overhead accumulates.

### 13-Queens — collect from recursion (73,712 solutions)

Deep backtracking recursion collects all solutions into a dynamically-resized array. Both implementations use identical backtracking logic and an array-based collector with capacity doubling. The Ariandel version stores the board and solution handles as `ARENA_PTR` values inside a single `SCOPE_NEW`; the baseline uses raw pointers and `malloc`/`realloc`.

| Phase | Idiomatic C | Ariandel | Ratio |
|---|---|---|---|
| Solve | 1,600 ms | 2,108 ms | **0.76×** |
| Cleanup | 3 ms | 1 ms | **~3×** |
| **Total** | **1,603 ms** | **2,109 ms** | **0.76×** |

**Solve** is slower with Ariandel. The `is_safe` check runs in a tight recursive loop and accesses the board via `DEREF(board, int)[r]` on every iteration — one `base + offset` add per element versus a direct pointer load in the baseline. At N=13 depth with 73,712 solutions this overhead is clearly measurable: approximately 30% slower. The `DEREF` cost is not free in compute-intensive inner loops.

**Cleanup** at this solution count is fast in both implementations — the baseline's per-solution `free` loop completes in ~3 ms, so the O(1) arena reset offers only a marginal absolute saving here. The cleanup asymptote holds in principle; it dominates where per-object reclamation is expensive relative to solve time, as in the tree benchmark.

The honest takeaway: Ariandel's model is strongest where scope-boundary reclamation is the dominant cost — bulk object cleanup, tree-shaped workloads, large arenas. In tight numeric loops where every element access goes through a handle dereference, the indirection overhead is real. A production compiler targeting Ariandel would hoist the `base` pointer out of hot loops as a standard optimisation; the macro shim cannot do this automatically.

These numbers are unoptimized. No `-O2`, no arena pre-sizing. Production use with compiler optimisation would reduce the `DEREF` overhead significantly.

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

1. **Allocation routing.** The child already holds the parent arena ID and can route allocations that need to outlive its own scope directly into the parent arena via `SCOPE(ptr)` — no runtime search, no separate mechanism.
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
- **Handle validity after arena reuse.** When a slot is freed and reacquired by a new scope, the backing memory is not zeroed — only the bump pointer resets. A stale handle with the old `arena_id` persisting into the new scope would read uninitialised or foreign data. The model's argument against this is sound: the scope that held the stale handle has exited by the time the arena is released, so no live reference to that handle can exist. This is a design axiom — "a handle cannot outlive the scope that created it" — that arena routing discipline and structured concurrency together enforce. The PoC should stress-test this under concurrent arena reuse to confirm no edge case violates it in practice.
