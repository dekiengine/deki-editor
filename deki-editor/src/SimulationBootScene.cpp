#include <deki-editor/build/SimulationBootScene.h>

#include <deki/TypeId.h>

#include <algorithm>
#include <cstdint>

namespace DekiEditor
{

namespace
{
constexpr const char* kPlatformSetupType = "Deki::PlatformSetupComponent";

nlohmann::json IdentityTransform()
{
    return { { "rotation", 0.0 }, { "scaleX", 1.0 }, { "scaleY", 1.0 }, { "x", 0.0 }, { "y", 0.0 } };
}

void CollectIds(const nlohmann::json& objects, int64_t& maxId)
{
    if (!objects.is_array())
        return;
    for (const auto& obj : objects)
    {
        if (obj.contains("id") && obj["id"].is_number_integer())
            maxId = std::max<int64_t>(maxId, obj["id"].get<int64_t>());
        if (obj.contains("children"))
            CollectIds(obj["children"], maxId);
    }
}

// The object carrying the PlatformSetupComponent, searched depth-first.
nlohmann::json* FindSetupObject(nlohmann::json& objects)
{
    if (!objects.is_array())
        return nullptr;
    for (auto& obj : objects)
    {
        if (obj.contains("components") && obj["components"].is_array())
            for (const auto& comp : obj["components"])
                if (comp.value("type", std::string()) == kPlatformSetupType)
                    return &obj;
        if (obj.contains("children"))
            if (nlohmann::json* found = FindSetupObject(obj["children"]))
                return found;
    }
    return nullptr;
}
}  // namespace

bool AddSimulationSteps(nlohmann::json& scene, const nlohmann::json& steps, std::string& error)
{
    if (!steps.is_array())
    {
        error = "the simulator's boot steps are not a list";
        return false;
    }
    for (const auto& step : steps)
    {
        if (!step.is_object() || !step.contains("type") || !step["type"].is_string())
        {
            error = "a simulator boot step has no component type";
            return false;
        }
    }

    if (scene.is_null())
        scene = { { "formatVersion", 1 }, { "objects", nlohmann::json::array() } };
    if (!scene.contains("objects") || !scene["objects"].is_array())
    {
        error = "the boot scene has no objects";
        return false;
    }

    int64_t nextId = 0;
    CollectIds(scene["objects"], nextId);
    ++nextId;

    nlohmann::json* setup = FindSetupObject(scene["objects"]);
    if (!setup)
    {
        nlohmann::json obj = {
            { "active", true },
            { "children", nlohmann::json::array() },
            { "components",
              nlohmann::json::array({ { { "type", kPlatformSetupType },
                                        { "properties", { { "setupComponents", nlohmann::json::array() } } } } }) },
            { "id", nextId++ },
            { "name", "Setup" },
            { "transform", IdentityTransform() }
        };
        scene["objects"].push_back(obj);
        setup = &scene["objects"].back();
    }

    nlohmann::json* setupProps = nullptr;
    for (auto& comp : (*setup)["components"])
        if (comp.value("type", std::string()) == kPlatformSetupType)
        {
            if (!comp.contains("properties") || !comp["properties"].is_object())
                comp["properties"] = nlohmann::json::object();
            setupProps = &comp["properties"];
        }
    if (!setupProps->contains("setupComponents") || !(*setupProps)["setupComponents"].is_array())
        (*setupProps)["setupComponents"] = nlohmann::json::array();
    if (!setup->contains("children") || !(*setup)["children"].is_array())
        (*setup)["children"] = nlohmann::json::array();

    for (const auto& step : steps)
    {
        const std::string type = step["type"].get<std::string>();
        const int64_t id = nextId++;
        nlohmann::json child = {
            { "active", true },
            { "children", nlohmann::json::array() },
            { "components",
              nlohmann::json::array({ { { "type", type },
                                        { "properties", step.value("properties", nlohmann::json::object()) } } }) },
            { "id", id },
            { "name", step.value("name", type) },
            { "transform", IdentityTransform() }
        };
        (*setup)["children"].push_back(child);
        (*setupProps)["setupComponents"].push_back({ { "id", id }, { "typeId", Deki::HashString(type.c_str()) } });
    }
    return true;
}

}  // namespace DekiEditor
