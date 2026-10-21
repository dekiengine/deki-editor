#include "InstanceCommands.h"
#include "CommandText.h"
#include "ObjectSerializationHelper.h"
#include <deki/reflection/ComponentRegistry.h>
#include <deki/Component.h>
#include <deki/Object.h>
#include <deki/Scene.h>
#include <deki-editor/AssetDatabase.h>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace DekiEditor
{

std::string CreateInstanceCommand::GetDescription() const
{
    return CommandText::Format(Tr("Create Instance %s"), m_InstanceName.c_str());
}

void CreateInstanceCommand::Execute()
{
    m_InstanceObject = m_Scene->CreateDekiObject(m_InstanceName);
    if (!m_InstanceObject)
    {
        return;
    }

    m_InstanceObject->SetSourceSceneGuid(m_SourceSceneGuid);

    // Single root: a null parent means "inside the scene root", not "beside it".
    // With no root yet, the new object becomes the root.
    Deki::Object* resolvedParent = m_Parent;
    if (!resolvedParent)
    {
        auto& roots = m_Scene->GetObjects();
        if (!roots.empty())
        {
            resolvedParent = roots[0];
        }
    }

    if (resolvedParent)
    {
        m_Scene->RemoveObject(m_InstanceObject);
        m_InstanceObject->SetOwnerScene(m_Scene);
        resolvedParent->AddChild(m_InstanceObject);
    }

    m_Scene->ExpandInstance(m_InstanceObject);

    if (m_OnCreated)
    {
        m_OnCreated(m_InstanceObject);
    }
}

void CreateInstanceCommand::Undo()
{
    if (m_InstanceObject)
    {
        if (m_OnUndone)
        {
            m_OnUndone();
        }
        m_Scene->DestroyDekiObject(m_InstanceObject);
        m_InstanceObject = nullptr;
    }
}

void CreateInstanceCommand::Redo()
{
    Execute();
}

bool CreateInstanceCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::HasAny(dead, m_Scene, m_Parent, m_InstanceObject);
}

std::string UnlinkInstanceCommand::GetDescription() const
{
    return Tr("Unlink Instance");
}

void UnlinkInstanceCommand::Execute()
{
    // Clear the instance markers on the root and all children.
    m_InstanceObject->SetSourceSceneGuid("");
    m_InstanceObject->SetInstanceOverridesJson("");
    ClearInstanceFlags(m_InstanceObject);

    if (m_OnUnlinked)
    {
        m_OnUnlinked();
    }
}

void UnlinkInstanceCommand::Undo()
{
    m_InstanceObject->SetSourceSceneGuid(m_SavedGuid);
    m_InstanceObject->SetInstanceOverridesJson(m_SavedOverrides);
    SetInstanceFlags(m_InstanceObject);
}

bool UnlinkInstanceCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::Has(dead, m_InstanceObject);
}

void UnlinkInstanceCommand::ClearInstanceFlags(Deki::Object* obj)
{
    obj->SetPartOfInstance(false);
    for (auto* child : obj->GetChildren())
    {
        ClearInstanceFlags(child);
    }
}

void UnlinkInstanceCommand::SetInstanceFlags(Deki::Object* obj)
{
    for (auto* child : obj->GetChildren())
    {
        child->SetPartOfInstance(true);
        SetInstanceFlags(child);
    }
}

std::string LinkInstanceCommand::GetDescription() const
{
    return Tr("Link to Scene");
}

