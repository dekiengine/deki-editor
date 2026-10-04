#pragma once

#include <deki-editor/Command.h>
#include "CommandRefCheck.h"
#include "ObjectSerializationHelper.h"
#include <deki/Scene.h>
#include <deki/Object.h>
#include <deki/Component.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki-editor/EditorExtension.h>
#include <functional>
#include <string>
#include <vector>

namespace DekiEditor
{

// Named imports rather than `using namespace`: a wholesale import in a header
// is open-ended, so anything later added to ObjectSerializationHelper would
// silently appear in every DekiEditor TU that includes this.
using ObjectSerializationHelper::ComponentData;
using ObjectSerializationHelper::RestoreComponentProperties;
using ObjectSerializationHelper::SerializeComponent;

/**
 * Command for deleting a Deki::Object
 * Keeps the object alive in memory (detached) so undo preserves all runtime state
 */
class DeleteObjectCommand : public Command
{
public:
    DeleteObjectCommand(Deki::Scene* scene, Deki::Object* obj, std::function<void()> onDeleted = nullptr,
                        std::function<void(Deki::Object*)> onRestored = nullptr)
        : m_Scene(scene),
          m_Object(obj),
          m_ObjectName(obj->GetName()),
          m_OnDeleted(onDeleted),
          m_OnRestored(onRestored)
    {
        // Record position for re-insertion
        Deki::Object* parent = obj->GetParent();
        m_ParentId = parent ? parent->GetId() : 0;
        m_OriginalIndex = parent ? parent->GetChildIndex(obj) : scene->GetObjectIndex(obj);
    }

    ~DeleteObjectCommand()
    {
        // If object is still detached (not in the scene), we own it
        if (m_IsDetached && m_Object)
        {
            delete m_Object;
        }
    }

    std::string GetDescription() const override;

    void Execute() override;

    void Undo() override;

    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    Deki::Scene* m_Scene;
    Deki::Object* m_Object;
    std::string m_ObjectName;
    uint32_t m_ParentId = 0;
    int m_OriginalIndex = -1;
    bool m_IsDetached = false;
    std::function<void()> m_OnDeleted;
    std::function<void(Deki::Object*)> m_OnRestored;
};

/**
 * Command for toggling a Deki::Object's active flag.
 */
class SetActiveCommand : public Command
{
public:
    SetActiveCommand(Deki::Object* obj, bool active)
        : m_Object(obj),
          m_ObjectName(obj->GetName()),
          m_Old(obj->IsActive()),
          m_New(active)
    {
    }

    std::string GetDescription() const override;
    void Execute() override;
    void Undo() override;
    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    Deki::Object* m_Object;
    std::string m_ObjectName;
    bool m_Old;
    bool m_New;
};

/**
 * Command for adding a component to a Deki::Object
 */
class AddComponentCommand : public Command
{
public:
    AddComponentCommand(Deki::Object* obj, const std::string& typeName,
                        std::function<void(Deki::Component*)> onAdded = nullptr)
        : m_Object(obj),
          m_TypeName(typeName),
          m_OnAdded(onAdded)
    {
    }
    ~AddComponentCommand() override;

    std::string GetDescription() const override;

    void Execute() override;

    void Undo() override;

    bool ReferencesAny(const DeadMemory& dead) const override;

    Deki::Component* GetComponent() const { return m_Component; }

private:
    Deki::Object* m_Object;
    std::string m_TypeName;
    std::function<void(Deki::Component*)> m_OnAdded;
    // The instance this command added; while undone it is off the object and
    // owned here (m_Detached), and redo puts the same one back at m_Index.
    Deki::Component* m_Component = nullptr;
    bool m_Detached = false;
    int m_Index = -1;
};

/**
 * Command for removing a component from a Deki::Object
 * Serializes component properties for undo support
 */
class RemoveComponentCommand : public Command
{
public:
    RemoveComponentCommand(Deki::Object* obj, Deki::Component* comp, std::function<void()> onRemoved = nullptr)
        : m_Object(obj),
          m_Component(comp),
          m_OnRemoved(onRemoved)
    {
        m_TypeName = SerializeComponent(comp).typeName;
    }
    ~RemoveComponentCommand() override;

    std::string GetDescription() const override;

    void Execute() override;

    void Undo() override;

    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    Deki::Object* m_Object;
    std::string m_TypeName;
    // The removed instance, kept while removed (m_Detached) so undo puts the
    // same one back at m_Index.
    Deki::Component* m_Component;
    bool m_Detached = false;
    int m_Index = -1;
    std::function<void()> m_OnRemoved;
};

/**
 * Command for duplicating a Deki::Object in the scene
 * Uses the engine's DuplicateObject (JSON round-trip) for full state fidelity.
 * Undo/redo detaches/reattaches the clone to preserve runtime state.
 */
class DuplicateObjectCommand : public Command
{
public:
    DuplicateObjectCommand(Deki::Scene* scene, Deki::Object* source, Deki::Object* parent = nullptr,
                           std::function<void(Deki::Object*)> onDuplicated = nullptr,
                           std::function<void()> onUndone = nullptr)
        : m_Scene(scene),
          m_Source(source),
          m_Parent(parent),
          m_SourceName(source->GetName()),
          m_OnDuplicated(onDuplicated),
          m_OnUndone(onUndone)
    {
    }

    ~DuplicateObjectCommand()
    {
        // If clone is detached (undone), we own it
        if (m_IsDetached && m_DuplicatedObject)
        {
            delete m_DuplicatedObject;
        }
    }

    std::string GetDescription() const override;

    void Execute() override;

    void Undo() override;

    void Redo() override;

    Deki::Object* GetDuplicatedObject() const { return m_DuplicatedObject; }

    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    Deki::Scene* m_Scene;
    Deki::Object* m_Source;
    Deki::Object* m_Parent;
    std::string m_SourceName;
    std::function<void(Deki::Object*)> m_OnDuplicated;
    std::function<void()> m_OnUndone;
    Deki::Object* m_DuplicatedObject = nullptr;
    bool m_IsDetached = false;
};

}  // namespace DekiEditor
