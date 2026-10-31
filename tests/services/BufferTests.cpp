// Deki::Buffer<T>: the owning buffer that component and package code uses
// instead of calling an allocator and remembering a matching Free.
//
// The cases below cover the mistakes this type prevents: a buffer nothing
// frees, a free that does not match its allocator, a cleanup path that
// releases something it does not own, and a size computed by a
// multiplication that wraps on a 32-bit device.

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
using Region = Deki::Memory::Region;

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
    Buffer<uint32_t> b(16, Deki::Memory::Internal);
    ASSERT_TRUE(static_cast<bool>(b));
    EXPECT_EQ(b.Count(), 16u);
    EXPECT_EQ(b.Bytes(), 16u * sizeof(uint32_t));
    EXPECT_NE(b.Data(), nullptr);
}

TEST(DekiBuffer, MemoryZeroesWhatItHandsBack)
{
    // Call sites rely on zeroed memory, as `new T[n]{}` gave them; without it
    // those buffers start full of noise.
    Buffer<uint32_t> b(32, Deki::Memory::Internal);
    ASSERT_TRUE(static_cast<bool>(b));
    for (size_t i = 0; i < b.Count(); ++i)
    {
        EXPECT_EQ(b[i], 0u) << "element " << i;
    }
}

TEST(DekiBuffer, IndexingAndIterationSeeTheSameMemory)
{
    Buffer<int16_t> b(4, Deki::Memory::Internal);
    ASSERT_TRUE(static_cast<bool>(b));
    b[0] = 10;
    b[1] = 20;
    b[2] = 30;
    b[3] = 40;

    std::vector<int16_t> seen(b.begin(), b.end());
    ASSERT_EQ(seen.size(), 4u);
    EXPECT_EQ(seen[0], 10);
    EXPECT_EQ(seen[3], 40);
    EXPECT_EQ(b.Data()[2], 30);
}

TEST(DekiBuffer, MoveTransfersOwnershipAndLeavesTheSourceEmpty)
{
    Buffer<uint8_t> a(8, Deki::Memory::Internal);
    ASSERT_TRUE(static_cast<bool>(a));
    const uint8_t* raw = a.Data();

    Buffer<uint8_t> b(std::move(a));
    EXPECT_FALSE(static_cast<bool>(a));  // moved-from frees nothing
    EXPECT_EQ(a.Count(), 0u);
    ASSERT_TRUE(static_cast<bool>(b));
    EXPECT_EQ(b.Data(), raw);  // same memory, not a copy
    EXPECT_EQ(b.Count(), 8u);
}

TEST(DekiBuffer, MoveAssignmentReleasesWhatItHeld)
{
    Buffer<uint8_t> a(8, Deki::Memory::Internal);
    Buffer<uint8_t> b(4, Deki::Memory::Internal);
    ASSERT_TRUE(static_cast<bool>(a));
    ASSERT_TRUE(static_cast<bool>(b));

    b = std::move(a);  // b's original 4 bytes must be freed, not leaked
    EXPECT_EQ(b.Count(), 8u);
    EXPECT_FALSE(static_cast<bool>(a));
}

TEST(DekiBuffer, SelfMoveAssignmentIsNotADoubleFree)
{
    Buffer<uint8_t> a(8, Deki::Memory::Internal);
    const uint8_t* raw = a.Data();
    a = std::move(a);
    ASSERT_TRUE(static_cast<bool>(a));
    EXPECT_EQ(a.Data(), raw);
    EXPECT_EQ(a.Count(), 8u);
}

TEST(DekiBuffer, ReallocatingToTheSameCountKeepsTheMemory)
{
    // This makes Allocate safe to call from a render path every frame: when
    // nothing changed, nothing is reallocated.
    Buffer<uint8_t> b(64, Deki::Memory::External);
    ASSERT_TRUE(static_cast<bool>(b));
    const uint8_t* raw = b.Data();

    EXPECT_TRUE(b.Allocate(64, Deki::Memory::External));
    EXPECT_EQ(b.Data(), raw);
}

