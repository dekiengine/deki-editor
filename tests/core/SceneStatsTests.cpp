#include <gtest/gtest.h>
#include "core/SceneStats.h"
#include <functional>

using namespace DekiEditor;
using nlohmann::json;

// The crash-recovery dialog reports this number to help decide whether a
// recovered scene is worth keeping, so getting it wrong is worse than showing
// nothing - it would make the dialog confidently misdescribe unsaved work.

TEST(SceneStats, EmptyDocumentHasNoObjects)
{
    EXPECT_EQ(CountSceneObjects(json::parse(R"({"objects":[]})")), 0);
    EXPECT_EQ(CountSceneObjects(json::parse(R"({})")), 0);
}

TEST(SceneStats, CountsFlatObjects)
{
    const auto doc = json::parse(R"({"objects":[
        {"name":"A","children":[]},
        {"name":"B","children":[]},
        {"name":"C","children":[]}
    ]})");
    EXPECT_EQ(CountSceneObjects(doc), 3);
}

TEST(SceneStats, CountsNestedChildren)
{
    // Root > Child > Grandchild, plus a sibling: 4 objects in total.
    const auto doc = json::parse(R"({"objects":[
        {"name":"Root","children":[
            {"name":"Child","children":[
                {"name":"Grandchild","children":[]}
            ]}
        ]},
        {"name":"Sibling","children":[]}
    ]})");
    EXPECT_EQ(CountSceneObjects(doc), 4);
}

TEST(SceneStats, DoesNotDoubleCount)
{
    // The failure mode this guards: an object carrying both keys, or a walk
    // that visits a node once per key, silently inflating every count.
    const auto doc = json::parse(R"({"objects":[
        {"name":"Root","objects":[],"children":[{"name":"Kid","children":[]}]}
    ]})");
    EXPECT_EQ(CountSceneObjects(doc), 2);
}

TEST(SceneStats, IgnoresMalformedShapes)
{
    // Recovered files come from a crashed process, so they can be truncated or
    // otherwise odd. Counting must degrade rather than throw.
    EXPECT_EQ(CountSceneObjects(json::parse(R"({"objects":"not-an-array"})")), 0);
    EXPECT_EQ(CountSceneObjects(json::parse(R"({"objects":[{"children":"nope"}]})")), 1);
    EXPECT_EQ(CountSceneObjects(json::parse(R"([])")), 0);
}

// The single-object case is common in real recovery folders: one empty Root,
// from a crash before any work existed. "1 object" tells you it is safe to
// discard.
TEST(SceneStats, LoneRootReadsAsOneObject)
{
    const auto doc = json::parse(R"({"objects":[{"name":"Root","children":[],"components":[]}]})");
    EXPECT_EQ(CountSceneObjects(doc), 1);
}

// ---------------------------------------------------------------------------
// Outline - what the recovery preview shows when an entry is double-clicked.
// ---------------------------------------------------------------------------

TEST(SceneOutline, CapturesNamesComponentsAndNesting)
{
    const auto doc = json::parse(R"({"objects":[
        {"name":"Root","components":[{"class":"CameraComponent"}],"children":[
            {"name":"Hero","components":[{"class":"SpriteComponent"},{"class":"HeroController"}],"children":[]}
        ]}
    ]})");

    const auto outline = BuildSceneOutline(doc);
    ASSERT_EQ(outline.size(), 1u);
    EXPECT_EQ(outline[0].name, "Root");
    ASSERT_EQ(outline[0].components.size(), 1u);
    EXPECT_EQ(outline[0].components[0], "CameraComponent");

    ASSERT_EQ(outline[0].children.size(), 1u);
    EXPECT_EQ(outline[0].children[0].name, "Hero");
    ASSERT_EQ(outline[0].children[0].components.size(), 2u);
    EXPECT_EQ(outline[0].children[0].components[1], "HeroController");
}

TEST(SceneOutline, NamesMissingObjectsRatherThanSkippingThem)
{
    // An object with no name still occupies a row - dropping it would make the
    // preview disagree with the object count shown on the card.
    const auto doc = json::parse(R"({"objects":[{"children":[]},{"name":"B","children":[]}]})");
    const auto outline = BuildSceneOutline(doc);
    ASSERT_EQ(outline.size(), 2u);
    EXPECT_EQ(outline[0].name, "(unnamed)");
    EXPECT_EQ(outline[1].name, "B");
}

TEST(SceneOutline, SurvivesMalformedInput)
{
    // Straight from a crashed process, so wrong types must be skipped, not throw.
    EXPECT_TRUE(BuildSceneOutline(json::parse(R"({"objects":"nope"})")).empty());
    EXPECT_TRUE(BuildSceneOutline(json::parse(R"({})")).empty());

    const auto odd = BuildSceneOutline(
        json::parse(R"({"objects":[{"name":"A","components":"nope","children":"nope"},"loose-string"]})"));
    ASSERT_EQ(odd.size(), 1u);  // the bare string is not an object
    EXPECT_EQ(odd[0].name, "A");
    EXPECT_TRUE(odd[0].components.empty());
    EXPECT_TRUE(odd[0].children.empty());
}

TEST(SceneOutline, AgreesWithTheObjectCount)
{
    // The preview and the card's "N objects" are read by the same person a
    // second apart; they must not disagree.
    const auto doc = json::parse(R"({"objects":[
        {"name":"Root","children":[
            {"name":"A","children":[{"name":"A1","children":[]}]},
            {"name":"B","children":[]}
        ]}
    ]})");

    std::function<int(const std::vector<SceneOutlineNode>&)> countOutline =
        [&](const std::vector<SceneOutlineNode>& nodes)
    {
        int n = 0;
        for (const auto& node : nodes)
        {
            n += 1 + countOutline(node.children);
        }
        return n;
    };

    EXPECT_EQ(countOutline(BuildSceneOutline(doc)), CountSceneObjects(doc));
}
