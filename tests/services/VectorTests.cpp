// Deki::Vector<T>: a growable array whose growth can fail.
//
// std::vector cannot fail gracefully here: push_back calls operator new,
// which aborts with exceptions off, and the board reboots. Vector returns a
// bool instead, so most of these tests cover what happens when it is false,
// and the moved-from state.

#include <gtest/gtest.h>

#include <deki/providers/Vector.h>
#include <deki/providers/IMemoryProvider.h>

#include <cstdint>
#include <cstdlib>
#include <utility>

using Region = Deki::Memory::Region;
using Deki::Vector;

namespace
{

// Hands out a fixed number of allocations and then refuses, so a host test can
// reach the failure paths. A 64-bit desktop heap commits anything a size_t can
// express, so there is no other way in.
class FailAfterProvider : public Deki::IMemoryProvider
{
public:
    explicit FailAfterProvider(int allowed)
        : m_Allowed(allowed)
    {
    }

    bool Initialize() override { return true; }
    void Shutdown() override {}
    bool Serves(Deki::Memory::Region region) const override
    {
        return region == Deki::Memory::Internal || region == Deki::Memory::External;
    }

    void* Allocate(Deki::Memory::Region region, size_t bytes, bool needsDma) override
    {
        (void)needsDma;
        if (!Serves(region) || m_Allowed <= 0)
        {
            return nullptr;
        }
        --m_Allowed;
        return malloc(bytes);
    }

    void Free(Deki::Memory::Region, void* ptr) override { free(ptr); }

    // The budget counts allocations, not bytes, so there is no byte figure to
    // report; a made-up one would show up in the out-of-memory log line.
    size_t GetAvailable(Deki::Memory::Region) const override { return 0; }

private:
    int m_Allowed;
};

struct ScopedFailingAllocator
{
    explicit ScopedFailingAllocator(int allowed) { Deki::Memory::SetBackend(new FailAfterProvider(allowed)); }
    ~ScopedFailingAllocator() { Deki::Memory::SetBackend(nullptr); }
};

}  // namespace

TEST(DekiVector, AFreshVectorIsEmptyAndHoldsNothing)
{
    Vector<int> v(Deki::Memory::Internal);
    EXPECT_EQ(v.Size(), 0u);
    EXPECT_EQ(v.Capacity(), 0u);
    EXPECT_TRUE(v.Empty());
    EXPECT_EQ(v.Data(), nullptr);
    EXPECT_EQ(v.begin(), v.end());
}

TEST(DekiVector, PushBackGrowsAndKeepsOrder)
{
    Vector<int> v(Deki::Memory::Internal);
    for (int i = 0; i < 10; ++i)
    {
        ASSERT_TRUE(v.PushBack(i)) << i;
    }

    ASSERT_EQ(v.Size(), 10u);
    EXPECT_GE(v.Capacity(), 10u);
    for (int i = 0; i < 10; ++i)
    {
        EXPECT_EQ(v[i], i) << i;
    }
    EXPECT_EQ(v.Back(), 9);
}

TEST(DekiVector, EndFollowsSizeNotCapacity)
{
    // Otherwise a range-for walks uninitialised tail elements.
    Vector<int> v(Deki::Memory::Internal);
    ASSERT_TRUE(v.Reserve(32));
    ASSERT_TRUE(v.PushBack(7));
    ASSERT_TRUE(v.PushBack(8));

    EXPECT_EQ(v.Capacity(), 32u);
    EXPECT_EQ(static_cast<size_t>(v.end() - v.begin()), 2u);

    int sum = 0;
    for (int x : v)
    {
        sum += x;
    }
    EXPECT_EQ(sum, 15);
}

TEST(DekiVector, ReserveNeverShrinksAndIsIdempotent)
{
    Vector<int> v(Deki::Memory::Internal);
    ASSERT_TRUE(v.Reserve(16));
    const int* raw = v.Data();
    EXPECT_EQ(v.Capacity(), 16u);

    EXPECT_TRUE(v.Reserve(16));
    EXPECT_EQ(v.Data(), raw);  // no churn
    EXPECT_TRUE(v.Reserve(4));
    EXPECT_EQ(v.Capacity(), 16u);  // and no shrink
    EXPECT_EQ(v.Data(), raw);
}

