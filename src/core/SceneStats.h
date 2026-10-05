#pragma once

// Cheap facts about a scene document, read straight from its JSON.
//
// The crash-recovery dialog uses them to show what a recovered file holds
// without opening it: 1 object is safe to throw away, 200 objects is not.

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace DekiEditor
{

/// Total objects in a scene document, including nested children.
///
/// A document holds "objects"; an object holds "children". Neither key appears
/// on the other, so one recursive walk covers both without counting anything
/// twice. Kept separate from the dialog so this is tested.
inline int CountSceneObjects(const nlohmann::json& node)
{
    int n = 0;
    for (const char* key : { "objects", "children" })
    {
        auto it = node.find(key);
        if (it == node.end() || !it->is_array())
        {
            continue;
        }
        for (const auto& child : *it)
        {
            n += 1 + CountSceneObjects(child);
        }
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
/// without opening it. Reads names and component classes only.
///
/// Recovered files come from a process that just crashed, so anything missing
/// or of the wrong type is skipped rather than throwing.
inline std::vector<SceneOutlineNode> BuildSceneOutline(const nlohmann::json& node)
{
    std::vector<SceneOutlineNode> out;

    for (const char* key : { "objects", "children" })
    {
        auto it = node.find(key);
        if (it == node.end() || !it->is_array())
        {
            continue;
        }

        for (const auto& child : *it)
        {
            if (!child.is_object())
            {
                continue;
            }

            SceneOutlineNode n;

            auto nameIt = child.find("name");
            n.name = (nameIt != child.end() && nameIt->is_string()) ? nameIt->get<std::string>() : "(unnamed)";

            auto compIt = child.find("components");
            if (compIt != child.end() && compIt->is_array())
            {
                for (const auto& c : *compIt)
                {
                    if (!c.is_object())
                    {
                        continue;
                    }
                    // The class name without its namespace: "Deki2D::TextComponent" shows as "TextComponent".
                    auto typeIt = c.find("type");
                    if (typeIt != c.end() && typeIt->is_string())
                    {
                        const std::string type = typeIt->get<std::string>();
                        const size_t colons = type.rfind("::");
                        n.components.push_back(colons == std::string::npos ? type : type.substr(colons + 2));
                    }
                }
            }

            n.children = BuildSceneOutline(child);
            out.push_back(std::move(n));
        }
    }

    return out;
}

}  // namespace DekiEditor
