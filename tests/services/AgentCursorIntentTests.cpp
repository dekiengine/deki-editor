// AgentCursorIntent: what the agent pointer shows, and where it goes, for an
// MCP tool call. The drawing and the waiting are AgentCursor's.

#include <gtest/gtest.h>

#include "services/AgentCursorIntent.h"

using namespace DekiEditor;
using Target = AgentIntent::Target;
using json = nlohmann::json;

TEST(AgentCursorIntent, ReadOnlyToolsShowNothing)
{
    for (const char* tool : { "get_object", "list_objects", "get_properties", "list_assets", "screenshot",
                              "list_panels", "build_status", "toolchain_status", "get_selection", "not_a_tool" })
    {
        const AgentIntent in = IntentForTool(tool, json::object());
        EXPECT_FALSE(in.Shows()) << tool;
        EXPECT_FALSE(in.Travels()) << tool;
    }
}

TEST(AgentCursorIntent, SetPropertyPointsAtTheRow)
{
    const AgentIntent in = IntentForTool(
        "set_property",
        { { "object_id", 12 }, { "component_type", "SpriteComponent" }, { "property_name", "color" }, { "value", 1 } });
    EXPECT_EQ(in.target, Target::Property);
    EXPECT_TRUE(in.hasObject);
    EXPECT_EQ(in.objectId, 12u);
    EXPECT_EQ(in.component, "SpriteComponent");
    EXPECT_EQ(in.property, "color");
    EXPECT_EQ(in.label, "Setting SpriteComponent.color");
    // Not on screen: the Inspector panel stands in.
    EXPECT_EQ(in.panel, "###Inspector");
    EXPECT_TRUE(in.Travels());
}

TEST(AgentCursorIntent, LabelsDropTheNamespaceButMatchingKeepsIt)
{
    const AgentIntent in =
        IntentForTool("set_property", { { "object_id", 402 },
                                        { "component_type", "Deki::ScreenAnchor" },
                                        { "property_name", "offsetY" } });
    EXPECT_EQ(in.label, "Setting ScreenAnchor.offsetY");
    EXPECT_EQ(in.component, "Deki::ScreenAnchor");
    EXPECT_EQ(IntentForTool("add_component", { { "object_id", 1 }, { "component_type", "Deki2D::TextComponent" } }).label,
              "Adding TextComponent to {object}");
}

TEST(AgentCursorIntent, ObjectToolsPointAtTheHierarchyRow)
{
    const AgentIntent del = IntentForTool("delete_object", { { "object_id", 3 } });
    EXPECT_EQ(del.target, Target::Object);
    EXPECT_EQ(del.objectId, 3u);
    EXPECT_EQ(del.panel, "###Hierarchy");
    EXPECT_EQ(del.label, "Deleting {object}");

    const AgentIntent ren = IntentForTool("rename_object", { { "object_id", 3 }, { "name", "Enemy" } });
    EXPECT_EQ(ren.label, "Renaming {object} to 'Enemy'");
}

TEST(AgentCursorIntent, TransformToolsNameTheirRow)
{
    const AgentIntent pos = IntentForTool("set_position", { { "object_id", 5 }, { "x", 1 }, { "y", 2 } });
    EXPECT_EQ(pos.target, Target::Transform);
    EXPECT_EQ(pos.property, "position");
    EXPECT_EQ(pos.label, "Moving {object}");
    EXPECT_EQ(IntentForTool("set_rotation", { { "object_id", 5 } }).property, "rotation");
    EXPECT_EQ(IntentForTool("set_scale", { { "object_id", 5 } }).label, "Scaling {object}");
}

TEST(AgentCursorIntent, ObjectIdMayBeADigitString)
{
    // --script passes values as JSON, but a hand-written one may quote them.
    const AgentIntent in = IntentForTool("delete_object", { { "object_id", "42" } });
    EXPECT_TRUE(in.hasObject);
    EXPECT_EQ(in.objectId, 42u);
}

TEST(AgentCursorIntent, ObjectToolWithoutAnIdStillLabelsButDoesNotTravel)
{
    const AgentIntent in = IntentForTool("delete_object", { { "object_id", "twelve" } });
    EXPECT_TRUE(in.Shows());
    EXPECT_FALSE(in.Travels());
}

TEST(AgentCursorIntent, CreateObjectGoesToTheParentOrThePanel)
{
    const AgentIntent root = IntentForTool("create_object", { { "name", "Player" } });
    EXPECT_EQ(root.target, Target::Panel);
    EXPECT_EQ(root.panel, "###Hierarchy");
    EXPECT_EQ(root.label, "Creating 'Player'");

    const AgentIntent child = IntentForTool("create_object", { { "name", "Gun" }, { "parent_id", 7 } });
    EXPECT_EQ(child.target, Target::Object);
    EXPECT_EQ(child.objectId, 7u);
    EXPECT_EQ(child.label, "Creating 'Gun' under {object}");

    EXPECT_EQ(IntentForTool("create_object", json::object()).label, "Creating 'DekiObject'");
}

