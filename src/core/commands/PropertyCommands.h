#pragma once

#include <deki-editor/Command.h>
#include "CommandRefCheck.h"
#include "PropertyTypeHandlers.h"
#include <deki/Object.h>
#include <deki/Component.h>
#include <deki/reflection/Property.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/ObjectRef.h>
#include <deki/assets/AssetRef.h>
#include <nlohmann/json.hpp>
#include <functional>
#include <string>
#include <vector>

namespace DekiEditor
{

/**
 * Command for setting object position (X and Y together)
 * Calls SetLocalPosition() to properly update world transforms and children
 */
class SetPositionCommand : public Command
{
public:
    // Constructor that captures current position as old value
    SetPositionCommand(Deki::Object* obj, float newX, float newY)
        : m_Object(obj),
          m_OldX(obj->GetX()),
          m_OldY(obj->GetY()),
          m_NewX(newX),
          m_NewY(newY)
    {
    }

    // Constructor with explicit old/new values (for drag-end recording)
    SetPositionCommand(Deki::Object* obj, float oldX, float oldY, float newX, float newY)
        : m_Object(obj),
          m_OldX(oldX),
          m_OldY(oldY),
          m_NewX(newX),
          m_NewY(newY)
    {
    }

    std::string GetDescription() const override { return "Move Object"; }

    void Execute() override;

    void Undo() override;

    bool CanMerge(const Command* other) const override;

    void Merge(const Command* other) override;

    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    Deki::Object* m_Object;
    float m_OldX, m_OldY;
    float m_NewX, m_NewY;
};

/**
 * Command for setting object world position (X and Y together)
 * Calls SetWorldPosition() to properly convert to local and update transforms
 */
class SetWorldPositionCommand : public Command
{
public:
    SetWorldPositionCommand(Deki::Object* obj, float newWorldX, float newWorldY,
                            const std::string& description = "Move Object")
        : m_Object(obj),
          m_OldWorldX(obj->GetWorldX()),
          m_OldWorldY(obj->GetWorldY()),
          m_NewWorldX(newWorldX),
          m_NewWorldY(newWorldY),
          m_Description(description)
    {
    }

    std::string GetDescription() const override { return m_Description; }

    void Execute() override;

    void Undo() override;

    bool CanMerge(const Command* other) const override;

    void Merge(const Command* other) override;

    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    Deki::Object* m_Object;
    float m_OldWorldX, m_OldWorldY;
    float m_NewWorldX, m_NewWorldY;
    std::string m_Description;
};

/**
 * Command for setting object rotation
 * Accepts explicit old/new values for drag-end undo recording
 */
class SetRotationCommand : public Command
{
public:
    SetRotationCommand(Deki::Object* obj, float oldRotation, float newRotation)
        : m_Object(obj),
          m_OldRotation(oldRotation),
          m_NewRotation(newRotation)
    {
    }

    std::string GetDescription() const override { return "Rotate Object"; }

    void Execute() override;

    void Undo() override;

    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    Deki::Object* m_Object;
    float m_OldRotation, m_NewRotation;
};

/**
 * Command for setting object scale
 * Accepts explicit old/new values for drag-end undo recording
 */
class SetScaleCommand : public Command
{
public:
    SetScaleCommand(Deki::Object* obj, float oldScaleX, float oldScaleY, float newScaleX, float newScaleY)
        : m_Object(obj),
          m_OldScaleX(oldScaleX),
          m_OldScaleY(oldScaleY),
          m_NewScaleX(newScaleX),
          m_NewScaleY(newScaleY)
    {
    }

    std::string GetDescription() const override { return "Scale Object"; }

    void Execute() override;

    void Undo() override;

    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    Deki::Object* m_Object;
    float m_OldScaleX, m_OldScaleY;
    float m_NewScaleX, m_NewScaleY;
};

/**
 * When a Bool property with DEKI_TOGGLE_OBJECT is changed, auto-set the
 * referenced ObjectRef's object active state to match the bool value.
 */
inline void ApplyToggleObject(Deki::Component* comp, const char* propertyName)
{
    const Deki::ComponentMeta* meta = Deki::ComponentRegistry::Instance().GetMeta(comp->GetType());
    if (!meta)
    {
        return;
    }

    auto scan = [&](const Deki::PropertyInfo* props, int count)
    {
        for (int i = 0; i < count; i++)
        {
            if (props[i].type != Deki::PropertyType::Bool)
            {
                continue;
            }
            if (!props[i].togglesObjectOf)
            {
                continue;
            }
            if (strcmp(props[i].name, propertyName) != 0)
            {
                continue;
            }

            bool value = Deki::GetPropertyValue<bool>(comp, props[i]);
            const char* targetName = props[i].togglesObjectOf;

            // Find the ObjectRef property by name
            auto findRef = [&](const Deki::PropertyInfo* p, int c) -> Deki::ObjectRefBase*
            {
                for (int j = 0; j < c; j++)
                {
                    if (p[j].type == Deki::PropertyType::ObjectRef && strcmp(p[j].name, targetName) == 0)
                    {
                        return reinterpret_cast<Deki::ObjectRefBase*>(reinterpret_cast<char*>(comp) + p[j].offset);
                    }
                }
                return nullptr;
            };

            Deki::ObjectRefBase* ref = findRef(meta->properties, meta->propertyCount);
            if (!ref && meta->baseTypeId != 0)
            {
                const Deki::ComponentMeta* baseMeta = Deki::ComponentRegistry::Instance().GetMeta(meta->baseTypeId);
                if (baseMeta)
                {
                    ref = findRef(baseMeta->properties, baseMeta->propertyCount);
                }
            }
            if (ref && ref->resolved)
            {
                ref->resolved->SetActive(value);
            }
        }
    };

    if (meta->baseTypeId != 0)
    {
        const Deki::ComponentMeta* baseMeta = Deki::ComponentRegistry::Instance().GetMeta(meta->baseTypeId);
        if (baseMeta)
        {
            scan(baseMeta->properties, baseMeta->propertyCount);
        }
    }
    scan(meta->properties, meta->propertyCount);
}

/**
 * Command for modifying a component property using reflection.
 * Uses PropertyTypeHandlers for type-specific capture/apply operations,
 * eliminating the need for large switch statements.
 */
class ModifyComponentPropertyCommand : public Command
{
public:
    ModifyComponentPropertyCommand(Deki::Component* comp, const Deki::PropertyInfo& prop,
                                   const nlohmann::json& newValue, std::function<void()> onChange = nullptr)
        : m_Component(comp),
          m_PropertyOffset(prop.offset),
          m_PropertyType(prop.type),
          m_ElementType(prop.elementType),
          m_EnumSize(prop.enumSize),
          m_PropertyName(prop.name),
          m_NewValue(newValue),
          m_OnChange(onChange),
          m_Executed(false)
    {
        // Capture old value - use special handlers for Array and Enum.
        // Enum needs the actual storage size so we don't read adjacent fields.
        if (prop.type == Deki::PropertyType::Array)
        {
            m_OldValue = CaptureArray(comp, prop);
        }
        else if (prop.type == Deki::PropertyType::Enum)
        {
            m_OldValue = CaptureEnumSized(comp, prop.offset, m_EnumSize);
        }
        else
        {
            const auto& handler = GetPropertyTypeHandler(prop.type);
            m_OldValue = handler.capture(comp, prop.offset);
        }
    }

