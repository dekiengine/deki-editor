// Screen Anchor: an anchored object sits at its point on the screen plus its
// offset, on every screen size and wherever the camera is.

#include <gtest/gtest.h>

#include <deki/Engine.h>
#include <deki/ICamera.h>
#include <deki/Object.h>
#include <deki/Scene.h>
#include <deki/ScreenAnchor.h>

#include <nlohmann/json.hpp>

namespace
{

// An orthographic camera at (x, y) showing `ppm` pixels per meter, centred on
// the screen, as CameraComponent maps it without Pixel Perfect.
class TestCamera : public Deki::ICamera
{
public:
    float x = 0.0f;
    float y = 0.0f;
    float ppm = 20.0f;

    void ScreenToWorld(float screenX, float screenY, int width, int height, float& worldX, float& worldY) const override
    {
        worldX = (screenX - width * 0.5f) / ppm + x;
        worldY = -(screenY - height * 0.5f) / ppm + y;
    }
    void WorldToScreen(float worldX, float worldY, int width, int height, float& screenX, float& screenY) const override
    {
        screenX = (worldX - x) * ppm + width * 0.5f;
        screenY = -(worldY - y) * ppm + height * 0.5f;
    }
    void GetClearColor(uint8_t& r, uint8_t& g, uint8_t& b) const override { r = g = b = 0; }
    void SetClearColor(uint8_t, uint8_t, uint8_t) override {}
    float GetPixelsPerMeter(int, int) const override { return ppm; }
    float GetOrthoHeight() const override { return 0.0f; }
    void SetOrthoHeight(float) override {}
    void SetFixedPixelsPerMeter(float) override {}
    Deki::ProjectionMode GetProjectionMode() const override { return Deki::ProjectionMode::Orthographic; }
    void SetProjectionMode(Deki::ProjectionMode) override {}
    float GetFieldOfView() const override { return 60.0f; }
    float GetNearPlane() const override { return 0.1f; }
    float GetFarPlane() const override { return 100.0f; }
    Deki::Mat4 GetProjectionMatrix(int, int) const override { return Deki::Mat4{}; }
};

Deki::Object* AddAnchored(Deki::Scene& scene, Deki::ScreenPoint point, float offsetX, float offsetY)
{
    auto* obj = new Deki::Object("Anchored");
    scene.AddObject(obj);
    auto* anchor = obj->AddComponent<Deki::ScreenAnchor>();
    anchor->anchor = point;
    anchor->offsetX = offsetX;
    anchor->offsetY = offsetY;
    return obj;
}

}  // namespace

// 320x240 at 20 px/m is 16 x 12 meters, so the corners are at (+-8, +-6).
TEST(ScreenAnchor, PlacesTheObjectAtItsPointPlusTheOffset)
{
    Deki::Scene scene;
    TestCamera camera;
    Deki::Object* topLeft = AddAnchored(scene, Deki::ScreenPoint::TopLeft, 1.0f, -0.5f);
    Deki::Object* bottomRight = AddAnchored(scene, Deki::ScreenPoint::BottomRight, -1.0f, 0.5f);
    Deki::Object* center = AddAnchored(scene, Deki::ScreenPoint::Center, 0.0f, 0.0f);
    Deki::Object* top = AddAnchored(scene, Deki::ScreenPoint::Top, 0.0f, 0.0f);

    Deki::ScreenAnchor::PlaceAll(&scene, camera, 320, 240);

    EXPECT_FLOAT_EQ(topLeft->GetWorldX(), -7.0f);
    EXPECT_FLOAT_EQ(topLeft->GetWorldY(), 5.5f);
    EXPECT_FLOAT_EQ(bottomRight->GetWorldX(), 7.0f);
    EXPECT_FLOAT_EQ(bottomRight->GetWorldY(), -5.5f);
    EXPECT_FLOAT_EQ(center->GetWorldX(), 0.0f);
    EXPECT_FLOAT_EQ(center->GetWorldY(), 0.0f);
    EXPECT_FLOAT_EQ(top->GetWorldX(), 0.0f);
    EXPECT_FLOAT_EQ(top->GetWorldY(), 6.0f);
}

// A wider screen of the same height moves the side anchors out and leaves the
// vertical ones alone.
TEST(ScreenAnchor, FollowsTheScreenWidth)
{
    Deki::Scene scene;
    TestCamera camera;
    Deki::Object* topRight = AddAnchored(scene, Deki::ScreenPoint::TopRight, -1.0f, -1.0f);

    Deki::ScreenAnchor::PlaceAll(&scene, camera, 320, 240);
    EXPECT_FLOAT_EQ(topRight->GetWorldX(), 7.0f);

    Deki::ScreenAnchor::PlaceAll(&scene, camera, 536, 240);
    EXPECT_FLOAT_EQ(topRight->GetWorldX(), 536.0f / 40.0f - 1.0f);
    EXPECT_FLOAT_EQ(topRight->GetWorldY(), 5.0f);
}

TEST(ScreenAnchor, StaysOnScreenWhenTheCameraMoves)
{
    Deki::Scene scene;
    TestCamera camera;
    Deki::Object* obj = AddAnchored(scene, Deki::ScreenPoint::BottomLeft, 1.0f, 1.0f);

    camera.x = 100.0f;
    camera.y = -20.0f;
    Deki::ScreenAnchor::PlaceAll(&scene, camera, 320, 240);

    float sx = 0.0f, sy = 0.0f;
    camera.WorldToScreen(obj->GetWorldX(), obj->GetWorldY(), 320, 240, sx, sy);
    EXPECT_FLOAT_EQ(sx, 20.0f);
    EXPECT_FLOAT_EQ(sy, 220.0f);
}

