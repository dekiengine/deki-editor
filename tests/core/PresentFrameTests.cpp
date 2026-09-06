/**
 * @file PresentFrameTests.cpp
 * @brief Deki::Engine::PresentFrame routes the render system's changed
 *        rectangles to a display that can present partial frames, falls back
 *        to a whole-frame present otherwise, and the runtime package-setting
 *        reader finds values in a v5 dproject.bin without the settings registry.
 */

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <deki/Engine.h>
#include <deki/ProjectSettings.h>
#include <deki/Component.h>
#include <deki/reflection/Property.h>
#include <deki/providers/IDisplay.h>
#include <deki/providers/IRenderSystem.h>

namespace
{

class FakeDisplay : public Deki::IDisplay
{
public:
    bool partial = false;
    int fullCalls = 0;
    int regionCalls = 0;
    int refreshCalls = 0;
    int32_t lastCount = -2;
    std::vector<Deki::Rect> lastRects;

    bool Initialize(int32_t, int32_t) override { return true; }
    void Shutdown() override {}
    void Present(const uint8_t*, int, int, int) override { ++fullCalls; }
    bool SupportsPartialPresent() const override { return partial; }
    void PresentRegions(const uint8_t*, int, int, int, const Deki::Rect* rects, int32_t count) override
    {
        ++regionCalls;
        lastCount = count;
        lastRects.assign(rects, rects + (count > 0 ? count : 0));
    }
    void GetDisplaySize(int32_t* w, int32_t* h) const override { if (w) *w = 8; if (h) *h = 8; }
    bool IsInitialized() const override { return true; }
    void RequestFullRefresh() override { ++refreshCalls; }
    bool ProcessEvents() override { return true; }
    void* CreateUIOverlay(int32_t, int32_t) override { return nullptr; }
    bool UpdateUIOverlay(void*, int32_t, int32_t, int32_t, int32_t, const uint32_t*) override { return false; }
    bool UpdateUIOverlayRGB565A8(void*, int32_t, int32_t, int32_t, int32_t, const uint8_t*) override { return false; }
    void DestroyUIOverlay(void*) override {}
    void SetActiveUIOverlay(void*) override {}
    void ClearActiveUIOverlay() override {}
};

class FakeRenderSystem : public Deki::IRenderSystem
{
public:
    int32_t count = -1;
    std::vector<Deki::Rect> rects;
    int markAllDirtyCalls = 0;
    uint8_t buffer[8 * 8 * 2] = {};

    bool Setup(int32_t, int32_t, Deki::ColorFormat) override { return true; }
    void Render(Deki::Scene*) override {}
    const uint8_t* GetFrameBuffer() const override { return buffer; }
    int32_t GetScreenWidth() const override { return 8; }
    int32_t GetScreenHeight() const override { return 8; }
    Deki::ColorFormat GetColorFormat() const override { return Deki::ColorFormat::RGB565; }
    void SetRenderer(DekiRenderer*) override {}
    DekiRenderer* GetRenderer() const override { return nullptr; }
    void RenderToBuffer(Deki::Scene*, Deki::ICamera*, uint8_t*, int32_t, int32_t, Deki::ColorFormat) override {}
    void MarkAllDirty() override { ++markAllDirtyCalls; }
    const Deki::Rect* GetPresentRects(int32_t* out) const override
    {
        if (out) *out = count;
        return count > 0 ? rects.data() : nullptr;
    }
};

// Installs the fakes on the engine for one test and restores what was there.
struct EngineSwap
{
    Deki::IRenderSystem* prevRs;
    Deki::IDisplay* prevDisplay;
    EngineSwap(FakeRenderSystem& rs, FakeDisplay& d)
    {
        Deki::Engine& e = Deki::Engine::GetInstance();
        prevRs = e.GetRenderSystem();
        prevDisplay = e.GetDisplay();
        e.SetRenderSystem(&rs);
        e.SetDisplay(&d, "fake");
    }
    ~EngineSwap()
    {
        Deki::Engine& e = Deki::Engine::GetInstance();
        e.SetDisplay(prevDisplay, prevDisplay ? "restored" : "");
        e.SetRenderSystem(prevRs);
    }
};

}  // namespace

TEST(PresentFrame, ChangedRectsReachADisplayThatPresentsPartialFrames)
{
    FakeRenderSystem rs;
    FakeDisplay d;
    d.partial = true;
    rs.rects = { { 0, 0, 4, 4 }, { 4, 4, 8, 8 } };
    rs.count = 2;
    EngineSwap swap(rs, d);

    Deki::Engine::GetInstance().PresentFrame();
    EXPECT_EQ(d.regionCalls, 1);
    EXPECT_EQ(d.fullCalls, 0);
    EXPECT_EQ(d.lastCount, 2);
    ASSERT_EQ(d.lastRects.size(), 2u);
    EXPECT_EQ(d.lastRects[1].left, 4);
}

TEST(PresentFrame, UnknownFrameIsAWholeFramePresent)
{
    FakeRenderSystem rs;
    FakeDisplay d;
    d.partial = true;
    rs.count = -1;
    EngineSwap swap(rs, d);

    Deki::Engine::GetInstance().PresentFrame();
    EXPECT_EQ(d.fullCalls, 1);
    EXPECT_EQ(d.regionCalls, 0);
}

