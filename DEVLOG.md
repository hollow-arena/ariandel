# Ariandel — Development Log

## Origin

Ariandel grew out of a separate project: an AOT compiler translating Python to C (working title Firesnek / Estus). While building that compiler's memory model, two problems became unavoidable:

1. **Dangling pointers on return.** Functions allocating into their own arena and returning a pointer hand the caller a reference into memory that gets freed when the callee's scope closes.
2. **No mid-arena deallocation.** Heap objects allocated in a scope's arena cannot be individually freed — the arena is all-or-nothing until scope exit.

These are the canonical failure modes of naive arena allocation. The attempt to solve them cleanly is what eventually produced Ariandel.

---

## Phase 1 — Understanding the Landscape

Before designing anything, the problem space was mapped by studying how existing GC strategies work:

- **Reference counting** — deterministic, low latency, fatal flaw: cycles. Swift and Python use RC but bolt on a separate cycle detector to patch it.
- **Mark-and-sweep** — handles cycles, but requires stop-the-world pauses and a full heap scan.
- **Copying / semi-space** — fast allocation and collection, but 2× memory overhead.
- **Generational GC** — exploits the empirical observation that most objects die young. Go and Java use this. Still non-deterministic pause timing.
- **Concurrent / incremental GC** — reduces pauses but requires write barriers on every pointer write and complex tri-color marking.

The conclusion: every mainstream GC strategy trades off either pause predictability, memory overhead, implementation complexity, or programmer burden. The goal for Ariandel was to sidestep the tradeoff entirely by making lifetime structural rather than tracked.

---

## Phase 2 — The Allocating Mutator Problem

The first concrete wall: how does this function work?

```python
def process(x: list[obj]) -> None:
    x.pop()
    x.append(obj())
```

This looks like a pure consumer but is actually an **allocating mutator** — it produces new heap memory with a lifetime tied to the caller's data structure, not the callee's scope. Several approaches were considered and rejected:

- **Caller-owned arena threading** — pass the caller's arena implicitly into the callee. Workable but adds implicit parameters everywhere and requires the compiler to thread arena IDs through all call frames.
- **Return-by-value with copying** — only works for small, immutable return values. Falls apart for mutable containers.
- **Contiguous return-slot region** — pre-allocate all potential return values contiguously at the front of the callee's arena, promote the slab on return in O(1), reset the rest. Elegant for return values but doesn't solve the mutation-through-reference problem.

The mutation-through-reference problem kept resurfacing. Any object appended into `x` needs to live in `x`'s arena — but the callee has no direct knowledge of which arena that is without either carrying the arena ID in the container handle or threading it explicitly through every call.

**Resolution:** containers carry their owning `arena_id` as a field. Allocating mutations read the target arena from the container handle directly. This was the seed of the handle-carries-arena-id design that became central to Ariandel.

---

## Phase 3 — The Stack Variable Registry and Fingerprints

A different angle was explored: a global registry of all live stack variables that hold pointers to heap data. Each heap object would carry a **fingerprint** — a bitmap encoding which stack variables currently reference it. At scope exit, dying stack variables would be popped from the registry, their bits cleared from all fingerprints, and any object reaching a zero fingerprint would be freed.

This was closer to reference counting with scope-structured decrements than to mark-and-sweep — which is actually better, because decrements only happen at statically-known scope boundaries rather than at every pointer assignment.

### Key derivation steps

**Fingerprint inheritance.** When a new object is inserted into a container, it inherits the container's fingerprint at insertion time. Ownership propagates transitively up to the root stack owner without any explicit traversal. When the container's owner dies, all contained objects lose their bits in the same pass.

**Aliasing is a non-issue.** Multiple stack variables referencing the same object simply OR their bits into the fingerprint. Both bits must clear before the object's fingerprint hits zero. Correct behavior, still O(1).

**XOR was explored and rejected.** Using XOR of stack variable addresses as a compact fingerprint seemed appealing, but `A XOR A = 0` means any set of addresses appearing an even number of times produces a false zero — incorrectly triggering a free on a live object. Hashing before XOR was also tried and also rejected: a good hash function has no structural reason to avoid cancellation. The positional bitmap is the only correct approach.

**The scope-depth collapse.** The critical insight: the fingerprint doesn't need to track *which specific* stack variable owns an object — only *whether any variable at a given scope depth level* does. That's a binary question per scope level. The fingerprint collapses from a per-variable bitmap to a **scope-depth bitmask**:

- Bit N = "at least one stack variable in scope N references this object"
- Scope exit = AND the object's fingerprint with a mask clearing bit N
- Fingerprint reaches zero → free

This eliminates the XOR problem entirely, reduces the fingerprint to a single integer, and makes scope exit O(objects in the dying scope) rather than O(heap).

At this point the model was named **Ariandel**, after the Painted World of Ariandel from Dark Souls III — a world that burns clean at the end of each cycle to make way for the new.

---

## Phase 4 — The Desugaring Rule

The fingerprint model was elegant but still required per-object metadata and a promotion pass for objects that survived scope exit. A further simplification emerged from thinking about the allocating mutator problem differently.

**The observation:** if the compiler knows where every allocation needs to live *before the program runs*, there is no runtime tracking problem at all. Objects meant to outlive a scope just go into the outer scope's arena from the start. Nothing needs to be promoted at runtime because it was never in the wrong place to begin with.

**The desugaring rule:** any function that returns a pointer is transformed at compile time into a pre-allocation in the caller's arena followed by a void constructor call writing into that slot.

