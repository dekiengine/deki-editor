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

AddComponentCommand::~AddComponentCommand()
{
    // Undone and never redone: the command still owns the instance.
    if (m_Detached)
        delete m_Component;
}

void AddComponentCommand::Execute()
{
    // Redo puts back the SAME instance the undo took off: edits recorded on it
    // after it was added still point at it.
    if (m_Component && m_Detached)
    {
        Deki::Internal::ReattachComponent(*m_Object, m_Component, static_cast<size_t>(m_Index));
        m_Detached = false;
    }
    else
    {
        m_Component = Deki::Internal::AddComponentByName(*m_Object, m_TypeName);
    }
    if (auto* scene = m_Object->GetOwnerScene())
        scene->InvalidateComponentsCache();
    if (m_Component && m_OnAdded) m_OnAdded(m_Component);
}

void AddComponentCommand::Undo()
{
    // Take it off, keep it: deleting it here and creating a new one on redo
    // left every later edit of it writing into freed memory.
    if (!m_Component || m_Detached)
        return;
    CustomEditorRegistry::Instance().ClearCacheForComponent(m_Component);
    m_Index = Deki::Internal::DetachComponent(*m_Object, m_Component);
    m_Detached = m_Index >= 0;
    if (auto* scene = m_Object->GetOwnerScene())
        scene->InvalidateComponentsCache();
}

bool AddComponentCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::HasAny(dead, m_Object, m_Component);
}

std::string RemoveComponentCommand::GetDescription() const
{
    return "Remove " + m_TypeName;
}

RemoveComponentCommand::~RemoveComponentCommand()
{
    // Removed and never undone: the command owns the instance.
    if (m_Detached)
        delete m_Component;
}

void RemoveComponentCommand::Execute()
{
    // Take it off and keep it, so undo puts back the SAME instance: edits made
    // to it before it was removed point at it. (Undo used to create a new
    // component, and undoing one of those edits then wrote into freed memory.)
    if (!m_Component || m_Detached)
        return;
    CustomEditorRegistry::Instance().ClearCacheForComponent(m_Component);
    m_Index = Deki::Internal::DetachComponent(*m_Object, m_Component);
    m_Detached = m_Index >= 0;
    if (auto* scene = m_Object->GetOwnerScene())
        scene->InvalidateComponentsCache();
    if (m_OnRemoved) m_OnRemoved();
}

void RemoveComponentCommand::Undo()
{
    if (!m_Component || !m_Detached)
        return;
    Deki::Internal::ReattachComponent(*m_Object, m_Component, static_cast<size_t>(m_Index));
    m_Detached = false;
    if (auto* scene = m_Object->GetOwnerScene())
        scene->InvalidateComponentsCache();
}

bool RemoveComponentCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::HasAny(dead, m_Object, m_Component);
}

std::string DuplicateObjectCommand::GetDescription() const
{
    return "Duplicate " + m_SourceName;
}

namespace
{
// "Name (1)", "Name (2)", ...: the first one no child of `parent` uses. A name
// that already ends in " (n)" counts on from there, so duplicating "Coin (1)"
// gives "Coin (2)", not "Coin (1) (1)".
std::string NextDuplicateName(const Deki::Object* parent, const std::string& name)
{
    std::string base = name;
    int n = 1;
    const size_t open = name.rfind(" (");
    if (open != std::string::npos && name.size() > open + 3 && name.back() == ')')
    {
        const std::string digits = name.substr(open + 2, name.size() - open - 3);
        if (!digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos && digits.size() < 6)
        {
            base = name.substr(0, open);
            n = std::stoi(digits) + 1;
        }
    }
    auto taken = [parent](const std::string& candidate)
    {
        if (!parent)
            return false;
        for (const Deki::Object* child : parent->GetChildren())
            if (child->GetName() == candidate)
                return true;
        return false;
    };
    std::string candidate;
    do
        candidate = base + " (" + std::to_string(n++) + ")";
    while (taken(candidate));
    return candidate;
}
}  // namespace

void DuplicateObjectCommand::Execute()
{
    if (!m_DuplicatedObject)
    {
        // First time: clone via engine's JSON round-trip
        m_DuplicatedObject = m_Scene->DuplicateObject(m_Source);
        if (!m_DuplicatedObject) return;
        // A copy named like its original was indistinguishable in the
        // Hierarchy; number it the way other editors do.
        m_DuplicatedObject->SetName(NextDuplicateName(m_Source ? m_Source->GetParent() : nullptr, m_Source ? m_Source->GetName() : m_DuplicatedObject->GetName()));
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
        // Right after the original when they share a parent, as a copy is
        // expected to appear; otherwise at the end.
        int sourceIndex = (m_Source && m_Source->GetParent() == resolvedParent) ? resolvedParent->GetChildIndex(m_Source) : -1;
        if (sourceIndex >= 0)
            resolvedParent->InsertChildAt(m_DuplicatedObject, static_cast<size_t>(sourceIndex) + 1);
        else
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
