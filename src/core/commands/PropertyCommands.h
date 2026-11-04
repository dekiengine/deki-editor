#pragma once

#include <deki-editor/Command.h>
#include "CommandRefCheck.h"
#include "CommandText.h"
#include <deki/Object.h>
#include <deki/Scene.h>
#include <deki/Component.h>
#include <deki/ScreenAnchor.h>
#include <deki/reflection/Property.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/ObjectRef.h>
#include <deki/assets/AssetRef.h>
#include <deki-editor/EditorNaming.h>
#include <nlohmann/json.hpp>
#include <functional>
#include <string>
#include <vector>

namespace DekiEditor
{

/// Sets an object's local position (X and Y together) through
/// SetLocalPosition(), which also updates world transforms and children.
class SetPositionCommand : public Command
{
public:
    // Takes the current position as the old value.
    SetPositionCommand(Deki::Object* obj, float newX, float newY)
        : m_Object(obj),
          m_OldX(obj->GetX()),
          m_OldY(obj->GetY()),
          m_NewX(newX),
          m_NewY(newY)
    {
    }

    // Explicit old and new values, for recording a finished drag.
    SetPositionCommand(Deki::Object* obj, float oldX, float oldY, float newX, float newY)
        : m_Object(obj),
          m_OldX(oldX),
          m_OldY(oldY),
          m_NewX(newX),
          m_NewY(newY)
    {
    }

    std::string GetDescription() const override { return Tr("Move Object"); }

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

/// Moves an object that has a Screen Anchor by changing the anchor's offset,
/// since the anchor sets its position. A Scene view drag records one.
class SetAnchorOffsetCommand : public Command
{
public:
    SetAnchorOffsetCommand(Deki::ScreenAnchor* anchor, float oldX, float oldY, float newX, float newY)
        : m_Anchor(anchor),
          m_OldX(oldX),
          m_OldY(oldY),
          m_NewX(newX),
          m_NewY(newY)
    {
    }

    std::string GetDescription() const override { return Tr("Move Object"); }

    void Execute() override;

    void Undo() override;

    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    Deki::ScreenAnchor* m_Anchor;
    float m_OldX, m_OldY;
    float m_NewX, m_NewY;
};

/// Sets an object's world position (X and Y together) through
/// SetWorldPosition(), which converts it to local and updates transforms.
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

    // The caller's text, translated when the table has it.
    std::string GetDescription() const override { return Tr(m_Description.c_str()); }

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

/// Sets an object's rotation. Takes explicit old and new values, for
/// recording a finished drag.
class SetRotationCommand : public Command
{
public:
    SetRotationCommand(Deki::Object* obj, float oldRotation, float newRotation)
        : m_Object(obj),
          m_OldRotation(oldRotation),
          m_NewRotation(newRotation)
    {
    }

    std::string GetDescription() const override { return Tr("Rotate Object"); }

    void Execute() override;

    void Undo() override;

    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    Deki::Object* m_Object;
    float m_OldRotation, m_NewRotation;
};

/// Sets an object's scale. Takes explicit old and new values, for recording
/// a finished drag.
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

    std::string GetDescription() const override { return Tr("Scale Object"); }

    void Execute() override;

    void Undo() override;

    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    Deki::Object* m_Object;
    float m_OldScaleX, m_OldScaleY;
    float m_NewScaleX, m_NewScaleY;
};

/// An object's local transform, the third axis included: what a drag in the
/// Scene view's 3D view changes, and the tools that set depth.
struct Transform3D
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float rotation = 0.0f, rotationX = 0.0f, rotationY = 0.0f;  // radians
    float scaleX = 1.0f, scaleY = 1.0f, scaleZ = 1.0f;

    static Transform3D Of(const Deki::Object* obj);
    void ApplyTo(Deki::Object* obj) const;
    bool operator==(const Transform3D& other) const = default;
};

/// Sets an object's whole local transform, depth included. Takes the
/// transforms before and after, for recording a finished drag.
class SetTransform3DCommand : public Command
{
public:
    SetTransform3DCommand(Deki::Object* obj, const Transform3D& before, const Transform3D& after,
                          const std::string& description = "Move Object")
        : m_Object(obj),
          m_Before(before),
          m_After(after),
          m_Description(description)
    {
    }

    // The caller's text, translated when the table has it.
    std::string GetDescription() const override { return Tr(m_Description.c_str()); }
    void Execute() override;
    void Undo() override;
    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    Deki::Object* m_Object;
    Transform3D m_Before;
    Transform3D m_After;
    std::string m_Description;
};