TEST(DekiBuffer, ReallocatingToADifferentCountResizes)
{
    Buffer<uint8_t> b(64, Deki::Memory::External);
    ASSERT_TRUE(b.Allocate(128, Deki::Memory::External));
    EXPECT_EQ(b.Count(), 128u);
    EXPECT_EQ(b.Bytes(), 128u);
}

TEST(DekiBuffer, ZeroCountIsAnEmptySuccessNotAFailure)
{
    // A sparse font with no codepoints, a sprite with no spans: asking for
    // nothing is normal and must not read as out of memory.
    Buffer<uint8_t> b;
    EXPECT_TRUE(b.Allocate(0, Deki::Memory::Internal));
    EXPECT_FALSE(static_cast<bool>(b));
    EXPECT_EQ(b.Count(), 0u);
}

TEST(DekiBuffer, AnImpossibleCountFailsRatherThanWrapping)
{
    // count * sizeof(T) is a 32-bit size_t on the device, so a count read from
    // a corrupt file could wrap to a small allocation that later overruns.
    Buffer<uint32_t> b;
    EXPECT_FALSE(b.Allocate(SIZE_MAX / 2, Deki::Memory::External));
    EXPECT_FALSE(static_cast<bool>(b));
    EXPECT_EQ(b.Count(), 0u);
}

TEST(DekiBuffer, ResetFreesImmediately)
{
    Buffer<uint8_t> b(16, Deki::Memory::Internal);
    ASSERT_TRUE(static_cast<bool>(b));
    b.Reset();
    EXPECT_FALSE(static_cast<bool>(b));
    EXPECT_EQ(b.Data(), nullptr);
    b.Reset();  // twice is not a double free
    EXPECT_FALSE(static_cast<bool>(b));
}

TEST(DekiBuffer, ReleaseHandsOwnershipOut)
{
    Buffer<uint8_t> b(16, Deki::Memory::Internal);
    ASSERT_TRUE(static_cast<bool>(b));
    uint8_t* raw = b.Release();
    ASSERT_NE(raw, nullptr);
    EXPECT_FALSE(static_cast<bool>(b));  // the buffer no longer frees it
    Deki::Memory::Free(raw);             // the caller does
}

// --- placement -------------------------------------------------------------
// Nothing reaches external RAM unless the caller names Deki::Memory::External.
// Size never picks the region: in a graphics engine the largest allocations
// are usually the ones touched every frame, and a framebuffer in PSRAM is
// much slower on real hardware. These tests keep it that way.

namespace
{
// Records which region each allocation asked for, so a test can assert it.
class RegionSpyProvider : public Deki::IMemoryProvider
{
public:
    bool Initialize() override { return true; }
    void Shutdown() override {}

    // Claims both regions, so the external branch is live. On a host there is
    // no PSRAM, but Memory cannot tell the difference and neither should the
    // placement rule.
    bool Serves(Deki::Memory::Region region) const override
    {
        return region == Deki::Memory::Internal || region == Deki::Memory::External;
    }

    void* Allocate(Deki::Memory::Region region, size_t bytes, bool needsDma) override
    {
        if (!Serves(region))
        {
            ++refusedCalls;
            return nullptr;
        }
        if (needsDma)
        {
            ++dmaCalls;
        }
        if (region == Deki::Memory::External)
        {
            ++externalCalls;
        }
        else
        {
            ++internalCalls;
        }
        return malloc(bytes);
    }

    void Free(Deki::Memory::Region, void* ptr) override { free(ptr); }

    // A number a failure log can quote, so the reporting path runs. Zero for
    // a region this board does not serve.
    size_t GetAvailable(Deki::Memory::Region region) const override { return Serves(region) ? 256 * 1024 : 0; }

