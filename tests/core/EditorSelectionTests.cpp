#include <gtest/gtest.h>

#include <functional>
#include <vector>

#include "core/EditorSelection.h"
#include <deki/Object.h>
#include <deki/Scene.h>

using namespace DekiEditor;

class EditorSelectionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Scene = std::make_unique<Deki::Scene>();
        m_ObjA = m_Scene->CreateDekiObject("A");
        m_ObjB = m_Scene->CreateDekiObject("B");
        m_ObjC = m_Scene->CreateDekiObject("C");
    }

    EditorSelection m_Sel;
    std::unique_ptr<Deki::Scene> m_Scene;
    Deki::Object* m_ObjA = nullptr;
    Deki::Object* m_ObjB = nullptr;
    Deki::Object* m_ObjC = nullptr;
};

// ---- Basic object selection ----

TEST_F(EditorSelectionTest, InitialStateIsEmpty)
{
    EXPECT_EQ(m_Sel.GetSelectedObject(), nullptr);
    EXPECT_TRUE(m_Sel.GetSelectedObjects().empty());
    EXPECT_EQ(m_Sel.GetSelectionCount(), 0u);
    EXPECT_FALSE(m_Sel.HasMultipleSelected());
    EXPECT_FALSE(m_Sel.HasAssetSelection());
}

TEST_F(EditorSelectionTest, SetSelectedObject)
{
    m_Sel.SetSelectedObject(m_ObjA);
    EXPECT_EQ(m_Sel.GetSelectedObject(), m_ObjA);
    EXPECT_EQ(m_Sel.GetSelectionCount(), 1u);
    EXPECT_TRUE(m_Sel.IsSelected(m_ObjA));
    EXPECT_FALSE(m_Sel.IsSelected(m_ObjB));
}

TEST_F(EditorSelectionTest, SetSelectedObjectNull)
{
    m_Sel.SetSelectedObject(m_ObjA);
    m_Sel.SetSelectedObject(nullptr);
    EXPECT_EQ(m_Sel.GetSelectedObject(), nullptr);
    EXPECT_EQ(m_Sel.GetSelectionCount(), 0u);
}

TEST_F(EditorSelectionTest, SetSelectedObjects)
{
    m_Sel.SetSelectedObjects({ m_ObjA, m_ObjB });
    EXPECT_EQ(m_Sel.GetSelectionCount(), 2u);
    EXPECT_TRUE(m_Sel.HasMultipleSelected());
    EXPECT_TRUE(m_Sel.IsSelected(m_ObjA));
    EXPECT_TRUE(m_Sel.IsSelected(m_ObjB));
    EXPECT_FALSE(m_Sel.IsSelected(m_ObjC));
}

TEST_F(EditorSelectionTest, AddToSelection)
{
    m_Sel.SetSelectedObject(m_ObjA);
    m_Sel.AddToSelection(m_ObjB);
    EXPECT_EQ(m_Sel.GetSelectionCount(), 2u);
    EXPECT_TRUE(m_Sel.IsSelected(m_ObjA));
    EXPECT_TRUE(m_Sel.IsSelected(m_ObjB));
}

TEST_F(EditorSelectionTest, AddToSelectionIgnoresDuplicates)
{
    m_Sel.SetSelectedObject(m_ObjA);
    m_Sel.AddToSelection(m_ObjA);
    EXPECT_EQ(m_Sel.GetSelectionCount(), 1u);
}

TEST_F(EditorSelectionTest, RemoveFromSelection)
{
    m_Sel.SetSelectedObjects({ m_ObjA, m_ObjB });
    m_Sel.RemoveFromSelection(m_ObjA);
    EXPECT_EQ(m_Sel.GetSelectionCount(), 1u);
    EXPECT_FALSE(m_Sel.IsSelected(m_ObjA));
    EXPECT_TRUE(m_Sel.IsSelected(m_ObjB));
}

TEST_F(EditorSelectionTest, ToggleSelectionAdds)
{
    m_Sel.ToggleSelection(m_ObjA);
    EXPECT_TRUE(m_Sel.IsSelected(m_ObjA));
}

TEST_F(EditorSelectionTest, ToggleSelectionRemoves)
{
    m_Sel.SetSelectedObject(m_ObjA);
    m_Sel.ToggleSelection(m_ObjA);
    EXPECT_FALSE(m_Sel.IsSelected(m_ObjA));
}

TEST_F(EditorSelectionTest, ClearObjectSelection)
{
    m_Sel.SetSelectedObjects({ m_ObjA, m_ObjB });
    m_Sel.ClearObjectSelection();
    EXPECT_EQ(m_Sel.GetSelectionCount(), 0u);
}

TEST_F(EditorSelectionTest, NullObjectIsNeverSelected)
{
    EXPECT_FALSE(m_Sel.IsSelected(nullptr));
}