TEST(DekiVector, ReserveUpFrontMakesTheFollowingPushesInfallible)
{
    // The pattern to use on a device: one check, not one per element.
    Vector<int> v(Deki::Memory::Internal);
    ASSERT_TRUE(v.Reserve(64));
    const int* raw = v.Data();

    for (int i = 0; i < 64; ++i)
    {
        ASSERT_TRUE(v.PushBack(i)) << i;
    }

    EXPECT_EQ(v.Data(), raw);  // never reallocated, so no push could fail
}

TEST(DekiVector, GrowthPreservesWhatWasAlreadyThere)
{
    Vector<uint32_t> v(Deki::Memory::Internal);
    for (uint32_t i = 0; i < 100; ++i)
    {
        ASSERT_TRUE(v.PushBack(i * 7u)) << i;
    }
    for (uint32_t i = 0; i < 100; ++i)
    {
        EXPECT_EQ(v[i], i * 7u) << i;
    }
}

TEST(DekiVector, PopBackShrinksAndReportsAnEmptyVector)
{
    Vector<int> v(Deki::Memory::Internal);
    ASSERT_TRUE(v.PushBack(1));
    ASSERT_TRUE(v.PushBack(2));

    EXPECT_TRUE(v.PopBack());
    EXPECT_EQ(v.Size(), 1u);
    EXPECT_EQ(v.Back(), 1);
    EXPECT_TRUE(v.PopBack());
    EXPECT_FALSE(v.PopBack());  // nothing left; not a crash
    EXPECT_TRUE(v.Empty());
}

TEST(DekiVector, RemoveUnorderedFillsTheHoleWithTheLast)
{
    Vector<int> v(Deki::Memory::Internal);
    for (int i = 0; i < 4; ++i)
    {
        ASSERT_TRUE(v.PushBack(i));
    }

    ASSERT_TRUE(v.RemoveUnordered(1));
    EXPECT_EQ(v.Size(), 3u);
    EXPECT_EQ(v[0], 0);
    EXPECT_EQ(v[1], 3);  // the last element moved down
    EXPECT_EQ(v[2], 2);
}

TEST(DekiVector, RemoveUnorderedOfTheLastJustShrinks)
{
    Vector<int> v(Deki::Memory::Internal);
    ASSERT_TRUE(v.PushBack(5));
    ASSERT_TRUE(v.PushBack(6));

    ASSERT_TRUE(v.RemoveUnordered(1));
    EXPECT_EQ(v.Size(), 1u);
    EXPECT_EQ(v[0], 5);
}

TEST(DekiVector, RemoveUnorderedRejectsAnIndexPastTheEnd)
{
    Vector<int> v(Deki::Memory::Internal);
    ASSERT_TRUE(v.Reserve(8));
    ASSERT_TRUE(v.PushBack(1));

    EXPECT_FALSE(v.RemoveUnordered(1));  // reserved but not live
    EXPECT_FALSE(v.RemoveUnordered(99));
    EXPECT_EQ(v.Size(), 1u);  // and neither removed anything
}

TEST(DekiVector, ClearKeepsTheMemoryAndResetReleasesIt)
{
    Vector<int> v(Deki::Memory::Internal);
    for (int i = 0; i < 8; ++i)
    {
        ASSERT_TRUE(v.PushBack(i));
    }
    const size_t capacity = v.Capacity();

    v.Clear();
    EXPECT_EQ(v.Size(), 0u);
    EXPECT_EQ(v.Capacity(), capacity);  // refilling every frame must not churn

    v.Reset();
    EXPECT_EQ(v.Capacity(), 0u);
    EXPECT_EQ(v.Data(), nullptr);
}

TEST(DekiVector, MoveTransfersAndLeavesTheSourceEmpty)
{
    // The move constructor is written out because a defaulted one would copy
    // m_Size across, leaving the source claiming elements over a Buffer that
    // no longer holds any, so Back() would dereference null.
    Vector<int> a(Deki::Memory::Internal);
    ASSERT_TRUE(a.PushBack(1));
    ASSERT_TRUE(a.PushBack(2));
    const int* raw = a.Data();

    Vector<int> b(std::move(a));
    EXPECT_EQ(a.Size(), 0u);
    EXPECT_EQ(a.Capacity(), 0u);
    EXPECT_TRUE(a.Empty());
    EXPECT_EQ(a.begin(), a.end());  // a range-for over it walks nothing

    ASSERT_EQ(b.Size(), 2u);
    EXPECT_EQ(b.Data(), raw);  // same memory, not a copy
    EXPECT_EQ(b.Back(), 2);
}

