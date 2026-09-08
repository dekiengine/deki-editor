#include "ObjectInternal.h"
#include "ObjectCommands.h"
#include "ObjectSerializationHelper.h"
#include <deki-editor/EditorExtension.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/Component.h>
#include <deki/Object.h>
#include <deki/Scene.h>

namespace DekiEditor
{

std::string DeleteObjectCommand::GetDescription() const
{
    return "Delete " + m_ObjectName;
}

void DeleteObjectCommand::Execute()
{
    if (!m_Object) return;

    // Detach from scene without deleting
    if (m_Object->GetParent())
        m_Object->RemoveFromParent();
    m_Scene->RemoveObject(m_Object);

    m_IsDetached = true;

    if (m_OnDeleted) m_OnDeleted();
}

void DeleteObjectCommand::Undo()
{
    if (!m_Object) return;

    size_t index = m_OriginalIndex >= 0 ? static_cast<size_t>(m_OriginalIndex) : 0;

    if (m_ParentId != 0)
    {
        Deki::Object* parent = m_Scene->FindDekiObject(m_ParentId);
        if (parent)
        {
            parent->InsertChildAt(m_Object, index);
            m_Object->SetOwnerScene(m_Scene);
        }
        else
        {
            m_Scene->InsertObjectAt(m_Object, index);
        }
    }
    else
    {
        m_Scene->InsertObjectAt(m_Object, index);
    }

    m_IsDetached = false;
    if (m_OnRestored) m_OnRestored(m_Object);
}

bool DeleteObjectCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::HasAny(dead, m_Scene, m_Object);
}

std::string SetActiveCommand::GetDescription() const
{
    return (m_New ? "Activate " : "Deactivate ") + m_ObjectName;
}

void SetActiveCommand::Execute()
{
    m_Object->SetActive(m_New);
}

void SetActiveCommand::Undo()
{
    m_Object->SetActive(m_Old);
}

bool SetActiveCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::Has(dead, m_Object);
}

std::string AddComponentCommand::GetDescription() const
{
    return "Add " + m_TypeName;
}

void AddComponentCommand::Execute()
{
    Deki::Component* comp = Deki::Internal::AddComponentByName(*m_Object, m_TypeName);
    if (auto* scene = m_Object->GetOwnerScene())
        scene->InvalidateComponentsCache();
    if (comp && m_OnAdded) m_OnAdded(comp);
}

void AddComponentCommand::Undo()
{
    // Remove the last component of this type
    auto& comps = m_Object->GetComponents();
    for (auto it = comps.rbegin(); it != comps.rend(); ++it)
    {
        auto& registry = Deki::ComponentRegistry::Instance();
        const Deki::ComponentMeta* meta = registry.GetMeta((*it)->GetType());
        if (meta && meta->serializedName == m_TypeName)
        {
            m_Object->RemoveComponent(*it);
            if (auto* scene = m_Object->GetOwnerScene())
                scene->InvalidateComponentsCache();
            break;
        }
    }
}

bool AddComponentCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::Has(dead, m_Object);
}

std::string RemoveComponentCommand::GetDescription() const
{
    return "Remove " + m_TypeName;
}

void RemoveComponentCommand::Execute()
{
    if (m_ComponentToRemove)
    {
        CustomEditorRegistry::Instance().ClearCacheForComponent(m_ComponentToRemove);
        m_Object->RemoveComponent(m_ComponentToRemove);
        if (auto* scene = m_Object->GetOwnerScene())
            scene->InvalidateComponentsCache();
        m_ComponentToRemove = nullptr;
        if (m_OnRemoved) m_OnRemoved();
    }
}

void RemoveComponentCommand::Undo()
{
    // Recreate the component using AddComponent
    Deki::Component* comp = Deki::Internal::AddComponentByName(*m_Object, m_TypeName);
    if (comp)
    {
        RestoreComponentProperties(comp, m_StoredData);
        if (auto* scene = m_Object->GetOwnerScene())
            scene->InvalidateComponentsCache();
        m_ComponentToRemove = comp;
    }
}

bool RemoveComponentCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::HasAny(dead, m_Object, m_ComponentToRemove);
}

std::string DuplicateObjectCommand::GetDescription() const
{
    return "Duplicate " + m_SourceName;
}

void DuplicateObjectCommand::Execute()
{
    if (!m_DuplicatedObject)
    {
        // First time: clone via engine's JSON round-trip
        m_DuplicatedObject = m_Scene->DuplicateObject(m_Source);
        if (!m_DuplicatedObject) return;
    }

    // Single-root invariant: a null parent means "inside the scene root", not "as a sibling of root".
    // Resolved each (re)execute so a redo after the root has changed identity still lands inside.
    Deki::Object* resolvedParent = m_Parent;
    if (!resolvedParent)
    {
        auto& roots = m_Scene->GetObjects();
        for (Deki::Object* obj : roots)
        {
            if (obj != m_DuplicatedObject)
            {
                resolvedParent = obj;
                break;
            }
        }
    }

    if (resolvedParent)
    {
        resolvedParent->AddChild(m_DuplicatedObject);
        m_DuplicatedObject->SetOwnerScene(m_Scene);
    }
    else
    {
        // No existing root: the duplicate becomes the root
        m_Scene->AddObject(m_DuplicatedObject);
    }

    m_IsDetached = false;
    if (m_OnDuplicated) m_OnDuplicated(m_DuplicatedObject);
}

void DuplicateObjectCommand::Undo()
{
    if (m_DuplicatedObject)
    {
        if (m_OnUndone) m_OnUndone();

        Deki::Object* parent = m_DuplicatedObject->GetParent();
        if (parent)
            m_DuplicatedObject->RemoveFromParent();
        m_Scene->RemoveObject(m_DuplicatedObject);

        m_IsDetached = true;
    }
}

void DuplicateObjectCommand::Redo()
{
    Execute();
}

bool DuplicateObjectCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::HasAny(dead, m_Scene, m_Source, m_Parent, m_DuplicatedObject);
}

}  // namespace DekiEditor