```c
// Source
MyStruct* s = MyConstructor(args);

// Desugared
uint64_t s = arena_alloc(current_arena_id, sizeof(MyStruct));
MyConstructor(s, args);   // void — writes into pre-allocated handle
```

Because the language is statically typed, `sizeof` is always known at the call site. The transformation is mechanical with zero runtime decision-making.

**Consequence:** everything remaining in a scope's arena at scope exit is dead by construction. The compiler guaranteed that anything meant to outlive the scope was allocated into an outer arena from the start. Scope exit becomes a single bump-pointer reset — O(1), no per-object inspection, no promotion step.

The fingerprint bitmask — the result of three phases of derivation — was no longer needed for per-object lifetime tracking. The model had simplified dramatically.

---

## Phase 5 — The Handle and Arena Pool

With allocation routing solved at compile time, the runtime model reduced to:

**The pointer handle** — a packed `uint64_t` with a 32/32 split:
- Upper 32 bits: `arena_id` — index into the global arena pool
- Lower 32 bits: `offset` — byte offset from the arena's base

The 32/32 split is symmetric, keeps handle arithmetic simple, supports ~4.3 billion simultaneous arenas (a practical ceiling; RAM is the real constraint), and 4GB per arena (generous for any single scope). For applications exceeding these bounds, widening to `uint128_t` with a 64/64 split requires no architectural changes.

**The bitmask repurposed.** The scope-depth fingerprint bitmask was no longer needed for per-object lifetime tracking — but the same data structure turned out to be the ideal availability map for the arena pool itself.

A **two-level bitmap** indexes the pool:
- `bitmap_top` (1 word): bit N set = `bitmap_bottom[N]` has a free slot
- `bitmap_bottom` (64 words): each bit = one arena slot (64 × 64 = 4,096 total)

Arena acquisition: two `ctz` calls, always. O(log₆₄ N) — a fixed constant of at most a handful of operations for any realistic pool size. Practically O(1).

Regular bitmap operations (acquire/release) are lock-free via C11 `_Atomic`. Pool expansion (appending a new 4,096-slot tier when fully occupied) is a multi-step transaction that requires a mutex — but expansion is expected to occur at most once or twice in the lifetime of a massive concurrent application. Physical RAM exhaustion is a far more binding constraint.

---

## Phase 6 — Safety, Concurrency, and Finalizers

### Memory safety

The model eliminates entire bug classes by construction:

- **Dangling stack pointers** — returning a raw pointer from a function is not syntactically expressible. Pointer-returning functions are arena-routed by the desugaring rule. Stack allocation for local use (construct-in-place via void function, use within scope) remains fully supported.
- **Use-after-free** — cross-scope handles always point into the outer arena by the desugaring rule. Inner arena resets never invalidate handles visible to outer scopes.
- **Double free / memory leak** — no manual free calls; scope exit resets the arena unconditionally.
- **Cycles** — owned references cannot form cycles through scope structure; no cycle detector needed.

### Concurrency

Each concurrent execution unit acquires its own arena slot atomically. Isolated arenas mean no synchronization is needed for allocation or deallocation.

**The implicit parent arena argument.** When a scope spawns a concurrent child, the parent's `arena_id` is passed as an implicit runtime argument. This serves two purposes simultaneously: the child can pre-route allocations meant to outlive its scope directly into the parent arena (desugaring rule applies), and the parent can check the bitmap against its child arena IDs before resetting — blocking if any child is still live. Structured concurrency falls out of the bitmap for free.

**Memory ordering.** Four access patterns are covered by the model without explicit programmer annotation:

| Pattern | Mechanism |
|---|---|
| Scope writing to its own arena | Private by construction — no ordering needed |
| Parent reading child-written data after join | Acquire/release on bitmap flip — automatic |
| Single child with exclusive handle | Ordered by join point — automatic |
| Multiple children reading a shared handle | Reads don't race — no ordering needed |

Concurrent writes to a shared handle require an atomic type — delegated to the host language. The join point (child arena bit clearing) is a C11 acquire/release pair, establishing the happens-before edge automatically.

### Finalizers and context scopes

Bump-pointer reset does not invoke any code on the objects it reclaims. Objects owning external resources (file descriptors, sockets, locks, GPU buffers) require explicit cleanup before their arena is reset.

Ariandel specifies a **context scope** — a scoped construct whose syntax is determined by the host language. A context scope acquires its own arena, stores cleanup metadata inline at the base of the arena (destructor pointer, resource handle, object count), calls the destructor at scope exit, then resets the bump pointer. Regular scopes write no such header and pay no overhead. The distinction is compile-time: the compiler emits different exit code for context scopes, no runtime flag needed.

---

## Where Things Stand

The spec (SPEC.md) is complete enough to serve as the basis for both a PoC implementation and a whitepaper. The model is coherent, the major holes are closed, and the remaining open questions are implementation parameters rather than design gaps:

- Handle width policy for large applications (`uint128_t` upgrade path)
- Arena backing memory growth policy (configurable default size, `realloc()` up to 4GB max)
- Handle validity under concurrent arena reuse — the argument is sound but the PoC should stress-test it

**The novel contribution** relative to prior art (primarily Tofte & Talpin 1997 region inference): allocation routing is solved by a single mechanical compile-time desugaring rule rather than by whole-program static region inference. The arena ID embedded in every handle makes cross-scope routing a constant-time field read rather than an analysis problem. T&T regions are all-or-nothing; Ariandel routes individual allocations across scope boundaries without any inference pass.

**Target:** arXiv preprint positioning Ariandel in the space between region-based memory management and ownership type systems, with a working PoC and benchmark comparison against a naive bump allocator baseline.
