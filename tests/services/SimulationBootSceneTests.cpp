#include <gtest/gtest.h>

#include <deki/TypeId.h>

#include <deki-editor/build/SimulationBootScene.h>

using namespace DekiEditor;

namespace
{
nlohmann::json BoardBootScene()
{
    return nlohmann::json::parse(R"({
      "formatVersion": 1,
      "objects": [{
        "active": true, "id": 1, "name": "Setup",
        "children": [{ "active": true, "id": 600, "name": "Display", "children": [],
                       "components": [{ "type": "Board::PanelSetup", "properties": { "pin": 5 } }] }],
        "components": [{ "type": "Deki::PlatformSetupComponent",
                         "properties": { "setupComponents": [{ "id": 600, "typeId": 1 }] } }]
      }]
    })");
}

const nlohmann::json kSteps = nlohmann::json::parse(R"([
  { "name": "QEMU Display", "type": "DekiEsp32::ESP32QemuDisplaySetup",
    "properties": { "width": 320, "height": 240, "format": "RGB565" } }
])");
}  // namespace

// The board's steps are kept as they were; the simulator's comes after them,
// with a fresh id and its type's hash.
TEST(SimulationBootScene, AddsTheStepAfterTheBoards)
{
    nlohmann::json scene = BoardBootScene();
    std::string error;
    ASSERT_TRUE(AddSimulationSteps(scene, kSteps, error)) << error;

    const auto& setup = scene["objects"][0];
    ASSERT_EQ(setup["children"].size(), 2u);
    EXPECT_EQ(setup["children"][0]["components"][0]["type"], "Board::PanelSetup");
    EXPECT_EQ(setup["children"][0]["components"][0]["properties"]["pin"], 5);

    const auto& added = setup["children"][1];
    EXPECT_EQ(added["name"], "QEMU Display");
    EXPECT_EQ(added["id"], 601);
    EXPECT_EQ(added["components"][0]["properties"]["width"], 320);

    const auto& order = setup["components"][0]["properties"]["setupComponents"];
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0]["id"], 600);
    EXPECT_EQ(order[1]["id"], 601);
    EXPECT_EQ(order[1]["typeId"].get<uint32_t>(), Deki::HashString("DekiEsp32::ESP32QemuDisplaySetup"));
}

// A platform with no boot scene of its own gets one with just the steps.
TEST(SimulationBootScene, MakesABootSceneWhenThereIsNone)
{
    nlohmann::json scene;
    std::string error;
    ASSERT_TRUE(AddSimulationSteps(scene, kSteps, error)) << error;
    ASSERT_EQ(scene["objects"].size(), 1u);
    const auto& setup = scene["objects"][0];
    EXPECT_EQ(setup["components"][0]["type"], "Deki::PlatformSetupComponent");
    ASSERT_EQ(setup["children"].size(), 1u);
    EXPECT_EQ(setup["components"][0]["properties"]["setupComponents"].size(), 1u);
}

TEST(SimulationBootScene, RejectsStepsWithoutAType)
{
    nlohmann::json scene = BoardBootScene();
    std::string error;
    EXPECT_FALSE(AddSimulationSteps(scene, nlohmann::json::parse(R"([{ "name": "x" }])"), error));
    EXPECT_FALSE(error.empty());
}
