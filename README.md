# Ariandel

Scope-structured arena memory model for compiled languages. Automatic O(1) heap reclamation at scope exit — no GC, no borrow checker, no manual frees. See [SPEC.md](SPEC.md) for the full model specification.

---

## Requirements

- GCC or Clang (C11)
- `__attribute__((cleanup))` and `__builtin_ctz` — not compatible with MSVC

---

## Building

Compile `ariandel_rt.c` alongside your source and include `ariandel.h`:

```sh
gcc your_program.c runtime/ariandel_rt.c -I runtime -std=c11 -o your_program
```

---

## API

### Lifecycle

```c
ARIANDEL_INIT()
    // ... program body
ARIANDEL_DESTROY()
```

`ARIANDEL_INIT()` initialises the registry and opens a root scope. `ARIANDEL_DESTROY()` closes it and frees the registry. These are matched macros — everything between them has access to the arena pool.

---

### Scopes

```c
SCOPE_NEW {
    // fresh arena, freed automatically on any exit
}

SCOPE(handle) {
    // enters the arena that owns handle — does not free on exit
}
```

`SCOPE_NEW` acquires a new arena and sets it as the active arena. The arena is freed and the previous active arena is restored automatically on any exit from the block — including `return` and `break`.

`SCOPE(ptr)` enters the arena of an existing handle without taking ownership. Use this to allocate into an outer scope's arena from within an inner scope.

---

### Allocation

```c
ARENA_PTR handle = ALLOC(sizeof(MyStruct));
```

Allocates `obj_size` bytes in the current active arena. Returns an `ARENA_PTR` — a `uint64_t` encoding `arena_id` (upper 32 bits) and `offset` (lower 32 bits). Handles are stable across reallocations; raw pointers from `DEREF` are not.

---

### Dereference

```c
DEREF(handle, Type)        // resolves handle to Type*
COPY(dst, src, size)       // memcpy between two handles
```

`DEREF` resolves a handle to a typed pointer. The result is valid only until the next allocation that triggers a slab resize — do not cache it across allocations or pass it across scope boundaries.

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
    } // inner scope freed; item survives in list's arena
}
```

---

## Benchmarks

All benchmarks compiled with GCC (MinGW64), no optimisation flags.

| Benchmark | Phase | Idiomatic C | Ariandel | Ratio |
|---|---|---|---|---|
| Tree — 1M nodes | Build | 53 ms | 54 ms | ~1× |
| Tree — 1M nodes | Cleanup | 31 ms | 1 ms | ~30× |
| 13-Queens — 73,712 solutions | Solve | 1,600 ms | 2,108 ms | 0.76× |
| 13-Queens — 73,712 solutions | Cleanup | 3 ms | 1 ms | ~3× |

Cleanup is the headline result — O(1) bump reset versus O(n) traversal. The n-queens solve regression is real: `DEREF` in tight inner loops adds measurable overhead that a native compiler would eliminate by hoisting the base pointer. See [SPEC.md](SPEC.md) for full benchmark discussion.
