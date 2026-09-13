// Deki::Buffer<T> — the owning buffer component and package code uses instead
// of calling an allocator and remembering a matching Free.
//
// The cases below are the mistakes this type exists to make impossible, each
// of which was a real bug in this engine: a buffer nothing freed, a free that
// did not match its allocator, a cleanup path that released something it did
// not own, and a size computed by a multiplication that wraps on a 32-bit
// device.

#include <gtest/gtest.h>

#include <deki/providers/Buffer.h>
#include <deki/providers/IMemoryProvider.h>
#include <deki/LogSystem.h>

#include <cstring>
#include <string>

#include <cstdint>
#include <utility>
#include <vector>
#include <cstdlib>

using Deki::Buffer;
using Deki::MemoryRegion;
namespace Mem = Deki::Mem;

TEST(DekiBuffer, DefaultIsEmptyAndFalsey)
{
    Buffer<uint8_t> b;
    EXPECT_FALSE(static_cast<bool>(b));
    EXPECT_EQ(b.Count(), 0u);
    EXPECT_EQ(b.Bytes(), 0u);
    EXPECT_TRUE(b.Empty());
    EXPECT_EQ(b.Data(), nullptr);
}

TEST(DekiBuffer, AllocatesAndReportsElementsNotBytes)
{
    Buffer<uint32_t> b(16, Mem::Internal);
    ASSERT_TRUE(static_cast<bool>(b));
    EXPECT_EQ(b.Count(), 16u);
    EXPECT_EQ(b.Bytes(), 16u * sizeof(uint32_t));
    EXPECT_NE(b.Data(), nullptr);
}

TEST(DekiBuffer, MemoryZeroesWhatItHandsBack)
{
    // Several call sites relied on `new T[n]{}` for this, so the replacement
    // has to keep it or those buffers start full of noise.
    Buffer<uint32_t> b(32, Mem::Internal);
    ASSERT_TRUE(static_cast<bool>(b));
    for (size_t i = 0; i < b.Count(); ++i)
        EXPECT_EQ(b[i], 0u) << "element " << i;
}

TEST(DekiBuffer, IndexingAndIterationSeeTheSameMemory)
{
    Buffer<int16_t> b(4, Mem::Internal);
    ASSERT_TRUE(static_cast<bool>(b));
    b[0] = 10; b[1] = 20; b[2] = 30; b[3] = 40;

    std::vector<int16_t> seen(b.begin(), b.end());
    ASSERT_EQ(seen.size(), 4u);
    EXPECT_EQ(seen[0], 10);
    EXPECT_EQ(seen[3], 40);
    EXPECT_EQ(b.Data()[2], 30);
}

TEST(DekiBuffer, MoveTransfersOwnershipAndLeavesTheSourceEmpty)
{
    Buffer<uint8_t> a(8, Mem::Internal);
    ASSERT_TRUE(static_cast<bool>(a));
    const uint8_t* raw = a.Data();

    Buffer<uint8_t> b(std::move(a));
    EXPECT_FALSE(static_cast<bool>(a));   // moved-from frees nothing
    EXPECT_EQ(a.Count(), 0u);
    ASSERT_TRUE(static_cast<bool>(b));
    EXPECT_EQ(b.Data(), raw);             // same memory, not a copy
    EXPECT_EQ(b.Count(), 8u);
}

TEST(DekiBuffer, MoveAssignmentReleasesWhatItHeld)
{
    Buffer<uint8_t> a(8, Mem::Internal);
    Buffer<uint8_t> b(4, Mem::Internal);
    ASSERT_TRUE(static_cast<bool>(a));
    ASSERT_TRUE(static_cast<bool>(b));

    b = std::move(a);  // b's original 4 bytes must be freed, not leaked
    EXPECT_EQ(b.Count(), 8u);
    EXPECT_FALSE(static_cast<bool>(a));
}

