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
- **The `MemoryUse` is required**, here as everywhere else.
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

## `MemoryUse` — you choose, every time

`<deki/providers/Memory.h>`. **There is no default and nothing is inferred.**
Every allocating call takes a `MemoryUse`, and omitting it does not compile.

| Use | Where it goes |
| --- | --- |
| `Internal` | Internal RAM. The plain choice for anything without a reason to be elsewhere |
| `Hot` | Internal RAM, and read or written every frame |
| `Buffer` | External (PSRAM) where the board has it, internal where it does not |
| `Dma` | DMA-capable internal RAM, via the platform's DMA allocator |

`Internal` and `Hot` place identically today. The difference is what they claim:
`Internal` means nobody asked for anything special, `Hot` means *this must never
be moved external*, whatever a later policy or tool decides. Sprite pixels the
blitter walks every frame are `Hot`; a glyph table read once at load is
`Internal`.

**`Buffer` is the only route to PSRAM**, and it is a claim that the latency does
not matter for that buffer.

### Why you choose and not the engine

Size does not predict placement. The largest allocation on the part is usually
the framebuffer, and it belongs in internal RAM; a decoded asset of exactly the
same size may never be read again after load. Only the caller knows which it is
holding.

So there is no size rule, and no default argument either — a default is the
engine choosing, one step removed. A buffer that lands in slow memory for
reasons invisible in the source is a performance bug nobody can find by reading
the code.

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
void* Allocate(size_t size, MemoryUse use, const char* context = nullptr);
T*    AllocateArray<T>(size_t count, MemoryUse use, const char* context = nullptr);
void  Free(void* ptr, const char* context = nullptr);
```

- Memory is **zeroed** before it is returned, so a buffer never starts full of
  noise and callers do not write their own clearing loop.
- `Free` works out which region the block came from by reading a header placed
  immediately before the user pointer, so callers never choose. That header
  also carries a magic value, which turns a double free or a foreign pointer
  into a logged error instead of heap corruption.
- **Naming the allocation is optional.** See `AllocContext` below.
- A failure reports how much was free in the region it tried, which separates
  "this asset is too big for this board" from "something is holding memory it
  should have released".
- `Memory::Initialize` logs the free internal and external RAM once at startup,
  so a project can be sized against real numbers rather than against the first
  out-of-memory line.
- `AllocateInternal` / `FreeInternal` bypass the placement decision and exist
  for the platform layer. Do not reach for them from a component.

### Finding out where the memory went

```cpp
Deki::Memory::LogStats("after level load");
// Memory at after level load: 48216 bytes allocated (48216 internal, 0 external);
//                             237096 free internal
```

- `LogStats(label)` answers "how much memory am I using?" in one call. In a
  debug build it also reports the live and peak allocation counts. It costs
  nothing unless called.
- `GetStats()` returns the same numbers as a struct;
  `GetAvailableInternal()` and `GetAvailablePSRAM()` give the free side.
  `IsPSRAMAvailable()` says whether there is an external region at all. None
  of these is a licence to place memory yourself — they are for sizing a
  project and for finding what is holding memory it should have released.

## `AllocContext` — the label writes itself

Every allocating call takes one, and almost nobody writes one:

```cpp
posX.Allocate(capacity, Deki::MemoryUse::Internal);
```

In a debug build the default argument captures the call site, so a failure
reads:

```
Failed to allocate 153600 bytes in internal RAM for
'void ParticlePool::SetCapacity(size_t)' at ParticlePool.h:47
(126556 bytes free there)
```

Pass a literal where the call site is not the useful name — one function
allocating three different things — and it wins over the derived one:

```cpp
posX.Allocate(capacity, Deki::MemoryUse::Internal, "ParticlePool::posX");
```

**It names the caller, not the engine.** `Buffer<T>::Allocate` and
`Memory::Allocate` both take the context and forward it rather than
re-defaulting it, so the location is where *you* wrote the call, not
`Buffer.h`. A test asserts exactly that, because a version that got this wrong
would report a plausible-looking file on every allocation in the engine and be
confidently useless.

**Nothing is copied.** Every member points at a string literal with static
storage duration, which is what lets the debug leak table hold the pointer. Do
not build one from a `std::string` — the label has to outlive the allocation.

### It is a debug-build feature

Capturing costs flash: the file paths and function names live in the binary
forever. On device that is a poor trade, so the capture compiles out entirely
unless `DEBUG` is defined (or you set `DEKI_NO_ALLOC_CONTEXT` to switch it off
in a debug build too).

| | Debug / editor | Release / firmware |
| --- | --- | --- |
| `AllocContext` | label, file, function, line | **one pointer** |
| Failure line | `for '<caller>' at <file>:<line>` | `for '<label>'; build with DEBUG for the call site` |
| Flash cost | strings embedded | **+32 bytes measured, 0.002%** |
| `Deki::Vector` | +12 bytes per instance | unchanged |

So on device a hand-written label is still worth having for anything you would
want named in a crash log: without one, an allocation reports `'?'`.

Note the layout differs between debug and release, so an engine and the
packages calling it have to agree — the same constraint `MemoryStats` already
imposes.

## `Deki::Vector<T>` — when the count changes at runtime

`<deki/providers/Vector.h>`. `Buffer<T>` covers a count you know when you
allocate. When it changes and has no fixed bound, `std::vector` is the wrong
answer on a device: `push_back` calls `operator new`, which cannot report
exhaustion with exceptions off, so it aborts and the board reboots.

`Deki::Vector` is that shape with the one thing changed that matters here —
everything that can allocate returns `bool`:

```cpp
Deki::Vector<Enemy> enemies(Deki::MemoryUse::Internal, "wave");

