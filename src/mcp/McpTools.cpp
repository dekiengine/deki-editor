#include "McpTools.h"
#include "../services/FrameTiming.h"
#include "McpToolRegistry.h"
#include "../EditorApp.h"
#include <deki-editor/CommandHistory.h>
#include "../core/Commands.h"
#include <deki/Scene.h>
#include <deki/Object.h>
#include <deki/Component.h>
#include <deki/Math.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/SettingsRegistry.h>
#include <deki/reflection/Property.h>
#include "../core/commands/PropertyTypeHandlers.h"
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/AssetTypeRegistry.h>
#include <deki-editor/CliTool.h>
#include <deki-editor/EditorRegistry.h>
#include <deki-editor/Paths.h>
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace DekiEditor
{

// Helper: serialize a single Deki::Object to JSON (non-recursive)
static nlohmann::json SerializeObject(Deki::Object* obj)
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

    if (obj->GetParent())
        j["parentId"] = obj->GetParent()->GetId();

    // List components
    nlohmann::json comps = nlohmann::json::array();
    auto& registry = Deki::ComponentRegistry::Instance();
    for (auto* comp : obj->GetComponents())
    {
        const Deki::ComponentMeta* meta = registry.GetMeta(comp->GetType());
        if (meta)
        {
            comps.push_back({ { "type", meta->serializedName },
                              { "displayName", meta->name } });
        }
    }
    j["components"] = comps;

    // List children IDs
    nlohmann::json childIds = nlohmann::json::array();
    for (auto* child : obj->GetChildren())
        childIds.push_back(child->GetId());
    j["children"] = childIds;

    return j;
}

// Helper: recursively serialize object tree
static void SerializeObjectTree(Deki::Object* obj, nlohmann::json& arr)
{
    arr.push_back(SerializeObject(obj));
    for (auto* child : obj->GetChildren())
        SerializeObjectTree(child, arr);
}

// Helper: find component by type name on an object
static Deki::Component* FindComponentByType(Deki::Object* obj, const std::string& typeName)
{
    auto& registry = Deki::ComponentRegistry::Instance();
    for (auto* comp : obj->GetComponents())
    {
        const Deki::ComponentMeta* meta = registry.GetMeta(comp->GetType());
        if (meta && meta->serializedName == typeName)
            return comp;
    }
    return nullptr;
}

// Read one property as JSON.
//
// Enum and AssetRef need the property info, not just its offset, so they cannot
// go through the offset-only handler table: that table maps Enum to the
// size-agnostic CaptureEnum (reads 4 bytes off a possibly 1-byte enum, pulling
// in whatever follows it in the struct) and AssetRef to a no-op that always
// reports null. Both were visible over MCP as garbage enum values and sprites
// that render on screen while get_properties claims they are unset.
static nlohmann::json CapturePropertyValue(const Deki::Component* comp, const Deki::PropertyInfo& prop)
{
    if (prop.type == Deki::PropertyType::Enum)
        return CaptureEnumSized(comp, prop.offset, prop.enumSize);

    if (prop.type == Deki::PropertyType::AssetRef)
    {
        const auto& ref = *reinterpret_cast<const Deki::AssetRefBase*>(
            reinterpret_cast<const char*>(comp) + prop.offset);
        if (ref.guid.empty())
            return nullptr;
        nlohmann::json j;
        j["guid"] = ref.guid;
        if (!ref.source.empty())
            j["source"] = ref.source;
        return j;
    }

    return GetPropertyTypeHandler(prop.type).capture(comp, prop.offset);
}

// Resolve an enum argument to its index. Accepts the integer index or the name
// as listed in get_properties' enumValues, which is the only form a caller can
// discover without reading the C++ header.
static int ResolveEnumValue(const Deki::PropertyInfo& prop, const nlohmann::json& value)
{
    if (value.is_number_integer())
        return value.get<int>();

    if (value.is_string())
    {
        const std::string name = value.get<std::string>();
        for (int i = 0; i < prop.enumCount; ++i)
        {
            if (prop.enumValues && prop.enumValues[i] && name == prop.enumValues[i])
                return i;
        }
        throw std::runtime_error("Unknown value '" + name + "' for enum property '" +
                                 std::string(prop.name) + "'");
    }

    throw std::runtime_error("Enum property '" + std::string(prop.name) +
                             "' expects an index or one of its enumValues names");
}

// Helper: find property info by name on a component meta
static const Deki::PropertyInfo* FindProperty(const Deki::ComponentMeta* meta, const std::string& propName)
{
    for (int i = 0; i < meta->propertyCount; ++i)
    {
        if (meta->properties[i].name == propName)
            return &meta->properties[i];
    }
    return nullptr;
}

// Helper: get property type as string
static const char* PropertyTypeToString(Deki::PropertyType type)
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

