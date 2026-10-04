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

    EditorSelection sel;
    std::unique_ptr<Deki::Scene> m_Scene;
    Deki::Object* m_ObjA = nullptr;
    Deki::Object* m_ObjB = nullptr;
    Deki::Object* m_ObjC = nullptr;
};

// ---- Basic object selection ----

TEST_F(EditorSelectionTest, InitialStateIsEmpty)
{
    EXPECT_EQ(sel.GetSelectedObject(), nullptr);
    EXPECT_TRUE(sel.GetSelectedObjects().empty());
    EXPECT_EQ(sel.GetSelectionCount(), 0u);
    EXPECT_FALSE(sel.HasMultipleSelected());
    EXPECT_FALSE(sel.HasAssetSelection());
}

TEST_F(EditorSelectionTest, SetSelectedObject)
{
    sel.SetSelectedObject(m_ObjA);
    EXPECT_EQ(sel.GetSelectedObject(), m_ObjA);
    EXPECT_EQ(sel.GetSelectionCount(), 1u);
    EXPECT_TRUE(sel.IsSelected(m_ObjA));
    EXPECT_FALSE(sel.IsSelected(m_ObjB));
}

TEST_F(EditorSelectionTest, SetSelectedObjectNull)
{
    sel.SetSelectedObject(m_ObjA);
    sel.SetSelectedObject(nullptr);
    EXPECT_EQ(sel.GetSelectedObject(), nullptr);
    EXPECT_EQ(sel.GetSelectionCount(), 0u);
}

TEST_F(EditorSelectionTest, SetSelectedObjects)
{
    sel.SetSelectedObjects({ m_ObjA, m_ObjB });
    EXPECT_EQ(sel.GetSelectionCount(), 2u);
    EXPECT_TRUE(sel.HasMultipleSelected());
    EXPECT_TRUE(sel.IsSelected(m_ObjA));
    EXPECT_TRUE(sel.IsSelected(m_ObjB));
    EXPECT_FALSE(sel.IsSelected(m_ObjC));
}

TEST_F(EditorSelectionTest, AddToSelection)
{
    sel.SetSelectedObject(m_ObjA);
    sel.AddToSelection(m_ObjB);
    EXPECT_EQ(sel.GetSelectionCount(), 2u);
    EXPECT_TRUE(sel.IsSelected(m_ObjA));
    EXPECT_TRUE(sel.IsSelected(m_ObjB));
}

TEST_F(EditorSelectionTest, AddToSelectionIgnoresDuplicates)
{
    sel.SetSelectedObject(m_ObjA);
    sel.AddToSelection(m_ObjA);
    EXPECT_EQ(sel.GetSelectionCount(), 1u);
}

TEST_F(EditorSelectionTest, RemoveFromSelection)
{
    sel.SetSelectedObjects({ m_ObjA, m_ObjB });
    sel.RemoveFromSelection(m_ObjA);
    EXPECT_EQ(sel.GetSelectionCount(), 1u);
    EXPECT_FALSE(sel.IsSelected(m_ObjA));
    EXPECT_TRUE(sel.IsSelected(m_ObjB));
}

TEST_F(EditorSelectionTest, ToggleSelectionAdds)
{
    sel.ToggleSelection(m_ObjA);
    EXPECT_TRUE(sel.IsSelected(m_ObjA));
}

TEST_F(EditorSelectionTest, ToggleSelectionRemoves)
{
    sel.SetSelectedObject(m_ObjA);
    sel.ToggleSelection(m_ObjA);
    EXPECT_FALSE(sel.IsSelected(m_ObjA));
}

TEST_F(EditorSelectionTest, ClearObjectSelection)
{
    sel.SetSelectedObjects({ m_ObjA, m_ObjB });
    sel.ClearObjectSelection();
    EXPECT_EQ(sel.GetSelectionCount(), 0u);
}

TEST_F(EditorSelectionTest, NullObjectIsNeverSelected)
{
    EXPECT_FALSE(sel.IsSelected(nullptr));
}

TEST_F(EditorSelectionTest, AddNullIsIgnored)
{
    sel.AddToSelection(nullptr);
    EXPECT_EQ(sel.GetSelectionCount(), 0u);
}

TEST_F(EditorSelectionTest, ToggleNullIsIgnored)
{
    sel.ToggleSelection(nullptr);
    EXPECT_EQ(sel.GetSelectionCount(), 0u);
}

