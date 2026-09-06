#include "InstanceCommands.h"
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
    return "Create Instance " + m_InstanceName;
}

void CreateInstanceCommand::Execute()
{
    m_InstanceObject = m_Scene->CreateDekiObject(m_InstanceName);
    if (!m_InstanceObject) return;

    m_InstanceObject->SetSourceSceneGuid(m_SourceSceneGuid);

    // Single-root invariant: a null parent means "inside the scene root", not "as a sibling of root".
    // If no root exists yet, the new object becomes the root.
    Deki::Object* resolvedParent = m_Parent;
    if (!resolvedParent)
    {
        auto& roots = m_Scene->GetObjects();
        if (!roots.empty())
            resolvedParent = roots[0];
    }

    if (resolvedParent)
    {
        m_Scene->RemoveObject(m_InstanceObject);
        m_InstanceObject->SetOwnerScene(m_Scene);
        resolvedParent->AddChild(m_InstanceObject);
    }

    m_Scene->ExpandInstance(m_InstanceObject);

    if (m_OnCreated) m_OnCreated(m_InstanceObject);
}

void CreateInstanceCommand::Undo()
{
    if (m_InstanceObject)
    {
        if (m_OnUndone) m_OnUndone();
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
    return "Unlink Instance";
}

void UnlinkInstanceCommand::Execute()
{
    // Clear instance markers on root and all children
    m_InstanceObject->SetSourceSceneGuid("");
    m_InstanceObject->SetInstanceOverridesJson("");
    ClearInstanceFlags(m_InstanceObject);

    if (m_OnUnlinked) m_OnUnlinked();
}

void UnlinkInstanceCommand::Undo()
{
    // Restore instance state
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
        ClearInstanceFlags(child);
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
    return "Link to Scene";
}

void LinkInstanceCommand::Execute()
{
    // Set the source GUID so CollectOverrides can diff against source
    m_Object->SetSourceSceneGuid(m_SourceSceneGuid);

    // Collect overrides by comparing current children vs source scene
    nlohmann::json overrides = nlohmann::json::array();
    m_Scene->CollectOverrides(m_Object, m_Object, overrides);

    if (!overrides.empty())
        m_Object->SetInstanceOverridesJson(overrides.dump());
    else
        m_Object->SetInstanceOverridesJson("");

    // Delete existing children (will be replaced by expanded source)
    auto children = m_Object->GetChildren();  // copy
    for (auto* child : children)
    {
        m_Object->RemoveChild(child);
        delete child;
    }

    // Clear existing components (ExpandInstance will re-add from source;
    // without this, Redo would duplicate them)
    auto comps = m_Object->GetComponents();  // copy
    for (auto* comp : comps)
        m_Object->RemoveComponent(comp);

    // Expand from source scene with overrides applied
    std::string savedName = m_Object->GetName();
    m_Scene->ExpandInstance(m_Object);
    m_Object->SetName(savedName);

    if (m_OnLinked) m_OnLinked();
}

void LinkInstanceCommand::Undo()
{
    // Collapse expanded instance children
    m_Scene->CollapseInstance(m_Object);

    // Remove components added by ExpandInstance, restore originals
    auto comps = m_Object->GetComponents();  // copy
    for (auto* comp : comps)
        m_Object->RemoveComponent(comp);
    RestoreComponents(m_Object, m_SavedComponents);

    // Restore previous instance state (or clear if wasn't linked before)
    m_Object->SetSourceSceneGuid(m_PreviousGuid);
    m_Object->SetInstanceOverridesJson(m_PreviousOverrides);

    // Restore original children
    CreateChildrenRecursive(m_Object, m_SavedChildren);

    // Restore instance flags if was previously linked
    if (!m_PreviousGuid.empty())
    {
        for (auto* child : m_Object->GetChildren())
        {
            std::function<void(Deki::Object*)> markRecursive = [&](Deki::Object* o)
            {
                o->SetPartOfInstance(true);
                for (auto* c : o->GetChildren())
                    markRecursive(c);
            };
            markRecursive(child);
        }
    }

    if (m_OnUndone) m_OnUndone();
}

void LinkInstanceCommand::Redo()
{
    // Re-save state before re-executing (undo restored them)
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
    return "Apply to Scene";
}

void ApplyInstanceToSceneCommand::Execute()
{
    using namespace InstanceCommandsDetail;

    if (!m_Scene || !m_Instance || !m_Instance->IsSceneInstance())
        return;
    if (!m_SelectedOverrides.is_array() || m_SelectedOverrides.empty())
        return;

    m_SourceGuid = m_Instance->GetSourceSceneGuid();
    if (!Deki::Scene::s_GuidToPathResolver) return;

    m_SourcePath = Deki::Scene::s_GuidToPathResolver(m_SourceGuid);
    if (m_SourcePath.empty()) return;

    // Snapshot for undo.
    m_SavedSourceJson = ReadFileToString(m_SourcePath);
    m_SavedInstanceOverrides = m_Instance->GetInstanceOverridesJson();

    // Load source, patch only the selected overrides, save.
    Deki::Scene source;
    if (!source.LoadFromFile(m_SourcePath)) return;

    const auto& sourceRoots = source.GetObjects();
    if (sourceRoots.empty()) return;
    if (sourceRoots.size() > 1)
    {
        DEKI_LOG_WARNING("ApplyInstanceToScene: multi-root source '%s' — "
                         "only the first root is targeted by overrides",
                         m_SourcePath.c_str());
    }
    source.ApplyOverrides(sourceRoots[0], m_SelectedOverrides);
    if (!source.SaveToFile(m_SourcePath))
        return;

    // Propagate to unopened scene files (each one's LoadFromFile expands
    // against the new source; SaveToFile recomputes its overrides fresh).
    m_SavedOtherScenes.clear();
    for (const auto& path : m_OtherScenePaths)
    {
        if (path == m_SourcePath) continue;

        Deki::Scene otherScene;
        if (!otherScene.LoadFromFile(path)) continue;
        if (!SceneReferencesGuid(otherScene, m_SourceGuid)) continue;

        m_SavedOtherScenes.emplace_back(path, ReadFileToString(path));
        otherScene.SaveToFile(path);
    }

    // Refresh sibling instances of this same scene in the current scene
    // so they render against the new source. The applied instance itself
    // is left alone — its in-memory render is already correct, and
    // touching it would discard any uncommitted live edits.
    std::vector<Deki::Object*> siblings;
    CollectInstancesByGuid(m_Scene, m_SourceGuid, /*exclude*/ m_Instance,
                           siblings);
    for (auto* inst : siblings)
        ReexpandInstance(m_Scene, inst);
}

void ApplyInstanceToSceneCommand::Undo()
{
    using namespace InstanceCommandsDetail;

    if (m_SourcePath.empty()) return;

    // Roll back any scene files we rewrote.
    for (const auto& [path, content] : m_SavedOtherScenes)
        WriteStringToFile(path, content);
    m_SavedOtherScenes.clear();

    WriteStringToFile(m_SourcePath, m_SavedSourceJson);
    if (m_Instance)
        m_Instance->SetInstanceOverridesJson(m_SavedInstanceOverrides);

    // Re-expand siblings against the rolled-back source. The applied
    // instance wasn't re-expanded in Execute, so no need to touch it here.
    std::vector<Deki::Object*> siblings;
    CollectInstancesByGuid(m_Scene, m_SourceGuid, /*exclude*/ m_Instance,
                           siblings);
    for (auto* inst : siblings)
        ReexpandInstance(m_Scene, inst);
}

bool ApplyInstanceToSceneCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::HasAny(dead, m_Scene, m_Instance);
}