void LinkInstanceCommand::Execute()
{
    // CollectOverrides diffs against the source this GUID names.
    m_Object->SetSourceSceneGuid(m_SourceSceneGuid);

    // Overrides are what the current children change from the source scene.
    nlohmann::json overrides = nlohmann::json::array();
    m_Scene->CollectOverrides(m_Object, m_Object, overrides);

    if (!overrides.empty())
    {
        m_Object->SetInstanceOverridesJson(overrides.dump());
    }
    else
    {
        m_Object->SetInstanceOverridesJson("");
    }

    // Delete the current children; the expanded source replaces them. Through
    // the scene, which tells the editor first: selection and undo history hold
    // pointers into them.
    auto children = m_Object->GetChildren();  // copy
    for (auto* child : children)
    {
        m_Scene->DestroyDekiObject(child);
    }

    // Clear the components: ExpandInstance re-adds them from the source, and
    // Redo would duplicate them otherwise.
    auto comps = m_Object->GetComponents();  // copy
    for (auto* comp : comps)
    {
        HistoryGuards::ForgetComponent(comp);
        m_Object->RemoveComponent(comp);
    }

    // Expand from the source scene with the overrides applied.
    std::string savedName = m_Object->GetName();
    m_Scene->ExpandInstance(m_Object);
    m_Object->SetName(savedName);

    if (m_OnLinked)
    {
        m_OnLinked();
    }
}

void LinkInstanceCommand::Undo()
{
    m_Scene->CollapseInstance(m_Object);

    // Swap the components ExpandInstance added for the originals.
    auto comps = m_Object->GetComponents();  // copy
    for (auto* comp : comps)
    {
        HistoryGuards::ForgetComponent(comp);
        m_Object->RemoveComponent(comp);
    }
    RestoreComponents(m_Object, m_SavedComponents);

    // Empty when the object was not linked before.
    m_Object->SetSourceSceneGuid(m_PreviousGuid);
    m_Object->SetInstanceOverridesJson(m_PreviousOverrides);

    CreateChildrenRecursive(m_Object, m_SavedChildren);

    // Mark the children as instance parts again if it was linked before.
    if (!m_PreviousGuid.empty())
    {
        for (auto* child : m_Object->GetChildren())
        {
            std::function<void(Deki::Object*)> markRecursive = [&](Deki::Object* o)
            {
                o->SetPartOfInstance(true);
                for (auto* c : o->GetChildren())
                {
                    markRecursive(c);
                }
            };
            markRecursive(child);
        }
    }

    if (m_OnUndone)
    {
        m_OnUndone();
    }
}

void LinkInstanceCommand::Redo()
{
    // Snapshot again: Undo rebuilt the children and components.
    m_SavedComponents = SerializeComponents(m_Object);
    m_SavedChildren = SerializeChildren(m_Object);
    Execute();
}

bool LinkInstanceCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::HasAny(dead, m_Scene, m_Object);
}

std::string ApplyInstanceToSceneCommand::GetDescription() const
{
    return Tr("Apply to Scene");
}

void ApplyInstanceToSceneCommand::Execute()
{
    using namespace InstanceCommandsDetail;

    if (!m_Scene || !m_Instance || !m_Instance->IsSceneInstance())
    {
        return;
    }
    if (!m_SelectedOverrides.is_array() || m_SelectedOverrides.empty())
    {
        return;
    }

    m_SourceGuid = m_Instance->GetSourceSceneGuid();
    if (!Deki::Scene::s_GuidToPathResolver)
    {
        return;
    }

    m_SourcePath = Deki::Scene::s_GuidToPathResolver(m_SourceGuid);
    if (m_SourcePath.empty())
    {
        return;
    }

    // Snapshot for undo.
    m_SavedSourceJson = ReadFileToString(m_SourcePath);
    m_SavedInstanceOverrides = m_Instance->GetInstanceOverridesJson();

    // Load source, patch only the selected overrides, save.
    Deki::Scene source;
    if (!source.LoadFromFile(m_SourcePath))
    {
        return;
    }

    const auto& sourceRoots = source.GetObjects();
    if (sourceRoots.empty())
    {
        return;
    }
    if (sourceRoots.size() > 1)
    {
        DEKI_LOG_WARNING("ApplyInstanceToScene: multi-root source '%s' - "
                         "only the first root is targeted by overrides",
                         m_SourcePath.c_str());
    }
    source.ApplyOverrides(sourceRoots[0], m_SelectedOverrides);
    if (!source.SaveToFile(m_SourcePath))
    {
        return;
    }

    // Update scene files that are not open: LoadFromFile expands each against
    // the new source, and SaveToFile recomputes its overrides.
    m_SavedOtherScenes.clear();
    for (const auto& path : m_OtherScenePaths)
    {
        if (path == m_SourcePath)
        {
            continue;
        }

        Deki::Scene otherScene;
        if (!otherScene.LoadFromFile(path))
        {
            continue;
        }
        if (!SceneReferencesGuid(otherScene, m_SourceGuid))
        {
            continue;
        }

        m_SavedOtherScenes.emplace_back(path, ReadFileToString(path));
        otherScene.SaveToFile(path);
    }

    // Re-expand other instances of the same scene in the current scene so
    // they show the new source. The applied instance stays as it is: it
    // already shows the right result, and re-expanding it would discard
    // uncommitted live edits.
    std::vector<Deki::Object*> siblings;
    CollectInstancesByGuid(m_Scene, m_SourceGuid, /*exclude*/ m_Instance, siblings);
    for (auto* inst : siblings)
    {
        ReexpandInstance(m_Scene, inst);
    }
}