TEST(DekiVector, MoveAssignmentReleasesWhatItHeld)
{
    Vector<int> a(Deki::Memory::Internal);
    ASSERT_TRUE(a.PushBack(1));
    Vector<int> b(Deki::Memory::Internal);
    ASSERT_TRUE(b.PushBack(9));
    ASSERT_TRUE(b.PushBack(9));

    b = std::move(a);
    EXPECT_EQ(b.Size(), 1u);
    EXPECT_EQ(b[0], 1);
    EXPECT_EQ(a.Size(), 0u);
}

TEST(DekiVector, SelfMoveAssignmentIsNotADoubleFree)
{
    Vector<int> a(Deki::Memory::Internal);
    ASSERT_TRUE(a.PushBack(3));
    const int* raw = a.Data();

    a = std::move(a);
    ASSERT_EQ(a.Size(), 1u);
    EXPECT_EQ(a.Data(), raw);
    EXPECT_EQ(a[0], 3);
}

// --- what the bool is for --------------------------------------------------

TEST(DekiVector, ReserveReportsOutOfMemoryInsteadOfAborting)
{
    ScopedFailingAllocator oom(0);

    Vector<int> v(Deki::Memory::Internal);
    EXPECT_FALSE(v.Reserve(16));
    EXPECT_EQ(v.Capacity(), 0u);
    EXPECT_EQ(v.Size(), 0u);
}

TEST(DekiVector, PushBackReportsAFailedGrowth)
{
    ScopedFailingAllocator oom(0);

    Vector<int> v(Deki::Memory::Internal);
    EXPECT_FALSE(v.PushBack(1));  // the very first block cannot be had
    EXPECT_EQ(v.Size(), 0u);
}

TEST(DekiVector, AFailedGrowthLeavesEverythingAlreadyStoredIntact)
{
    // A caller that drops the new element still has a working vector, not a
    // half-grown one.
    Vector<int> v(Deki::Memory::Internal);
    ASSERT_TRUE(v.Reserve(4));
    for (int i = 0; i < 4; ++i)
    {
        ASSERT_TRUE(v.PushBack(i));
    }
    const int* raw = v.Data();

    {
        ScopedFailingAllocator oom(0);
        EXPECT_FALSE(v.PushBack(99));  // needs to double, cannot
    }

    EXPECT_EQ(v.Size(), 4u);
    EXPECT_EQ(v.Data(), raw);  // still the original block
    for (int i = 0; i < 4; ++i)
    {
        EXPECT_EQ(v[i], i) << i;
    }
}

TEST(DekiVector, AVectorKeepsWorkingAfterAFailedGrowth)
{
    Vector<int> v(Deki::Memory::Internal);
    ASSERT_TRUE(v.Reserve(2));
    ASSERT_TRUE(v.PushBack(1));
    ASSERT_TRUE(v.PushBack(2));

    {
        ScopedFailingAllocator oom(0);
        EXPECT_FALSE(v.PushBack(3));
    }

    // Memory came back; the vector was not left in a state that prevents it.
    EXPECT_TRUE(v.PushBack(3));
    EXPECT_EQ(v.Size(), 3u);
    EXPECT_EQ(v.Back(), 3);
}

TEST(DekiVector, AnImpossibleReserveFailsRatherThanWrapping)
{
    // count * sizeof(T) is a 32-bit size_t on the device, so a huge count
    // could wrap to a small allocation the vector then overruns. Buffer
    // checks it, and Vector must keep that check.
    Vector<uint32_t> v(Deki::Memory::Internal);
    EXPECT_FALSE(v.Reserve(SIZE_MAX / 2));
    EXPECT_EQ(v.Capacity(), 0u);
}

TEST(DekiVector, GrowthKeepsThePlacementReserveAsked)
{
    // Reserve records the Memory::Region, so a later growth inside PushBack
    // lands in the same region.
    Vector<int> v(Deki::Memory::Internal);
    ASSERT_TRUE(v.Reserve(1));
    ASSERT_TRUE(v.PushBack(1));
    ASSERT_TRUE(v.PushBack(2));  // grows, still Internal
    EXPECT_EQ(v.Size(), 2u);
    EXPECT_GE(v.Capacity(), 2u);
}