// ---- Asset selection ----

TEST_F(EditorSelectionTest, SetSelectedAsset)
{
    sel.SetSelectedAsset("assets/sprite.png", "cache/sprite.dtex", true, "guid-123");
    EXPECT_EQ(sel.GetSelectedAssetPath(), "assets/sprite.png");
    EXPECT_EQ(sel.GetSelectedAssetCachePath(), "cache/sprite.dtex");
    EXPECT_TRUE(sel.GetSelectedAssetHasCached());
    EXPECT_EQ(sel.GetSelectedAssetGuid(), "guid-123");
    EXPECT_TRUE(sel.HasAssetSelection());
}

TEST_F(EditorSelectionTest, BakedCacheOnlyAssetDetected)
{
    // Baked cache-only: empty path but has cached data
    sel.SetSelectedAsset("", "cache/font_atlas.dtex", true, "baked-guid");
    EXPECT_TRUE(sel.HasAssetSelection());
    EXPECT_TRUE(sel.GetSelectedAssetPath().empty());
}

// ---- Mutual exclusion: the core invariant ----

TEST_F(EditorSelectionTest, SelectingAssetClearsObjectSelection)
{
    sel.SetSelectedObject(m_ObjA);
    sel.SetSelectedAsset("assets/tex.png", "", false);
    EXPECT_EQ(sel.GetSelectionCount(), 0u);
    EXPECT_TRUE(sel.HasAssetSelection());
}

TEST_F(EditorSelectionTest, SetSelectedObjectClearsAsset)
{
    sel.SetSelectedAsset("assets/tex.png", "cache/tex.dtex", true, "g1");
    sel.SetSelectedObject(m_ObjA);
    EXPECT_TRUE(sel.GetSelectedAssetPath().empty());
    EXPECT_TRUE(sel.GetSelectedAssetCachePath().empty());
    EXPECT_FALSE(sel.GetSelectedAssetHasCached());
    EXPECT_TRUE(sel.GetSelectedAssetGuid().empty());
    EXPECT_FALSE(sel.HasAssetSelection());
}

TEST_F(EditorSelectionTest, SetSelectedObjectsClearsAsset)
{
    sel.SetSelectedAsset("assets/tex.png", "cache/tex.dtex", true, "g1");
    sel.SetSelectedObjects({ m_ObjA });
    EXPECT_FALSE(sel.HasAssetSelection());
    EXPECT_TRUE(sel.GetSelectedAssetCachePath().empty());
    EXPECT_FALSE(sel.GetSelectedAssetHasCached());
}

TEST_F(EditorSelectionTest, AddToSelectionClearsAsset)
{
    sel.SetSelectedAsset("assets/tex.png", "cache/tex.dtex", true, "g1");
    sel.AddToSelection(m_ObjA);
    EXPECT_FALSE(sel.HasAssetSelection());
    EXPECT_TRUE(sel.GetSelectedAssetCachePath().empty());
    EXPECT_FALSE(sel.GetSelectedAssetHasCached());
}

TEST_F(EditorSelectionTest, ToggleSelectionClearsAsset)
{
    sel.SetSelectedAsset("assets/tex.png", "cache/tex.dtex", true, "g1");
    sel.ToggleSelection(m_ObjA);
    EXPECT_FALSE(sel.HasAssetSelection());
    EXPECT_TRUE(sel.GetSelectedAssetCachePath().empty());
    EXPECT_FALSE(sel.GetSelectedAssetHasCached());
}

// ---- Regression: baked cache-only asset must be fully cleared ----

TEST_F(EditorSelectionTest, SetSelectedObjectClearsBakedCacheOnlyAsset)
{
    // This is the exact scenario from the bug:
    // 1. Select a baked-cache-only asset (empty path, hasCached=true, non-empty cachePath)
    sel.SetSelectedAsset("", "cache/font_atlas.dtex", true, "baked-guid");
    EXPECT_TRUE(sel.HasAssetSelection());

    // 2. Click on an object in the hierarchy
    sel.SetSelectedObject(m_ObjA);

    // 3. ALL asset fields must be cleared - not just the path
    EXPECT_FALSE(sel.HasAssetSelection());
    EXPECT_TRUE(sel.GetSelectedAssetPath().empty());
    EXPECT_TRUE(sel.GetSelectedAssetCachePath().empty());
    EXPECT_FALSE(sel.GetSelectedAssetHasCached());
    EXPECT_TRUE(sel.GetSelectedAssetGuid().empty());
    EXPECT_EQ(sel.GetSelectedObject(), m_ObjA);
}

