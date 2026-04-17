# Ariandel

Scope-structured arena memory model for compiled languages. Automatic O(1) heap reclamation at scope exit. No GC, no borrow checker, no manual frees. See [SPEC.md](SPEC.md) for the full model specification.

---

## Dev Announcement

The Ariandel arena-scope memory model is being integrated and leveraged with the [Darksign](https://github.com/hollow-arena/darksign) project, this repo will stay active for archival purposes, but may not reflect the latest changes available. Please check out Darksign for the latest additions and features.

---

## Requirements

- GCC or Clang (C11)
- `__attribute__((cleanup))` and `__builtin_ctz` - MSVC users should use Clang-cl ("Platform Toolset → LLVM (clang-cl)" in Visual Studio)

---

## Building

Compile `ariandel_rt.c` alongside your source and include `ariandel.h`:

```sh
gcc your_program.c runtime/ariandel_rt.c -I runtime -std=c11 -o your_program
```

---

## Configuration

`REGISTRY_SIZE` controls the arena pool capacity. Default is `uint16_t` (4,096 arenas). Define it before including `ariandel.h`:

```c
#define REGISTRY_SIZE uint16_t   // 4,096 arenas by default
```

See [SPEC.md](SPEC.md) for the full configuration table and handle-width tradeoffs.

---

## API

### Lifecycle

```c
ARIANDEL_INIT()
    // ... program body
ARIANDEL_DESTROY()
```

`ARIANDEL_INIT()` initialises the registry and opens a root scope. `ARIANDEL_DESTROY()` closes it and frees the registry. These are matched macros, everything between them has access to the arena pool.

---

### Scopes

```c
SCOPE_NEW {
    // fresh arena, freed automatically on any exit
}

SCOPE(ptr) {
    // enters the arena that owns handle, does not free on exit
}
```

`SCOPE_NEW` acquires a new arena and sets it as the active arena. The arena is freed and the previous active arena is restored automatically on any exit from the block, including `return` and `break`.

`SCOPE(ptr)` enters the arena of an existing handle without taking ownership. Use this to allocate into an outer scope's arena from within an inner scope.

Any function called within a scope inherits the active arena automatically. `ALLOC` always routes to the current thread-local arena without requiring an explicit arena parameter. Functions that allocate simply call `ALLOC`; the caller controls which arena they land in by controlling which scope is active.

---

### Allocation

```c
ARENA_PTR handle = ALLOC(sizeof(MyStruct));
```

Allocates the specified number of bytes in the current active arena. Returns an `ARENA_PTR` — a `uint64_t` packed handle whose upper bits encode the `arena_id` and lower bits encode the byte offset within that arena. The exact split is determined by `REGISTRY_SIZE` (default `uint16_t`: 12-bit arena_id / 52-bit offset). Handles are stable across reallocations; raw pointers from `DEREF` are not since arenas can `realloc` to resize their memory block.

---

### Dereference

```c
DEREF(handle, Type)        // resolves handle to Type*
COPY(dst, src, size)       // memcpy between two handles
```

`DEREF` resolves a handle to a typed pointer. The result is valid only until the next allocation that triggers a slab resize. Do not cache it across allocations or pass it across scope boundaries.

---

## Example

```c
#include "ariandel.h"

typedef struct {
    int value;
    ARENA_PTR left;
    ARENA_PTR right;
} Node;

ARENA_PTR make_node(int val, ARENA_PTR left, ARENA_PTR right) {
    ARENA_PTR n = ALLOC(sizeof(Node));
    DEREF(n, Node)->value = val;
    DEREF(n, Node)->left  = left;
    DEREF(n, Node)->right = right;
    return n;
}

int main() {
    ARIANDEL_INIT()

    SCOPE_NEW {
        ARENA_PTR root = make_node(1,
            make_node(2, ARENA_NULL, ARENA_NULL),
            make_node(3, ARENA_NULL, ARENA_NULL));
        // use root...
    } // entire tree freed here in O(1)

    ARIANDEL_DESTROY()
    return 0;
}
```

Cross-scope allocation — keeping an object alive past the current scope:

```c
SCOPE_NEW {
    ARENA_PTR list = ALLOC(sizeof(List));
    // ...
    SCOPE_NEW {
        SCOPE(list) {
            ARENA_PTR item = ALLOC(sizeof(Item)); // allocated into list's arena
            list_append(list, item);
        }
    } // inner scope freed, item survives in list's arena
}
```

Loop-scoped allocation:

```c
SCOPE_NEW {
    for (int i = 0; i < 1000; i++) {
        ARENA_PTR ptr = ALLOC(sizeof(BigObject));
        // do stuff with ptr
    }
} // All BigObject heap pointers freed here

// Similarly

while (1) {
    SCOPE_NEW {
        ARENA_PTR ptr = ALLOC(sizeof(BigObject));
        // do stuff with ptr
    } // Frees each BigObject per loop iteration - this will never run out of memory
}
```

Functions inherit the caller's scope:

```c
ARENA_PTR alloc_int(int x) {
    ARENA_PTR ptr = ALLOC(sizeof(int));
    *(DEREF(ptr, int)) = x;
    return ptr;
}

int main() {
    ARIANDEL_INIT()

    SCOPE_NEW {
        ARENA_PTR y = alloc_int(42); // 42 lives on heap, no dangling pointer
    } // 42 freed when the scope exits

    ARIANDEL_DESTROY()
    return 0;
}
```

This is safe because `alloc_int` inherits the active arena from its caller — `ALLOC` always routes to the current scope's arena. The returned `ARENA_PTR` is a packed integer encoding `arena_id + offset`, not a raw pointer, so it resolves correctly at dereference time regardless of how deep the call stack went. No dangling pointer is possible at a return boundary.

---

## Benchmarks

All benchmarks compiled with GCC (MinGW64), no optimisation flags.

| Benchmark | Phase | Idiomatic C | Ariandel | Ratio |
|---|---|---|---|---|
| Tree — 1M nodes | Build | 53 ms | 54 ms | ~1× |
| Tree — 1M nodes | Cleanup | 31 ms | 1 ms | ~30× |
| 13-Queens — 73,712 solutions | Solve | 1,600 ms | 2,108 ms | 0.76× |
| 13-Queens — 73,712 solutions | Cleanup | 3 ms | 1 ms | ~3× |

Cleanup is the big win here, O(1) single free versus O(n) traversal. There is notable overhead in the N-Queens example: `DEREF` in tight inner loops adds measurable overhead that a native compiler could optimize better. See [SPEC.md](SPEC.md) for full benchmark discussion.

**Concurrency stress test (Black Swan):** 64 threads × 1,000,000 scope transitions each (~65M total), with randomly-sized allocations and nested scopes, run without any explicit synchronization in user code. The test is a destructive probe for the atomic three-level bitmap: no double-acquired slots, no lost releases, no data races across 65M round-trips. All ordering is provided by the `_Atomic` bitmap operations in the runtime.