// The anchor places the object on the screen, not relative to its parent;
// its children follow it as usual.
TEST(ScreenAnchor, IgnoresTheParentPositionAndCarriesChildren)
{
    Deki::Scene scene;
    TestCamera camera;
    auto* parent = new Deki::Object("Parent");
    scene.AddObject(parent);
    parent->SetLocalPosition(3.0f, 4.0f);
    auto* anchored = new Deki::Object("Anchored");
    parent->AddChild(anchored);
    anchored->AddComponent<Deki::ScreenAnchor>()->anchor = Deki::ScreenPoint::TopLeft;
    auto* child = new Deki::Object("Child");
    anchored->AddChild(child);
    child->SetLocalPosition(0.5f, 0.0f);

    Deki::ScreenAnchor::PlaceAll(&scene, camera, 320, 240);

    EXPECT_FLOAT_EQ(anchored->GetWorldX(), -8.0f);
    EXPECT_FLOAT_EQ(anchored->GetWorldY(), 6.0f);
    EXPECT_FLOAT_EQ(child->GetWorldX(), -7.5f);
    EXPECT_FLOAT_EQ(child->GetWorldY(), 6.0f);
}

TEST(ScreenAnchor, LeavesOtherScenesAlone)
{
    Deki::Scene scene;
    Deki::Scene other;
    TestCamera camera;
    Deki::Object* elsewhere = AddAnchored(other, Deki::ScreenPoint::TopLeft, 0.0f, 0.0f);

    Deki::ScreenAnchor::PlaceAll(&scene, camera, 320, 240);

    EXPECT_FLOAT_EQ(elsewhere->GetWorldX(), 0.0f);
    EXPECT_FLOAT_EQ(elsewhere->GetWorldY(), 0.0f);
}

// The editor detaches a component to undo adding it; a detached anchor
// places nothing.
TEST(ScreenAnchor, ADetachedAnchorPlacesNothing)
{
    Deki::Scene scene;
    TestCamera camera;
    Deki::Object* obj = AddAnchored(scene, Deki::ScreenPoint::TopLeft, 0.0f, 0.0f);
    auto* anchor = obj->GetComponent<Deki::ScreenAnchor>();
    ASSERT_GE(Deki::Internal::DetachComponent(*obj, anchor), 0);

    Deki::ScreenAnchor::PlaceAll(&scene, camera, 320, 240);
    EXPECT_FLOAT_EQ(obj->GetWorldX(), 0.0f);

    delete anchor;
}

TEST(ScreenAnchor, ARemovedAnchorIsForgotten)
{
    Deki::Scene scene;
    TestCamera camera;
    Deki::Object* obj = AddAnchored(scene, Deki::ScreenPoint::TopLeft, 0.0f, 0.0f);
    obj->RemoveComponent<Deki::ScreenAnchor>();
    obj->SetLocalPosition(2.0f, 2.0f);

    Deki::ScreenAnchor::PlaceAll(&scene, camera, 320, 240);
    EXPECT_FLOAT_EQ(obj->GetWorldX(), 2.0f);
}

// The saved position does not depend on the screen the editor showed.
TEST(ScreenAnchor, TheFileKeepsZeroForAnAnchoredPosition)
{
    Deki::Scene scene;
    TestCamera camera;
    AddAnchored(scene, Deki::ScreenPoint::TopRight, -1.0f, -1.0f);
    Deki::ScreenAnchor::PlaceAll(&scene, camera, 536, 240);

    const nlohmann::json saved = nlohmann::json::parse(scene.SaveToJsonString());
    const nlohmann::json& transform = saved["objects"][0]["transform"];
    EXPECT_EQ(transform["x"].get<float>(), 0.0f);
    EXPECT_EQ(transform["y"].get<float>(), 0.0f);
}

// In the game, a Once anchor keeps the place it got on the first frame, while
// an Always one follows the new screen.
TEST(ScreenAnchor, OnceStaysPutInTheGame)
{
    Deki::Scene scene;
    TestCamera camera;
    Deki::Object* once = AddAnchored(scene, Deki::ScreenPoint::TopRight, -1.0f, -1.0f);
    once->GetComponent<Deki::ScreenAnchor>()->update = Deki::AnchorUpdate::Once;
    Deki::Object* always = AddAnchored(scene, Deki::ScreenPoint::TopRight, -1.0f, -1.0f);

    Deki::Engine::SetRuntimeMode(true);
    Deki::ScreenAnchor::PlaceAll(&scene, camera, 320, 240);
    camera.x = 5.0f;
    Deki::ScreenAnchor::PlaceAll(&scene, camera, 536, 240);
    Deki::Engine::SetRuntimeMode(false);

    EXPECT_FLOAT_EQ(once->GetWorldX(), 7.0f);
    EXPECT_FLOAT_EQ(once->GetWorldY(), 5.0f);
    EXPECT_FLOAT_EQ(always->GetWorldX(), 5.0f + 536.0f / 40.0f - 1.0f);
}

// Outside play the editor previews the picked screen, so a Once anchor
// follows it there.
TEST(ScreenAnchor, OnceFollowsTheScreenInTheEditor)
{
    Deki::Scene scene;
    TestCamera camera;
    Deki::Object* obj = AddAnchored(scene, Deki::ScreenPoint::TopRight, -1.0f, -1.0f);
    obj->GetComponent<Deki::ScreenAnchor>()->update = Deki::AnchorUpdate::Once;

    Deki::ScreenAnchor::PlaceAll(&scene, camera, 320, 240);
    Deki::ScreenAnchor::PlaceAll(&scene, camera, 536, 240);
    EXPECT_FLOAT_EQ(obj->GetWorldX(), 536.0f / 40.0f - 1.0f);
}
