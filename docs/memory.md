# Memory

## The rule

**Every allocation in code that runs on a device goes through the engine.** No
`new[]`, no `malloc`, no `heap_caps_*` in the engine's runtime code or in a
package's. `Deki::Memory` is the orchestrator; `Deki::Buffer<T>` is what you
almost always want.

This is not a style preference, and it is enforced by
[`tools/ci/check_allocations.py`](../tools/ci/check_allocations.py). Three
reasons it has to hold:

1. **`new[]` cannot fail gracefully.** ESP-IDF builds with exceptions off
   (`CONFIG_COMPILER_CXX_EXCEPTIONS` is not set), so a failed allocation
   reaches `__cxa_allocate_exception` and calls `abort()` — the board reboots.
   There is no catch site to add. `std::nothrow` does *not* help: libstdc++
   implements the nothrow form by calling the throwing form inside a
   `try`/`catch` that `-fno-exceptions` has removed, so it aborts identically.
   This was diagnosed from a backtrace showing
   `operator new[](unsigned int, std::nothrow_t const&)` calling straight into
   the throwing form.
2. **Only the engine knows where memory should live.** Whether this board has
   external RAM, how much slower it is, and whether a peripheral can reach a
   given region are properties of the platform, not of the caller. A caller
   that picks a region is wrong on the next board.
3. **`Deki::Memory` is also the accounting and the leak check.** Statistics,
   the double-free guard and (in debug builds) the leak table all live behind
   it. An allocation that skips it is invisible.

## `Deki::Buffer<T>` — use this

`<deki/providers/Buffer.h>`. An owning array that allocates through
`Deki::Memory` and frees itself.

```cpp
#include <deki/providers/Buffer.h>

Deki::Buffer<uint8_t> baked;

if (!baked.Allocate(width * height * 2, Deki::MemoryUse::Buffer, "Gradient::bake"))
{
    DEKI_LOG_WARNING("no room for a %dx%d bake; not drawing it", width, height);
    return false;          // carry on without the thing, do not abort
}

RenderInto(baked.Data());  // Data(), Count(), Bytes(), operator[], begin/end
```

- **Move-only.** Copying would need a second allocation that could fail, and a
  copy constructor cannot report that.
- **`T` must be trivially copyable and trivially destructible**, both enforced
  by `static_assert`. Nothing here constructs or destroys elements — these are
  the raw pixel, glyph, span and sample buffers the engine hands to blitters
  and drivers. A `T` with a destructor would silently never run it.
- **`Allocate` on an unchanged count is a no-op**, which is what makes it safe
  to call from a render path every frame. A different count releases the old
  buffer first rather than copying: every caller overwrites it immediately
  afterwards, and holding both allocations at once is the one thing a
  constrained part cannot spare.
- **`Allocate(0)` is a success**, not a failure. A sparse font with no
  codepoints and a sprite with no spans are both normal.
- **The element count is checked against overflow.** `count * sizeof(T)` is
  computed in a 32-bit `size_t` on the device, so a count read from a file or
  a scene could wrap to a small allocation that later overruns.
- `Reset()` frees early; `Release()` hands the raw pointer out and the caller
  becomes responsible for `Deki::Memory::Free`. `Release` is rare by design.

The one thing not automated is *whether the memory was there*: check `if
(!buffer)` once, where the buffer is created, instead of unwinding on every
branch after it.

### Why not an arena

A frame arena would remove the failure handling too, but it has to be sized up
front. One large enough for a single full-screen gradient bake reserves 150 KB
whether or not a project has a gradient, which on a part with a few hundred KB
of heap is half the memory held against a feature that may not exist. Arenas
pay off where there is headroom. Most allocations here are long-lived and owned
by something anyway — glyph tables, pixel data, particle columns, framebuffers
— which is what RAII serves and an arena does not.

## `MemoryUse` — say what the memory is *for*

`<deki/providers/Memory.h>`. Callers describe the use; the engine picks the
region.

| Use | Meaning | Where it goes |
| --- | --- | --- |
| `Hot` | Read or written every frame | Internal RAM, always |
| `Buffer` | Large and read-mostly: a gradient bake, a decoded image, an asset blob | External (PSRAM) where the board has it, internal where it does not |
| `Dma` | A peripheral has to read it (a framebuffer handed to a display) | DMA-capable internal RAM, via the platform's DMA allocator |
| `Auto` | No opinion | By size: at or above `Memory::kLargeAllocation` (16 KB) it is treated as `Buffer`, below it as internal |

Pick `Hot` or `Buffer` when you know; `Auto` is for generic code that does not.

