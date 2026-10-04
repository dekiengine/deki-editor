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

void SerializePropertiesFromMeta(const Deki::Component* comp, const Deki::ComponentMeta* meta,
                                 nlohmann::json& properties)
{
    for (int i = 0; i < meta->propertyCount; ++i)
    {
        const Deki::PropertyInfo& prop = meta->properties[i];
        switch (prop.type)
        {
            case Deki::PropertyType::Int32: properties[prop.name] = Deki::GetPropertyValue<int32_t>(comp, prop); break;
            case Deki::PropertyType::Float: properties[prop.name] = Deki::GetPropertyValue<float>(comp, prop); break;
            case Deki::PropertyType::Bool: properties[prop.name] = Deki::GetPropertyValue<bool>(comp, prop); break;
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
            case Deki::PropertyType::Color: properties[prop.name] = Deki::GetPropertyValue<uint32_t>(comp, prop); break;
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
                // By name, like the scene writer, so an enum has one spelling
                // in JSON everywhere. This also reaches the clipboard, which
                // can be pasted into another session.
                if (const char* enumName = Deki::EnumNameForValue(prop, enumValue))
                {
                    properties[prop.name] = enumName;
                }
                else
                {
                    properties[prop.name] = enumValue;
                }
                break;
            }
            case Deki::PropertyType::ObjectRef: properties[prop.name] = CaptureObjectRef(comp, prop.offset); break;
            case Deki::PropertyType::Array: properties[prop.name] = CaptureArray(comp, prop); break;
            default: break;
        }
    }
}

// Includes the properties inherited from base classes.
ComponentData SerializeComponent(const Deki::Component* comp)
{
    ComponentData cd;
    auto& registry = Deki::ComponentRegistry::Instance();
    const Deki::ComponentMeta* meta = registry.GetMeta(comp->GetType());

    if (meta)
    {
        cd.typeName = meta->serializedName;

        // This class first, then each base class.
        const Deki::ComponentMeta* currentMeta = meta;
        while (currentMeta)
        {
            SerializePropertiesFromMeta(comp, currentMeta, cd.properties);

            if (currentMeta->baseTypeId != 0)
            {
                currentMeta = registry.GetMeta(currentMeta->baseTypeId);
            }
            else
            {
                currentMeta = nullptr;
            }
        }
    }

    return cd;
}

// Restores the properties declared at one level of the class hierarchy.
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
                    {
                        Deki::SetPropertyValue<int32_t>(comp, prop, value.get<int32_t>());
                    }
                    break;
                case Deki::PropertyType::Float:
                    if (value.is_number())
                    {
                        Deki::SetPropertyValue<float>(comp, prop, value.get<float>());
                    }
                    break;
                case Deki::PropertyType::Bool:
                    if (value.is_boolean())
                    {
                        Deki::SetPropertyValue<bool>(comp, prop, value.get<bool>());
                    }
                    break;
                case Deki::PropertyType::String:
                    if (value.is_string())
                    {
                        Deki::SetPropertyValue<std::string>(comp, prop, value.get<std::string>());
                    }
                    break;
                case Deki::PropertyType::AssetRef:
                    if (value.is_object())
                    {
                        Deki::AssetRefBase& assetRef = Deki::GetPropertyRef<Deki::AssetRefBase>(comp, prop);
                        if (value.contains("guid"))
                        {
                            assetRef.guid = value["guid"].get<std::string>();
                        }
                        if (value.contains("source"))
                        {
                            assetRef.source = value["source"].get<std::string>();
                        }
                        assetRef.ptr = nullptr;  // loaded on demand
                    }
                    break;
                case Deki::PropertyType::Color:
                    if (value.is_number_unsigned())
                    {
                        Deki::SetPropertyValue<uint32_t>(comp, prop, value.get<uint32_t>());
                    }
                    break;
                case Deki::PropertyType::Enum:
                {
                    // Accepts a name or a bare number: older clipboard and undo
                    // payloads hold numbers.
                    bool haveValue = false;
                    int32_t v = 0;
                    if (value.is_string())
                    {
                        haveValue = Deki::EnumValueForName(prop, value.get<std::string>().c_str(), v);
                    }
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
                case Deki::PropertyType::ObjectRef: ApplyObjectRef(comp, prop.offset, value); break;
                case Deki::PropertyType::Array:
                    if (value.is_array())
                    {
                        ApplyArray(comp, prop, value);
                    }
                    break;
                default: break;
            }
        }
    }
}

// Restores inherited properties too.
void RestoreComponentProperties(Deki::Component* comp, const ComponentData& data)
{
    auto& registry = Deki::ComponentRegistry::Instance();
    const Deki::ComponentMeta* meta = registry.GetMeta(data.typeName);

    if (meta)
    {
        // This class first, then each base class.
        const Deki::ComponentMeta* currentMeta = meta;
        while (currentMeta)
        {
            RestorePropertiesFromMeta(comp, currentMeta, data.properties);

            if (currentMeta->baseTypeId != 0)
            {
                currentMeta = registry.GetMeta(currentMeta->baseTypeId);
            }
            else
            {
                currentMeta = nullptr;
            }
        }
    }
}

// The name, transform, active flag, id and place under the parent.
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

    // Parent and index, so the object can be put back where it was.
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

// Applies the transform and active flag only.
void ApplyObjectData(Deki::Object* obj, const ObjectData& data)
{
    obj->SetLocalPosition(data.x, data.y);
    obj->SetRotation(data.rotation);
    obj->SetScale(data.scaleX, data.scaleY);
    obj->SetActive(data.active);
}

std::vector<ComponentData> SerializeComponents(const Deki::Object* obj)
{
    std::vector<ComponentData> components;
    for (const auto* comp : obj->GetComponents())
    {
        components.push_back(SerializeComponent(comp));
    }
    return components;
}

// Adds each component to `obj`; the components it already has stay.
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

std::vector<ChildData> SerializeChildren(const Deki::Object* obj)
{
    std::vector<ChildData> children;
    for (Deki::Object* child : obj->GetChildren())
    {
        children.push_back(SerializeChildRecursive(child));
    }
    return children;
}

void CreateChildrenRecursive(Deki::Object* parent, const std::vector<ChildData>& children)
{
    for (const auto& childData : children)
    {
        Deki::Object* child = new Deki::Object(childData.objectData.name);
        if (childData.objectData.objectId != 0)
        {
            child->SetId(childData.objectData.objectId);
        }
        parent->AddChild(child);                       // first, so the local position is relative to it
        ApplyObjectData(child, childData.objectData);  // the world transform now comes out right
        RestoreComponents(child, childData.components);
        CreateChildrenRecursive(child, childData.children);
    }
}

}  // namespace ObjectSerializationHelper
}  // namespace DekiEditor