void ApplyInstanceToSceneCommand::Undo()
{
    using namespace InstanceCommandsDetail;

    if (m_SourcePath.empty())
    {
        return;
    }

    // Roll back the other scene files Execute rewrote.
    for (const auto& [path, content] : m_SavedOtherScenes)
    {
        WriteStringToFile(path, content);
    }
    m_SavedOtherScenes.clear();

    WriteStringToFile(m_SourcePath, m_SavedSourceJson);
    if (m_Instance)
    {
        m_Instance->SetInstanceOverridesJson(m_SavedInstanceOverrides);
    }

    // Re-expand the other instances against the rolled-back source. The
    // applied instance was not re-expanded in Execute, so it stays as it is.
    std::vector<Deki::Object*> siblings;
    CollectInstancesByGuid(m_Scene, m_SourceGuid, /*exclude*/ m_Instance, siblings);
    for (auto* inst : siblings)
    {
        ReexpandInstance(m_Scene, inst);
    }
}

bool ApplyInstanceToSceneCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::HasAny(dead, m_Scene, m_Instance);
}

bool ApplyInstanceToSceneCommand::SceneReferencesGuid(const Deki::Scene& p, const std::string& guid)
{
    std::function<bool(const Deki::Object*)> walk = [&](const Deki::Object* obj) -> bool
    {
        if (!obj)
        {
            return false;
        }
        if (obj->GetSourceSceneGuid() == guid)
        {
            return true;
        }
        for (auto* c : obj->GetChildren())
        {
            if (walk(c))
            {
                return true;
            }
        }
        return false;
    };
    for (auto* root : p.GetObjects())
    {
        if (walk(root))
        {
            return true;
        }
    }
    return false;
}

std::string RevertInstanceOverridesCommand::GetDescription() const
{
    return Tr("Revert Instance");
}

void RevertInstanceOverridesCommand::Execute()
{
    using namespace InstanceCommandsDetail;
    if (!m_Scene || !m_Instance || !m_Instance->IsSceneInstance())
    {
        return;
    }

    m_SavedOverrides = m_Instance->GetInstanceOverridesJson();
    m_Instance->SetInstanceOverridesJson("");
    ReexpandInstance(m_Scene, m_Instance);
}

void RevertInstanceOverridesCommand::Undo()
{
    using namespace InstanceCommandsDetail;
    if (!m_Scene || !m_Instance)
    {
        return;
    }

    m_Instance->SetInstanceOverridesJson(m_SavedOverrides);
    ReexpandInstance(m_Scene, m_Instance);
}

bool RevertInstanceOverridesCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::HasAny(dead, m_Scene, m_Instance);
}

}  // namespace DekiEditor
