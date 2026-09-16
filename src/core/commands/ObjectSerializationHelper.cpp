#include "ObjectInternal.h"
#include "ObjectSerializationHelper.h"

#include "PropertyTypeHandlers.h"
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/Property.h>
#include <deki/Component.h>
#include <deki/Object.h>
#include <deki/Scene.h>

namespace DekiEditor
{
namespace ObjectSerializationHelper
{

void SerializePropertiesFromMeta(const Deki::Component* comp, const Deki::ComponentMeta* meta, nlohmann::json& properties)
{
    for (int i = 0; i < meta->propertyCount; ++i)
    {
        const Deki::PropertyInfo& prop = meta->properties[i];
        switch (prop.type)
        {
            case Deki::PropertyType::Int32:
                properties[prop.name] = Deki::GetPropertyValue<int32_t>(comp, prop);
                break;
            case Deki::PropertyType::Float:
                properties[prop.name] = Deki::GetPropertyValue<float>(comp, prop);
                break;
            case Deki::PropertyType::Bool:
                properties[prop.name] = Deki::GetPropertyValue<bool>(comp, prop);
                break;
            case Deki::PropertyType::String:
                properties[prop.name] = Deki::GetPropertyValue<std::string>(comp, prop);
                break;
            case Deki::PropertyType::AssetRef:
            {
                const Deki::AssetRefBase& assetRef = Deki::GetPropertyRef<Deki::AssetRefBase>(comp, prop);
                nlohmann::json assetJson;
                assetJson["guid"] = assetRef.guid;
                assetJson["source"] = assetRef.source;
                properties[prop.name] = assetJson;
                break;
            }
            case Deki::PropertyType::Color:
                properties[prop.name] = Deki::GetPropertyValue<uint32_t>(comp, prop);
                break;
            case Deki::PropertyType::Enum:
            {
                uint8_t enumSize = prop.enumSize > 0 ? prop.enumSize : 1;
                void* ptr = reinterpret_cast<char*>(const_cast<Deki::Component*>(comp)) + prop.offset;
                int32_t enumValue = 0;
                switch (enumSize)
                {
                    case 1: enumValue = static_cast<int32_t>(*reinterpret_cast<uint8_t*>(ptr)); break;
                    case 2: enumValue = static_cast<int32_t>(*reinterpret_cast<uint16_t*>(ptr)); break;
                    default: enumValue = *reinterpret_cast<int32_t*>(ptr); break;
                }
                // By NAME, matching the scene writer, so an enum has one
                // spelling in JSON wherever it appears. This shape also reaches
                // the clipboard, where it can be pasted into another session.
                if (const char* enumName = Deki::EnumNameForValue(prop, enumValue))
                    properties[prop.name] = enumName;
                else
                    properties[prop.name] = enumValue;
                break;
            }
            case Deki::PropertyType::ObjectRef:
                properties[prop.name] = CaptureObjectRef(comp, prop.offset);
                break;
            case Deki::PropertyType::Array:
                properties[prop.name] = CaptureArray(comp, prop);
                break;
            default:
                break;
        }
    }
}

/**
 * Serialize a component's properties to JSON (including inherited properties)
 */

ComponentData SerializeComponent(const Deki::Component* comp)
{
    ComponentData cd;
    auto& registry = Deki::ComponentRegistry::Instance();
    const Deki::ComponentMeta* meta = registry.GetMeta(comp->GetType());

    if (meta)
    {
        cd.typeName = meta->serializedName;

        // Serialize properties from this component and all base classes
        const Deki::ComponentMeta* currentMeta = meta;
        while (currentMeta)
        {
            SerializePropertiesFromMeta(comp, currentMeta, cd.properties);

            // Move to base class
            if (currentMeta->baseTypeId != 0)
                currentMeta = registry.GetMeta(currentMeta->baseTypeId);
            else
                currentMeta = nullptr;
        }
    }

    return cd;
}

/**
 * Helper to restore properties from a single metadata level
 */

void RestorePropertiesFromMeta(Deki::Component* comp, const Deki::ComponentMeta* meta, const nlohmann::json& properties)
{
    for (int i = 0; i < meta->propertyCount; ++i)
    {
        const Deki::PropertyInfo& prop = meta->properties[i];
        if (properties.contains(prop.name))
        {
            const auto& value = properties.at(prop.name);
            switch (prop.type)
            {
                case Deki::PropertyType::Int32:
                    if (value.is_number_integer())
                        Deki::SetPropertyValue<int32_t>(comp, prop, value.get<int32_t>());
                    break;
                case Deki::PropertyType::Float:
                    if (value.is_number())
                        Deki::SetPropertyValue<float>(comp, prop, value.get<float>());
                    break;
                case Deki::PropertyType::Bool:
                    if (value.is_boolean())
                        Deki::SetPropertyValue<bool>(comp, prop, value.get<bool>());
                    break;
                case Deki::PropertyType::String:
                    if (value.is_string())
                        Deki::SetPropertyValue<std::string>(comp, prop, value.get<std::string>());
                    break;
                case Deki::PropertyType::AssetRef:
                    if (value.is_object())
                    {
                        Deki::AssetRefBase& assetRef = Deki::GetPropertyRef<Deki::AssetRefBase>(comp, prop);
                        if (value.contains("guid"))
                            assetRef.guid = value["guid"].get<std::string>();
                        if (value.contains("source"))
                            assetRef.source = value["source"].get<std::string>();
                        assetRef.ptr = nullptr;  // Will be loaded on demand
                    }
                    break;
                case Deki::PropertyType::Color:
                    if (value.is_number_unsigned())
                        Deki::SetPropertyValue<uint32_t>(comp, prop, value.get<uint32_t>());
                    break;
                case Deki::PropertyType::Enum:
                {
                    // Accepts the name written now and the bare number written
                    // before it, so older clipboard and undo payloads still apply.
                    bool haveValue = false;
                    int32_t v = 0;
                    if (value.is_string())
                        haveValue = Deki::EnumValueForName(prop, value.get<std::string>().c_str(), v);
                    else if (value.is_number_integer())
                    {
                        v = value.get<int32_t>();
                        haveValue = true;
                    }
                    if (haveValue)
                    {
                        uint8_t enumSize = prop.enumSize > 0 ? prop.enumSize : 1;
                        void* ptr = reinterpret_cast<char*>(comp) + prop.offset;
                        switch (enumSize)
                        {
                            case 1: *reinterpret_cast<uint8_t*>(ptr) = static_cast<uint8_t>(v); break;
                            case 2: *reinterpret_cast<uint16_t*>(ptr) = static_cast<uint16_t>(v); break;
                            default: *reinterpret_cast<int32_t*>(ptr) = static_cast<int32_t>(v); break;
                        }
                    }
                    break;
                }
                case Deki::PropertyType::ObjectRef:
                    ApplyObjectRef(comp, prop.offset, value);
                    break;
                case Deki::PropertyType::Array:
                    if (value.is_array())
                        ApplyArray(comp, prop, value);
                    break;
                default:
                    break;
            }
        }
    }
}

/**
 * Restore component properties from serialized data (including inherited properties)
 */

void RestoreComponentProperties(Deki::Component* comp, const ComponentData& data)
{
    auto& registry = Deki::ComponentRegistry::Instance();
    const Deki::ComponentMeta* meta = registry.GetMeta(data.typeName);

    if (meta)
    {
        // Restore properties from this component and all base classes
        const Deki::ComponentMeta* currentMeta = meta;
        while (currentMeta)
        {
            RestorePropertiesFromMeta(comp, currentMeta, data.properties);

            // Move to base class
            if (currentMeta->baseTypeId != 0)
                currentMeta = registry.GetMeta(currentMeta->baseTypeId);
            else
                currentMeta = nullptr;
        }
    }
}

/**
 * Serialize object transform data
 */

ObjectData SerializeObjectData(const Deki::Object* obj)
{
    ObjectData data;
    data.name = obj->GetName();
    data.x = obj->GetX();
    data.y = obj->GetY();
    data.rotation = obj->GetLocalRotation();
    data.scaleX = obj->GetScaleX();
    data.scaleY = obj->GetScaleY();
    data.active = obj->IsActive();
    data.objectId = obj->GetId();

    // Store parent info and index for restoration
    Deki::Object* parent = obj->GetParent();
    if (parent)
    {
        data.parentId = parent->GetId();
        data.originalIndex = parent->GetChildIndex(const_cast<Deki::Object*>(obj));
    }
    else if (obj->GetOwnerScene())
    {
        data.parentId = 0;
        data.originalIndex = obj->GetOwnerScene()->GetObjectIndex(const_cast<Deki::Object*>(obj));
    }

    return data;
}

/**
 * Apply object transform data
 */

void ApplyObjectData(Deki::Object* obj, const ObjectData& data)
{
    obj->SetLocalPosition(data.x, data.y);
    obj->SetRotation(data.rotation);
    obj->SetScale(data.scaleX, data.scaleY);
    obj->SetActive(data.active);
}

/**
 * Serialize all components of an object
 */

std::vector<ComponentData> SerializeComponents(const Deki::Object* obj)
{
    std::vector<ComponentData> components;
    for (const auto* comp : obj->GetComponents())
    {
        components.push_back(SerializeComponent(comp));
    }
    return components;
}

/**
 * Restore components to an object from serialized data
 */

void RestoreComponents(Deki::Object* obj, const std::vector<ComponentData>& components)
{
    for (const auto& cd : components)
    {
        Deki::Component* comp = Deki::Internal::AddComponentByName(*obj, cd.typeName);
        if (comp)
        {
            RestoreComponentProperties(comp, cd);
        }
    }
}

/**
 * Recursively serialize a child object and all its descendants
 */

ChildData SerializeChildRecursive(const Deki::Object* obj)
{
    ChildData data;
    data.objectData = SerializeObjectData(obj);
    data.components = SerializeComponents(obj);

    for (Deki::Object* child : obj->GetChildren())
    {
        data.children.push_back(SerializeChildRecursive(child));
    }

    return data;
}

/**
 * Serialize all children of an object
 */

std::vector<ChildData> SerializeChildren(const Deki::Object* obj)
{
    std::vector<ChildData> children;
    for (Deki::Object* child : obj->GetChildren())
    {
        children.push_back(SerializeChildRecursive(child));
    }
    return children;
}

/**
 * Recursively create children from serialized data
 */

void CreateChildrenRecursive(Deki::Object* parent, const std::vector<ChildData>& children)
{
    for (const auto& childData : children)
    {
        Deki::Object* child = new Deki::Object(childData.objectData.name);
        if (childData.objectData.objectId != 0)
            child->SetId(childData.objectData.objectId);
        parent->AddChild(child);                       // Set parent FIRST so ApplyObjectData knows the parent
        ApplyObjectData(child, childData.objectData);  // Now SetLocalPosition updates world transform correctly
        RestoreComponents(child, childData.components);
        CreateChildrenRecursive(child, childData.children);
    }
}

}  // namespace ObjectSerializationHelper
}  // namespace DekiEditor