/// Call after `propertyName` changes. When it is a Bool property with
/// DEKI_TOGGLE_OBJECT, sets the object its ObjectRef points at active or
/// inactive to match.
inline void ApplyToggleObject(Deki::Component* comp, const char* propertyName)
{
    const Deki::ComponentMeta* meta = Deki::ComponentRegistry::Instance().GetMeta(comp->GetType());
    if (!meta)
    {
        return;
    }
    const Deki::PropertyInfo* toggle = Deki::Scene::FindProperty(*meta, propertyName);
    if (!toggle || toggle->type != Deki::PropertyType::Bool || !toggle->togglesObjectOf)
    {
        return;
    }
    // The ObjectRef property named by DEKI_TOGGLE_OBJECT, on the class or a base.
    const Deki::PropertyInfo* target = Deki::Scene::FindProperty(*meta, toggle->togglesObjectOf);
    if (!target || target->type != Deki::PropertyType::ObjectRef)
    {
        return;
    }
    auto* ref = reinterpret_cast<Deki::ObjectRefBase*>(reinterpret_cast<char*>(comp) + target->offset);
    if (ref->resolved)
    {
        ref->resolved->SetActive(Deki::GetPropertyValue<bool>(comp, *toggle));
    }
}

/// Sets a component property through reflection. The value is the editor's
/// one JSON form of a property (Deki::Scene::PropertyToJson, read back by
/// PropertyFromJson), so an edit, its undo and a saved scene agree.
///
/// Holds a copy of the PropertyInfo: its strings and enum tables live in the
/// package that declared the type, and a reload of that package clears the
/// history first (EditorApp::OnBeforePluginReload).
class ModifyComponentPropertyCommand : public Command
{
public:
    ModifyComponentPropertyCommand(Deki::Component* comp, const Deki::PropertyInfo& prop,
                                   const nlohmann::json& newValue, std::function<void()> onChange = nullptr)
        : m_Component(comp),
          m_Prop(prop),
          m_PropertyName(prop.name),
          m_DisplayName(EditorNaming::GetDisplayName(prop)),
          m_OldValue(Deki::Scene::PropertyToJson(comp, prop)),
          m_NewValue(newValue),
          m_OnChange(onChange),
          m_Executed(false)
    {
    }

    std::string GetDescription() const override;

    void Execute() override;

    void Undo() override;

    bool CanMerge(const Command* other) const override;

    void Merge(const Command* other) override;

    bool ReferencesAny(const DeadMemory& dead) const override;

private:
    void Apply(const nlohmann::json& value);

    Deki::Component* m_Component;
    Deki::PropertyInfo m_Prop;
    std::string m_PropertyName;
    std::string m_DisplayName;  // The Inspector's label, for the description
    nlohmann::json m_OldValue;
    nlohmann::json m_NewValue;
    std::function<void()> m_OnChange;
    bool m_Executed;
};

/// Sets the same property on several components as one undo step. Used when
/// the inspector edits a multi-selection.
class ModifyMultipleComponentPropertyCommand : public Command
{
public:
    ModifyMultipleComponentPropertyCommand(const std::vector<Deki::Component*>& components,
                                           const Deki::PropertyInfo& prop, const nlohmann::json& newValue,
                                           std::function<void()> onChange = nullptr)
        : m_Prop(prop),
          m_PropertyName(prop.name),
          m_DisplayName(EditorNaming::GetDisplayName(prop)),
          m_NewValue(newValue),
          m_OnChange(onChange),
          m_Executed(false)
    {
        // Each component's old value, in the same form.
        for (Deki::Component* comp : components)
        {
            ComponentEntry entry;
            entry.component = comp;
            entry.oldValue = Deki::Scene::PropertyToJson(comp, prop);
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
    Deki::PropertyInfo m_Prop;  // see ModifyComponentPropertyCommand
    std::string m_PropertyName;
    std::string m_DisplayName;  // The Inspector's label, for the description
    nlohmann::json m_NewValue;
    std::function<void()> m_OnChange;
    bool m_Executed;
};

/// Sets an AssetRefBase property (its guid and source). Every apply resets the
/// runtime fields (ptr, loadAttempted), so the asset is resolved again on the
/// next access.
class ModifyAssetRefCommand : public Command
{
public:
    ModifyAssetRefCommand(Deki::Component* comp, const Deki::PropertyInfo& prop, const std::string& newGuid,
                          const std::string& newSource)
        : m_Component(comp),
          m_PropertyOffset(prop.offset),
          m_PropertyName(prop.name),
          m_DisplayName(EditorNaming::GetDisplayName(prop)),
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
    std::string m_DisplayName;  // The Inspector's label, for the description
    std::string m_OldGuid;
    std::string m_OldSource;
    std::string m_NewGuid;
    std::string m_NewSource;
};

}  // namespace DekiEditor