void RegisterAllTools(McpToolRegistry& registry, EditorApp* editor)
{
    // Firmware build / flash / toolchain tools live in McpTools.Build.cpp
    RegisterBuildTools(registry, editor);

    // Project / package manager / rebuild / capture live in McpTools.Editor.cpp
    RegisterEditorTools(registry, editor);

    // =========================================================================
    // Object Tools
    // =========================================================================

    registry.Register({ "create_object",
                        "Create a new DekiObject in the current scene. Returns the new object's ID.",
                        { { "type", "object" }, { "properties", { { "name", { { "type", "string" }, { "description", "Object name (default: \"DekiObject\")" } } }, { "parent_id", { { "type", "integer" }, { "description", "Parent object ID (optional, creates as root if omitted)" } } } } } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            auto* scene = editor->GetCurrentScene();

                            std::string name = args.value("name", "DekiObject");
                            Deki::Object* createdObj = nullptr;

                            auto cmd = std::make_unique<CreateObjectCommand>(scene, name,
                                                                             [&createdObj](Deki::Object* obj)
                                                                             { createdObj = obj; });
                            CommandHistory::Instance().Execute(std::move(cmd));

                            if (!createdObj) throw std::runtime_error("Failed to create object");

                            // Reparent if parent_id specified
                            if (args.contains("parent_id"))
                            {
                                uint32_t parentId = args["parent_id"].get<uint32_t>();
                                Deki::Object* parent = scene->FindDekiObject(parentId);
                                if (parent)
                                {
                                    CommandHistory::Instance().Execute(
                                        std::make_unique<ReparentObjectCommand>(createdObj, parent));
                                }
                            }

                            return SerializeObject(createdObj);
                        } });

    registry.Register({ "delete_object",
                        "Delete a DekiObject from the current scene.",
                        { { "type", "object" }, { "properties", { { "object_id", { { "type", "integer" }, { "description", "ID of the object to delete" } } } } }, { "required", nlohmann::json::array({ "object_id" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            auto* scene = editor->GetCurrentScene();

                            uint32_t objId = args["object_id"].get<uint32_t>();
                            Deki::Object* obj = scene->FindDekiObject(objId);
                            if (!obj) throw std::runtime_error("Object not found: " + std::to_string(objId));

                            // Clear selection if this object is selected
                            if (editor->IsSelected(obj))
                                editor->RemoveFromSelection(obj);

                            CommandHistory::Instance().Execute(
                                std::make_unique<DeleteObjectCommand>(scene, obj));

                            return { { "deleted", objId } };
                        } });

    // The copy's name, place and parent can be given in the same call. A
    // --script is a fixed list of calls, so it cannot pass the id this returns
    // on to a later set_position: without these, laying out copies from the
    // command line was not possible at all.
    registry.Register({ "duplicate_object",
                        "Duplicate a DekiObject in the current scene. Returns the new object. Optionally names "
                        "the copy, places it (x and y together, local position) and puts it under another "
                        "parent, in one step.",
                        { { "type", "object" },
                          { "properties",
                            { { "object_id", { { "type", "integer" }, { "description", "ID of the object to duplicate" } } },
                              { "name", { { "type", "string" }, { "description", "Name for the copy (default: the duplicate's own)" } } },
                              { "x", { { "type", "number" }, { "description", "Local X of the copy; give with y" } } },
                              { "y", { { "type", "number" }, { "description", "Local Y of the copy; give with x" } } },
                              { "parent_id", { { "type", "integer" }, { "description", "Parent for the copy (default: the original's parent)" } } } } },
                          { "required", nlohmann::json::array({ "object_id" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            auto* scene = editor->GetCurrentScene();

                            uint32_t objId = args["object_id"].get<uint32_t>();
                            Deki::Object* obj = scene->FindDekiObject(objId);
                            if (!obj) throw std::runtime_error("Object not found: " + std::to_string(objId));

                            if (args.contains("x") != args.contains("y"))
                                throw std::runtime_error("duplicate_object takes x and y together");

                            Deki::Object* parent = obj->GetParent();
                            if (args.contains("parent_id") && !args["parent_id"].is_null())
                            {
                                const uint32_t parentId = args["parent_id"].get<uint32_t>();
                                parent = scene->FindDekiObject(parentId);
                                if (!parent) throw std::runtime_error("Parent not found: " + std::to_string(parentId));
                            }

                            Deki::Object* duplicated = nullptr;
                            CommandHistory::Instance().Execute(
                                std::make_unique<DuplicateObjectCommand>(scene, obj, parent,
                                                                         [&duplicated](Deki::Object* dup)
                                                                         { duplicated = dup; }));

                            if (!duplicated) throw std::runtime_error("Failed to duplicate object");

                            if (args.contains("name") && args["name"].is_string())
                                CommandHistory::Instance().Execute(
                                    std::make_unique<RenameObjectCommand>(duplicated, args["name"].get<std::string>()));
                            if (args.contains("x"))
                                CommandHistory::Instance().ExecuteNoMerge(std::make_unique<SetPositionCommand>(
                                    duplicated, args["x"].get<float>(), args["y"].get<float>()));

                            return SerializeObject(duplicated);
                        } });

    registry.Register({ "get_object",
                        "Get details of a DekiObject by ID.",
                        { { "type", "object" }, { "properties", { { "object_id", { { "type", "integer" }, { "description", "ID of the object" } } } } }, { "required", nlohmann::json::array({ "object_id" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            auto* scene = editor->GetCurrentScene();

                            uint32_t objId = args["object_id"].get<uint32_t>();
                            Deki::Object* obj = scene->FindDekiObject(objId);
                            if (!obj) throw std::runtime_error("Object not found: " + std::to_string(objId));

                            return SerializeObject(obj);
                        } });

    registry.Register({ "list_objects",
                        "List all objects in the current scene.",
                        { { "type", "object" }, { "properties", nlohmann::json::object() } },
                        [editor](const nlohmann::json&) -> nlohmann::json
                        {
                            auto* scene = editor->GetCurrentScene();

                            nlohmann::json objects = nlohmann::json::array();
                            for (auto* obj : scene->GetObjects())
                                SerializeObjectTree(obj, objects);

                            return { { "objects", objects }, { "count", objects.size() } };
                        } });

    registry.Register({ "set_position",
                        "Set the local position of a DekiObject.",
                        { { "type", "object" }, { "properties", { { "object_id", { { "type", "integer" }, { "description", "ID of the object" } } }, { "x", { { "type", "number" }, { "description", "X position" } } }, { "y", { { "type", "number" }, { "description", "Y position" } } } } }, { "required", nlohmann::json::array({ "object_id", "x", "y" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            auto* scene = editor->GetCurrentScene();

                            uint32_t objId = args["object_id"].get<uint32_t>();
                            Deki::Object* obj = scene->FindDekiObject(objId);
                            if (!obj) throw std::runtime_error("Object not found: " + std::to_string(objId));

                            float x = args["x"].get<float>();
                            float y = args["y"].get<float>();

                            CommandHistory::Instance().ExecuteNoMerge(
                                std::make_unique<SetPositionCommand>(obj, x, y));

                            return { { "id", objId }, { "x", x }, { "y", y } };
                        } });

    registry.Register({ "set_rotation",
                        "Set the rotation of a DekiObject in degrees.",
                        { { "type", "object" }, { "properties", { { "object_id", { { "type", "integer" }, { "description", "ID of the object" } } }, { "angle", { { "type", "number" }, { "description", "Rotation angle in degrees" } } } } }, { "required", nlohmann::json::array({ "object_id", "angle" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            auto* scene = editor->GetCurrentScene();

                            uint32_t objId = args["object_id"].get<uint32_t>();
                            Deki::Object* obj = scene->FindDekiObject(objId);
                            if (!obj) throw std::runtime_error("Object not found: " + std::to_string(objId));

                            // MCP API takes degrees; storage is radians.
                            float angleDeg = args["angle"].get<float>();
                            float angleRad = angleDeg * Deki::Math::kDegToRad;
                            float oldAngleRad = obj->GetLocalRotation();

                            CommandHistory::Instance().ExecuteNoMerge(
                                std::make_unique<SetRotationCommand>(obj, oldAngleRad, angleRad));

                            return { { "id", objId }, { "rotation", angleDeg } };
                        } });

    registry.Register({ "set_scale",
                        "Set the scale of a DekiObject.",
                        { { "type", "object" }, { "properties", { { "object_id", { { "type", "integer" }, { "description", "ID of the object" } } }, { "sx", { { "type", "number" }, { "description", "Scale X" } } }, { "sy", { { "type", "number" }, { "description", "Scale Y" } } } } }, { "required", nlohmann::json::array({ "object_id", "sx", "sy" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            auto* scene = editor->GetCurrentScene();

                            uint32_t objId = args["object_id"].get<uint32_t>();
                            Deki::Object* obj = scene->FindDekiObject(objId);
                            if (!obj) throw std::runtime_error("Object not found: " + std::to_string(objId));

                            float sx = args["sx"].get<float>();
                            float sy = args["sy"].get<float>();
                            float oldSx = obj->GetScaleX();
                            float oldSy = obj->GetScaleY();

                            CommandHistory::Instance().ExecuteNoMerge(
                                std::make_unique<SetScaleCommand>(obj, oldSx, oldSy, sx, sy));

                            return { { "id", objId }, { "scaleX", sx }, { "scaleY", sy } };
                        } });

    registry.Register({ "reparent_object",
                        "Change the parent of a DekiObject. Pass null parent_id to unparent.",
                        { { "type", "object" }, { "properties", { { "object_id", { { "type", "integer" }, { "description", "ID of the object to reparent" } } }, { "parent_id", { { "type", "integer" }, { "description", "ID of the new parent (null to unparent)" } } } } }, { "required", nlohmann::json::array({ "object_id" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            auto* scene = editor->GetCurrentScene();

                            uint32_t objId = args["object_id"].get<uint32_t>();
                            Deki::Object* obj = scene->FindDekiObject(objId);
                            if (!obj) throw std::runtime_error("Object not found: " + std::to_string(objId));

                            Deki::Object* newParent = nullptr;
                            if (args.contains("parent_id") && !args["parent_id"].is_null())
                            {
                                uint32_t parentId = args["parent_id"].get<uint32_t>();
                                newParent = scene->FindDekiObject(parentId);
                                if (!newParent) throw std::runtime_error("Parent not found: " + std::to_string(parentId));
                            }

                            CommandHistory::Instance().Execute(
                                std::make_unique<ReparentObjectCommand>(obj, newParent));

                            return { { "id", objId }, { "parentId", newParent ? nlohmann::json(newParent->GetId()) : nlohmann::json(nullptr) } };
                        } });

    registry.Register({ "rename_object",
                        "Rename a DekiObject.",
                        { { "type", "object" }, { "properties", { { "object_id", { { "type", "integer" }, { "description", "ID of the object" } } }, { "name", { { "type", "string" }, { "description", "New name" } } } } }, { "required", nlohmann::json::array({ "object_id", "name" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            auto* scene = editor->GetCurrentScene();

                            uint32_t objId = args["object_id"].get<uint32_t>();
                            Deki::Object* obj = scene->FindDekiObject(objId);
                            if (!obj) throw std::runtime_error("Object not found: " + std::to_string(objId));

                            std::string newName = args["name"].get<std::string>();
                            CommandHistory::Instance().Execute(
                                std::make_unique<RenameObjectCommand>(obj, newName));

                            return { { "id", objId }, { "name", newName } };
                        } });

    // =========================================================================
    // Component Tools
    // =========================================================================

    registry.Register({ "add_component",
                        "Add a component to a DekiObject. Use list_component_types to get available types.",
                        { { "type", "object" }, { "properties", { { "object_id", { { "type", "integer" }, { "description", "ID of the object" } } }, { "component_type", { { "type", "string" }, { "description", "Component serialized name (from list_component_types)" } } } } }, { "required", nlohmann::json::array({ "object_id", "component_type" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            auto* scene = editor->GetCurrentScene();

                            uint32_t objId = args["object_id"].get<uint32_t>();
                            Deki::Object* obj = scene->FindDekiObject(objId);
                            if (!obj) throw std::runtime_error("Object not found: " + std::to_string(objId));

                            std::string typeName = args["component_type"].get<std::string>();

                            // Verify component type exists
                            auto& reg = Deki::ComponentRegistry::Instance();
                            const auto* meta = reg.GetMeta(typeName);
                            if (!meta) throw std::runtime_error("Unknown component type: " + typeName);

                            CommandHistory::Instance().Execute(
                                std::make_unique<AddComponentCommand>(obj, typeName));

                            return { { "id", objId }, { "added", typeName }, { "displayName", meta->name } };
                        } });

    registry.Register({ "remove_component",
                        "Remove a component from a DekiObject.",
                        { { "type", "object" }, { "properties", { { "object_id", { { "type", "integer" }, { "description", "ID of the object" } } }, { "component_type", { { "type", "string" }, { "description", "Component serialized name to remove" } } } } }, { "required", nlohmann::json::array({ "object_id", "component_type" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            auto* scene = editor->GetCurrentScene();

                            uint32_t objId = args["object_id"].get<uint32_t>();
                            Deki::Object* obj = scene->FindDekiObject(objId);
                            if (!obj) throw std::runtime_error("Object not found: " + std::to_string(objId));

                            std::string typeName = args["component_type"].get<std::string>();
                            Deki::Component* comp = FindComponentByType(obj, typeName);
                            if (!comp) throw std::runtime_error("Component not found on object: " + typeName);

                            CommandHistory::Instance().Execute(
                                std::make_unique<RemoveComponentCommand>(obj, comp));

                            return { { "id", objId }, { "removed", typeName } };
                        } });

    registry.Register({ "set_property",
                        "Set a component property value. Use get_properties to see available properties and types. "
                        "For an assetRef property pass the asset GUID as a string (see list_assets), or an object "
                        "{\"guid\": \"...\", \"source\": \"...\"}; pass null or \"\" to clear it.",
                        { { "type", "object" }, { "properties", { { "object_id", { { "type", "integer" }, { "description", "ID of the object" } } }, { "component_type", { { "type", "string" }, { "description", "Component serialized name" } } }, { "property_name", { { "type", "string" }, { "description", "Property name" } } }, { "value", { { "description", "New value (type depends on property)" } } } } }, { "required", nlohmann::json::array({ "object_id", "component_type", "property_name", "value" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            auto* scene = editor->GetCurrentScene();

                            uint32_t objId = args["object_id"].get<uint32_t>();
                            Deki::Object* obj = scene->FindDekiObject(objId);
                            if (!obj) throw std::runtime_error("Object not found: " + std::to_string(objId));

                            std::string typeName = args["component_type"].get<std::string>();
                            Deki::Component* comp = FindComponentByType(obj, typeName);
                            if (!comp) throw std::runtime_error("Component not found: " + typeName);

                            auto& reg = Deki::ComponentRegistry::Instance();
                            const auto* meta = reg.GetMeta(typeName);
                            if (!meta) throw std::runtime_error("Unknown component type: " + typeName);

                            std::string propName = args["property_name"].get<std::string>();
                            const Deki::PropertyInfo* prop = FindProperty(meta, propName);
                            if (!prop) throw std::runtime_error("Property not found: " + propName);

                            // AssetRef is a (guid, source) pair, not a plain value, and the
                            // generic property command's handler table deliberately no-ops on
                            // it. Routing it here is what makes "give this sprite a texture"
                            // reachable from automation at all - otherwise the call silently
                            // succeeds and leaves the reference null.
                            if (prop->type == Deki::PropertyType::AssetRef)
                            {
                                const auto& value = args["value"];
                                std::string guid, source;
                                if (value.is_string())
                                {
                                    guid = value.get<std::string>();
                                }
                                else if (value.is_object())
                                {
                                    guid = value.value("guid", "");
                                    source = value.value("source", "");
                                }
                                else if (!value.is_null())
                                {
                                    throw std::runtime_error("assetRef property '" + propName +
                                                             "' expects a GUID string, an object, or null");
                                }

                                CommandHistory::Instance().ExecuteNoMerge(
                                    std::make_unique<ModifyAssetRefCommand>(comp, *prop, guid, source));

                                return { { "id", objId }, { "component", typeName }, { "property", propName }, { "value", { { "guid", guid }, { "source", source } } } };
                            }

                            // The command stores enums by index; accept the name too, since that
                            // is what get_properties advertises in enumValues.
                            nlohmann::json value = args["value"];
                            if (prop->type == Deki::PropertyType::Enum)
                                value = ResolveEnumValue(*prop, value);

                            CommandHistory::Instance().ExecuteNoMerge(
                                std::make_unique<ModifyComponentPropertyCommand>(comp, *prop, value));

                            return { { "id", objId }, { "component", typeName }, { "property", propName }, { "value", value } };
                        } });

    registry.Register({ "get_properties",
                        "Get all property values of a component on an object.",
                        { { "type", "object" }, { "properties", { { "object_id", { { "type", "integer" }, { "description", "ID of the object" } } }, { "component_type", { { "type", "string" }, { "description", "Component serialized name" } } } } }, { "required", nlohmann::json::array({ "object_id", "component_type" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            auto* scene = editor->GetCurrentScene();

                            uint32_t objId = args["object_id"].get<uint32_t>();
                            Deki::Object* obj = scene->FindDekiObject(objId);
                            if (!obj) throw std::runtime_error("Object not found: " + std::to_string(objId));

                            std::string typeName = args["component_type"].get<std::string>();
                            Deki::Component* comp = FindComponentByType(obj, typeName);
                            if (!comp) throw std::runtime_error("Component not found: " + typeName);

                            auto& reg = Deki::ComponentRegistry::Instance();
                            const auto* meta = reg.GetMeta(typeName);
                            if (!meta) throw std::runtime_error("Unknown component type: " + typeName);

                            nlohmann::json properties = nlohmann::json::object();
                            for (int i = 0; i < meta->propertyCount; ++i)
                            {
                                const auto& prop = meta->properties[i];
                                nlohmann::json propInfo;
                                propInfo["type"] = PropertyTypeToString(prop.type);

                                if (prop.type == Deki::PropertyType::Array)
                                    propInfo["value"] = CaptureArray(comp, prop);
                                else
                                    propInfo["value"] = CapturePropertyValue(comp, prop);

                                if (prop.hasRange)
                                {
                                    propInfo["min"] = prop.minValue;
                                    propInfo["max"] = prop.maxValue;
                                }
                                if (prop.tooltip)
                                    propInfo["tooltip"] = prop.tooltip;
                                if (prop.enumValues && prop.enumCount > 0)
                                {
                                    nlohmann::json enumVals = nlohmann::json::array();
                                    for (int e = 0; e < prop.enumCount; ++e)
                                        enumVals.push_back(prop.enumValues[e]);
                                    propInfo["enumValues"] = enumVals;
                                }

                                properties[prop.name] = propInfo;
                            }

                            return { { "id", objId }, { "component", typeName }, { "properties", properties } };
                        } });

    registry.Register({ "list_component_types",
                        "List all available component types, optionally filtered by category.",
                        { { "type", "object" }, { "properties", { { "category", { { "type", "string" }, { "description", "Filter by category (optional)" } } } } } },
                        [](const nlohmann::json& args) -> nlohmann::json
                        {
                            auto& reg = Deki::ComponentRegistry::Instance();

                            std::vector<const Deki::ComponentMeta*> components;
                            if (args.contains("category") && args["category"].is_string())
                                components = reg.GetComponentsByCategory(args["category"].get<std::string>());
                            else
                                components = std::vector<const Deki::ComponentMeta*>(
                                    reg.GetAllComponents().begin(), reg.GetAllComponents().end());

                            nlohmann::json result = nlohmann::json::array();
                            for (const auto* meta : components)
                            {
                                if (meta->isAbstract) continue;

                                nlohmann::json comp;
                                comp["name"] = meta->name;
                                comp["serializedName"] = meta->serializedName;
                                if (meta->category)
                                    comp["category"] = meta->category;
                                if (meta->description)
                                    comp["description"] = meta->description;
                                comp["propertyCount"] = meta->propertyCount;

                                // List property names and types
                                nlohmann::json props = nlohmann::json::array();
                                for (int i = 0; i < meta->propertyCount; ++i)
                                {
                                    props.push_back({ { "name", meta->properties[i].name },
                                                      { "type", PropertyTypeToString(meta->properties[i].type) } });
                                }
                                comp["properties"] = props;

                                result.push_back(comp);
                            }

                            // Also return categories
                            auto categories = reg.GetCategories();

                            return { { "components", result }, { "count", result.size() }, { "categories", categories } };
                        } });

    // =========================================================================
    // Editor Tools
    // =========================================================================

    registry.Register({ "get_selection",
                        "Get the currently selected objects in the editor.",
                        { { "type", "object" }, { "properties", nlohmann::json::object() } },
                        [editor](const nlohmann::json&) -> nlohmann::json
                        {
                            const auto& selected = editor->GetSelectedObjects();
                            nlohmann::json objects = nlohmann::json::array();
                            for (auto* obj : selected)
                                objects.push_back({ { "id", obj->GetId() }, { "name", obj->GetName() } });
                            return { { "selected", objects }, { "count", objects.size() } };
                        } });

    registry.Register({ "set_selection",
                        "Set the editor selection to the specified object IDs.",
                        { { "type", "object" }, { "properties", { { "object_ids", { { "type", "array" }, { "items", { { "type", "integer" } } }, { "description", "Array of object IDs to select" } } } } }, { "required", nlohmann::json::array({ "object_ids" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            auto* scene = editor->GetCurrentScene();

                            std::vector<Deki::Object*> objects;
                            for (const auto& id : args["object_ids"])
                            {
                                Deki::Object* obj = scene->FindDekiObject(id.get<uint32_t>());
                                if (obj) objects.push_back(obj);
                            }

                            editor->SetSelectedObjects(objects);
                            return { { "selected", objects.size() } };
                        } });

    registry.Register({ "undo",
                        "Undo the last editor action.",
                        { { "type", "object" }, { "properties", nlohmann::json::object() } },
                        [](const nlohmann::json&) -> nlohmann::json
                        {
                            auto& history = CommandHistory::Instance();
                            if (!history.CanUndo())
                                return { { "success", false }, { "reason", "Nothing to undo" } };
                            std::string desc = history.GetUndoDescription();
                            history.Undo();
                            return { { "success", true }, { "undone", desc } };
                        } });

    registry.Register({ "redo",
                        "Redo the last undone action.",
                        { { "type", "object" }, { "properties", nlohmann::json::object() } },
                        [](const nlohmann::json&) -> nlohmann::json
                        {
                            auto& history = CommandHistory::Instance();
                            if (!history.CanRedo())
                                return { { "success", false }, { "reason", "Nothing to redo" } };
                            std::string desc = history.GetRedoDescription();
                            history.Redo();
                            return { { "success", true }, { "redone", desc } };
                        } });

    registry.Register({ "save_scene",
                        "Save the current scene to disk. Fails if it has never been saved - "
                        "use save_scene_as to give it a path first.",
                        { { "type", "object" }, { "properties", nlohmann::json::object() } },
                        [editor](const nlohmann::json&) -> nlohmann::json
                        {
                            if (!editor->SaveScene())
                                throw std::runtime_error(
                                    editor->GetCurrentScenePath().empty()
                                        ? "Scene has no path yet - use save_scene_as"
                                        : "Failed to write " + editor->GetCurrentScenePath());
                            return { { "saved", editor->GetCurrentScenePath() } };
                        } });

    registry.Register({ "new_scene",
                        "Create a new empty scene, replacing the current one.",
                        { { "type", "object" }, { "properties", nlohmann::json::object() } },
                        [editor](const nlohmann::json&) -> nlohmann::json
                        {
                            editor->NewScene();
                            return { { "success", true } };
                        } });

    registry.Register({ "save_scene_as",
                        "Save the current scene to a file path. A relative path is taken as "
                        "project-relative; missing directories are created.",
                        { { "type", "object" }, { "properties", { { "path", { { "type", "string" }, { "description", "Destination path, absolute or project-relative" } } } } }, { "required", nlohmann::json::array({ "path" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            std::string path = args["path"].get<std::string>();
                            if (!editor->SaveSceneAs(path))
                                throw std::runtime_error("Failed to save scene to " +
                                                         editor->ResolveProjectPath(path));
                            return { { "success", true }, { "path", editor->GetCurrentScenePath() } };
                        } });

    registry.Register({ "open_scene",
                        "Open an existing scene file. A relative path is taken as "
                        "project-relative. Fails if the file cannot be read, or if the open is "
                        "waiting on the unsaved-changes dialog.",
                        { { "type", "object" }, { "properties", { { "path", { { "type", "string" }, { "description", "Scene path, absolute or project-relative" } } } } }, { "required", nlohmann::json::array({ "path" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            std::string path = args["path"].get<std::string>();
                            if (!editor->OpenScene(path))
                                throw std::runtime_error("Could not open " + editor->ResolveProjectPath(path) +
                                                         " (missing, unreadable, or blocked by an open dialog)");
                            return { { "success", true }, { "path", editor->GetCurrentScenePath() } };
                        } });

    registry.Register({ "get_scene_info",
                        "Get information about the currently open scene.",
                        { { "type", "object" }, { "properties", nlohmann::json::object() } },
                        [editor](const nlohmann::json&) -> nlohmann::json
                        {
                            auto* scene = editor->GetCurrentScene();

                            // Count all objects recursively
                            nlohmann::json objects = nlohmann::json::array();
                            for (auto* obj : scene->GetObjects())
                                SerializeObjectTree(obj, objects);

                            return {
                                { "path", editor->GetCurrentScenePath() },
                                { "objectCount", objects.size() },
                                { "rootObjectCount", scene->GetObjects().size() },
                                { "dirty", CommandHistory::Instance().IsDirty() }
                            };
                        } });

    registry.Register({ "frame_timing",
                        "Where the editor's frame time goes. The first call (or reset: true) starts "
                        "counting; a later call reports, per frame: the average and longest frame, "
                        "the main loop's phases, and every panel and custom editor, slowest first. "
                        "Times nest (a panel includes its custom editors), and the swap phase is "
                        "time spent waiting for vsync, not work.",
                        { { "type", "object" },
                          { "properties",
                            { { "reset", { { "type", "boolean" }, { "description", "Start counting again from now." } } },
                              { "top", { { "type", "integer" }, { "description", "How many entries to list (default 25)." } } } } } },
                        [](const nlohmann::json& args) -> nlohmann::json
                        {
                            if (!FrameTiming::IsCollecting() || args.value("reset", false))
                            {
                                FrameTiming::Restart();
                                return { { "collecting", true }, { "note", "counting started; call again to read it" } };
                            }
                            nlohmann::json report = FrameTiming::Report();
                            const size_t top = static_cast<size_t>(std::max(1, args.value("top", 25)));
                            if (report["items"].size() > top)
                                report["items"].erase(report["items"].begin() + static_cast<std::ptrdiff_t>(top),
                                                      report["items"].end());
                            return report;
                        } });

    registry.Register({ "get_project_info",
                        "Get information about the current project.",
                        { { "type", "object" }, { "properties", nlohmann::json::object() } },
                        [editor](const nlohmann::json&) -> nlohmann::json
                        {
                            auto* project = editor->GetProject();
                            if (!project)
                                return { { "hasProject", false } };

                            return {
                                { "hasProject", true },
                                { "name", project->GetName() },
                                { "path", project->GetPath() },
                                { "targetFps", DekiEditor::GetCoreSettings() ? DekiEditor::GetCoreSettings()->targetFps : 60 },
                                { "startupScene", project->GetStartupScene() }
                            };
                        } });

    // A new asset of any registered type, with the content the Asset Browser's
    // Create menu gives it, imported at once. The type's own tools (a node
    // graph's, say) then edit it.
    registry.Register({ "create_asset",
                        "Create a new asset of a registered type (e.g. \"FsmGraph\") at a project-relative path, "
                        "with the default content the Asset Browser's Create menu gives it, and import it. "
                        "Refuses to overwrite.",
                        { { "type", "object" },
                          { "properties",
                            { { "asset_type", { { "type", "string" }, { "description", "The type's name, as its asset type editor registers it" } } },
                              { "path", { { "type", "string" }, { "description", "Project-relative path, with the type's extension (e.g. assets/fsm/bob.asset)" } } } } },
                          { "required", nlohmann::json::array({ "asset_type", "path" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            namespace fs = std::filesystem;
                            auto* project = editor->GetProject();
                            auto* pipeline = editor->GetAssetPipeline();
                            if (!project || !pipeline) throw std::runtime_error("No project open");

                            const std::string typeName = args["asset_type"].get<std::string>();
                            const AssetTypeHandler* handler = AssetTypeRegistry::Instance().GetHandler(typeName);
                            if (!handler) throw std::runtime_error("No asset type '" + typeName + "' is registered");
                            if (handler->defaultContent.empty())
                                throw std::runtime_error("Asset type '" + typeName + "' has no default content to create from");

                            const std::string rel = args["path"].get<std::string>();
                            const std::string ext = fs::path(rel).extension().string();
                            if (std::find(handler->extensions.begin(), handler->extensions.end(), ext) == handler->extensions.end())
                                throw std::runtime_error("'" + rel + "' does not have a " + typeName + " extension");
                            const fs::path abs = fs::path(project->GetPath()) / rel;
                            if (fs::exists(abs)) throw std::runtime_error("'" + rel + "' already exists");

                            std::error_code ec;
                            fs::create_directories(abs.parent_path(), ec);
                            {
                                std::ofstream out(abs, std::ios::binary);
                                out << handler->defaultContent;
                                if (!out) throw std::runtime_error("Could not write '" + rel + "'");
                            }
                            pipeline->RefreshAsset(rel);

                            const AssetInfo* info = pipeline->GetAssetInfo(rel);
                            return { { "path", rel }, { "assetType", typeName },
                                     { "guid", info ? info->guid : std::string() } };
                        } });

    // What double-clicking an asset in the Asset Browser does: open it in the
    // tool window that edits its type (a node graph in the Node Graph window).
    registry.Register({ "open_asset",
                        "Open an asset in the editor window that edits it, as double-clicking it in the Asset "
                        "Browser does (e.g. a node graph in the Node Graph window).",
                        { { "type", "object" },
                          { "properties", { { "path", { { "type", "string" }, { "description", "Project-relative path" } } } } },
                          { "required", nlohmann::json::array({ "path" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            namespace fs = std::filesystem;
                            auto* project = editor->GetProject();
                            auto* pipeline = editor->GetAssetPipeline();
                            if (!project || !pipeline) throw std::runtime_error("No project open");
                            if (!editor->GetToolHost()) throw std::runtime_error("No tool windows in this session");

                            const std::string rel = args["path"].get<std::string>();
                            const fs::path abs = fs::path(project->GetPath()) / rel;
                            if (!fs::exists(abs)) throw std::runtime_error("'" + rel + "' does not exist");
                            const AssetInfo* info = pipeline->GetAssetInfo(rel);
                            const std::string cache = info ? GetCachePath(project->GetPath(), info->guid) : std::string();
                            if (!editor->GetToolHost()->OpenToolForFile(abs.string().c_str(), cache.c_str()))
                                throw std::runtime_error("No editor window opens '" + rel + "'");
                            return { { "path", rel }, { "opened", true } };
                        } });

    registry.Register({ "select_asset",
                        "Select an asset, as clicking it in the Asset Browser does: the inspector then shows "
                        "its settings (a texture's format, a model's, ...). Give the project-relative path or "
                        "the GUID.",
                        { { "type", "object" },
                          { "properties",
                            { { "path", { { "type", "string" }, { "description", "Project-relative path" } } },
                              { "guid", { { "type", "string" }, { "description", "Asset GUID" } } } } } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            namespace fs = std::filesystem;
                            auto* project = editor->GetProject();
                            auto* pipeline = editor->GetAssetPipeline();
                            if (!project || !pipeline) throw std::runtime_error("No project open");

                            const AssetInfo* info = nullptr;
                            if (args.contains("path") && args["path"].is_string())
                                info = pipeline->GetAssetInfo(args["path"].get<std::string>());
                            else if (args.contains("guid") && args["guid"].is_string())
                                info = pipeline->GetAssetInfoByGuid(args["guid"].get<std::string>());
                            else
                                throw std::runtime_error("select_asset takes \"path\" or \"guid\"");
                            if (!info)
                                throw std::runtime_error("no such asset in this project");

                            // The Asset Browser's own form: absolute, forward slashes.
                            std::string abs = pipeline->GetAbsolutePath(info->path);
                            std::replace(abs.begin(), abs.end(), '\\', '/');
                            const std::string cache = GetCachePath(project->GetPath(), info->guid);
                            editor->SetSelectedAsset(abs, cache, fs::exists(cache), info->guid);
                            return { { "path", info->path }, { "guid", info->guid }, { "selected", true } };
                        } });

    // Tools the loaded packages add (CliTool). Looked up by name on every call
    // rather than held, so a package that unloads takes its tools with it.
    registry.SetDynamicSource(
        [editor]() -> std::vector<McpToolDescriptor>
        {
            std::vector<McpToolDescriptor> tools;
            for (const auto& factory : DekiEditorInternal::EditorRegistry::Instance().GetAllFactories())
            {
                if (!factory.prototype || factory.prototype->GetEditorType() != EditorType::CliTool)
                    continue;
                auto* tool = static_cast<CliTool*>(factory.prototype);
                McpToolDescriptor d;
                d.name = tool->GetToolName();
                d.description = tool->GetToolDescription();
                d.inputSchema = nlohmann::json::parse(tool->GetInputSchema(), nullptr, /*allow_exceptions=*/false);
                if (d.inputSchema.is_discarded())
                    d.inputSchema = { { "type", "object" } };
                const std::string name = d.name;
                d.handler = [editor, name](const nlohmann::json& args) -> nlohmann::json
                {
                    CliTool* found = nullptr;
                    for (const auto& f : DekiEditorInternal::EditorRegistry::Instance().GetAllFactories())
                        if (f.prototype && f.prototype->GetEditorType() == EditorType::CliTool &&
                            name == static_cast<CliTool*>(f.prototype)->GetToolName())
                            found = static_cast<CliTool*>(f.prototype);
                    if (!found) throw std::runtime_error("The package providing '" + name + "' is no longer loaded");

                    CliToolContext context;
                    if (auto* project = editor->GetProject())
                        context.projectPath = project->GetPath();
                    context.refreshAsset = [editor](const std::string& rel)
                    {
                        if (auto* pipeline = editor->GetAssetPipeline())
                            pipeline->RefreshAsset(rel);
                    };

                    std::string result, error;
                    if (!found->Run(context, args.dump(), result, error))
                        throw std::runtime_error(error.empty() ? name + " failed" : error);
                    nlohmann::json value = nlohmann::json::parse(result, nullptr, /*allow_exceptions=*/false);
                    if (value.is_discarded())
                        throw std::runtime_error(name + " returned something that is not JSON");
                    return value;
                };
                tools.push_back(std::move(d));
            }
            return tools;
        });

    // Project settings are components too (one live instance per section in
    // the SettingsRegistry), written with the same per-type handlers as a
    // scene property; then the project is saved, as the Project Settings
    // panel does after every edit.
    //
    // Written directly, not through CommandHistory: that history is the open
    // scene's, and an entry there marks the SCENE unsaved, so the next
    // open_scene stopped at a save-changes prompt no command line can answer.
    registry.Register({ "set_project_setting",
                        "Set a project setting: a property of one of the Project Settings sections (e.g. "
                        "section \"Core\", property \"targetFps\"), then save the project. Enum values "
                        "take their index or name.",
                        { { "type", "object" },
                          { "properties",
                            { { "section", { { "type", "string" }, { "description", "Settings section, as the Project Settings panel names it" } } },
                              { "property", { { "type", "string" }, { "description", "Property name within the section" } } },
                              { "value", { { "description", "New value (type depends on the property)" } } } } },
                          { "required", nlohmann::json::array({ "section", "property", "value" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            auto* project = editor->GetProject();
                            if (!project) throw std::runtime_error("No project open");

                            const std::string section = args["section"].get<std::string>();
                            const Deki::SettingsTypeInfo* info = nullptr;
                            std::string known;
                            for (const auto& entry : Deki::SettingsRegistry::Instance().GetAll())
                            {
                                known += (known.empty() ? "" : ", ") + std::string(entry.sectionName);
                                if (section == entry.sectionName)
                                    info = &entry;
                            }
                            if (!info || !info->instance || !info->meta)
                                throw std::runtime_error("No settings section '" + section + "' (there are: " + known + ")");

                            const std::string propName = args["property"].get<std::string>();
                            const Deki::PropertyInfo* prop = FindProperty(info->meta, propName);
                            if (!prop) throw std::runtime_error("Section '" + section + "' has no property '" + propName + "'");
                            if (prop->type == Deki::PropertyType::AssetRef)
                                throw std::runtime_error("set_project_setting does not set asset references");

                            if (prop->type == Deki::PropertyType::Enum)
                                ApplyEnumSized(info->instance, prop->offset, ResolveEnumValue(*prop, args["value"]),
                                               prop->enumSize);
                            else
                                GetPropertyTypeHandler(prop->type).apply(info->instance, prop->offset, args["value"]);
                            DekiEditor::ApplySettingsToGlobalCache();
                            if (!project->Save())
                                throw std::runtime_error("could not save the project");

                            return { { "section", section }, { "property", propName },
                                     { "value", CapturePropertyValue(info->instance, *prop) } };
                        } });

    // =========================================================================
    // Asset Tools
    // =========================================================================

    registry.Register({ "list_assets",
                        "List all assets in the project, optionally filtered by file extension.",
                        { { "type", "object" }, { "properties", { { "type_filter", { { "type", "string" }, { "description", "File extension filter e.g. \".png\", \".scene\" (optional)" } } } } } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            auto* pipeline = editor->GetAssetPipeline();
                            if (!pipeline) throw std::runtime_error("No project open");

                            std::string filter = args.value("type_filter", "");

                            nlohmann::json assets = nlohmann::json::array();
                            for (const auto& [path, info] : pipeline->GetAllAssets())
                            {
                                if (!filter.empty())
                                {
                                    size_t dotPos = path.rfind('.');
                                    if (dotPos == std::string::npos) continue;
                                    std::string ext = path.substr(dotPos);
                                    if (ext != filter) continue;
                                }

                                assets.push_back({ { "path", info.path },
                                                   { "guid", info.guid },
                                                   { "hasCached", info.hasCachedVersion } });
                            }

                            return { { "assets", assets }, { "count", assets.size() } };
                        } });

    registry.Register({ "get_asset_info",
                        "Get detailed information about an asset by its GUID.",
                        { { "type", "object" }, { "properties", { { "guid", { { "type", "string" }, { "description", "Asset GUID" } } } } }, { "required", nlohmann::json::array({ "guid" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            auto* pipeline = editor->GetAssetPipeline();
                            if (!pipeline) throw std::runtime_error("No project open");

                            std::string guid = args["guid"].get<std::string>();
                            const auto* info = pipeline->GetAssetInfoByGuid(guid);
                            if (!info) throw std::runtime_error("Asset not found: " + guid);

                            return {
                                { "path", info->path },
                                { "guid", info->guid },
                                { "cachePath", info->cachePath },
                                { "hasCached", info->hasCachedVersion }
                            };
                        } });
}

}  // namespace DekiEditor
