#pragma once

// What the agent pointer shows for one MCP tool call: where it points and what
// its label says. Pure data and a pure mapping, kept apart from the drawing
// (AgentCursor) so it can be tested without ImGui.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace DekiEditor
{

struct AgentIntent
{
    enum class Target
    {
        None,       // label only: a build, a save, an undo - nothing on screen to point at
        Panel,      // a whole panel, `panel`
        Object,     // an object's Hierarchy row
        Component,  // a component's Inspector header, else the object's row
        Property,   // a property's Inspector row, else the component, else the object's row
        Transform,  // a Transform row (`property` is position, rotation or scale)
        Asset,      // an Asset Browser row, `assetPath`
        Point       // exact pixels, `x`/`y` relative to `panel` (the window when empty), as EditorInput takes them
    };

    Target target = Target::None;

    /// Shown next to the pointer. May hold "{object}", which the caller replaces
    /// with the object's name (it needs the scene; this mapping does not).
    std::string label;

    /// The ImGui window to point at when the item itself is not on screen, or
    /// for Panel and Point the window itself. "###Id" finds a panel by its id.
    std::string panel;

    uint32_t objectId = 0;
    bool hasObject = false;
    std::string component;  // serialized name, as the tools take it
    std::string property;   // Property: its name; Transform: position / rotation / scale
    std::string assetPath;  // project-relative
    float x = 0.0f, y = 0.0f;

    /// Something to show at all.
    bool Shows() const { return !label.empty(); }
    /// The pointer moves to a target, so the tool waits until it is there.
    bool Travels() const { return target != Target::None; }
};

/// The intent for calling `tool` with `args`. Read-only tools (get_*, list_*,
/// screenshot, statuses) give an empty intent: the pointer shows what the
/// agent changes, not what it looks at.
AgentIntent IntentForTool(const std::string& tool, const nlohmann::json& args);

}  // namespace DekiEditor
