// Saving a scene re-saves the scenes that instance it, and only those: a scene
// that just links to it must stay as it is on disk.

#include <gtest/gtest.h>

#include "core/SceneInstanceRefs.h"
#include <deki/Object.h>
#include <deki/Scene.h>

#include <string>
#include <vector>

using DekiEditor::InstancedSceneGuids;

namespace
{

const char* kMenu = "c2d75c6e-a3d0-415c-99eb-6e2a135b81f6";
const char* kHud = "0f0f0f0f-0000-4000-8000-000000000001";

}  // namespace

// deki-demo's scenes each had a Back button linking to the menu; saving the
// menu re-saved every one of them.
TEST(SceneInstanceRefs, ASceneLinkIsNotAnInstance)
{
    const std::string scene = R"({
        "objects": [
            {
                "name": "Back",
                "components": [
                    { "properties": { "scene": "c2d75c6e-a3d0-415c-99eb-6e2a135b81f6" }, "type": "SceneLink" },
                    { "properties": { "sprite": { "guid": "7a3c1e55-1111-4a2b-9c3d-0123456789ab" } },
                      "type": "Deki2D::SpriteComponent" }
                ]
            }
        ]
    })";

    EXPECT_TRUE(InstancedSceneGuids(scene).empty());
}

TEST(SceneInstanceRefs, InstancesAreFoundOnceEach)
{
    const std::string scene = R"({
        "objects": [
            { "name": "Hud", "instance": "0f0f0f0f-0000-4000-8000-000000000001", "overrides": [] },
            { "name": "Hud 2", "instance":"0f0f0f0f-0000-4000-8000-000000000001" },
            { "name": "Menu", "instance": "c2d75c6e-a3d0-415c-99eb-6e2a135b81f6" }
        ]
    })";

    EXPECT_EQ(InstancedSceneGuids(scene), (std::vector<std::string>{ kHud, kMenu }));
}

// What the engine writes for an instance is what the scan reads.
TEST(SceneInstanceRefs, ReadsTheEnginesInstanceKey)
{
    Deki::Scene scene;
    Deki::Object* hud = scene.CreateDekiObject("Hud");
    hud->SetSourceSceneGuid(kHud);
    scene.CreateDekiObject("Plain");

    EXPECT_EQ(InstancedSceneGuids(scene.SaveToJsonString()), (std::vector<std::string>{ kHud }));
}