TEST(PresentFrame, DisplayWithoutPartialSupportGetsWholeFrames)
{
    FakeRenderSystem rs;
    FakeDisplay d;
    d.partial = false;
    rs.rects = { { 0, 0, 4, 4 } };
    rs.count = 1;
    EngineSwap swap(rs, d);

    Deki::Engine::GetInstance().PresentFrame();
    EXPECT_EQ(d.fullCalls, 1);
    EXPECT_EQ(d.regionCalls, 0);
}

TEST(PresentFrame, NothingChangedStillReachesThePartialDisplay)
{
    FakeRenderSystem rs;
    FakeDisplay d;
    d.partial = true;
    rs.count = 0;
    EngineSwap swap(rs, d);

    Deki::Engine::GetInstance().PresentFrame();
    EXPECT_EQ(d.regionCalls, 1);
    EXPECT_EQ(d.lastCount, 0);  // a windowed display redraws its window; an LCD does nothing
    EXPECT_EQ(d.fullCalls, 0);
}

TEST(PresentFrame, SettingADisplayMarksEverythingDirty)
{
    FakeRenderSystem rs;
    FakeDisplay d;
    EngineSwap swap(rs, d);  // SetDisplay inside
    EXPECT_GE(rs.markAllDirtyCalls, 1);
}

TEST(PresentFrame, RequestFullRefreshReachesBothSides)
{
    FakeRenderSystem rs;
    FakeDisplay d;
    EngineSwap swap(rs, d);
    const int before = rs.markAllDirtyCalls;
    Deki::Engine::GetInstance().RequestFullRefresh();
    EXPECT_EQ(rs.markAllDirtyCalls, before + 1);
    EXPECT_EQ(d.refreshCalls, 1);
}

// ---------------------------------------------------------------- runtime setting reader

namespace
{
void Put(std::vector<uint8_t>& out, const void* p, size_t n)
{
    const uint8_t* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
}

// A v5 dproject.bin with one package section, "Rendering", holding a Bool and
// an Int32, laid out the way Deki::ProjectSettings::Write emits them.
std::vector<uint8_t> BuildImage(bool tracking, int32_t align)
{
    std::vector<uint8_t> img;
    Deki::ProjectSettings::FileHeader hdr{ Deki::ProjectSettings::MAGIC, 5, 0 };
    Put(img, &hdr, sizeof(hdr));
    Deki::ProjectSettings::DisplaySettings ds{ 320, 240, 0, 0, 0, 0, 0.0f };
    Put(img, &ds, sizeof(ds));
    const int32_t fps = 60;
    Put(img, &fps, sizeof(fps));
    const uint16_t sceneLen = 0, rendererLen = 0;
    Put(img, &sceneLen, 2);
    Put(img, &rendererLen, 2);
    const uint8_t passCount = 0;
    Put(img, &passCount, 1);

    std::vector<uint8_t> payload;
    const uint16_t propCount = 2;
    Put(payload, &propCount, 2);
    uint32_t h = Deki::HashStringLen("dirtyTileTracking", 17);
    uint8_t tag = static_cast<uint8_t>(Deki::PropertyType::Bool);
    uint8_t bv = tracking ? 1 : 0;
    Put(payload, &h, 4); Put(payload, &tag, 1); Put(payload, &bv, 1);
    h = Deki::HashStringLen("dirtyTileSize", 13);
    tag = static_cast<uint8_t>(Deki::PropertyType::Int32);
    Put(payload, &h, 4); Put(payload, &tag, 1); Put(payload, &align, 4);

    const uint16_t sections = 1;
    Put(img, &sections, 2);
    const std::string name = "Rendering";
    const uint16_t nameLen = static_cast<uint16_t>(name.size());
    Put(img, &nameLen, 2);
    Put(img, name.data(), name.size());
    const uint32_t payloadSize = static_cast<uint32_t>(payload.size());
    Put(img, &payloadSize, 4);
    Put(img, payload.data(), payload.size());
    return img;
}
}  // namespace

TEST(ProjectSettingsRuntime, ReadPackageSettingFindsBoolAndInt32InAV5Image)
{
    const std::vector<uint8_t> img = BuildImage(true, 16);
    ASSERT_TRUE(Deki::ProjectSettings::Load(img.data(), img.size()));

    bool tracking = false;
    int32_t align = 0;
    float f = 0.0f;
    EXPECT_TRUE(Deki::ProjectSettings::ReadPackageSettingBool("Rendering", "dirtyTileTracking", tracking));
    EXPECT_TRUE(tracking);
    EXPECT_TRUE(Deki::ProjectSettings::ReadPackageSettingInt32("Rendering", "dirtyTileSize", align));
    EXPECT_EQ(align, 16);
    EXPECT_FALSE(Deki::ProjectSettings::ReadPackageSettingFloat("Rendering", "dirtyTileSize", f)) << "type mismatch";
    EXPECT_FALSE(Deki::ProjectSettings::ReadPackageSettingBool("Rendering", "nope", tracking));
    EXPECT_FALSE(Deki::ProjectSettings::ReadPackageSettingBool("Physics", "dirtyTileTracking", tracking));

    // A reload replaces the retained chunk.
    const std::vector<uint8_t> img2 = BuildImage(false, 8);
    ASSERT_TRUE(Deki::ProjectSettings::Load(img2.data(), img2.size()));
    EXPECT_TRUE(Deki::ProjectSettings::ReadPackageSettingBool("Rendering", "dirtyTileTracking", tracking));
    EXPECT_FALSE(tracking);
    EXPECT_TRUE(Deki::ProjectSettings::ReadPackageSettingInt32("Rendering", "dirtyTileSize", align));
    EXPECT_EQ(align, 8);
}