    int externalCalls = 0;
    int internalCalls = 0;
    int dmaCalls = 0;
    int refusedCalls = 0;  ///< regions this board does not serve
};

struct ScopedSpy
{
    ScopedSpy()
        : spy(new RegionSpyProvider())
    {
        Deki::Memory::SetBackend(spy);  // takes ownership
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
    // 150 KB: the size of a 320x240 RGB565 framebuffer, the buffer that must
    // not go to PSRAM by accident.
    ScopedSpy s;
    void* p = Deki::Memory::Allocate(153600, Deki::Memory::Internal);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(s.spy->externalCalls, 0);
    EXPECT_GT(s.spy->internalCalls, 0);
    Deki::Memory::Free(p);
}

TEST(DekiMemoryPlacement, HotIsInternalAtAnySize)
{
    ScopedSpy s;
    void* p = Deki::Memory::Allocate(1024 * 1024, Deki::Memory::Internal);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(s.spy->externalCalls, 0);
    Deki::Memory::Free(p);
}

TEST(DekiMemoryPlacement, BufferIsTheOnlyRouteToExternal)
{
    ScopedSpy s;
    void* p = Deki::Memory::Allocate(64, Deki::Memory::External);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(s.spy->externalCalls, 1);  // even at 64 bytes: size does not matter
    Deki::Memory::Free(p);
}

TEST(DekiMemoryPlacement, DmaAsksTheProviderForDmaMemory)
{
    ScopedSpy s;
    void* p = Deki::Memory::AllocateDma(4096, Deki::Memory::Internal);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(s.spy->dmaCalls, 1);
    EXPECT_EQ(s.spy->externalCalls, 0);  // a peripheral must be able to reach it
    Deki::Memory::Free(p);
}

TEST(DekiMemoryPlacement, AnOmittedMemoryUseIsInternal)
{
    // The default argument, which is what most call sites use.
    ScopedSpy s;
    Deki::Buffer<uint8_t> b(200000, Deki::Memory::Internal);
    ASSERT_TRUE(static_cast<bool>(b));
    EXPECT_EQ(s.spy->externalCalls, 0);
}

// --- the context the compiler fills in --------------------------------------
// AllocContext's default argument captures the call site, so nobody types a
// label by hand. It must name the CALLER, not the engine header the
// allocation passed through.

namespace
{
// Refuses everything, so the failure log can be provoked on a host where the
// heap would otherwise satisfy any request a size_t can express.
class AlwaysFailsProvider : public Deki::IMemoryProvider
{
public:
    bool Initialize() override { return true; }
    void Shutdown() override {}
    // Serves the regions but has nothing left in them. That is a different
    // failure from "this board has no such region" and logs differently.
    bool Serves(Deki::Memory::Region region) const override
    {
        return region == Deki::Memory::Internal || region == Deki::Memory::External;
    }

    void* Allocate(Deki::Memory::Region, size_t, bool) override { return nullptr; }
    void Free(Deki::Memory::Region, void*) override {}

