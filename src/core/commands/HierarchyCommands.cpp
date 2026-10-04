#include "HierarchyCommands.h"

#include "ObjectSerializationHelper.h"
#include <deki/reflection/ComponentRegistry.h>
#include <deki/Component.h>
#include <deki/Object.h>
#include <deki/Scene.h>

namespace DekiEditor
{

std::string CreateObjectCommand::GetDescription() const
{
    return "Create " + m_Name;
}

void CreateObjectCommand::Execute()
{
    const bool firstRun = (m_CreatedObject == nullptr);

    if (firstRun)
    {
        m_CreatedObject = m_Scene->CreateDekiObject(m_Name);
        if (!m_CreatedObject)
        {
            if (m_OnCreated)
            {
                m_OnCreated(m_CreatedObject);
            }
            return;
        }
    }

    // Single root: a null parent means "inside the scene root", not "beside it".
    // With no root yet, the new object becomes the root. Resolved on every
    // (re)execute, so a redo after the root was replaced still lands inside it.
    Deki::Object* resolvedParent = m_Parent;
    if (!resolvedParent)
    {
        auto& roots = m_Scene->GetObjects();
        // Skip the object just created when looking for an existing root.
        for (Deki::Object* obj : roots)
        {
            if (obj != m_CreatedObject)
            {
                resolvedParent = obj;
                break;
            }
        }
    }

    if (resolvedParent)
    {
        m_Scene->RemoveObject(m_CreatedObject);
        m_CreatedObject->SetOwnerScene(m_Scene);
        // On redo, put it back where it was rather than at the end.
        if (!firstRun && m_DetachedIndex >= 0)
        {
            resolvedParent->InsertChildAt(m_CreatedObject, static_cast<size_t>(m_DetachedIndex));
        }
        else
        {
            resolvedParent->AddChild(m_CreatedObject);
        }
    }
    else if (!firstRun)
    {
        // No parent to host it: it is the root again.
        const size_t index = m_DetachedIndex >= 0 ? static_cast<size_t>(m_DetachedIndex) : 0;
        m_Scene->InsertObjectAt(m_CreatedObject, index);
    }

    m_IsDetached = false;
    if (m_OnCreated)
    {
        m_OnCreated(m_CreatedObject);
    }
}

void CreateObjectCommand::Undo()
{
    if (!m_CreatedObject)
    {
        return;
    }

    if (m_OnUndone)
    {
        m_OnUndone();
    }

    // Remember where it sat so redo restores the same position.
    Deki::Object* parent = m_CreatedObject->GetParent();
    m_DetachedIndex = parent ? parent->GetChildIndex(m_CreatedObject) : m_Scene->GetObjectIndex(m_CreatedObject);

    if (parent)
    {
        m_CreatedObject->RemoveFromParent();
    }
    m_Scene->RemoveObject(m_CreatedObject);

    m_IsDetached = true;
}

void CreateObjectCommand::Redo()
{
    Execute();
}

bool CreateObjectCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::HasAny(dead, m_Scene, m_Parent, m_CreatedObject);
}

std::string RenameObjectCommand::GetDescription() const
{
    return "Rename " + m_OldName;
}

void RenameObjectCommand::Execute()
{
    m_Object->SetName(m_NewName);
}

void RenameObjectCommand::Undo()
{
    m_Object->SetName(m_OldName);
}

bool RenameObjectCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::Has(dead, m_Object);
}

std::string ReparentObjectCommand::GetDescription() const
{
    if (m_NewParent)
    {
        return "Parent " + m_ObjectName;
    }
    else
    {
        return "Unparent " + m_ObjectName;
    }
}

void ReparentObjectCommand::Execute()
{
    if (m_OldParent)
    {
        m_OldParent->RemoveChild(m_Object);
    }

    // Single root: a null new parent means the scene root.
    Deki::Object* targetParent = m_NewParent;
    if (!targetParent && m_Scene)
    {
        auto& roots = m_Scene->GetObjects();
        if (!roots.empty())
        {
            targetParent = roots[0];
        }
    }

    if (targetParent)
    {
        if (m_Scene && !m_OldParent)
        {
            // It was a root object: take it off the scene's root list.
            m_Scene->RemoveObject(m_Object);
            m_Object->SetOwnerScene(m_Scene);
        }
        targetParent->AddChild(m_Object);
    }
    else
    {
        // No scene root: it becomes a root object.
        m_Object->SetParent(nullptr);
        if (m_Scene)
        {
            m_Scene->AddObject(m_Object);
        }
    }
}

void ReparentObjectCommand::Undo()
{
    if (m_NewParent)
    {
        m_NewParent->RemoveChild(m_Object);
    }

    if (m_OldParent)
    {
        if (m_Scene && !m_NewParent)
        {
            // Execute left it at root level: take it off the scene's root list.
            m_Scene->RemoveObject(m_Object);
            m_Object->SetOwnerScene(m_Scene);
        }
        m_OldParent->AddChild(m_Object);
    }
    else
    {
        // It started as a root object: put it back on the scene's root list.
        m_Object->SetParent(nullptr);
        if (m_Scene)
        {
            m_Scene->AddObject(m_Object);
        }
    }
}

bool ReparentObjectCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::HasAny(dead, m_Object, m_Scene, m_OldParent, m_NewParent);
}

