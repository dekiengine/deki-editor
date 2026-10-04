#pragma once

#include <deki-editor/Command.h>
#include "CommandRefCheck.h"
#include "ObjectSerializationHelper.h"
#include <vector>
#include <string>
#include <functional>

namespace Deki
{
class Object;
}
namespace Deki
{
class Component;
}
namespace Deki
{
class Scene;
}

namespace DekiEditor
{

// Named imports rather than `using namespace`, so things later added to
// ObjectSerializationHelper do not leak into every file that includes this.
using ObjectSerializationHelper::ChildData;
using ObjectSerializationHelper::ComponentData;
using ObjectSerializationHelper::ObjectData;

/// Pastes component property values onto one or more components.
class PasteComponentValuesCommand : public Command
{
public:
    PasteComponentValuesCommand(const std::vector<Deki::Component*>& components, const ComponentData& sourceData,
                                std::function<void()> onChange = nullptr);

    std::string GetDescription() const override;
    void Execute() override;
    void Undo() override;

    bool ReferencesAny(const DeadMemory& dead) const override
    {
        for (const auto& e : m_Entries)
        {
            if (CommandRefs::Has(dead, e.component))
            {
                return true;
            }
        }
        return false;
    }

private:
    struct Entry
    {
        Deki::Component* component;
        ComponentData oldData;
    };
    std::vector<Entry> m_Entries;
    ComponentData m_SourceData;
    std::function<void()> m_OnChange;
};

/// Pastes a component as a new component on one or more objects.
class PasteComponentAsNewCommand : public Command
{
public:
    PasteComponentAsNewCommand(const std::vector<Deki::Object*>& objects, const ComponentData& sourceData,
                               std::function<void(Deki::Component*)> onAdded = nullptr);
    ~PasteComponentAsNewCommand() override;

    std::string GetDescription() const override;
    void Execute() override;
    void Undo() override;

    bool ReferencesAny(const DeadMemory& dead) const override
    {
        for (const auto& e : m_Entries)
        {
            if (CommandRefs::HasAny(dead, e.object, e.addedComponent))
            {
                return true;
            }
        }
        return false;
    }

private:
    struct Entry
    {
        Deki::Object* object;
        Deki::Component* addedComponent = nullptr;
        bool detached = false;  // undone: off the object, owned here
        int index = -1;
    };
    std::vector<Entry> m_Entries;
    ComponentData m_SourceData;
    std::function<void(Deki::Component*)> m_OnAdded;
};

/// Pastes transform values onto one or more objects.
class PasteTransformCommand : public Command
{
public:
    PasteTransformCommand(const std::vector<Deki::Object*>& objects, const ObjectData& sourceData,
                          std::function<void()> onChange = nullptr);

    std::string GetDescription() const override;
    void Execute() override;
    void Undo() override;

    bool ReferencesAny(const DeadMemory& dead) const override
    {
        for (const auto& e : m_Entries)
        {
            if (CommandRefs::Has(dead, e.object))
            {
                return true;
            }
        }
        return false;
    }

private:
    struct Entry
    {
        Deki::Object* object;
        ObjectData oldData;
    };
    std::vector<Entry> m_Entries;
    ObjectData m_SourceData;
    std::function<void()> m_OnChange;
};

/// Pastes objects, with their children and components, into a scene.
class PasteObjectsCommand : public Command
{
public:
    PasteObjectsCommand(Deki::Scene* scene, const std::vector<ChildData>& objectsData, Deki::Object* parent = nullptr,
                        std::function<void(const std::vector<Deki::Object*>&)> onPasted = nullptr);

    std::string GetDescription() const override;
    void Execute() override;
    void Undo() override;
    void Redo() override { Execute(); }

    bool ReferencesAny(const DeadMemory& dead) const override
    {
        return CommandRefs::HasAny(dead, m_Scene, m_Parent) || CommandRefs::HasAnyIn(dead, m_CreatedObjects);
    }

private:
    Deki::Scene* m_Scene;
    Deki::Object* m_Parent;
    std::vector<ChildData> m_ObjectsData;
    std::vector<Deki::Object*> m_CreatedObjects;
    std::function<void(const std::vector<Deki::Object*>&)> m_OnPasted;
};

}  // namespace DekiEditor
