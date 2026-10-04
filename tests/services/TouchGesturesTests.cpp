// TouchGestures: what fingers on a touchscreen mean to the editor. One finger
// is decided lazily (tap, drag or hold), two are a pan/pinch gesture with no
// button pressed, and a finger in a direct region (the running game) presses
// at once.

#include <gtest/gtest.h>

#include "services/TouchGestures.h"

#include <vector>

using namespace DekiEditor;
using Kind = TouchGestures::MouseEvent::Kind;

namespace
{
std::vector<Kind> Kinds(const TouchGestures::Output& out)
{
    std::vector<Kind> kinds;
    for (const auto& e : out.mouse)
    {
        kinds.push_back(e.kind);
    }
    return kinds;
}

// Button events only, as (button, down) pairs: the moves between them are
// where the pointer goes, the buttons are what the touch did.
std::vector<std::pair<int, bool>> Buttons(const TouchGestures::Output& out)
{
    std::vector<std::pair<int, bool>> b;
    for (const auto& e : out.mouse)
    {
        if (e.kind == Kind::Down || e.kind == Kind::Up)
        {
            b.push_back({ e.button, e.kind == Kind::Down });
        }
    }
    return b;
}
}  // namespace

TEST(TouchGestures, FingerDownPressesNothingYet)
{
    TouchGestures t;
    t.FingerDown(1, 100, 100, 0.0, false);
    auto out = t.Update(0.1);
    EXPECT_EQ(Kinds(out), std::vector<Kind>{ Kind::Move });
    EXPECT_TRUE(Buttons(out).empty());
    EXPECT_TRUE(t.OwnsPointer());
}

TEST(TouchGestures, TapClicksWhereTheFingerLanded)
{
    TouchGestures t;
    t.FingerDown(1, 100, 100, 0.0, false);
    t.FingerMove(1, 104, 103);  // inside the slop
    t.FingerUp(1, 105, 104);
    auto out = t.Update(0.1);

    ASSERT_EQ(Buttons(out), (std::vector<std::pair<int, bool>>{ { 0, true }, { 0, false } }));
    for (const auto& e : out.mouse)
    {
        if (e.kind == Kind::Down || e.kind == Kind::Up)
        {
            EXPECT_EQ(e.x, 100.0f);
            EXPECT_EQ(e.y, 100.0f);
        }
    }
    // Nothing hovers once the finger is gone.
    EXPECT_EQ(out.mouse.back().kind, Kind::Leave);
    EXPECT_FALSE(t.OwnsPointer());
}

TEST(TouchGestures, DragPressesAtTheOriginThenFollows)
{
    TouchGestures t;
    t.FingerDown(1, 100, 100, 0.0, false);
    t.FingerMove(1, 130, 100);  // past the slop
    auto out = t.Update(0.05);

    ASSERT_EQ(Buttons(out), (std::vector<std::pair<int, bool>>{ { 0, true } }));
    // Pressed at the landing point, so the drag grabs what was touched.
    for (size_t i = 0; i < out.mouse.size(); ++i)
    {
        if (out.mouse[i].kind == Kind::Down)
        {
            ASSERT_GT(i, 0u);
            EXPECT_EQ(out.mouse[i - 1].kind, Kind::Move);
            EXPECT_EQ(out.mouse[i - 1].x, 100.0f);
        }
    }
    EXPECT_EQ(out.mouse.back().kind, Kind::Move);
    EXPECT_EQ(out.mouse.back().x, 130.0f);

    t.FingerMove(1, 160, 110);
    t.FingerUp(1, 160, 110);
    out = t.Update(0.1);
    EXPECT_EQ(Buttons(out), (std::vector<std::pair<int, bool>>{ { 0, false } }));
    EXPECT_EQ(out.mouse.back().kind, Kind::Leave);
}

TEST(TouchGestures, HoldingStillRightClicks)
{
    TouchGestures t;
    t.FingerDown(1, 50, 60, 0.0, false);
    EXPECT_TRUE(Buttons(t.Update(0.3)).empty());

    auto out = t.Update(0.6);
    EXPECT_EQ(Buttons(out), (std::vector<std::pair<int, bool>>{ { 1, true }, { 1, false } }));

    // The hold has done its thing: moving or lifting afterwards clicks nothing.
    t.FingerMove(1, 200, 200);
    t.FingerUp(1, 200, 200);
    out = t.Update(0.9);
    EXPECT_TRUE(Buttons(out).empty());
    EXPECT_EQ(out.mouse.back().kind, Kind::Leave);
}

