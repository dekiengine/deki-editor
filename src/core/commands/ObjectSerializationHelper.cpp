#include "ObjectInternal.h"
#include "ObjectSerializationHelper.h"

#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/Property.h>
#include <deki/Component.h>
#include <deki/Object.h>
#include <deki/Scene.h>

namespace DekiEditor
{
namespace ObjectSerializationHelper
{

// Properties are in the editor's one form of a property, the scene file's
// (Deki::Scene::PropertyToJson), so what a copy, a paste or an undo carries
// reads like a saved scene, every type included.

void SerializePropertiesFromMeta(const Deki::Component* comp, const Deki::ComponentMeta* meta,
                                 nlohmann::json& properties)
{
    for (int i = 0; i < meta->propertyCount; ++i)
    {
        nlohmann::json value = Deki::Scene::PropertyToJson(comp, meta->properties[i]);
        if (!value.is_discarded())
        {
            properties[meta->properties[i].name] = std::move(value);
        }
    }
}

// Includes the properties inherited from base classes.
ComponentData SerializeComponent(const Deki::Component* comp)
{
    ComponentData cd;
    const Deki::ComponentMeta* meta = Deki::ComponentRegistry::Instance().GetMeta(comp->GetType());
    if (meta)
    {
        cd.typeName = meta->serializedName;
        cd.properties = Deki::Scene::ComponentPropertiesToJson(comp, *meta);
    }
    return cd;
}

void RestorePropertiesFromMeta(Deki::Component* comp, const Deki::ComponentMeta* meta, const nlohmann::json& properties)
{
    for (int i = 0; i < meta->propertyCount; ++i)
    {
        Deki::Scene::PropertyFromJsonObject(comp, meta->properties[i], properties);
    }
}

// Restores inherited properties too.
void RestoreComponentProperties(Deki::Component* comp, const ComponentData& data)
{
    const Deki::ComponentMeta* meta = Deki::ComponentRegistry::Instance().GetMeta(data.typeName);
    if (meta)
    {
        for (const Deki::PropertyInfo* prop : Deki::Scene::AllProperties(*meta))
        {
            Deki::Scene::PropertyFromJsonObject(comp, *prop, data.properties);
        }
    }
}

// The name, transform, active flag, id and place under the parent.
ObjectData SerializeObjectData(const Deki::Object* obj)
{
    ObjectData data;
    data.name = obj->GetName();
    data.transform = Deki::Scene::TransformToJson(obj);
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
    Deki::Scene::TransformFromJson(obj, data.transform);
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
