// One project serves several targets, and a scene is shared by all of them. An
// object can say what it needs from the platform, and it is simply not part of
// the build for a target that cannot offer it. Not an error, and nothing
// removed from the project: excluding is the normal case, which is what makes
// "a board with wifi and a board without" work from one scene.
//
// The filter runs on the scene JSON at build time, so the runtime never learns
// that targets exist.

#include <gtest/gtest.h>

#include <deki-editor/SceneTargetFilter.h>

#include <set>
#include <string>

using namespace DekiEditor;

namespace
{

// A boot scene with two objects, one of which only makes sense where there is
// wifi. The component node carries an "id" as well, which is the trap: matching
// on id alone tags the component instead of the object.
nlohmann::json TwoObjects()
{
    return nlohmann::json::parse(R"({
      "objects": [
        {
          "id": 600,
          "name": "Display",
          "children": [],
          "components": [ { "type": "SDL3DisplaySetup", "id": 700, "properties": {} } ]
        },
        {
          "id": 601,
          "name": "WiFi",
          "requires": ["wifi"],
          "children": [
            { "id": 602, "name": "Child", "children": [],
              "components": [ { "type": "ChildComponent", "id": 702, "properties": {} } ] }
          ],
          "components": [ { "type": "WiFiSetup", "id": 701, "properties": {} } ]
        }
      ]
    })");
}

size_t ObjectCount(const nlohmann::json& scene)
{
    return scene["objects"].size();
}

const std::set<std::string> kWithWifi{ "filesystem", "wifi" };
const std::set<std::string> kNoWifi{ "filesystem" };

}  // namespace

TEST(SceneTargetFilter, ObjectWithoutRequirementsIsOnEveryTarget)
{
    const nlohmann::json scene = TwoObjects();
    EXPECT_TRUE(ObjectMissingCapabilities(scene["objects"][0], kNoWifi).empty());
    EXPECT_TRUE(ObjectMissingCapabilities(scene["objects"][0], kWithWifi).empty());
}

TEST(SceneTargetFilter, TargetThatOffersItKeepsTheObject)
{
    const nlohmann::json kept = FilterSceneForTarget(TwoObjects(), kWithWifi);
    EXPECT_EQ(ObjectCount(kept), 2u);
}

TEST(SceneTargetFilter, TargetThatDoesNotOfferItDropsTheObject)
{
    const nlohmann::json kept = FilterSceneForTarget(TwoObjects(), kNoWifi);
    ASSERT_EQ(ObjectCount(kept), 1u);
    EXPECT_EQ(kept["objects"][0]["id"].get<int>(), 600);

    const auto missing = ObjectMissingCapabilities(TwoObjects()["objects"][1], kNoWifi);
    ASSERT_EQ(missing.size(), 1u);
    EXPECT_EQ(missing[0], "wifi");
}

// A child exists only as part of its parent, so dropping the parent drops it.
TEST(SceneTargetFilter, DroppingAParentDropsItsChildren)
{
    const nlohmann::json kept = FilterSceneForTarget(TwoObjects(), kNoWifi);
    const std::string text = kept.dump();
    EXPECT_EQ(text.find("ChildComponent"), std::string::npos);
    EXPECT_EQ(text.find("WiFiSetup"), std::string::npos);
    EXPECT_NE(text.find("SDL3DisplaySetup"), std::string::npos);
}

// The editor has no target in hand and must show everything, or an object that
// exists only on a board could never be authored.
TEST(SceneTargetFilter, NoCapabilitiesKeepsEverything)
{
    const nlohmann::json kept = FilterSceneForTarget(TwoObjects(), {});
    EXPECT_EQ(ObjectCount(kept), 2u);
}

TEST(SceneTargetFilter, ASceneWithNoRequirementsIsUntouched)
{
    nlohmann::json plain = TwoObjects();
    plain["objects"][1].erase("requires");
    EXPECT_FALSE(SceneHasTargetRequirements(plain));
    EXPECT_EQ(FilterSceneForTarget(plain, kNoWifi), plain);
}

// Components carry an "id" as well. Collecting by id alone picked up the
// component inside the object and wrote the requirement twice.
TEST(SceneTargetFilter, RequirementsAreCollectedFromObjectsNotComponents)
{
    const auto reqs = CollectTargetRequirements(TwoObjects());
    ASSERT_EQ(reqs.size(), 1u);
    ASSERT_EQ(reqs.count(601), 1u);
    EXPECT_EQ(reqs.at(601).size(), 1u);
    EXPECT_EQ(reqs.at(601)[0], "wifi");
}
