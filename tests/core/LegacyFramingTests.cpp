// Cameras frame the world themselves now. A project saved before that kept its
// framing in the Framebuffer section instead, and a scene's camera migrates
// from what it said when the scene loads (SceneView::GetLegacyFraming). These
// check that opening a project reads that section in both of its old shapes,
// and that nothing rewrites or removes it.

#include <gtest/gtest.h>

#include "core/Project.h"
#include <deki-editor/SceneView.h>
#include <deki-editor/build/ProjectPaths.h>
#include <deki/Engine.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using DekiEditor::Project;
using DekiEditor::SceneView;

namespace
{

class LegacyFramingTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Root = fs::temp_directory_path() / "deki_legacy_framing_test";
        std::error_code ec;
        fs::remove_all(m_Root, ec);
        fs::create_directories(m_Root / "assets");
        fs::create_directories(m_Root / "settings");
        Write("deki.json", { { "name", "legacy-framing" }, { "structure", DekiEditor::ProjectPaths::kStructureVersion } });
        Write("settings/core.json", { { "Units", { { "pixelsPerMeter", 16.0 } } } });
        m_SavedPpm = Deki::EngineSettings::Global().pixelsPerMeter;
        m_SavedLegacy = SceneView::Get().GetLegacyFraming();
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_Root, ec);
        Deki::EngineSettings::Global().pixelsPerMeter = m_SavedPpm;
        SceneView::Get().SetLegacyFraming(m_SavedLegacy);
    }

    void Write(const std::string& rel, const nlohmann::json& j)
    {
        std::ofstream(m_Root / rel) << j.dump(2);
    }

    nlohmann::json Read(const std::string& rel)
    {
        nlohmann::json j;
        std::ifstream(m_Root / rel) >> j;
        return j;
    }

    fs::path m_Root;
    float m_SavedPpm = 16.0f;
    SceneView::LegacyFraming m_SavedLegacy;
};

}  // namespace

TEST_F(LegacyFramingTest, A016ProjectsRenderSizeIsItsHeight)
{
    // 0.16: the render size in pixels.
    Write("settings/framebuffer.json", { { "Framebuffer", { { "targetWidth", 1280 }, { "targetHeight", 720 } } } });
    Project p;
    ASSERT_TRUE(p.Load(m_Root.string()));
    EXPECT_FLOAT_EQ(SceneView::Get().GetLegacyFraming().designHeight, 45.0f);  // 720 px at 16 px/m
    EXPECT_FALSE(SceneView::Get().GetLegacyFraming().pixelPerfect);
    EXPECT_FALSE(p.IsDirty());
}

TEST_F(LegacyFramingTest, A018ProjectsDesignAreaIsItsHeight)
{
    // 0.18 before cameras framed: the design area in meters, and Pixel Perfect.
    Write("settings/framebuffer.json",
          { { "Framebuffer", { { "designWidth", 20.0 }, { "designHeight", 15.0 }, { "pixelPerfect", true } } } });
    Project p;
    ASSERT_TRUE(p.Load(m_Root.string()));
    EXPECT_FLOAT_EQ(SceneView::Get().GetLegacyFraming().designHeight, 15.0f);
    EXPECT_TRUE(SceneView::Get().GetLegacyFraming().pixelPerfect);
}

TEST_F(LegacyFramingTest, AProjectWithoutOneHasTheOldDefaults)
{
    Project p;
    ASSERT_TRUE(p.Load(m_Root.string()));
    EXPECT_FLOAT_EQ(SceneView::Get().GetLegacyFraming().designHeight, 15.0f);
    EXPECT_FALSE(SceneView::Get().GetLegacyFraming().pixelPerfect);
}

TEST_F(LegacyFramingTest, SavingLeavesTheOldSectionForScenesNotYetLoaded)
{
    Write("settings/framebuffer.json", { { "Framebuffer", { { "targetWidth", 1280 }, { "targetHeight", 720 } } } });
    Project p;
    ASSERT_TRUE(p.Load(m_Root.string()));
    ASSERT_TRUE(p.Save());
    EXPECT_EQ(Read("settings/framebuffer.json")["Framebuffer"]["targetHeight"], 720);
    EXPECT_FALSE(fs::exists(m_Root / "settings" / "editor.json"));
}
