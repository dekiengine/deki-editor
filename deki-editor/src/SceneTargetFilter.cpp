#include <deki-editor/SceneTargetFilter.h>

#include <fstream>
#include <functional>

namespace DekiEditor
{

namespace
{

// An object node is anything carrying "components" or "children"; the scene's
// own "objects" array holds them and they nest through "children".
bool LooksLikeObject(const nlohmann::json& node)
{
    return node.is_object() && (node.contains("components") || node.contains("children"));
}

// Walk every array that can hold objects, dropping the ones this target cannot
// satisfy. Returns a filtered copy of `node`.
nlohmann::json FilterNode(const nlohmann::json& node, const std::set<std::string>& capabilities)
{
    if (node.is_array())
    {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& item : node)
        {
            if (LooksLikeObject(item) && !ObjectMissingCapabilities(item, capabilities).empty())
            {
                continue;  // and with it every child, which is the point
            }
            out.push_back(FilterNode(item, capabilities));
        }
        return out;
    }

    if (node.is_object())
    {
        nlohmann::json out = nlohmann::json::object();
        for (const auto& entry : node.items())
        {
            out[entry.key()] = FilterNode(entry.value(), capabilities);
        }
        return out;
    }

    return node;
}

bool NodeHasRequirements(const nlohmann::json& node)
{
    if (node.is_array())
    {
        for (const auto& item : node)
        {
            if (NodeHasRequirements(item))
            {
                return true;
            }
        }
        return false;
    }
    if (node.is_object())
    {
        if (node.contains("requires") && node["requires"].is_array() && !node["requires"].empty())
        {
            return true;
        }
        for (const auto& entry : node.items())
        {
            if (NodeHasRequirements(entry.value()))
            {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

std::vector<std::string> ObjectMissingCapabilities(const nlohmann::json& obj, const std::set<std::string>& capabilities)
{
    // No target in hand means nothing is judged: the editor has to see every
    // object, or an object that exists only on a board could not be authored.
    if (capabilities.empty())
    {
        return {};
    }
    if (!obj.is_object() || !obj.contains("requires") || !obj["requires"].is_array())
    {
        return {};
    }

    std::vector<std::string> missing;
    for (const auto& need : obj["requires"])
    {
        if (!need.is_string())
        {
            continue;
        }
        const std::string name = need.get<std::string>();
        if (capabilities.count(name) == 0)
        {
            missing.push_back(name);
        }
    }
    return missing;
}

nlohmann::json FilterSceneForTarget(const nlohmann::json& scene, const std::set<std::string>& capabilities)
{
    if (capabilities.empty() || !SceneHasTargetRequirements(scene))
    {
        return scene;
    }
    return FilterNode(scene, capabilities);
}

bool SceneHasTargetRequirements(const nlohmann::json& scene)
{
    return NodeHasRequirements(scene);
}

std::map<int64_t, std::vector<std::string>> CollectTargetRequirements(const nlohmann::json& scene)
{
    std::map<int64_t, std::vector<std::string>> out;
    std::function<void(const nlohmann::json&)> visit = [&](const nlohmann::json& node)
    {
        if (node.is_array())
        {
            for (const auto& item : node)
            {
                visit(item);
            }
            return;
        }
        if (!node.is_object())
        {
            return;
        }
        // Components carry an "id" too, so matching on that alone tags the
        // wrong node. Only an object holds "components" or "children".
        if (LooksLikeObject(node) && node.contains("id") && node["id"].is_number_integer() &&
            node.contains("requires") && node["requires"].is_array())
        {
            std::vector<std::string> needs;
            for (const auto& n : node["requires"])
            {
                if (n.is_string())
                {
                    needs.push_back(n.get<std::string>());
                }
            }
            if (!needs.empty())
            {
                out[node["id"].get<int64_t>()] = needs;
            }
        }
        for (const auto& entry : node.items())
        {
            visit(entry.value());
        }
    };
    visit(scene);
    return out;
}

bool PreserveTargetRequirements(const std::string& scenePath, const nlohmann::json& before)
{
    const auto wanted = CollectTargetRequirements(before);
    if (wanted.empty())
    {
        return false;
    }

    nlohmann::json after;
    try
    {
        std::ifstream in(scenePath);
        if (!in)
        {
            return false;
        }
        in >> after;
    }
    catch (const std::exception&)
    {
        return false;
    }

    bool changed = false;
    std::function<void(nlohmann::json&)> visit = [&](nlohmann::json& node)
    {
        if (node.is_array())
        {
            for (auto& item : node)
            {
                visit(item);
            }
            return;
        }
        if (!node.is_object())
        {
            return;
        }
        if (LooksLikeObject(node) && node.contains("id") && node["id"].is_number_integer())
        {
            auto it = wanted.find(node["id"].get<int64_t>());
            if (it != wanted.end() && !node.contains("requires"))
            {
                node["requires"] = it->second;
                changed = true;
            }
        }
        for (auto& entry : node.items())
        {
            visit(entry.value());
        }
    };
    visit(after);

    if (!changed)
    {
        return false;
    }

    try
    {
        std::ofstream out(scenePath);
        out << after.dump(2) << std::endl;
    }
    catch (const std::exception&)
    {
        return false;
    }
    return true;
}

}  // namespace DekiEditor