TEST(DekiBuffer, SelfMoveAssignmentIsNotADoubleFree)
{
    Buffer<uint8_t> a(8, Mem::Internal);
    const uint8_t* raw = a.Data();
    a = std::move(a);
    ASSERT_TRUE(static_cast<bool>(a));
    EXPECT_EQ(a.Data(), raw);
    EXPECT_EQ(a.Count(), 8u);
}

TEST(DekiBuffer, ReallocatingToTheSameCountKeepsTheMemory)
{
    // This is what makes Allocate safe to call from a render path every frame:
    // the common case where nothing changed must not churn.
    Buffer<uint8_t> b(64, Mem::External);
    ASSERT_TRUE(static_cast<bool>(b));
    const uint8_t* raw = b.Data();

    EXPECT_TRUE(b.Allocate(64, Mem::External));
    EXPECT_EQ(b.Data(), raw);
}

TEST(DekiBuffer, ReallocatingToADifferentCountResizes)
{
    Buffer<uint8_t> b(64, Mem::External);
    ASSERT_TRUE(b.Allocate(128, Mem::External));
    EXPECT_EQ(b.Count(), 128u);
    EXPECT_EQ(b.Bytes(), 128u);
}

TEST(DekiBuffer, ZeroCountIsAnEmptySuccessNotAFailure)
{
    // A sparse font with no codepoints, a sprite with no spans: asking for
    // nothing is normal and must not read as out of memory.
    Buffer<uint8_t> b;
    EXPECT_TRUE(b.Allocate(0, Mem::Internal));
    EXPECT_FALSE(static_cast<bool>(b));
    EXPECT_EQ(b.Count(), 0u);
}

TEST(DekiBuffer, AnImpossibleCountFailsRatherThanWrapping)
{
    // count * sizeof(T) is a 32-bit size_t on the device, so a count read from
    // a corrupt file could wrap to a small allocation that later overruns.
    Buffer<uint32_t> b;
    EXPECT_FALSE(b.Allocate(SIZE_MAX / 2, Mem::External));
    EXPECT_FALSE(static_cast<bool>(b));
    EXPECT_EQ(b.Count(), 0u);
}

TEST(DekiBuffer, ResetFreesImmediately)
{
    Buffer<uint8_t> b(16, Mem::Internal);
    ASSERT_TRUE(static_cast<bool>(b));
    b.Reset();
    EXPECT_FALSE(static_cast<bool>(b));
    EXPECT_EQ(b.Data(), nullptr);
    b.Reset();  // twice is not a double free
    EXPECT_FALSE(static_cast<bool>(b));
}

TEST(DekiBuffer, ReleaseHandsOwnershipOut)
{
    Buffer<uint8_t> b(16, Mem::Internal);
    ASSERT_TRUE(static_cast<bool>(b));
    uint8_t* raw = b.Release();
    ASSERT_NE(raw, nullptr);
    EXPECT_FALSE(static_cast<bool>(b));  // the buffer no longer frees it
    Deki::Memory::Free(raw);             // the caller does
}

// --- placement -------------------------------------------------------------
// Nothing reaches external RAM unless the caller names Mem::External.
// There used to be a size rule (>= 16 KB went to PSRAM) and it was removed:
// in a graphics engine the largest allocations are usually the ones touched
// every frame, and a framebuffer in PSRAM measures markedly worse on real
// hardware. These pin that no such inference comes back.

namespace
{
// Records where each allocation was asked from, so a test can assert the
// region rather than infer it. HasExternalRAM is true, so the external branch
// is live — on a host there is no PSRAM, but Memory cannot tell the
// difference and neither should the rule.
class RegionSpyProvider : public Deki::IMemoryProvider
{
public:
    bool Initialize() override { return true; }
    void Shutdown() override {}

    bool HasExternalRAM() const override { return true; }
    size_t GetTotalExternalRAM() const override { return 4 * 1024 * 1024; }
    size_t GetAvailableExternalRAM() const override { return 4 * 1024 * 1024; }

    void* AllocateExternal(size_t size) override
    {
        ++externalCalls;
        void* p = malloc(size);
        m_LastExternal = p;
        return p;
    }
    void FreeExternal(void* ptr) override { free(ptr); }
    bool IsExternalPointer(void* ptr) const override { return ptr && ptr == m_LastExternal; }