bool ApplyInstanceToSceneCommand::SceneReferencesGuid(const Deki::Scene& p, const std::string& guid)
{
    std::function<bool(const Deki::Object*)> walk = [&](const Deki::Object* obj) -> bool
    {
        if (!obj) return false;
        if (obj->GetSourceSceneGuid() == guid) return true;
        for (auto* c : obj->GetChildren())
            if (walk(c)) return true;
        return false;
    };
    for (auto* root : p.GetObjects())
        if (walk(root)) return true;
    return false;
}

std::string RevertInstanceOverridesCommand::GetDescription() const
{
    return "Revert Instance";
}

void RevertInstanceOverridesCommand::Execute()
{
    using namespace InstanceCommandsDetail;
    if (!m_Scene || !m_Instance || !m_Instance->IsSceneInstance())
        return;

    m_SavedOverrides = m_Instance->GetInstanceOverridesJson();
    m_Instance->SetInstanceOverridesJson("");
    ReexpandInstance(m_Scene, m_Instance);
}

void RevertInstanceOverridesCommand::Undo()
{
    using namespace InstanceCommandsDetail;
    if (!m_Scene || !m_Instance) return;

    m_Instance->SetInstanceOverridesJson(m_SavedOverrides);
    ReexpandInstance(m_Scene, m_Instance);
}

bool RevertInstanceOverridesCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::HasAny(dead, m_Scene, m_Instance);
}

}  // namespace DekiEditor
