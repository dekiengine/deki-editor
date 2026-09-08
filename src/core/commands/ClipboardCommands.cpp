#include "ObjectInternal.h"
#include "ClipboardCommands.h"
#include <deki/Object.h>
#include <deki/Component.h>
#include <deki/Scene.h>

namespace DekiEditor
{

// === PasteComponentValuesCommand ===

PasteComponentValuesCommand::PasteComponentValuesCommand(
    const std::vector<Deki::Component*>& components,
    const ComponentData& sourceData,
    std::function<void()> onChange)
    : m_SourceData(sourceData), m_OnChange(onChange)
{
    for (auto* comp : components)
    {
        Entry entry;
        entry.component = comp;
        entry.oldData = ObjectSerializationHelper::SerializeComponent(comp);
        m_Entries.push_back(entry);
    }
}

std::string PasteComponentValuesCommand::GetDescription() const
{
    if (m_Entries.size() == 1)
        return "Paste " + m_SourceData.typeName + " Values";
    return "Paste " + m_SourceData.typeName + " Values (" +
           std::to_string(m_Entries.size()) + " components)";
}

void PasteComponentValuesCommand::Execute()
{
    for (auto& entry : m_Entries)
    {
        RestoreComponentProperties(entry.component, m_SourceData);
    }
    if (m_OnChange)
        m_OnChange();
}

void PasteComponentValuesCommand::Undo()
{
    for (auto& entry : m_Entries)
    {
        RestoreComponentProperties(entry.component, entry.oldData);
    }
    if (m_OnChange)
        m_OnChange();
}

// === PasteComponentAsNewCommand ===

PasteComponentAsNewCommand::PasteComponentAsNewCommand(
    const std::vector<Deki::Object*>& objects,
    const ComponentData& sourceData,
    std::function<void(Deki::Component*)> onAdded)
    : m_SourceData(sourceData), m_OnAdded(onAdded)
{
    for (auto* obj : objects)
    {
        Entry entry;
        entry.object = obj;
        entry.addedComponent = nullptr;
        m_Entries.push_back(entry);
    }
}

std::string PasteComponentAsNewCommand::GetDescription() const
{
    if (m_Entries.size() == 1)
        return "Paste " + m_SourceData.typeName + " as New";
    return "Paste " + m_SourceData.typeName + " as New (" +
           std::to_string(m_Entries.size()) + " objects)";
}

void PasteComponentAsNewCommand::Execute()
{
    for (auto& entry : m_Entries)
    {
        Deki::Component* comp = Deki::Internal::AddComponentByName(*entry.object, m_SourceData.typeName);
        if (comp)
        {
            RestoreComponentProperties(comp, m_SourceData);
            entry.addedComponent = comp;
            if (auto* scene = entry.object->GetOwnerScene())
                scene->InvalidateComponentsCache();
            if (m_OnAdded)
                m_OnAdded(comp);
        }
    }
}

void PasteComponentAsNewCommand::Undo()
{
    for (auto& entry : m_Entries)
    {
        if (entry.addedComponent)
        {
            entry.object->RemoveComponent(entry.addedComponent);
            if (auto* scene = entry.object->GetOwnerScene())
                scene->InvalidateComponentsCache();
            entry.addedComponent = nullptr;
        }
    }
}

// === PasteTransformCommand ===

PasteTransformCommand::PasteTransformCommand(
    const std::vector<Deki::Object*>& objects,
    const ObjectData& sourceData,
    std::function<void()> onChange)
    : m_SourceData(sourceData), m_OnChange(onChange)
{
    for (auto* obj : objects)
    {
        Entry entry;
        entry.object = obj;
        entry.oldData = ObjectSerializationHelper::SerializeObjectData(obj);
        m_Entries.push_back(entry);
    }
}

std::string PasteTransformCommand::GetDescription() const
{
    if (m_Entries.size() == 1)
        return "Paste Transform";
    return "Paste Transform (" + std::to_string(m_Entries.size()) + " objects)";
}

void PasteTransformCommand::Execute()
{
    for (auto& entry : m_Entries)
    {
        ApplyObjectData(entry.object, m_SourceData);
    }
    if (m_OnChange)
        m_OnChange();
}

void PasteTransformCommand::Undo()
{
    for (auto& entry : m_Entries)
    {
        ApplyObjectData(entry.object, entry.oldData);
    }
    if (m_OnChange)
        m_OnChange();
}

// === PasteObjectsCommand ===

PasteObjectsCommand::PasteObjectsCommand(
    Deki::Scene* scene, const std::vector<ChildData>& objectsData,
    Deki::Object* parent,
    std::function<void(const std::vector<Deki::Object*>&)> onPasted)
    : m_Scene(scene), m_Parent(parent), m_ObjectsData(objectsData), m_OnPasted(onPasted)
{
}

std::string PasteObjectsCommand::GetDescription() const
{
    if (m_ObjectsData.size() == 1)
        return "Paste " + m_ObjectsData[0].objectData.name;
    return "Paste " + std::to_string(m_ObjectsData.size()) + " Objects";
}

void PasteObjectsCommand::Execute()
{
    m_CreatedObjects.clear();

    for (const auto& data : m_ObjectsData)
    {
        Deki::Object* obj = new Deki::Object(data.objectData.name);
        ApplyObjectData(obj, data.objectData);
        RestoreComponents(obj, data.components);
        CreateChildrenRecursive(obj, data.children);

        if (m_Parent)
        {
            m_Parent->AddChild(obj);
            obj->SetOwnerScene(m_Scene);
        }
        else
        {
            // Single-root model: paste as child of scene root
            auto& roots = m_Scene->GetObjects();
            if (!roots.empty())
            {
                roots[0]->AddChild(obj);
                obj->SetOwnerScene(m_Scene);
            }
            else
            {
                m_Scene->AddObject(obj);
            }
        }

        m_CreatedObjects.push_back(obj);
    }

    if (m_OnPasted && !m_CreatedObjects.empty())
        m_OnPasted(m_CreatedObjects);
}

void PasteObjectsCommand::Undo()
{
    for (auto* obj : m_CreatedObjects)
    {
        if (obj)
            m_Scene->DestroyDekiObject(obj);
    }
    m_CreatedObjects.clear();
}

}  // namespace DekiEditor