### Placement resolves once. There is no fallback.

If the chosen region is full, the allocation **fails and returns null**. It is
not retried in the other region. This is deliberate:

- Quietly moving a `Hot` buffer into external RAM turns a placement mistake
  into frame drops nobody can account for.
- Quietly moving a `Buffer` into internal RAM exhausts the heap that the next
  `Hot` allocation needed, so the failure surfaces somewhere unrelated.

A failure logs what it was, how big, which region, and that no fallback was
attempted. Every caller already has to handle null.

A board with no external RAM resolves everything to internal. That is not a
fallback; it is the only region there is.

## The rest of the API

`Deki::Memory` (`<deki/providers/Memory.h>`), for the cases `Buffer<T>` does
not cover:

```cpp
void* Allocate(size_t size, MemoryUse use = MemoryUse::Auto, const char* context = nullptr);
T*    AllocateArray<T>(size_t count, MemoryUse use = MemoryUse::Auto, const char* context = nullptr);
void  Free(void* ptr, const char* context = nullptr);
```

- Memory is **zeroed** before it is returned, so the `{}` that `new T[n]{}`
  used to provide is not lost.
- `Free` works out which region the block came from by reading a header placed
  immediately before the user pointer, so callers never choose. That header
  also carries a magic value, which turns a double free or a foreign pointer
  into a logged error instead of heap corruption.
- Always pass a `context` string. It is what the failure log and the debug
  leak table name, and `"GradientComponent::bake"` in an out-of-memory line is
  the difference between a diagnosis and a guess.
- `AllocateInternal` / `FreeInternal` bypass the placement decision and exist
  for the platform layer. Do not reach for them from a component.
- `IsPSRAMAvailable()`, `GetAvailablePSRAM()` and `GetStats()` report; they are
  not a licence to place memory yourself.

### What is deliberately *not* covered

`std::vector`, `std::string` and anything else using the default allocator call
the same `operator new` and abort the same way when it fails. The lint does not
flag them and this doc does not pretend otherwise: fixing it needs an allocator
shim, not a rule. Prefer `Deki::Buffer<T>` for anything whose size comes from a
file, a scene, or the screen — which is where the large allocations are.

## Platform layer: `IMemoryProvider`

`<deki/providers/IMemoryProvider.h>`. A platform supplies one and hands it over
before `Memory::Initialize()`:

```cpp
Deki::Memory::SetBackend(new MyBoardMemoryProvider());  // takes ownership
Deki::Memory::Initialize();
```

The interface reports external RAM (`HasExternalRAM`, totals, free) and
allocates from each region: `AllocateExternal` / `FreeExternal`,
`AllocateInternal` / `FreeInternal`, and `AllocateDma`. The defaults are
`malloc`/`free`, which is right for a host where every allocation is equally
reachable; `AllocateDma` defaults to `AllocateInternal` for the same reason.

`AllocateDma` is separate from `AllocateInternal` because on the ESP32-S3 most
internal RAM is DMA-reachable but not all of it, and a display peripheral
reading a framebuffer that is not gets silent corruption rather than an error.
The ESP32 provider asks for `MALLOC_CAP_DMA` explicitly. Whoever overrides one
of `AllocateInternal`/`FreeInternal` must override both: the pair has to match
(the ESP32 provider uses `heap_caps_aligned_alloc` with 16-byte alignment — it
also lets the blit dispatcher engage the S3 SIMD kernels — and
`heap_caps_free`).

**`IMemoryProvider` is part of the package ABI**, so its virtuals are locked by
[`tools/ci/check_abi.py`](../tools/ci/check_abi.py). Adding, removing or
reordering one moves vtable slots and requires bumping
`kDekiReflectionAbiVersion` in the same commit.

## The lint

```bash
python tools/ci/check_allocations.py            # packages beside the repo
python tools/ci/check_allocations.py --verbose  # also list files and waivers
```

Fails on `new[]`, `delete[]`, `malloc`/`calloc`/`realloc` and `heap_caps_*` in
package runtime code. It skips `editor/`, `tests/`, `generated/` and
`third_party/`, which run on the host where exceptions are on and the heap is a
desktop's. Comments and string literals are blanked before matching, so a
comment explaining this rule does not trip it.

A genuinely unavoidable case takes a waiver **with a reason** — one without a
reason does not count, so a diff always shows which exceptions were argued for:

```cpp
void* p = malloc(n);  // deki-alloc-ok: the C API frees this itself
```

It has already earned its keep: it found a `delete[]` on a `Deki::Memory`
buffer in `Standard2DRenderer` that a manual sweep had missed.
