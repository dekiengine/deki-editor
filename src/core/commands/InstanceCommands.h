#pragma once

#include <deki-editor/Command.h>
#include <deki-editor/FileIO.h>
#include "CommandRefCheck.h"
#include "HistoryGuards.h"
#include "ObjectSerializationHelper.h"
#include <deki/Scene.h>
#include <deki/Object.h>
#include <deki/LogSystem.h>
#include <deki/reflection/ComponentRegistry.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace DekiEditor
{

// Named imports rather than `using namespace`, so things later added to
// ObjectSerializationHelper do not leak into every file that includes this.
using ObjectSerializationHelper::ChildData;
using ObjectSerializationHelper::ComponentData;
using ObjectSerializationHelper::CreateChildrenRecursive;
using ObjectSerializationHelper::RestoreComponents;
using ObjectSerializationHelper::SerializeChildren;
using ObjectSerializationHelper::SerializeComponents;

/// Creates a linked scene instance: a Deki::Object with a source scene GUID,
/// expanded from that scene.
class CreateInstanceCommand : public Command
{
public:
    CreateInstanceCommand(Deki::Scene* scene, const std::string& sourceSceneGuid, const std::string& instanceName,
                          Deki::Object* parent = nullptr, std::function<void(Deki::Object*)> onCreated = nullptr,
                          std::function<void()> onUndone = nullptr)
        : m_Scene(scene),
          m_SourceSceneGuid(sourceSceneGuid),
          m_InstanceName(instanceName),
          m_Parent(parent),
          m_OnCreated(onCreated),
          m_OnUndone(onUndone),
          m_InstanceObject(nullptr)
    {
    }

    std::string GetDescription() const override;

    void Execute() override;

    void Undo() override;

    void Redo() override;

    Deki::Object* GetInstanceObject() const { return m_InstanceObject; }

    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    Deki::Scene* m_Scene;
    std::string m_SourceSceneGuid;
    std::string m_InstanceName;
    Deki::Object* m_Parent;
    std::function<void(Deki::Object*)> m_OnCreated;
    std::function<void()> m_OnUndone;
    Deki::Object* m_InstanceObject;
};

/// Unlinks a scene instance into regular objects. Clears the source GUID and
/// the instance markers, so the objects become editable.
class UnlinkInstanceCommand : public Command
{
public:
    UnlinkInstanceCommand(Deki::Object* instanceObj, std::function<void()> onUnlinked = nullptr)
        : m_InstanceObject(instanceObj),
          m_OnUnlinked(onUnlinked)
    {
        m_SavedGuid = instanceObj->GetSourceSceneGuid();
        m_SavedOverrides = instanceObj->GetInstanceOverridesJson();
    }

    std::string GetDescription() const override;

    void Execute() override;

    void Undo() override;

    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    void ClearInstanceFlags(Deki::Object* obj);

    void SetInstanceFlags(Deki::Object* obj);

    Deki::Object* m_InstanceObject;
    std::string m_SavedGuid;
    std::string m_SavedOverrides;
    std::function<void()> m_OnUnlinked;
};

/// True when an object's hierarchy matches the structure of the scene
/// `sceneGuid`: the same child count and component types at every level.
inline bool ValidateHierarchyMatch(Deki::Object* obj, const std::string& sceneGuid)
{
    if (!Deki::Scene::s_GuidToPathResolver)
    {
        return false;
    }

    std::string sourcePath = Deki::Scene::s_GuidToPathResolver(sceneGuid);
    if (sourcePath.empty())
    {
        return false;
    }

    Deki::Scene sourceScene;
    if (!sourceScene.LoadFromFile(sourcePath))
    {
        return false;
    }

    auto& registry = Deki::ComponentRegistry::Instance();

    std::function<bool(const Deki::Object*, const Deki::Object*)> compareNodes = [&](const Deki::Object* a,
                                                                                     const Deki::Object* b) -> bool
    {
        const auto& aComps = a->GetComponents();
        const auto& bComps = b->GetComponents();
        if (aComps.size() != bComps.size())
        {
            return false;
        }

        for (size_t i = 0; i < aComps.size(); ++i)
        {
            if (aComps[i]->GetType() != bComps[i]->GetType())
            {
                return false;
            }
        }

        const auto& aChildren = a->GetChildren();
        const auto& bChildren = b->GetChildren();
        if (aChildren.size() != bChildren.size())
        {
            return false;
        }

        for (size_t i = 0; i < aChildren.size(); ++i)
        {
            if (!compareNodes(aChildren[i], bChildren[i]))
            {
                return false;
            }
        }

        return true;
    };

    // A single-root source compares by its root's children, because
    // ExpandInstance skips the root wrapper.
    const auto& objChildren = obj->GetChildren();
    const auto& sourceRoots = sourceScene.GetObjects();

    const std::vector<Deki::Object*>& sourceCompareList =
        (sourceRoots.size() == 1) ? sourceRoots[0]->GetChildren() : sourceRoots;

    if (objChildren.size() != sourceCompareList.size())
    {
        return false;
    }

    for (size_t i = 0; i < objChildren.size(); ++i)
    {
        if (!compareNodes(objChildren[i], sourceCompareList[i]))
        {
            return false;
        }
    }

    return true;
}

/// Links an existing object tree to a scene, the reverse of Unlink. Records
/// the differences as overrides, then replaces the children with the
/// expanded instance.
class LinkInstanceCommand : public Command
{
public:
    LinkInstanceCommand(Deki::Scene* scene, Deki::Object* obj, const std::string& sourceSceneGuid,
                        std::function<void()> onLinked = nullptr, std::function<void()> onUndone = nullptr)
        : m_Scene(scene),
          m_Object(obj),
          m_SourceSceneGuid(sourceSceneGuid),
          m_OnLinked(onLinked),
          m_OnUndone(onUndone)
    {
        // The previous instance state, for Undo; the object may already be linked.
        m_PreviousGuid = obj->GetSourceSceneGuid();
        m_PreviousOverrides = obj->GetInstanceOverridesJson();
        m_SavedChildren = SerializeChildren(obj);
        m_SavedComponents = SerializeComponents(obj);
    }

    std::string GetDescription() const override;

    void Execute() override;

    void Undo() override;

    void Redo() override;

    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    Deki::Scene* m_Scene;
    Deki::Object* m_Object;
    std::string m_SourceSceneGuid;
    std::string m_PreviousGuid;
    std::string m_PreviousOverrides;
    std::function<void()> m_OnLinked;
    std::function<void()> m_OnUndone;
    std::vector<ChildData> m_SavedChildren;
    std::vector<ComponentData> m_SavedComponents;
};

// ============================================================================
// Apply / Revert
// ============================================================================

namespace InstanceCommandsDetail
{

inline std::string ReadFileToString(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// In one step (temp file + rename): applying to a prefab rewrites another
// scene, and a crash halfway must not leave it cut off.
inline bool WriteStringToFile(const std::string& path, const std::string& content)
{
    std::string err;
    return DekiEditor::AtomicWriteFile(path, content, err);
}

// Collects every object in the scene (roots and descendants) whose
// SourceSceneGuid is `guid`, except `exclude`. The raw pointers are valid
// only during one command call.
inline void CollectInstancesByGuid(Deki::Scene* scene, const std::string& guid, Deki::Object* exclude,
                                   std::vector<Deki::Object*>& out)
{
    if (!scene || guid.empty())
    {
        return;
    }

    std::function<void(Deki::Object*)> walk = [&](Deki::Object* obj)
    {
        if (!obj)
        {
            return;
        }
        if (obj != exclude && obj->GetSourceSceneGuid() == guid)
        {
            out.push_back(obj);
        }
        for (auto* child : obj->GetChildren())
        {
            walk(child);
        }
    };

    for (auto* root : scene->GetObjects())
    {
        walk(root);
    }
}

// Builds an in-memory Scene whose single root holds a deep copy of
// `instanceObj`'s components and children. The copy leaves out the instance
// data (SourceSceneGuid, overrides), so the written file is a plain source
// scene, not another instance.
inline bool BuildSourceSceneFromInstance(const Deki::Object* instanceObj, Deki::Scene& outScene)
{
    using namespace ObjectSerializationHelper;

    Deki::Object* root = outScene.CreateDekiObject(instanceObj->GetName());
    if (!root)
    {
        return false;
    }

    RestoreComponents(root, SerializeComponents(instanceObj));
    CreateChildrenRecursive(root, SerializeChildren(instanceObj));
    return true;
}

// Collapses and re-expands an instance, keeping its stored overrides JSON.
// CollapseInstance removes only the children, and ExpandInstance re-adds the
// source root's components, so the components are cleared first or they
// would duplicate on every re-expand.
inline void ReexpandInstance(Deki::Scene* scene, Deki::Object* inst)
{
    if (!scene || !inst || !inst->IsSceneInstance())
    {
        return;
    }
    scene->CollapseInstance(inst);
    auto comps = inst->GetComponents();  // copy; RemoveComponent mutates
    for (auto* c : comps)
    {
        HistoryGuards::ForgetComponent(c);
        inst->RemoveComponent(c);
    }
    scene->ExpandInstance(inst);
}

}  // namespace InstanceCommandsDetail

/// Writes some of a scene instance's overrides back to the source scene on
/// disk, property by property.
///
/// `selectedOverrides` is a JSON array, a subset of what `CollectOverrides`
/// emits. Execute:
///   - Loads the source `.scene`, applies only the selected overrides to its
///     root with `Scene::ApplyOverrides`, and saves it.
///   - Leaves the live instance as it is: it already shows the right result,
///     and its overrides are recomputed against the new source on the next
///     save, so the applied fields drop out.
///   - Re-expands the other instances of that scene in the current scene.
///   - Loads and saves every other `.scene` file that references the source
///     GUID, so scenes that are not open get fresh override JSON.
///
/// Undo restores the source file, the other scene files and the instance's
/// stored `InstanceOverridesJson`, then re-expands the other instances.
class ApplyInstanceToSceneCommand : public Command
{
public:
    ApplyInstanceToSceneCommand(Deki::Scene* scene, Deki::Object* instanceObj,
                                std::vector<std::string> otherScenePaths = {}, nlohmann::json selectedOverrides = {})
        : m_Scene(scene),
          m_Instance(instanceObj),
          m_OtherScenePaths(std::move(otherScenePaths)),
          m_SelectedOverrides(std::move(selectedOverrides))
    {
    }

    std::string GetDescription() const override;

    void Execute() override;

    void Undo() override;

    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    // True if any object in `p` (root or descendant) has SourceSceneGuid == guid.
    static bool SceneReferencesGuid(const Deki::Scene& p, const std::string& guid);

    Deki::Scene* m_Scene;
    Deki::Object* m_Instance;
    std::string m_SourceGuid;
    std::string m_SourcePath;
    std::string m_SavedSourceJson;
    std::string m_SavedInstanceOverrides;
    std::vector<std::string> m_OtherScenePaths;
    nlohmann::json m_SelectedOverrides;
    std::vector<std::pair<std::string, std::string>> m_SavedOtherScenes;
};

/// Reverts a scene instance to its source: drops all its overrides and
/// re-expands it from the source file. Undo restores the stored override
/// JSON and re-expands.
class RevertInstanceOverridesCommand : public Command
{
public:
    RevertInstanceOverridesCommand(Deki::Scene* scene, Deki::Object* instanceObj)
        : m_Scene(scene),
          m_Instance(instanceObj)
    {
    }

    std::string GetDescription() const override;

    void Execute() override;

    void Undo() override;

    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    Deki::Scene* m_Scene;
    Deki::Object* m_Instance;
    std::string m_SavedOverrides;
};

}  // namespace DekiEditor