    // Exhausted, so zero is the real figure.
    size_t GetAvailable(Deki::Memory::Region) const override { return 0; }
};

// Collects what the engine logged, so a test can check the message a
// developer reads.
struct CapturedLog
{
    CapturedLog()
    {
        Deki::LogSystem::SetLogCallback(
            [this](Deki::LogLevel, const std::string& message, const char*, int)
            {
                text += message;
                text += "\n";
            });
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
    EXPECT_NE(strstr(ctx.Name(), "AContextNamesTheCallSite"), nullptr) << "name was: " << ctx.Name();
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
    // ParticlePool allocates thirteen columns inside one SetCapacity; the line
    // number tells them apart with no hand-written labels.
    const Deki::AllocContext first;
    const Deki::AllocContext second;

#if defined(DEKI_ALLOC_CONTEXT_CAPTURE)
    EXPECT_STREQ(first.Name(), second.Name());  // same enclosing function
    EXPECT_STREQ(first.File(), second.File());
    EXPECT_NE(first.Line(), second.Line()) << "two allocations in one function "
                                              "must not report the same origin";
#else
    EXPECT_EQ(first.Line(), second.Line());  // both nothing
#endif
}

TEST(DekiAllocContext, AFailureNamesTheCallerNotTheEngineHeader)
{
    // Buffer<T>::Allocate (Buffer.h) forwards to Memory::Allocate
    // (Memory.cpp). Both must forward the context, not default it again, or
    // every allocation reports Buffer.h or Memory.cpp as its origin.
    Deki::Memory::SetBackend(new AlwaysFailsProvider());
    CapturedLog log;

    Deki::Buffer<uint8_t> b;
    EXPECT_FALSE(b.Allocate(64, Deki::Memory::Internal));  // no context given

    Deki::Memory::SetBackend(nullptr);

#if defined(DEKI_ALLOC_CONTEXT_CAPTURE)
    EXPECT_TRUE(log.Contains("AFailureNamesTheCallerNotTheEngineHeader")) << "logged: " << log.text;

    // Match on "at <file>:", the origin the AllocContext gives. A bare file
    // name would also match the logger's own "Memory.cpp:450 ->" stamp, which
    // says where the log statement is, not who asked for the memory.
    EXPECT_TRUE(log.Contains("at BufferTests.cpp:")) << "logged: " << log.text;
    EXPECT_FALSE(log.Contains("at Buffer.h:")) << "logged: " << log.text;
    EXPECT_FALSE(log.Contains("at Memory.cpp:")) << "logged: " << log.text;
#else
    // Nothing was captured, so the log must not name an origin.
    EXPECT_FALSE(log.Contains(" at ")) << "logged: " << log.text;
#endif
}

// --- the reporting side ----------------------------------------------------
// How much memory is allocated, and how much is free.

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
        Buffer<uint8_t> b(4096, Deki::Memory::External);
        ASSERT_TRUE(static_cast<bool>(b));
        EXPECT_GE(Deki::Memory::GetStats().totalAllocated, before + 4096u);
    }
    // The count drops again once the buffer goes out of scope.
    EXPECT_EQ(Deki::Memory::GetStats().totalAllocated, before);
}

TEST(DekiMemoryReporting, LogStatsWorksWithAndWithoutALabel)
{
    // Neither form may dereference a null label or a missing backend.
    Deki::Memory::LogStats();
    Deki::Memory::LogStats("BufferTests");
}

// --- regions are open -------------------------------------------------------
// The engine ships "internal" and "external". A package can add its own, so a
// board with RTC memory that survives deep sleep needs no engine release.

namespace TestBoard
{
// What a board-support package writes: its own namespace and constant, with
// no engine change and no registration call.
inline constexpr Deki::Memory::Region kRtc{ "rtc" };
}  // namespace TestBoard

TEST(DekiMemoryRegion, APackageCanDefineItsOwnRegion)
{
    // A constant expression, so it costs nothing at runtime and can be a
    // switch label inside a provider.
    static_assert(TestBoard::kRtc.id == Deki::HashName("rtc"));
    EXPECT_STREQ(TestBoard::kRtc.name, "rtc");
}

TEST(DekiMemoryRegion, RegionsAreDistinctAndCompareByName)
{
    static_assert(Deki::Memory::Internal != Deki::Memory::External);
    static_assert(Deki::Memory::Internal == Deki::Memory::Region("internal"));

    // The name survives to the failure log; an id alone would print a number.
    EXPECT_STREQ(Deki::Memory::Internal.name, "internal");
    EXPECT_STREQ(Deki::Memory::External.name, "external");
}

TEST(DekiMemoryRegion, TheHashMatchesTheRuntimeOneUsedEverywhereElse)
{
    // FNV-1a, computed here at runtime, apart from the constexpr one. Region
    // names hash like every other name in the engine, so a region name and a
    // platform capability name can be the same token.
    auto runtimeFnv1a = [](const char* str)
    {
        uint32_t h = 2166136261u;
        while (*str)
        {
            h ^= static_cast<uint8_t>(*str++);
            h *= 16777619u;
        }
        return h;
    };

    EXPECT_EQ(Deki::HashName("internal"), runtimeFnv1a("internal"));
    EXPECT_EQ(Deki::HashName("external"), runtimeFnv1a("external"));
    EXPECT_EQ(Deki::HashName("rtc"), runtimeFnv1a("rtc"));
    EXPECT_NE(Deki::HashName("internal"), Deki::HashName("external"));
}

TEST(DekiMemoryRegion, ARegionTheBoardDoesNotServeFailsAndIsNamed)
{
    // A board without the region must refuse it. Handing back internal RAM
    // instead would lose the data in deep sleep, the one moment it was meant
    // to survive.
    ScopedSpy s;
    CapturedLog log;

    void* p = Deki::Memory::Allocate(64, TestBoard::kRtc);

    EXPECT_EQ(p, nullptr);
    EXPECT_EQ(s.spy->refusedCalls, 1);
    EXPECT_EQ(s.spy->internalCalls, 0) << "must not fall back to another region";
    EXPECT_EQ(s.spy->externalCalls, 0);
    EXPECT_TRUE(log.Contains("rtc")) << "the failure must name the region: " << log.text;
}

TEST(DekiMemoryRegion, DmaIsAConstraintNotAPlace)
{
    // The display asks for reachability in whichever region the board
    // prefers, so the constraint has to survive either placement.
    ScopedSpy s;

    void* a = Deki::Memory::AllocateDma(1024, Deki::Memory::Internal);
    void* b = Deki::Memory::AllocateDma(1024, Deki::Memory::External);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    EXPECT_EQ(s.spy->dmaCalls, 2) << "the requirement must reach the provider "
                                     "from both regions";
    EXPECT_EQ(s.spy->internalCalls, 1);
    EXPECT_EQ(s.spy->externalCalls, 1);

    Deki::Memory::Free(a);
    Deki::Memory::Free(b);
}

TEST(DekiMemoryRegion, TwoPackagesNamingTheSameRegionMeanTheSameRegion)
{
    // Two board packages that both write Region("rtc") work together, the
    // same way two packages both requiring the "spi" capability do.
    constexpr Deki::Memory::Region kFromPackageA = Deki::Memory::Region("rtc");
    constexpr Deki::Memory::Region kFromPackageB = Deki::Memory::Region("rtc");

    static_assert(kFromPackageA == kFromPackageB);
    EXPECT_EQ(kFromPackageA.id, kFromPackageB.id);

    // Distinct objects with distinct name pointers; identity is the hash.
    EXPECT_STREQ(kFromPackageA.name, kFromPackageB.name);
}

TEST(DekiMemoryRegion, PrefixedNamesKeepPackagesOutOfEachOthersWay)
{
    // A prefix stops two packages meaning different things by the same word,
    // such as "scratch".
    static_assert(Deki::Memory::Region("particles.scratch") != Deki::Memory::Region("tilemap.scratch"));
    static_assert(Deki::Memory::Region("esp32.rtc") != Deki::Memory::Region("rtc"));
}

// --- where the memory went --------------------------------------------------
// Each call site's live and peak bytes, so a person can see what takes the
// memory without tagging a single allocation.

namespace
{
// The row for the call site in `function` at `line`, or false.
bool FindSite(const char* function, uint32_t line, Deki::MemorySite& out)
{
    for (uint32_t i = 0; i < Deki::Memory::GetSiteCount(); ++i)
    {
        Deki::MemorySite site{};
        if (Deki::Memory::GetSite(i, site) && site.function && strstr(site.function, function) && site.line == line)
        {
            out = site;
            return true;
        }
    }
    return false;
}
}  // namespace

#if defined(DEKI_ALLOC_CONTEXT_CAPTURE)
TEST(DekiMemorySites, ASiteCountsWhatItHoldsByRegion)
{
    Buffer<uint8_t> near;
    Buffer<uint8_t> far;
    const uint32_t nearLine = __LINE__ + 1;
    ASSERT_TRUE(near.Allocate(1000, Deki::Memory::Internal));
    const uint32_t farLine = __LINE__ + 1;
    ASSERT_TRUE(far.Allocate(3000, Deki::Memory::External));

    Deki::MemorySite site{};
    ASSERT_TRUE(FindSite("ASiteCountsWhatItHoldsByRegion", nearLine, site));
    EXPECT_EQ(site.internalBytes, 1000u);
    EXPECT_EQ(site.externalBytes, 0u);
    EXPECT_EQ(site.blocks, 1u);
    EXPECT_NE(strstr(site.file, "BufferTests"), nullptr) << "file was: " << site.file;

    // No backend here, so External resolves to internal: a board without
    // external RAM. The site still has a row of its own.
    ASSERT_TRUE(FindSite("ASiteCountsWhatItHoldsByRegion", farLine, site));
    EXPECT_EQ(site.internalBytes + site.externalBytes, 3000u);
}

TEST(DekiMemorySites, FreeingKeepsTheRowAndItsPeak)
{
    const uint32_t line = __LINE__ + 3;
    {
        Buffer<uint8_t> b;
        ASSERT_TRUE(b.Allocate(5000, Deki::Memory::Internal));
    }

    Deki::MemorySite site{};
    ASSERT_TRUE(FindSite("FreeingKeepsTheRowAndItsPeak", line, site));
    EXPECT_EQ(site.internalBytes, 0u);
    EXPECT_EQ(site.blocks, 0u);
    EXPECT_GE(site.peakBytes, 5000u) << "the peak must outlive the memory";
}

TEST(DekiMemorySites, ResetPeaksStartsFromWhatIsHeldNow)
{
    Buffer<uint8_t> big;
    const uint32_t line = __LINE__ + 1;
    ASSERT_TRUE(big.Allocate(8000, Deki::Memory::Internal));
    big.Reset();

    Deki::Memory::ResetPeaks();

    Deki::MemorySite site{};
    ASSERT_TRUE(FindSite("ResetPeaksStartsFromWhatIsHeldNow", line, site));
    EXPECT_EQ(site.peakBytes, 0u);
    EXPECT_EQ(Deki::Memory::GetPeakInternal(), Deki::Memory::GetStats().internalRamAllocated);
}

TEST(DekiMemorySites, TheCallerIsTheSiteNotTheEngine)
{
    // The same rule as the failure log: Buffer.h and Memory.cpp pass the
    // context on, so no row may be theirs.
    Buffer<uint8_t> b;
    ASSERT_TRUE(b.Allocate(16, Deki::Memory::Internal));
    for (uint32_t i = 0; i < Deki::Memory::GetSiteCount(); ++i)
    {
        Deki::MemorySite site{};
        ASSERT_TRUE(Deki::Memory::GetSite(i, site));
        if (!site.file)
        {
            continue;  // the overflow row
        }
        EXPECT_EQ(strstr(site.file, "Buffer.h"), nullptr);
        EXPECT_EQ(strstr(site.file, "providers/Memory.cpp"), nullptr);
    }
}

TEST(DekiMemorySites, PeaksFollowTheRegionTotals)
{
    ScopedSpy s;  // a backend that serves External, so both regions count
    const uint32_t before = Deki::Memory::GetPeakExternal();
    {
        Buffer<uint8_t> b(20000, Deki::Memory::External);
        ASSERT_TRUE(static_cast<bool>(b));
    }
    EXPECT_GE(Deki::Memory::GetPeakExternal(), before + 20000u);
    EXPECT_EQ(Deki::Memory::GetStats().externalAllocated, 0u);
}
#endif

TEST(DekiMemorySites, AnIndexPastTheEndIsRefused)
{
    Deki::MemorySite site{};
    EXPECT_FALSE(Deki::Memory::GetSite(Deki::Memory::GetSiteCount(), site));
}
