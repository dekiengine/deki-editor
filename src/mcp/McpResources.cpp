#include "McpResources.h"
#include "../EditorApp.h"
#include <deki/Scene.h>
#include <deki/Object.h>
#include <deki/Component.h>
#include <deki/Math.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/Property.h>
#include "../core/commands/PropertyTypeHandlers.h"

namespace DekiEditor
{

// Helper: recursively serialize object for resource
static nlohmann::json SerializeObjectForResource(Deki::Object* obj)
{
    nlohmann::json j;
    j["id"] = obj->GetId();
    j["name"] = obj->GetName();
    j["x"] = obj->GetX();
    j["y"] = obj->GetY();
    // Public MCP surface reports rotation in degrees (storage is radians).
    j["rotation"] = obj->GetLocalRotation() * Deki::Math::kRadToDeg;
    j["scaleX"] = obj->GetScaleX();
    j["scaleY"] = obj->GetScaleY();

    auto& registry = Deki::ComponentRegistry::Instance();

    // Components with property values
    nlohmann::json comps = nlohmann::json::array();
    for (auto* comp : obj->GetComponents())
    {
        const Deki::ComponentMeta* meta = registry.GetMeta(comp->GetType());
        if (!meta) continue;

        nlohmann::json compJson;
        compJson["type"] = meta->serializedName;
        compJson["displayName"] = meta->name;

        nlohmann::json props = nlohmann::json::object();
        for (int i = 0; i < meta->propertyCount; ++i)
        {
            const auto& prop = meta->properties[i];
            if (prop.type == Deki::PropertyType::Array)
                props[prop.name] = CaptureArray(comp, prop);
            else
            {
                const auto& handler = GetPropertyTypeHandler(prop.type);
                props[prop.name] = handler.capture(comp, prop.offset);
            }
        }
        compJson["properties"] = props;
        comps.push_back(compJson);
    }
    j["components"] = comps;

    // Children
    nlohmann::json children = nlohmann::json::array();
    for (auto* child : obj->GetChildren())
        children.push_back(SerializeObjectForResource(child));
    j["children"] = children;

    return j;
}

static const char* PropertyTypeToStr(Deki::PropertyType type)
{
    switch (type)
    {
        case Deki::PropertyType::Int8: return "int8";
        case Deki::PropertyType::Int16: return "int16";
        case Deki::PropertyType::Int32: return "int32";
        case Deki::PropertyType::Int64: return "int64";
        case Deki::PropertyType::UInt8: return "uint8";
        case Deki::PropertyType::UInt16: return "uint16";
        case Deki::PropertyType::UInt32: return "uint32";
        case Deki::PropertyType::UInt64: return "uint64";
        case Deki::PropertyType::Float: return "float";
        case Deki::PropertyType::Double: return "double";
        case Deki::PropertyType::Bool: return "bool";
        case Deki::PropertyType::String: return "string";
        case Deki::PropertyType::Color: return "color";
        case Deki::PropertyType::Vector2: return "vector2";
        case Deki::PropertyType::Vector3: return "vector3";
        case Deki::PropertyType::Asset: return "asset";
        case Deki::PropertyType::Enum: return "enum";
        case Deki::PropertyType::ObjectRef: return "objectRef";
        case Deki::PropertyType::Array: return "array";
        case Deki::PropertyType::AssetRef: return "assetRef";
        default: return "unknown";
    }
}

nlohmann::json HandleResourceRead(const std::string& uri, EditorApp* editor)
{
    if (uri == "scene://current")
    {
        auto* scene = editor->GetCurrentScene();

        nlohmann::json objects = nlohmann::json::array();
        for (auto* obj : scene->GetObjects())
            objects.push_back(SerializeObjectForResource(obj));

        return {
            { "path", editor->GetCurrentScenePath() },
            { "objects", objects }
        };
    }
    else if (uri == "components://registry")
    {
        auto& reg = Deki::ComponentRegistry::Instance();
        nlohmann::json components = nlohmann::json::array();

        for (const auto* meta : reg.GetAllComponents())
        {
            if (meta->isAbstract) continue;

            nlohmann::json comp;
            comp["name"] = meta->name;
            comp["serializedName"] = meta->serializedName;
            if (meta->category) comp["category"] = meta->category;
            if (meta->description) comp["description"] = meta->description;

            nlohmann::json props = nlohmann::json::array();
            for (int i = 0; i < meta->propertyCount; ++i)
            {
                const auto& prop = meta->properties[i];
                nlohmann::json p;
                p["name"] = prop.name;
                p["type"] = PropertyTypeToStr(prop.type);
                if (prop.hasRange)
                {
                    p["min"] = prop.minValue;
                    p["max"] = prop.maxValue;
                }
                if (prop.tooltip) p["tooltip"] = prop.tooltip;
                if (prop.enumValues && prop.enumCount > 0)
                {
                    nlohmann::json vals = nlohmann::json::array();
                    for (int e = 0; e < prop.enumCount; ++e)
                        vals.push_back(prop.enumValues[e]);
                    p["enumValues"] = vals;
                }
                if (prop.type == Deki::PropertyType::Array)
                    p["elementType"] = PropertyTypeToStr(prop.elementType);
                props.push_back(p);
            }
            comp["properties"] = props;
            components.push_back(comp);
        }

        return { { "components", components } };
    }
    else if (uri == "project://settings")
    {
        auto* project = editor->GetProject();
        if (!project)
            return { { "error", "No project open" } };

        auto* display = DekiEditor::GetDisplaySettings();
        return {
            { "name", project->GetName() },
            { "path", project->GetPath() },
            { "displayWidth", display ? display->targetWidth : 320 },
            { "displayHeight", display ? display->targetHeight : 240 },
            { "colorFormat", display ? static_cast<int>(display->colorFormat) : 0 },
            { "targetFps", DekiEditor::GetCoreSettings() ? DekiEditor::GetCoreSettings()->targetFps : 60 },
            { "startupScene", project->GetStartupScene() }
        };
    }

    return { { "error", "Unknown resource: " + uri } };
}

nlohmann::json ListResources()
{
    return nlohmann::json::array({ { { "uri", "scene://current" },
                                     { "name", "Current Scene" },
                                     { "description", "Full hierarchy and property values of the currently open scene" },
                                     { "mimeType", "application/json" } },
                                   { { "uri", "components://registry" },
                                     { "name", "Component Registry" },
                                     { "description", "All registered component types with property schemas" },
                                     { "mimeType", "application/json" } },
                                   { { "uri", "project://settings" },
                                     { "name", "Project Settings" },
                                     { "description", "Display size, color format, and performance settings" },
                                     { "mimeType", "application/json" } } });
}

}  // namespace DekiEditor