TEST_F(EditorSelectionTest, AddNullIsIgnored)
{
    m_Sel.AddToSelection(nullptr);
    EXPECT_EQ(m_Sel.GetSelectionCount(), 0u);
}

TEST_F(EditorSelectionTest, ToggleNullIsIgnored)
{
    m_Sel.ToggleSelection(nullptr);
    EXPECT_EQ(m_Sel.GetSelectionCount(), 0u);
}

// ---- Asset selection ----

TEST_F(EditorSelectionTest, SetSelectedAsset)
{
    m_Sel.SetSelectedAsset("assets/sprite.png", "cache/sprite.dtex", true, "guid-123");
    EXPECT_EQ(m_Sel.GetSelectedAssetPath(), "assets/sprite.png");
    EXPECT_EQ(m_Sel.GetSelectedAssetCachePath(), "cache/sprite.dtex");
    EXPECT_TRUE(m_Sel.GetSelectedAssetHasCached());
    EXPECT_EQ(m_Sel.GetSelectedAssetGuid(), "guid-123");
    EXPECT_TRUE(m_Sel.HasAssetSelection());
}

TEST_F(EditorSelectionTest, BakedCacheOnlyAssetDetected)
{
    // Baked cache-only: empty path but has cached data
    m_Sel.SetSelectedAsset("", "cache/font_atlas.dtex", true, "baked-guid");
    EXPECT_TRUE(m_Sel.HasAssetSelection());
    EXPECT_TRUE(m_Sel.GetSelectedAssetPath().empty());
}

// ---- Mutual exclusion: the core invariant ----

TEST_F(EditorSelectionTest, SelectingAssetClearsObjectSelection)
{
    m_Sel.SetSelectedObject(m_ObjA);
    m_Sel.SetSelectedAsset("assets/tex.png", "", false);
    EXPECT_EQ(m_Sel.GetSelectionCount(), 0u);
    EXPECT_TRUE(m_Sel.HasAssetSelection());
}

TEST_F(EditorSelectionTest, SetSelectedObjectClearsAsset)
{
    m_Sel.SetSelectedAsset("assets/tex.png", "cache/tex.dtex", true, "g1");
    m_Sel.SetSelectedObject(m_ObjA);
    EXPECT_TRUE(m_Sel.GetSelectedAssetPath().empty());
    EXPECT_TRUE(m_Sel.GetSelectedAssetCachePath().empty());
    EXPECT_FALSE(m_Sel.GetSelectedAssetHasCached());
    EXPECT_TRUE(m_Sel.GetSelectedAssetGuid().empty());
    EXPECT_FALSE(m_Sel.HasAssetSelection());
}

TEST_F(EditorSelectionTest, SetSelectedObjectsClearsAsset)
{
    m_Sel.SetSelectedAsset("assets/tex.png", "cache/tex.dtex", true, "g1");
    m_Sel.SetSelectedObjects({ m_ObjA });
    EXPECT_FALSE(m_Sel.HasAssetSelection());
    EXPECT_TRUE(m_Sel.GetSelectedAssetCachePath().empty());
    EXPECT_FALSE(m_Sel.GetSelectedAssetHasCached());
}

TEST_F(EditorSelectionTest, AddToSelectionClearsAsset)
{
    m_Sel.SetSelectedAsset("assets/tex.png", "cache/tex.dtex", true, "g1");
    m_Sel.AddToSelection(m_ObjA);
    EXPECT_FALSE(m_Sel.HasAssetSelection());
    EXPECT_TRUE(m_Sel.GetSelectedAssetCachePath().empty());
    EXPECT_FALSE(m_Sel.GetSelectedAssetHasCached());
}

TEST_F(EditorSelectionTest, ToggleSelectionClearsAsset)
{
    m_Sel.SetSelectedAsset("assets/tex.png", "cache/tex.dtex", true, "g1");
    m_Sel.ToggleSelection(m_ObjA);
    EXPECT_FALSE(m_Sel.HasAssetSelection());
    EXPECT_TRUE(m_Sel.GetSelectedAssetCachePath().empty());
    EXPECT_FALSE(m_Sel.GetSelectedAssetHasCached());
}

// ---- Regression: baked cache-only asset must be fully cleared ----

TEST_F(EditorSelectionTest, SetSelectedObjectClearsBakedCacheOnlyAsset)
{
    // 1. Select a baked-cache-only asset (empty path, hasCached=true, non-empty cachePath)
    m_Sel.SetSelectedAsset("", "cache/font_atlas.dtex", true, "baked-guid");
    EXPECT_TRUE(m_Sel.HasAssetSelection());

    // 2. Click on an object in the hierarchy
    m_Sel.SetSelectedObject(m_ObjA);

    // 3. ALL asset fields must be cleared - not just the path
    EXPECT_FALSE(m_Sel.HasAssetSelection());
    EXPECT_TRUE(m_Sel.GetSelectedAssetPath().empty());
    EXPECT_TRUE(m_Sel.GetSelectedAssetCachePath().empty());
    EXPECT_FALSE(m_Sel.GetSelectedAssetHasCached());
    EXPECT_TRUE(m_Sel.GetSelectedAssetGuid().empty());
    EXPECT_EQ(m_Sel.GetSelectedObject(), m_ObjA);
}

