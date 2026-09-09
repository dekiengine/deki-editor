// The third axis. The editor compiles DEKI_TRANSFORM_3D, so these run there;
// a build without it has no third axis to test.
//
// What matters most is the first suite: 2D content inside a 3D build composes
// through the same path as the 2D engine, so a scene that never rotates about
// X or Y is bit for bit what it was. The demo render hash is the same check
// at the pixel level.

#include <gtest/gtest.h>

#include <deki/Object.h>

#include <cmath>

#ifdef DEKI_TRANSFORM_3D

namespace
{
constexpr float kPi = 3.14159265358979f;

// Two objects, child under parent, both fresh.
struct Pair
{
    Deki::Object parent;
    Deki::Object child;
    Pair() { parent.AddChild(&child); }
    ~Pair() { parent.RemoveChild(&child); }
};
}  // namespace

TEST(ObjectTransform3D, FlatContentComposesExactlyAsTwoD)
{
    Pair p;
    p.parent.SetLocalPosition(1.5f, -2.25f);
    p.parent.SetRotation(0.7f);
    p.parent.SetScale(2.0f, 0.5f);
    p.child.SetLocalPosition(3.0f, 4.0f);
    p.child.SetRotation(0.1f);

    // The 2D formula, written out, must match bit for bit.
    const float sx = 3.0f * 2.0f, sy = 4.0f * 0.5f;
    const float c = std::cos(0.7f), s = std::sin(0.7f);
    const float ex = 1.5f + (sx * c - sy * s);
    const float ey = -2.25f + (sx * s + sy * c);

    EXPECT_EQ(p.child.GetWorldX(), ex);
    EXPECT_EQ(p.child.GetWorldY(), ey);
    EXPECT_EQ(p.child.GetWorldRotation(), 0.1f + 0.7f);
    EXPECT_EQ(p.child.GetWorldZ(), 0.0f);
    EXPECT_EQ(p.child.GetWorldRotationX(), 0.0f);
    EXPECT_EQ(p.child.GetWorldRotationY(), 0.0f);
    EXPECT_EQ(p.child.GetWorldScaleZ(), 1.0f);
}

TEST(ObjectTransform3D, DepthAddsUnderZOnlyRotation)
{
    Pair p;
    p.parent.SetZ(10.0f);
    p.parent.SetScaleZ(2.0f);
    p.parent.SetRotation(1.0f);  // about Z: leaves z alone
    p.child.SetZ(3.0f);

    EXPECT_FLOAT_EQ(p.child.GetWorldZ(), 10.0f + 3.0f * 2.0f);
    EXPECT_FLOAT_EQ(p.child.GetWorldScaleZ(), 2.0f);
}

TEST(ObjectTransform3D, ParentTurnedAboutYSwingsXIntoDepth)
{
    Pair p;
    p.parent.SetRotationY(kPi / 2.0f);
    p.child.SetLocalPosition(1.0f, 0.0f);

    // Ry(90): x -> -z
    EXPECT_NEAR(p.child.GetWorldX(), 0.0f, 1e-5f);
    EXPECT_NEAR(p.child.GetWorldY(), 0.0f, 1e-5f);
    EXPECT_NEAR(p.child.GetWorldZ(), -1.0f, 1e-5f);
}

TEST(ObjectTransform3D, ParentTurnedAboutXSwingsYIntoDepth)
{
    Pair p;
    p.parent.SetRotationX(kPi / 2.0f);
    p.child.SetLocalPosition(0.0f, 1.0f);

    // Rx(90): y -> z
    EXPECT_NEAR(p.child.GetWorldX(), 0.0f, 1e-5f);
    EXPECT_NEAR(p.child.GetWorldY(), 0.0f, 1e-5f);
    EXPECT_NEAR(p.child.GetWorldZ(), 1.0f, 1e-5f);
}

TEST(ObjectTransform3D, WorldAnglesRoundTripThroughTheMatrixPath)
{
    Pair p;
    p.parent.SetRotationX(0.3f);
    p.parent.SetRotationY(-0.4f);
    p.parent.SetRotation(0.5f);
    // A child with no rotation of its own inherits the parent's orientation
    // exactly, which exercises matrix -> angles -> matrix in one step.
    EXPECT_NEAR(p.child.GetWorldRotationX(), 0.3f, 1e-5f);
    EXPECT_NEAR(p.child.GetWorldRotationY(), -0.4f, 1e-5f);
    EXPECT_NEAR(p.child.GetWorldRotation(), 0.5f, 1e-5f);
}

TEST(ObjectTransform3D, SetWorldPositionUndoesATiltedParent)
{
    Pair p;
    p.parent.SetLocalPosition(2.0f, 3.0f);
    p.parent.SetRotationX(0.6f);
    p.parent.SetRotationY(0.2f);
    p.parent.SetScale(2.0f, 2.0f);

    p.child.SetWorldPosition(5.0f, -1.0f);
    EXPECT_NEAR(p.child.GetWorldX(), 5.0f, 1e-4f);
    EXPECT_NEAR(p.child.GetWorldY(), -1.0f, 1e-4f);
}

TEST(ObjectTransform3D, MovingTheParentInDepthInvalidatesTheChild)
{
    Pair p;
    p.child.SetZ(1.0f);
    EXPECT_FLOAT_EQ(p.child.GetWorldZ(), 1.0f);
    p.parent.SetZ(4.0f);
    EXPECT_FLOAT_EQ(p.child.GetWorldZ(), 5.0f);
}

#endif  // DEKI_TRANSFORM_3D
