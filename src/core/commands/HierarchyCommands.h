#pragma once

#include <deki-editor/Command.h>
#include "CommandRefCheck.h"
#include <deki/Scene.h>
#include <deki/Object.h>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace DekiEditor
{

/**
 * Command for creating a Deki::Object in the scene.
 *
 * Undo detaches the object and keeps it alive rather than destroying it — the
 * same ownership model DeleteObjectCommand uses, and for the same reason.
 * Destroying on undo and re-creating on redo handed back a *different* object
 * with a different id, which (a) left every other command still holding the old
 * pointer dangling, so the next redo wrote through freed memory, and (b) broke
 * ObjectRef properties and instance overrides that pointed at the original.
 * The object is only deleted when this command is destroyed while still undone,
 * at which point nothing can redo it back into the scene.
 */
class CreateObjectCommand : public Command
{
public:
    CreateObjectCommand(Deki::Scene* scene, const std::string& name = "DekiObject",
                        std::function<void(Deki::Object*)> onCreated = nullptr,
                        Deki::Object* parent = nullptr,
                        std::function<void()> onUndone = nullptr)
        : m_Scene(scene), m_Name(name), m_OnCreated(onCreated), m_Parent(parent), m_OnUndone(onUndone), m_CreatedObject(nullptr)
    {
    }

    ~CreateObjectCommand()
    {
        // Detached means undone and unreachable from the scene, so we own it.
        if (m_IsDetached && m_CreatedObject)
            delete m_CreatedObject;
    }

    std::string GetDescription() const override;

    void Execute() override;

    void Undo() override;

    void Redo() override;

    bool ReferencesAny(const DeadMemory& dead) const override;

    Deki::Object* GetCreatedObject() const { return m_CreatedObject; }

private:
    Deki::Scene* m_Scene;
    std::string m_Name;
    std::function<void(Deki::Object*)> m_OnCreated;
    Deki::Object* m_Parent;
    std::function<void()> m_OnUndone;
    Deki::Object* m_CreatedObject;
    int m_DetachedIndex = -1;
    bool m_IsDetached = false;
};

/**
 * Command for renaming a Deki::Object
 */
class RenameObjectCommand : public Command
{
public:
    RenameObjectCommand(Deki::Object* obj, const std::string& newName)
        : m_Object(obj), m_OldName(obj->GetName()), m_NewName(newName)
    {
    }

    std::string GetDescription() const override;

    void Execute() override;

    void Undo() override;

    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    Deki::Object* m_Object;
    std::string m_OldName;
    std::string m_NewName;
};

/**
 * Command for reparenting a Deki::Object (changing its parent)
 * Supports both parenting to another object and unparenting (setting parent to nullptr)
 * Also manages the scene's root object list appropriately
 */
class ReparentObjectCommand : public Command
{
public:
    ReparentObjectCommand(Deki::Object* obj, Deki::Object* newParent)
        : m_Object(obj), m_Scene(obj->GetOwnerScene()), m_OldParent(obj->GetParent()), m_NewParent(newParent), m_ObjectName(obj->GetName())
    {
    }

    std::string GetDescription() const override;

    void Execute() override;

    void Undo() override;

    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    Deki::Object* m_Object;
    Deki::Scene* m_Scene;
    Deki::Object* m_OldParent;
    Deki::Object* m_NewParent;
    std::string m_ObjectName;
};

/**
 * Command for moving multiple objects to a new parent/position atomically.
 * Handles reparenting + reordering in a single undo step.
 * Objects maintain their relative order at the insertion point.
 */
class MoveObjectsCommand : public Command
{
public:
    MoveObjectsCommand(Deki::Scene* scene, std::vector<Deki::Object*> objects,
                       Deki::Object* targetParent, size_t targetIndex)
        : m_Scene(scene), m_Objects(std::move(objects)), m_TargetParent(targetParent), m_TargetIndex(targetIndex)
    {
        // Capture original parent + index for each object
        m_OldStates.reserve(m_Objects.size());
        for (Deki::Object* obj : m_Objects)
        {
            ObjectState state;
            state.object = obj;
            state.parent = obj->GetParent();
            if (state.parent)
                state.index = static_cast<size_t>(state.parent->GetChildIndex(obj));
            else
                state.index = static_cast<size_t>(m_Scene->GetObjectIndex(obj));
            m_OldStates.push_back(state);
        }
    }

    std::string GetDescription() const override;

    void Execute() override;

    void Undo() override;

    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    struct ObjectState
    {
        Deki::Object* object;
        Deki::Object* parent;  // nullptr = root level
        size_t index;
    };

    Deki::Scene* m_Scene;
    std::vector<Deki::Object*> m_Objects;
    std::vector<ObjectState> m_OldStates;
    Deki::Object* m_TargetParent;
    size_t m_TargetIndex;
};

}  // namespace DekiEditor