TEST_F(EditorSelectionTest, SetSelectedObjectsClearsBakedCacheOnlyAsset)
{
    m_Sel.SetSelectedAsset("", "cache/font.dtex", true, "baked-guid");
    m_Sel.SetSelectedObjects({ m_ObjA, m_ObjB });
    EXPECT_FALSE(m_Sel.HasAssetSelection());
    EXPECT_FALSE(m_Sel.GetSelectedAssetHasCached());
    EXPECT_TRUE(m_Sel.GetSelectedAssetCachePath().empty());
}

TEST_F(EditorSelectionTest, AddToSelectionClearsBakedCacheOnlyAsset)
{
    m_Sel.SetSelectedAsset("", "cache/font.dtex", true, "baked-guid");
    m_Sel.AddToSelection(m_ObjA);
    EXPECT_FALSE(m_Sel.HasAssetSelection());
    EXPECT_FALSE(m_Sel.GetSelectedAssetHasCached());
    EXPECT_TRUE(m_Sel.GetSelectedAssetCachePath().empty());
}

TEST_F(EditorSelectionTest, ToggleSelectionClearsBakedCacheOnlyAsset)
{
    m_Sel.SetSelectedAsset("", "cache/font.dtex", true, "baked-guid");
    m_Sel.ToggleSelection(m_ObjA);
    EXPECT_FALSE(m_Sel.HasAssetSelection());
    EXPECT_FALSE(m_Sel.GetSelectedAssetHasCached());
    EXPECT_TRUE(m_Sel.GetSelectedAssetCachePath().empty());
}

// ---- Change notification ----
//
// EditorApp mirrors selection into the EditorApplication facade (the one package
// CustomEditors read) through this callback. Every mutator must notify,
// Add/Remove/ToggleSelection included: otherwise a Ctrl+click in the Hierarchy
// leaves GetActiveObject() naming the previously selected object, which
// dangles once that object is deleted.

namespace
{
struct ChangeRecorder
{
    int calls = 0;
    Deki::Object* last = nullptr;

    void Attach(EditorSelection& s)
    {
        s.SetOnChanged(
            [this](Deki::Object* active)
            {
                ++calls;
                last = active;
            });
    }
};
}  // namespace

TEST_F(EditorSelectionTest, EveryObjectMutatorNotifies)
{
    ChangeRecorder rec;
    rec.Attach(m_Sel);

    struct Step
    {
        const char* name;
        std::function<void()> apply;
        Deki::Object* expected;
    };

    const std::vector<Step> steps = {
        { "SetSelectedObject", [&] { m_Sel.SetSelectedObject(m_ObjA); }, m_ObjA },
        { "AddToSelection", [&] { m_Sel.AddToSelection(m_ObjB); }, m_ObjA },
        { "RemoveFromSelection", [&] { m_Sel.RemoveFromSelection(m_ObjA); }, m_ObjB },
        { "ToggleSelection (on)", [&] { m_Sel.ToggleSelection(m_ObjC); }, m_ObjB },
        { "ToggleSelection (off)", [&] { m_Sel.ToggleSelection(m_ObjB); }, m_ObjC },
        { "SetSelectedObjects", [&] { m_Sel.SetSelectedObjects({ m_ObjA, m_ObjB }); }, m_ObjA },
        { "RemoveObjects", [&] { m_Sel.RemoveObjects({ m_ObjA }); }, m_ObjB },
        { "SetSelectedAsset", [&] { m_Sel.SetSelectedAsset("p.png", "c.dtex", true); }, nullptr },
        { "ClearObjectSelection", [&] { m_Sel.ClearObjectSelection(); }, nullptr },
    };

    for (const Step& step : steps)
    {
        const int before = rec.calls;
        step.apply();
        EXPECT_GT(rec.calls, before) << step.name << " did not notify";
        EXPECT_EQ(rec.last, step.expected) << step.name << " reported the wrong active object";
        EXPECT_EQ(rec.last, m_Sel.GetSelectedObject())
            << step.name << " reported an active object the selection disagrees with";
    }
}

TEST_F(EditorSelectionTest, NoOpMutationsDoNotReportAStaleObject)
{
    m_Sel.SetSelectedObject(m_ObjA);

    ChangeRecorder rec;
    rec.Attach(m_Sel);

    // Removing something that was never selected changes nothing; if it did
    // notify, it would still have to report the correct active object.
    m_Sel.RemoveFromSelection(m_ObjC);
    EXPECT_EQ(m_Sel.GetSelectedObject(), m_ObjA);
    if (rec.calls > 0)
    {
        EXPECT_EQ(rec.last, m_ObjA);
    }
}
