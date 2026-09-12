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

#include <cstdint>
#include <utility>
#include <vector>

using Deki::Buffer;
using Deki::MemoryUse;

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
    Buffer<uint32_t> b(16, MemoryUse::Hot, "test");
    ASSERT_TRUE(static_cast<bool>(b));
    EXPECT_EQ(b.Count(), 16u);
    EXPECT_EQ(b.Bytes(), 16u * sizeof(uint32_t));
    EXPECT_NE(b.Data(), nullptr);
}

TEST(DekiBuffer, MemoryZeroesWhatItHandsBack)
{
    // Several call sites relied on `new T[n]{}` for this, so the replacement
    // has to keep it or those buffers start full of noise.
    Buffer<uint32_t> b(32, MemoryUse::Hot, "test");
    ASSERT_TRUE(static_cast<bool>(b));
    for (size_t i = 0; i < b.Count(); ++i)
        EXPECT_EQ(b[i], 0u) << "element " << i;
}

TEST(DekiBuffer, IndexingAndIterationSeeTheSameMemory)
{
    Buffer<int16_t> b(4, MemoryUse::Hot, "test");
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
    Buffer<uint8_t> a(8, MemoryUse::Hot, "test");
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
    Buffer<uint8_t> a(8, MemoryUse::Hot, "test");
    Buffer<uint8_t> b(4, MemoryUse::Hot, "test");
    ASSERT_TRUE(static_cast<bool>(a));
    ASSERT_TRUE(static_cast<bool>(b));

    b = std::move(a);  // b's original 4 bytes must be freed, not leaked
    EXPECT_EQ(b.Count(), 8u);
    EXPECT_FALSE(static_cast<bool>(a));
}

TEST(DekiBuffer, SelfMoveAssignmentIsNotADoubleFree)
{
    Buffer<uint8_t> a(8, MemoryUse::Hot, "test");
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
    Buffer<uint8_t> b(64, MemoryUse::Buffer, "test");
    ASSERT_TRUE(static_cast<bool>(b));
    const uint8_t* raw = b.Data();

    EXPECT_TRUE(b.Allocate(64, MemoryUse::Buffer, "test"));
    EXPECT_EQ(b.Data(), raw);
}

TEST(DekiBuffer, ReallocatingToADifferentCountResizes)
{
    Buffer<uint8_t> b(64, MemoryUse::Buffer, "test");
    ASSERT_TRUE(b.Allocate(128, MemoryUse::Buffer, "test"));
    EXPECT_EQ(b.Count(), 128u);
    EXPECT_EQ(b.Bytes(), 128u);
}

TEST(DekiBuffer, ZeroCountIsAnEmptySuccessNotAFailure)
{
    // A sparse font with no codepoints, a sprite with no spans: asking for
    // nothing is normal and must not read as out of memory.
    Buffer<uint8_t> b;
    EXPECT_TRUE(b.Allocate(0, MemoryUse::Hot, "test"));
    EXPECT_FALSE(static_cast<bool>(b));
    EXPECT_EQ(b.Count(), 0u);
}

TEST(DekiBuffer, AnImpossibleCountFailsRatherThanWrapping)
{
    // count * sizeof(T) is a 32-bit size_t on the device, so a count read from
    // a corrupt file could wrap to a small allocation that later overruns.
    Buffer<uint32_t> b;
    EXPECT_FALSE(b.Allocate(SIZE_MAX / 2, MemoryUse::Buffer, "test"));
    EXPECT_FALSE(static_cast<bool>(b));
    EXPECT_EQ(b.Count(), 0u);
}

TEST(DekiBuffer, ResetFreesImmediately)
{
    Buffer<uint8_t> b(16, MemoryUse::Hot, "test");
    ASSERT_TRUE(static_cast<bool>(b));
    b.Reset();
    EXPECT_FALSE(static_cast<bool>(b));
    EXPECT_EQ(b.Data(), nullptr);
    b.Reset();  // twice is not a double free
    EXPECT_FALSE(static_cast<bool>(b));
}

TEST(DekiBuffer, ReleaseHandsOwnershipOut)
{
    Buffer<uint8_t> b(16, MemoryUse::Hot, "test");
    ASSERT_TRUE(static_cast<bool>(b));
    uint8_t* raw = b.Release();
    ASSERT_NE(raw, nullptr);
    EXPECT_FALSE(static_cast<bool>(b));  // the buffer no longer frees it
    Deki::Memory::Free(raw);             // the caller does
}
