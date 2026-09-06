#pragma once

// Cheap facts about a scene document, read straight from its JSON.
//
// Used by the crash-recovery dialog to say what is actually in a recovered
// file. "recovered.scene, untitled, 1 object" is a crash before any work was
// done and safe to throw away; the same line reading 200 objects is not, and
// the difference has to be visible without opening it.

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace DekiEditor
{

/// Total objects in a scene document, including nested children.
///
/// A document holds "objects"; an object holds "children". Neither key appears
/// on the other, so one recursive walk covers both with no double counting —
/// which is the easy bug here, and the reason this is separated out and tested
/// rather than left inline in the dialog.
inline int CountSceneObjects(const nlohmann::json& node)
{
    int n = 0;
    for (const char* key : { "objects", "children" })
    {
        auto it = node.find(key);
        if (it == node.end() || !it->is_array())
            continue;
        for (const auto& child : *it)
            n += 1 + CountSceneObjects(child);
    }
    return n;
}

/// One object in a scene, flattened enough to render as an indented list.
struct SceneOutlineNode
{
    std::string name;
    std::vector<std::string> components;  // component class names, in file order
    std::vector<SceneOutlineNode> children;
};

/// The object tree of a scene document, for previewing a recovered file
/// without opening it — the counts alone say how much is in there, this says
/// what. Names and component classes only; property values are not read.
///
/// Tolerant by design: recovered files come from a process that just crashed,
/// so anything missing or of the wrong type is skipped rather than throwing.
inline std::vector<SceneOutlineNode> BuildSceneOutline(const nlohmann::json& node)
{
    std::vector<SceneOutlineNode> out;

    for (const char* key : { "objects", "children" })
    {
        auto it = node.find(key);
        if (it == node.end() || !it->is_array())
            continue;

        for (const auto& child : *it)
        {
            if (!child.is_object())
                continue;

            SceneOutlineNode n;

            auto nameIt = child.find("name");
            n.name = (nameIt != child.end() && nameIt->is_string())
                         ? nameIt->get<std::string>()
                         : "(unnamed)";

            auto compIt = child.find("components");
            if (compIt != child.end() && compIt->is_array())
            {
                for (const auto& c : *compIt)
                {
                    if (!c.is_object()) continue;
                    auto classIt = c.find("class");
                    if (classIt != c.end() && classIt->is_string())
                        n.components.push_back(classIt->get<std::string>());
                }
            }

            n.children = BuildSceneOutline(child);
            out.push_back(std::move(n));
        }
    }

    return out;
}

}  // namespace DekiEditor