TEST_F(EditorSelectionTest, SetSelectedObjectsClearsBakedCacheOnlyAsset)
{
    sel.SetSelectedAsset("", "cache/font.dtex", true, "baked-guid");
    sel.SetSelectedObjects({ m_ObjA, m_ObjB });
    EXPECT_FALSE(sel.HasAssetSelection());
    EXPECT_FALSE(sel.GetSelectedAssetHasCached());
    EXPECT_TRUE(sel.GetSelectedAssetCachePath().empty());
}

TEST_F(EditorSelectionTest, AddToSelectionClearsBakedCacheOnlyAsset)
{
    sel.SetSelectedAsset("", "cache/font.dtex", true, "baked-guid");
    sel.AddToSelection(m_ObjA);
    EXPECT_FALSE(sel.HasAssetSelection());
    EXPECT_FALSE(sel.GetSelectedAssetHasCached());
    EXPECT_TRUE(sel.GetSelectedAssetCachePath().empty());
}

TEST_F(EditorSelectionTest, ToggleSelectionClearsBakedCacheOnlyAsset)
{
    sel.SetSelectedAsset("", "cache/font.dtex", true, "baked-guid");
    sel.ToggleSelection(m_ObjA);
    EXPECT_FALSE(sel.HasAssetSelection());
    EXPECT_FALSE(sel.GetSelectedAssetHasCached());
    EXPECT_TRUE(sel.GetSelectedAssetCachePath().empty());
}

// ---- Change notification ----
//
// EditorApp mirrors selection into the EditorApplication facade (the one package
// CustomEditors read) through this callback. It used to be mirrored at each call
// site instead, and Add/Remove/ToggleSelection all forgot - so a Ctrl+click in
// the Hierarchy left GetActiveObject() naming the previously selected object,
// which then dangled once that object was deleted. Every mutator must notify.

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
    rec.Attach(sel);

    struct Step
    {
        const char* name;
        std::function<void()> apply;
        Deki::Object* expected;
    };

    const std::vector<Step> steps = {
        { "SetSelectedObject", [&] { sel.SetSelectedObject(m_ObjA); }, m_ObjA },
        { "AddToSelection", [&] { sel.AddToSelection(m_ObjB); }, m_ObjA },
        { "RemoveFromSelection", [&] { sel.RemoveFromSelection(m_ObjA); }, m_ObjB },
        { "ToggleSelection (on)", [&] { sel.ToggleSelection(m_ObjC); }, m_ObjB },
        { "ToggleSelection (off)", [&] { sel.ToggleSelection(m_ObjB); }, m_ObjC },
        { "SetSelectedObjects", [&] { sel.SetSelectedObjects({ m_ObjA, m_ObjB }); }, m_ObjA },
        { "RemoveObjects", [&] { sel.RemoveObjects({ m_ObjA }); }, m_ObjB },
        { "SetSelectedAsset", [&] { sel.SetSelectedAsset("p.png", "c.dtex", true); }, nullptr },
        { "ClearObjectSelection", [&] { sel.ClearObjectSelection(); }, nullptr },
    };

    for (const Step& step : steps)
    {
        const int before = rec.calls;
        step.apply();
        EXPECT_GT(rec.calls, before) << step.name << " did not notify";
        EXPECT_EQ(rec.last, step.expected) << step.name << " reported the wrong active object";
        EXPECT_EQ(rec.last, sel.GetSelectedObject())
            << step.name << " reported an active object the selection disagrees with";
    }
}

TEST_F(EditorSelectionTest, NoOpMutationsDoNotReportAStaleObject)
{
    sel.SetSelectedObject(m_ObjA);

    ChangeRecorder rec;
    rec.Attach(sel);

    // Removing something that was never selected changes nothing; if it did
    // notify, it would still have to report the correct active object.
    sel.RemoveFromSelection(m_ObjC);
    EXPECT_EQ(sel.GetSelectedObject(), m_ObjA);
    if (rec.calls > 0)
    {
        EXPECT_EQ(rec.last, m_ObjA);
    }
}