TEST(AgentCursorIntent, SelectionPointsAtTheFirstObject)
{
    const AgentIntent one = IntentForTool("set_selection", { { "object_ids", json::array({ 9 }) } });
    EXPECT_EQ(one.target, Target::Object);
    EXPECT_EQ(one.objectId, 9u);
    EXPECT_EQ(one.label, "Selecting {object}");

    const AgentIntent many = IntentForTool("set_selection", { { "object_ids", json::array({ 9, 10, 11 }) } });
    EXPECT_EQ(many.objectId, 9u);
    EXPECT_EQ(many.label, "Selecting 3 objects");

    const AgentIntent none = IntentForTool("set_selection", { { "object_ids", json::array() } });
    EXPECT_FALSE(none.Travels());
    EXPECT_EQ(none.label, "Clearing the selection");
}

TEST(AgentCursorIntent, ComponentToolsPointAtTheComponent)
{
    const AgentIntent in = IntentForTool("add_component", { { "object_id", 4 }, { "component_type", "Rigidbody" } });
    EXPECT_EQ(in.target, Target::Component);
    EXPECT_EQ(in.component, "Rigidbody");
    EXPECT_EQ(in.label, "Adding Rigidbody to {object}");
}

TEST(AgentCursorIntent, AssetToolsPointAtTheAsset)
{
    const AgentIntent in = IntentForTool("open_asset", { { "path", "assets/graphs/ai.graph" } });
    EXPECT_EQ(in.target, Target::Asset);
    EXPECT_EQ(in.assetPath, "assets/graphs/ai.graph");
    EXPECT_EQ(in.panel, "###Asset Browser");
    EXPECT_EQ(in.label, "Opening ai.graph");

    const AgentIntent move =
        IntentForTool("move_asset", { { "from", "assets/a.png" }, { "to", "assets/textures/a.png" } });
    EXPECT_EQ(move.assetPath, "assets/a.png");
    EXPECT_EQ(move.label, "Moving a.png to assets/textures/a.png");

    // By GUID only: nothing to find a row by.
    const AgentIntent byGuid = IntentForTool("select_asset", { { "guid", "abc" } });
    EXPECT_TRUE(byGuid.Shows());
    EXPECT_FALSE(byGuid.Travels());
}

TEST(AgentCursorIntent, InputToolsPointAtTheirPixels)
{
    const AgentIntent click =
        IntentForTool("click", { { "panel", "HIERARCHY###Hierarchy" }, { "x", 40.5 }, { "y", 80 } });
    EXPECT_EQ(click.target, Target::Point);
    EXPECT_EQ(click.panel, "HIERARCHY###Hierarchy");
    EXPECT_FLOAT_EQ(click.x, 40.5f);
    EXPECT_FLOAT_EQ(click.y, 80.0f);
    EXPECT_EQ(click.label, "Clicking");

    EXPECT_EQ(IntentForTool("click", { { "x", 1 }, { "y", 1 }, { "double", true } }).label, "Double-clicking");
    EXPECT_EQ(IntentForTool("click", { { "x", 1 }, { "y", 1 }, { "button", "right" } }).label, "Right-clicking");
    // No panel: window pixels.
    EXPECT_TRUE(IntentForTool("drag", { { "x", 1 }, { "y", 1 } }).panel.empty());
}

TEST(AgentCursorIntent, KeysAndTextAreLabelsOnly)
{
    const AgentIntent key = IntentForTool("key", { { "key", "S" }, { "ctrl", true }, { "shift", true } });
    EXPECT_EQ(key.label, "Pressing Ctrl+Shift+S");
    EXPECT_FALSE(key.Travels());
    EXPECT_FALSE(IntentForTool("type_text", { { "text", "hi" } }).Travels());
}

TEST(AgentCursorIntent, PanelTools)
{
    const AgentIntent focus = IntentForTool("focus_panel", { { "name", "HIERARCHY###Hierarchy" } });
    EXPECT_EQ(focus.target, Target::Panel);
    EXPECT_EQ(focus.panel, "HIERARCHY###Hierarchy");
    EXPECT_EQ(focus.label, "Showing Hierarchy");

    // Opening: the panel is not drawn yet, so there is nowhere to go.
    const AgentIntent open = IntentForTool("open_panel", { { "id", "Console" } });
    EXPECT_FALSE(open.Travels());
    EXPECT_EQ(open.label, "Opening Console");

    const AgentIntent close = IntentForTool("open_panel", { { "id", "Console" }, { "open", false } });
    EXPECT_EQ(close.target, Target::Panel);
    EXPECT_EQ(close.panel, "###Console");
}

TEST(AgentCursorIntent, LongJobsAreLabelsOnly)
{
    for (const char* tool : { "build_firmware", "deploy_build", "rebuild", "save_scene", "undo", "redo" })
    {
        const AgentIntent in = IntentForTool(tool, json::object());
        EXPECT_TRUE(in.Shows()) << tool;
        EXPECT_FALSE(in.Travels()) << tool;
    }
    EXPECT_EQ(IntentForTool("install_package", { { "id", "deki-2d" } }).label, "Installing deki-2d");
}