std::string MoveObjectsCommand::GetDescription() const
{
    if (m_Objects.size() == 1)
    {
        return "Move " + m_Objects[0]->GetName();
    }
    return "Move " + std::to_string(m_Objects.size()) + " objects";
}

void MoveObjectsCommand::Execute()
{
    // Single root: a null target parent means the scene root.
    Deki::Object* resolvedParent = m_TargetParent;
    if (!resolvedParent && m_Scene)
    {
        auto& roots = m_Scene->GetObjects();
        if (!roots.empty())
        {
            resolvedParent = roots[0];
        }
    }

    // The target index was taken with the moved objects still in the list.
    // Each one that sat before it under the same parent leaves a gap once
    // removed, so subtract those, or a move down within one parent lands one
    // place too far per object.
    size_t removedBefore = 0;
    for (size_t i = 0; i < m_Objects.size(); i++)
    {
        if (m_OldStates[i].parent == resolvedParent && m_OldStates[i].index < m_TargetIndex)
        {
            ++removedBefore;
        }
    }
    const size_t targetIndex = m_TargetIndex - removedBefore;

    // Remove by parent, then by descending index, so one removal does not
    // shift the index of the next.
    std::vector<size_t> removalOrder(m_Objects.size());
    for (size_t i = 0; i < m_Objects.size(); i++)
    {
        removalOrder[i] = i;
    }
    std::sort(removalOrder.begin(), removalOrder.end(),
              [this](size_t a, size_t b)
              {
                  if (m_OldStates[a].parent != m_OldStates[b].parent)
                  {
                      return m_OldStates[a].parent < m_OldStates[b].parent;
                  }
                  return m_OldStates[a].index > m_OldStates[b].index;  // descending index
              });

    for (size_t idx : removalOrder)
    {
        Deki::Object* obj = m_Objects[idx];
        if (m_OldStates[idx].parent)
        {
            m_OldStates[idx].parent->RemoveChild(obj);
        }
        else
        {
            m_Scene->RemoveObject(obj);
            obj->SetOwnerScene(m_Scene);
        }
    }

    // Insert in order at the target, keeping their relative order.
    if (resolvedParent)
    {
        size_t maxIdx = resolvedParent->GetChildren().size();
        size_t insertIdx = std::min(targetIndex, maxIdx);
        for (size_t i = 0; i < m_Objects.size(); i++)
        {
            resolvedParent->InsertChildAt(m_Objects[i], insertIdx + i);
        }
    }
    else
    {
        // No scene root: they become root objects.
        size_t maxIdx = m_Scene->GetObjects().size();
        size_t insertIdx = std::min(m_TargetIndex, maxIdx);
        for (size_t i = 0; i < m_Objects.size(); i++)
        {
            m_Objects[i]->SetParent(nullptr);
            m_Scene->InsertObjectAt(m_Objects[i], insertIdx + i);
        }
    }
}

void MoveObjectsCommand::Undo()
{
    // The same target parent Execute used.
    Deki::Object* resolvedParent = m_TargetParent;
    if (!resolvedParent && m_Scene)
    {
        auto& roots = m_Scene->GetObjects();
        if (!roots.empty())
        {
            resolvedParent = roots[0];
        }
    }

    // Reverse order, so removals do not shift each other's index.
    for (int i = static_cast<int>(m_Objects.size()) - 1; i >= 0; i--)
    {
        Deki::Object* obj = m_Objects[i];
        if (resolvedParent)
        {
            resolvedParent->RemoveChild(obj);
        }
        else
        {
            m_Scene->RemoveObject(obj);
            obj->SetOwnerScene(m_Scene);
        }
    }

    // Restore each object to its original parent and index, in ascending
    // index order per parent so each lands where it was.
    std::vector<size_t> restoreOrder(m_OldStates.size());
    for (size_t i = 0; i < m_OldStates.size(); i++)
    {
        restoreOrder[i] = i;
    }
    std::sort(restoreOrder.begin(), restoreOrder.end(),
              [this](size_t a, size_t b)
              {
                  if (m_OldStates[a].parent != m_OldStates[b].parent)
                  {
                      return m_OldStates[a].parent < m_OldStates[b].parent;
                  }
                  return m_OldStates[a].index < m_OldStates[b].index;  // ascending index
              });

    for (size_t idx : restoreOrder)
    {
        Deki::Object* obj = m_OldStates[idx].object;
        Deki::Object* oldParent = m_OldStates[idx].parent;
        size_t oldIndex = m_OldStates[idx].index;

        if (oldParent)
        {
            size_t maxIdx = oldParent->GetChildren().size();
            oldParent->InsertChildAt(obj, std::min(oldIndex, maxIdx));
        }
        else
        {
            obj->SetParent(nullptr);
            size_t maxIdx = m_Scene->GetObjects().size();
            m_Scene->InsertObjectAt(obj, std::min(oldIndex, maxIdx));
        }
    }
}

bool MoveObjectsCommand::ReferencesAny(const DeadMemory& dead) const
{
    if (CommandRefs::HasAny(dead, m_Scene, m_TargetParent))
    {
        return true;
    }
    if (CommandRefs::HasAnyIn(dead, m_Objects))
    {
        return true;
    }
    for (const auto& state : m_OldStates)
    {
        if (CommandRefs::HasAny(dead, state.object, state.parent))
        {
            return true;
        }
    }
    return false;
}

}  // namespace DekiEditor