if (!enemies.Reserve(32))            // out of memory, said once
    return;

if (!enemies.PushBack(spawned))      // and per growth, if it has to grow
    DEKI_LOG_WARNING("dropped a spawn");
```

- **The region is a constructor argument**, so the declaration is where a
  reader learns what memory this list occupies, and every growth lands in the
  same place. There is no default constructor: a vector that grows must know
  where to put the new block, and the engine will not decide for it.

- **`Reserve` up front makes the pushes that follow infallible.** One check
  instead of one per element; that's the pattern worth writing on a device.
- Growth **doubles from a first block of 4**, not 16: a device project has
  many short lists, and a large default on each adds up to more than the lists
  do.
- **A failed growth leaves everything already stored intact**, so a caller
  that drops the new element still has a working vector.
- `RemoveUnordered(i)` is O(1) and fills the hole with the last element — the
  same trick `ParticlePool::KillSwap` uses, and what a list of live entities
  wants. `Clear()` keeps the memory for refilling next frame; `Reset()`
  releases it.
- Same `T` constraint as `Buffer`: trivially copyable and trivially
  destructible. Growth moves elements with a `memcpy`.

Reach for `Buffer` first. Most "dynamic" data here turns out to have a bound
the project already knows, and a fixed allocation cannot fragment a small heap
the way repeated growth can.

### `std::vector`, `std::string` and plain `new`

These call `operator new`, which `Deki::Buffer<T>` cannot replace. On a device
build the engine replaces the operators themselves
([`src/providers/OperatorNew.cpp`](../deki-engine/src/providers/OperatorNew.cpp)),
which changes two things:

- **A failed allocation says so.** It logs the size it wanted and then aborts,
  because a throwing `operator new` has no other option with exceptions off.
  "Out of memory asking for 40960 bytes" is a diagnosis; the reboot it
  replaces was not.
- **`std::nothrow` works.** `new (std::nothrow) T[n]` returns `nullptr`
  instead of aborting, so code that wants to handle exhaustion itself can.
  Without the replacement it aborts, because libstdc++ implements the nothrow
  form by calling the throwing one inside a removed `try`/`catch`.

These **do** go through the engine: `operator new` calls
`IMemoryProvider::AllocateRaw`, so a board sees every allocation on the device
and can substitute its heap for all of them.

What it is *not* is `Memory::Allocate`. That would put a 16-byte header before
every `std::string` that outgrows its small-string buffer, memset memory a
vector is about to overwrite, and pay the ESP32 provider's 16-byte SIMD
alignment on small blocks — and ESP-IDF implements aligned allocation by
over-allocating, so that is more waste than the header. `AllocateRaw` has none
of it: natural alignment, no header, the pointer the heap returned.

Measured, 1.6M paired 48-byte allocations, engine at `-O2`:

| | ns/op |
|---|---|
| `malloc` + `free` | 47–52 |
| routed through the provider | 46–51 — indistinguishable |
| routed, plus `DEKI_MEMORY_TRACK_RAW` | 63–71 (+35%) |

So the routing is free and always on; **counting the bytes is opt-in**, because
with no header the block size has to come from the heap and on Windows that is
`_msize` calling `HeapSize` under a lock. Build with `DEKI_MEMORY_TRACK_RAW`
while hunting where memory went, and `Memory::LogStats` will report what
containers hold; without it that line says so rather than printing a
misleading zero.

What is given up along with the header is its magic value, so a **double free
of container memory is as undiagnosable as it was** when this path was plain
`malloc`. Anything the engine can describe goes through `Allocate` and keeps
the guard.

Placement is deliberately not decided here. Routing by size the way
the removed `Auto` rule did would silently move a large `std::vector` into
PSRAM and make a hot loop slow for reasons invisible in the code. A caller who
wants external memory names `MemoryUse::Buffer` and gets `Deki::Buffer`;
`operator new` cannot know, so it does not guess.

It is compiled only where `__cpp_exceptions` is undefined — i.e. exactly the
builds where the abort problem exists. Host and editor builds keep the standard
operators. The over-aligned forms (`align_val_t`) are left to libstdc++ so they
stay a matched pair.

So: for a count that changes at runtime, use `Deki::Vector<T>` and handle
the `false`. For anything whose size comes from a file, a scene, or the
screen, use `Deki::Buffer<T>`. `std::vector` stays right for editor and
tooling code, and on a device its exhaustion is now a logged abort rather
than a silent one.

## Platform layer: `IMemoryProvider`

`<deki/providers/IMemoryProvider.h>`. A platform supplies one and hands it over
before `Memory::Initialize()`:

```cpp
Deki::Memory::SetBackend(new MyBoardMemoryProvider());  // takes ownership
Deki::Memory::Initialize();
```

The interface reports external RAM (`HasExternalRAM`, totals, free) and
allocates from each region: `AllocateExternal` / `FreeExternal`,
`AllocateInternal` / `FreeInternal`, and `AllocateDMA`.

**Every placement virtual is pure — implement all of them.** A provider states
what each region means on its platform, even when the answer is "the same as
the others here". A desktop provider writing `return malloc(size);` in
`AllocateDMA` has made a claim you can read and check.

`AllocateDMA` is the one that matters. A provider that could inherit it from
`AllocateInternal` would hand a display peripheral memory it cannot reach on
any part where those differ — silent corruption, with nothing in the build to
say so.

`GetAvailableInternalRAM` is required for the same reason: a provider that
answers 0 without meaning it makes the startup line report an exhausted board.

Pure virtuals occupy the same vtable slots as implemented ones, so this costs
no ABI version. It stops a provider compiling until it decides.

`AllocateRaw` / `FreeRaw` / `GetRawBlockSize` are the fourth pair plus a query:
the plain natural-alignment allocator the global `operator new` routes through,
so a board's heap serves `std::vector` and scalar `new` as well as everything
the engine places. They default to `malloc`/`free`, and `GetRawBlockSize`
returns 0 where the platform cannot answer — which only costs accuracy in
`LogStats`. Override both of the pair or neither.

`AllocateDMA` is separate from `AllocateInternal` because on the ESP32-S3 most
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
runtime code — **the engine's own as well as every package's**. An allocation
in the engine reaches every project on every board, where a package's reaches
only the projects that installed it, so if anything the engine is the more
important half; leaving it out of the first version of this lint immediately
proved the point, since `ProjectSettings` was keeping its on-device settings
chunk with a bare `realloc`.

Skipped: `editor/`, `tests/`, `generated/`, `third_party/` and the engine's
`platforms/editor` and `platforms/desktop` layers, all of which run on the host
where exceptions are on and the heap is a desktop's. Exempt by path, because
they are the bottom of the stack rather than code with permission to break the
rule: `providers/Memory.cpp`, `providers/IMemoryProvider.h`, any
`*MemoryProvider.*`, and `providers/OperatorNew.cpp` — which has to define
`operator new[]` so that nothing else needs to.

Comments and string literals are blanked before matching, so a comment
explaining this rule does not trip it.

A genuinely unavoidable case takes a waiver **with a reason** — one without a
reason does not count, so a diff always shows which exceptions were argued for:

```cpp
void* p = malloc(n);  // deki-alloc-ok: the C API frees this itself
```

It has already earned its keep: it found a `delete[]` on a `Deki::Memory`
buffer in `Standard2DRenderer` that a manual sweep had missed.