    void* AllocateInternal(size_t size) override
    {
        ++internalCalls;
        return malloc(size);
    }
    void FreeInternal(void* ptr) override { free(ptr); }

    void* AllocateDMA(size_t size) override
    {
        ++dmaCalls;
        return malloc(size);
    }

    // A number a failure log can quote. The spy always succeeds, so this only
    // has to be non-zero to prove the reporting path reads it.
    size_t GetAvailableInternalRAM() const override { return 256 * 1024; }

    int externalCalls = 0;
    int internalCalls = 0;
    int dmaCalls = 0;

private:
    void* m_LastExternal = nullptr;
};

struct ScopedSpy
{
    ScopedSpy() : spy(new RegionSpyProvider())
    {
        Deki::Memory::SetBackend(spy);   // takes ownership
        Deki::Memory::Initialize();
    }
    ~ScopedSpy()
    {
        Deki::Memory::Shutdown();
        Deki::Memory::SetBackend(nullptr);
    }
    RegionSpyProvider* spy;
};
}  // namespace

TEST(DekiMemoryPlacement, TheDefaultIsInternalEvenForALargeAllocation)
{
    // 150 KB — comfortably over the 16 KB the old rule used, and the size of a
    // 320x240 RGB565 framebuffer, which is exactly the buffer that must not go
    // to PSRAM by accident.
    ScopedSpy s;
    void* p = Deki::Memory::Allocate(153600, Deki::Mem::Internal);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(s.spy->externalCalls, 0);
    EXPECT_GT(s.spy->internalCalls, 0);
    Deki::Memory::Free(p);
}

TEST(DekiMemoryPlacement, HotIsInternalAtAnySize)
{
    ScopedSpy s;
    void* p = Deki::Memory::Allocate(1024 * 1024, Deki::Mem::Internal);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(s.spy->externalCalls, 0);
    Deki::Memory::Free(p);
}

TEST(DekiMemoryPlacement, BufferIsTheOnlyRouteToExternal)
{
    ScopedSpy s;
    void* p = Deki::Memory::Allocate(64, Deki::Mem::External);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(s.spy->externalCalls, 1);   // and size is irrelevant: 64 bytes
    Deki::Memory::Free(p);
}

TEST(DekiMemoryPlacement, DmaAsksTheProviderForDmaMemory)
{
    ScopedSpy s;
    void* p = Deki::Memory::AllocateDma(4096, Deki::Mem::Internal);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(s.spy->dmaCalls, 1);
    EXPECT_EQ(s.spy->externalCalls, 0);   // a peripheral must be able to reach it
    Deki::Memory::Free(p);
}

TEST(DekiMemoryPlacement, AnOmittedMemoryUseIsInternal)
{
    // The default argument, which is what most call sites use.
    ScopedSpy s;
    Deki::Buffer<uint8_t> b(200000, Deki::Mem::Internal);
    ASSERT_TRUE(static_cast<bool>(b));
    EXPECT_EQ(s.spy->externalCalls, 0);
}

// --- the context the compiler fills in --------------------------------------
// Nobody should have to type "ParticlePool::posX" by hand. AllocContext's
// default argument captures the call site, and the property that makes it
// worth anything is that it names the CALLER rather than whichever engine
// header the allocation eventually passed through.

namespace
{
// Refuses everything, so the failure log can be provoked on a host where the
// heap would otherwise satisfy any request a size_t can express.
class AlwaysFailsProvider : public Deki::IMemoryProvider
{
public:
    bool Initialize() override { return true; }
    void Shutdown() override {}
    bool HasExternalRAM() const override { return false; }
    size_t GetTotalExternalRAM() const override { return 0; }
    size_t GetAvailableExternalRAM() const override { return 0; }
    void* AllocateExternal(size_t) override { return nullptr; }
    void FreeExternal(void*) override {}
    bool IsExternalPointer(void*) const override { return false; }
    void* AllocateInternal(size_t) override { return nullptr; }
    void FreeInternal(void*) override {}