    std::string GetDescription() const override;

    void Execute() override;

    void Undo() override;

    bool CanMerge(const Command* other) const override;

    void Merge(const Command* other) override;

    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    Deki::Component* m_Component;
    size_t m_PropertyOffset;
    Deki::PropertyType m_PropertyType;
    Deki::PropertyType m_ElementType;  // For Array type: element type
    uint8_t m_EnumSize;                // For Enum type: storage size in bytes (1, 2, or 4)
    std::string m_PropertyName;
    nlohmann::json m_OldValue;
    nlohmann::json m_NewValue;
    std::function<void()> m_OnChange;
    bool m_Executed;
};

/**
 * Command for modifying the same property across multiple components.
 * All changes are applied/reverted atomically for a single undo step.
 * Used for multi-selection editing in the inspector.
 */
class ModifyMultipleComponentPropertyCommand : public Command
{
public:
    ModifyMultipleComponentPropertyCommand(const std::vector<Deki::Component*>& components,
                                           const Deki::PropertyInfo& prop, const nlohmann::json& newValue,
                                           std::function<void()> onChange = nullptr)
        : m_PropertyOffset(prop.offset),
          m_PropertyType(prop.type),
          m_ElementType(prop.elementType),
          m_EnumSize(prop.enumSize),
          m_PropertyName(prop.name),
          m_NewValue(newValue),
          m_OnChange(onChange),
          m_Executed(false)
    {
        // Capture old values for each component
        for (Deki::Component* comp : components)
        {
            ComponentEntry entry;
            entry.component = comp;

            if (prop.type == Deki::PropertyType::Array)
            {
                entry.oldValue = CaptureArray(comp, prop);
            }
            else if (prop.type == Deki::PropertyType::Enum)
            {
                entry.oldValue = CaptureEnumSized(comp, prop.offset, m_EnumSize);
            }
            else
            {
                const auto& handler = GetPropertyTypeHandler(prop.type);
                entry.oldValue = handler.capture(comp, prop.offset);
            }

            m_Entries.push_back(entry);
        }
    }

    std::string GetDescription() const override;

    void Execute() override;

    void Undo() override;

    bool CanMerge(const Command* other) const override;

    void Merge(const Command* other) override;

    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    struct ComponentEntry
    {
        Deki::Component* component;
        nlohmann::json oldValue;
    };

    void ApplyValue(Deki::Component* comp, const nlohmann::json& value);

    std::vector<ComponentEntry> m_Entries;
    size_t m_PropertyOffset;
    Deki::PropertyType m_PropertyType;
    Deki::PropertyType m_ElementType;
    uint8_t m_EnumSize;
    std::string m_PropertyName;
    nlohmann::json m_NewValue;
    std::function<void()> m_OnChange;
    bool m_Executed;
};

/**
 * Command for setting an AssetRefBase property (guid + source pair).
 * The runtime fields (ptr, loadAttempted) are reset on every apply so the
 * asset is re-resolved on the next access.
 */
class ModifyAssetRefCommand : public Command
{
public:
    ModifyAssetRefCommand(Deki::Component* comp, const Deki::PropertyInfo& prop, const std::string& newGuid,
                          const std::string& newSource)
        : m_Component(comp),
          m_PropertyOffset(prop.offset),
          m_PropertyName(prop.name),
          m_NewGuid(newGuid),
          m_NewSource(newSource)
    {
        const auto& ref = GetRef();
        m_OldGuid = ref.guid;
        m_OldSource = ref.source;
    }

    std::string GetDescription() const override;

    void Execute() override { Apply(m_NewGuid, m_NewSource); }
    void Undo() override { Apply(m_OldGuid, m_OldSource); }

    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    Deki::AssetRefBase& GetRef();

    void Apply(const std::string& guid, const std::string& source);

    Deki::Component* m_Component;
    size_t m_PropertyOffset;
    std::string m_PropertyName;
    std::string m_OldGuid;
    std::string m_OldSource;
    std::string m_NewGuid;
    std::string m_NewSource;
};

}  // namespace DekiEditor