TEST(TouchGestures, TwoFingersPanAndPinchWithoutAButton)
{
    TouchGestures t;
    t.FingerDown(1, 100, 100, 0.0, false);
    t.FingerDown(2, 200, 100, 0.01, false);
    auto out = t.Update(0.02);
    EXPECT_TRUE(Buttons(out).empty());
    ASSERT_TRUE(out.gesture.active);
    EXPECT_FLOAT_EQ(out.gesture.centerX, 150.0f);
    EXPECT_FLOAT_EQ(out.gesture.panX, 0.0f);
    EXPECT_FLOAT_EQ(out.gesture.scale, 1.0f);

    // Both move down 20; they spread from 100 apart to 200 apart.
    t.FingerMove(1, 50, 120);
    t.FingerMove(2, 250, 120);
    out = t.Update(0.05);
    ASSERT_TRUE(out.gesture.active);
    EXPECT_FLOAT_EQ(out.gesture.panX, 0.0f);
    EXPECT_FLOAT_EQ(out.gesture.panY, 20.0f);
    EXPECT_FLOAT_EQ(out.gesture.scale, 2.0f);

    // Deltas are per update, not since the gesture began.
    out = t.Update(0.06);
    EXPECT_FLOAT_EQ(out.gesture.panY, 0.0f);
    EXPECT_FLOAT_EQ(out.gesture.scale, 1.0f);

    // One lifts: the gesture ends, and the finger left behind does not start
    // dragging whatever it rests on.
    t.FingerUp(2, 250, 120);
    t.FingerMove(1, 10, 300);
    out = t.Update(0.1);
    EXPECT_FALSE(out.gesture.active);
    EXPECT_TRUE(Buttons(out).empty());
    t.FingerUp(1, 10, 300);
    out = t.Update(0.2);
    EXPECT_TRUE(Buttons(out).empty());
    EXPECT_FALSE(t.OwnsPointer());
}

TEST(TouchGestures, SecondFingerDuringADragIsIgnored)
{
    TouchGestures t;
    t.FingerDown(1, 100, 100, 0.0, false);
    t.FingerMove(1, 150, 100);
    t.Update(0.02);
    t.FingerDown(2, 300, 300, 0.03, false);
    auto out = t.Update(0.04);
    EXPECT_FALSE(out.gesture.active);
    t.FingerUp(2, 300, 300);  // the extra finger lifting does not end the drag
    EXPECT_TRUE(Buttons(t.Update(0.05)).empty());
    t.FingerUp(1, 160, 100);
    EXPECT_EQ(Buttons(t.Update(0.06)), (std::vector<std::pair<int, bool>>{ { 0, false } }));
}

TEST(TouchGestures, DirectRegionPressesAtOnceAndNeverRightClicks)
{
    TouchGestures t;
    t.FingerDown(1, 10, 10, 0.0, true);
    auto out = t.Update(0.0);
    EXPECT_EQ(Buttons(out), (std::vector<std::pair<int, bool>>{ { 0, true } }));

    // Held still well past the long press: still the same left press.
    EXPECT_TRUE(Buttons(t.Update(2.0)).empty());
    t.FingerMove(1, 12, 11);
    out = t.Update(2.1);
    EXPECT_EQ(Kinds(out), std::vector<Kind>{ Kind::Move });

    t.FingerUp(1, 12, 11);
    EXPECT_EQ(Buttons(t.Update(2.2)), (std::vector<std::pair<int, bool>>{ { 0, false } }));
}

TEST(TouchGestures, CancelReleasesAHeldButton)
{
    TouchGestures t;
    t.FingerDown(1, 100, 100, 0.0, false);
    t.FingerMove(1, 150, 100);
    t.Update(0.02);
    t.CancelAll();
    auto out = t.Update(0.03);
    EXPECT_EQ(Buttons(out), (std::vector<std::pair<int, bool>>{ { 0, false } }));
    EXPECT_EQ(out.mouse.back().kind, Kind::Leave);
    EXPECT_FALSE(t.OwnsPointer());
}

TEST(TouchGestures, StrayEventsForUnknownFingersAreIgnored)
{
    TouchGestures t;
    t.FingerMove(7, 1, 1);
    t.FingerUp(7, 1, 1);
    auto out = t.Update(0.0);
    EXPECT_TRUE(out.mouse.empty());
    EXPECT_FALSE(t.OwnsPointer());
}