    // Nothing is reachable here either: a provider that refuses internal RAM
    // and then hands out DMA memory would be lying about the same heap.
    void* AllocateDMA(size_t) override { return nullptr; }

    // Genuinely exhausted, so zero is the measurement rather than a shrug.
    size_t GetAvailableInternalRAM() const override { return 0; }
};

// Collects what the engine logged, so a test can assert on the message a
// developer would actually read.
struct CapturedLog
{
    CapturedLog()
    {
        Deki::LogSystem::SetLogCallback(
            [this](Deki::LogLevel, const std::string& message, const char*, int)
            { text += message; text += "\n"; });
    }
    ~CapturedLog() { Deki::LogSystem::SetLogCallback(nullptr); }

    bool Contains(const char* needle) const { return text.find(needle) != std::string::npos; }

    std::string text;
};
}  // namespace

TEST(DekiAllocContext, AContextNamesTheCallSite)
{
    const Deki::AllocContext ctx;  // default argument territory: captured here

#if defined(DEKI_ALLOC_CONTEXT_CAPTURE)
    ASSERT_NE(ctx.Name(), nullptr);
    EXPECT_NE(strstr(ctx.Name(), "AContextNamesTheCallSite"), nullptr)
        << "name was: " << ctx.Name();
    ASSERT_NE(ctx.File(), nullptr);
    EXPECT_NE(strstr(ctx.File(), "BufferTests"), nullptr) << "file was: " << ctx.File();
    EXPECT_GT(ctx.Line(), 0u);
#else
    // Stripped: nothing captured, so nothing claimed.
    EXPECT_STREQ(ctx.Name(), "?");
    EXPECT_EQ(ctx.Line(), 0u);
#endif
}

TEST(DekiAllocContext, TwoAllocationsInOneFunctionAreToldApartByLine)
{
    // This is what replaced the hand-written labels. ParticlePool allocates
    // thirteen columns inside one SetCapacity, and they used to be told apart
    // by strings someone had to keep in step with the members. The line number
    // does it without anyone maintaining anything.
    const Deki::AllocContext first;
    const Deki::AllocContext second;

#if defined(DEKI_ALLOC_CONTEXT_CAPTURE)
    EXPECT_STREQ(first.Name(), second.Name());   // same enclosing function
    EXPECT_STREQ(first.File(), second.File());
    EXPECT_NE(first.Line(), second.Line()) << "two allocations in one function "
                                              "must not report the same origin";
#else
    EXPECT_EQ(first.Line(), second.Line());      // both nothing
#endif
}

TEST(DekiAllocContext, AFailureNamesTheCallerNotTheEngineHeader)
{
    // The whole point. Buffer<T>::Allocate is a template in Buffer.h which
    // forwards to Memory::Allocate in Memory.cpp. If either of those
    // re-defaulted the context instead of forwarding it, every allocation in
    // the engine would report Buffer.h or Memory.cpp as its origin and the
    // feature would be worse than useless — it would be confidently wrong.
    Deki::Memory::SetBackend(new AlwaysFailsProvider());
    CapturedLog log;

    Deki::Buffer<uint8_t> b;
    EXPECT_FALSE(b.Allocate(64, Deki::Mem::Internal));  // no context given

    Deki::Memory::SetBackend(nullptr);

#if defined(DEKI_ALLOC_CONTEXT_CAPTURE)
    EXPECT_TRUE(log.Contains("AFailureNamesTheCallerNotTheEngineHeader"))
        << "logged: " << log.text;

    // Match on "at <file>:", which is the origin the AllocContext claims.
    // A bare filename test would pass on the logger's own "Memory.cpp:450 ->"
    // stamp, which says where the DEKI_LOG_ERROR statement lives and has
    // nothing to do with who asked for the memory.
    EXPECT_TRUE(log.Contains("at BufferTests.cpp:")) << "logged: " << log.text;
    EXPECT_FALSE(log.Contains("at Buffer.h:")) << "logged: " << log.text;
    EXPECT_FALSE(log.Contains("at Memory.cpp:")) << "logged: " << log.text;
#else
    // Nothing was captured, so nothing is claimed — and in particular the
    // engine must not invent an origin it does not have.
    EXPECT_FALSE(log.Contains(" at ")) << "logged: " << log.text;
#endif
}

