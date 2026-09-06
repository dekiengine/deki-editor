#include <gtest/gtest.h>
#include <memory>

#include "core/SceneGuarantee.h"
#include <deki/Scene.h>

using namespace DekiEditor;

// The editor's invariant is that a scene is always available to edit — there is
// no usable "no scene" state (EditorApp::GetCurrentScene() never returns null).
// EnsureScene backs that guarantee; these tests pin its behavior.

TEST(SceneGuaranteeTest, CreatesSceneWhenNull)
{
    // A freshly-launched editor with no project open starts with a null pointer.
    std::unique_ptr<::Deki::Scene> scene;
    ASSERT_EQ(scene, nullptr);

    ::Deki::Scene* result = EnsureScene(scene);

    EXPECT_NE(result, nullptr);                  // never null
    EXPECT_EQ(result, scene.get());             // installed into the owner
    EXPECT_TRUE(scene->GetObjects().empty());   // a clean, empty scene
}

TEST(SceneGuaranteeTest, KeepsExistingScene)
{
    auto scene = std::make_unique<::Deki::Scene>();
    ::Deki::Scene* original = scene.get();
    scene->CreateDekiObject("Existing");

    ::Deki::Scene* result = EnsureScene(scene);

    EXPECT_EQ(result, original);                 // same instance, not replaced
    EXPECT_EQ(scene->GetObjects().size(), 1u);  // existing content preserved
}

TEST(SceneGuaranteeTest, AlwaysNonNullAcrossCalls)
{
    std::unique_ptr<::Deki::Scene> scene;
    ::Deki::Scene* first  = EnsureScene(scene);
    ::Deki::Scene* second = EnsureScene(scene);  // idempotent — does not re-create

    EXPECT_NE(first, nullptr);
    EXPECT_EQ(first, second);
}