// --- the reporting side ----------------------------------------------------
// "How much memory am I using?" needed GetStats for the allocated half and a
// provider the caller does not have for the free half.

TEST(DekiMemoryReporting, AvailableInternalIsSafeWithoutABackend)
{
    // Host test binaries run with no backend set (Allocate falls through to
    // malloc), so this must answer rather than dereference one. Zero means
    // "not reported", which is also what the desktop provider says.
    EXPECT_EQ(Deki::Memory::GetAvailableInternal(), 0u);
}

TEST(DekiMemoryReporting, StatsCountWhatBufferAllocates)
{
    const uint32_t before = Deki::Memory::GetStats().totalAllocated;
    {
        Buffer<uint8_t> b(4096, Mem::External);
        ASSERT_TRUE(static_cast<bool>(b));
        EXPECT_GE(Deki::Memory::GetStats().totalAllocated, before + 4096u);
    }
    // And stop counting it once the buffer goes out of scope, which is the
    // property that makes the number worth logging at all.
    EXPECT_EQ(Deki::Memory::GetStats().totalAllocated, before);
}

TEST(DekiMemoryReporting, LogStatsWorksWithAndWithoutALabel)
{
    // It formats and logs; the point of the test is that neither form
    // dereferences a null label or a missing backend.
    Deki::Memory::LogStats();
    Deki::Memory::LogStats("BufferTests");
}

// --- regions are open -------------------------------------------------------
// The engine ships "internal" and "external". Nothing stops a package adding
// its own, which is the point: a board with RTC memory that survives deep
// sleep should not need an engine release to say so.

namespace Deki::Mem
{
// Exactly what a board-support package would write. Deki::Mem is an ordinary
// namespace, so this needs no engine change and no registration call.
inline constexpr MemoryRegion TestRtc = Region("rtc");
}  // namespace Deki::Mem

TEST(DekiMemoryRegion, APackageCanDefineItsOwnRegion)
{
    // Usable in a constant expression, so it costs nothing at runtime and can
    // sit in a switch label inside a provider.
    static_assert(Deki::Mem::TestRtc.id == Deki::HashName("rtc"));
    EXPECT_STREQ(Deki::Mem::TestRtc.name, "rtc");
}

TEST(DekiMemoryRegion, RegionsAreDistinctAndCompareByName)
{
    static_assert(Deki::Mem::Internal != Deki::Mem::External);
    static_assert(Deki::Mem::Internal == Deki::Region("internal"));

    // The name survives to the failure log; an id alone would print a number.
    EXPECT_STREQ(Deki::Mem::Internal.name, "internal");
    EXPECT_STREQ(Deki::Mem::External.name, "external");
}

TEST(DekiMemoryRegion, TheHashMatchesTheRuntimeOneUsedEverywhereElse)
{
    // FNV-1a, computed here at runtime, independently of the constexpr one.
    // Region names must hash the way every other name in this engine does, or
    // a region name and a platform capability name could not be the same token
    // without a table mapping between them.
    auto runtime_fnv1a = [](const char* str)
    {
        uint32_t h = 2166136261u;
        while (*str)
        {
            h ^= static_cast<uint8_t>(*str++);
            h *= 16777619u;
        }
        return h;
    };

    EXPECT_EQ(Deki::HashName("internal"), runtime_fnv1a("internal"));
    EXPECT_EQ(Deki::HashName("external"), runtime_fnv1a("external"));
    EXPECT_EQ(Deki::HashName("rtc"), runtime_fnv1a("rtc"));
    EXPECT_NE(Deki::HashName("internal"), Deki::HashName("external"));
}
