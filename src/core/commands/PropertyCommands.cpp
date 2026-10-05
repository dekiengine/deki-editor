#include "PropertyCommands.h"
#include "PropertyTypeHandlers.h"
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/Property.h>
#include <deki/Component.h>
#include <deki/Object.h>
#include <deki/Scene.h>
#include <deki/ScreenAnchor.h>

namespace DekiEditor
{

void SetPositionCommand::Execute()
{
    m_Object->SetLocalPosition(m_NewX, m_NewY);
}

void SetPositionCommand::Undo()
{
    m_Object->SetLocalPosition(m_OldX, m_OldY);
}

bool SetPositionCommand::CanMerge(const Command* other) const
{
    auto* cmd = dynamic_cast<const SetPositionCommand*>(other);
    return cmd && cmd->m_Object == m_Object;
}

void SetPositionCommand::Merge(const Command* other)
{
    auto* cmd = dynamic_cast<const SetPositionCommand*>(other);
    if (cmd)
    {
        m_NewX = cmd->m_NewX;
        m_NewY = cmd->m_NewY;
    }
}

bool SetPositionCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::Has(dead, m_Object);
}

void SetAnchorOffsetCommand::Execute()
{
    m_Anchor->offsetX = m_NewX;
    m_Anchor->offsetY = m_NewY;
}

void SetAnchorOffsetCommand::Undo()
{
    m_Anchor->offsetX = m_OldX;
    m_Anchor->offsetY = m_OldY;
}

bool SetAnchorOffsetCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::Has(dead, m_Anchor);
}

void SetWorldPositionCommand::Execute()
{
    m_Object->SetWorldPosition(m_NewWorldX, m_NewWorldY);
}

void SetWorldPositionCommand::Undo()
{
    m_Object->SetWorldPosition(m_OldWorldX, m_OldWorldY);
}

bool SetWorldPositionCommand::CanMerge(const Command* other) const
{
    auto* cmd = dynamic_cast<const SetWorldPositionCommand*>(other);
    return cmd && cmd->m_Object == m_Object;
}

void SetWorldPositionCommand::Merge(const Command* other)
{
    auto* cmd = dynamic_cast<const SetWorldPositionCommand*>(other);
    if (cmd)
    {
        m_NewWorldX = cmd->m_NewWorldX;
        m_NewWorldY = cmd->m_NewWorldY;
    }
}

bool SetWorldPositionCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::Has(dead, m_Object);
}

void SetRotationCommand::Execute()
{
    m_Object->SetRotation(m_NewRotation);
}

void SetRotationCommand::Undo()
{
    m_Object->SetRotation(m_OldRotation);
}

bool SetRotationCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::Has(dead, m_Object);
}

void SetScaleCommand::Execute()
{
    m_Object->SetScale(m_NewScaleX, m_NewScaleY);
}

void SetScaleCommand::Undo()
{
    m_Object->SetScale(m_OldScaleX, m_OldScaleY);
}

bool SetScaleCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::Has(dead, m_Object);
}

std::string ModifyComponentPropertyCommand::GetDescription() const
{
    return "Change " + m_PropertyName;
}

void ModifyComponentPropertyCommand::Execute()
{
    // Array and Enum have their own apply functions; the rest use the type handler.
    if (m_PropertyType == Deki::PropertyType::Array)
    {
        Deki::PropertyInfo tempProp = {};
        tempProp.offset = m_PropertyOffset;
        tempProp.elementType = m_ElementType;
        ApplyArray(m_Component, tempProp, m_NewValue);
    }
    else if (m_PropertyType == Deki::PropertyType::Enum)
    {
        ApplyEnumSized(m_Component, m_PropertyOffset, m_NewValue, m_EnumSize);
    }
    else
    {
        const auto& handler = GetPropertyTypeHandler(m_PropertyType);
        handler.apply(m_Component, m_PropertyOffset, m_NewValue);
    }
    m_Executed = true;
    if (m_OnChange)
    {
        m_OnChange();
    }
    m_Component->OnPropertyChanged(m_PropertyName.c_str());
    ApplyToggleObject(m_Component, m_PropertyName.c_str());
}

void ModifyComponentPropertyCommand::Undo()
{
    // Array and Enum have their own apply functions; the rest use the type handler.
    if (m_PropertyType == Deki::PropertyType::Array)
    {
        Deki::PropertyInfo tempProp = {};
        tempProp.offset = m_PropertyOffset;
        tempProp.elementType = m_ElementType;
        ApplyArray(m_Component, tempProp, m_OldValue);
    }
    else if (m_PropertyType == Deki::PropertyType::Enum)
    {
        ApplyEnumSized(m_Component, m_PropertyOffset, m_OldValue, m_EnumSize);
    }
    else
    {
        const auto& handler = GetPropertyTypeHandler(m_PropertyType);
        handler.apply(m_Component, m_PropertyOffset, m_OldValue);
    }
    if (m_OnChange)
    {
        m_OnChange();
    }
    m_Component->OnPropertyChanged(m_PropertyName.c_str());
    ApplyToggleObject(m_Component, m_PropertyName.c_str());
}

bool ModifyComponentPropertyCommand::CanMerge(const Command* other) const
{
    if (!m_Executed)
    {
        return false;
    }
    auto* cmd = dynamic_cast<const ModifyComponentPropertyCommand*>(other);
    return cmd && cmd->m_Component == m_Component && cmd->m_PropertyOffset == m_PropertyOffset;
}

void ModifyComponentPropertyCommand::Merge(const Command* other)
{
    auto* cmd = dynamic_cast<const ModifyComponentPropertyCommand*>(other);
    if (cmd)
    {
        m_NewValue = cmd->m_NewValue;
    }
}

bool ModifyComponentPropertyCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::Has(dead, m_Component);
}

std::string ModifyMultipleComponentPropertyCommand::GetDescription() const
{
    if (m_Entries.size() == 1)
    {
        return "Change " + m_PropertyName;
    }
    return "Change " + m_PropertyName + " (" + std::to_string(m_Entries.size()) + " objects)";
}

void ModifyMultipleComponentPropertyCommand::Execute()
{
    for (auto& entry : m_Entries)
    {
        ApplyValue(entry.component, m_NewValue);
        entry.component->OnPropertyChanged(m_PropertyName.c_str());
        ApplyToggleObject(entry.component, m_PropertyName.c_str());
    }
    m_Executed = true;
    if (m_OnChange)
    {
        m_OnChange();
    }
}

void ModifyMultipleComponentPropertyCommand::Undo()
{
    for (auto& entry : m_Entries)
    {
        ApplyValue(entry.component, entry.oldValue);
        entry.component->OnPropertyChanged(m_PropertyName.c_str());
        ApplyToggleObject(entry.component, m_PropertyName.c_str());
    }
    if (m_OnChange)
    {
        m_OnChange();
    }
}

bool ModifyMultipleComponentPropertyCommand::CanMerge(const Command* other) const
{
    if (!m_Executed)
    {
        return false;
    }

    auto* cmd = dynamic_cast<const ModifyMultipleComponentPropertyCommand*>(other);
    if (!cmd)
    {
        return false;
    }
    if (cmd->m_PropertyOffset != m_PropertyOffset)
    {
        return false;
    }
    if (cmd->m_Entries.size() != m_Entries.size())
    {
        return false;
    }

    for (size_t i = 0; i < m_Entries.size(); ++i)
    {
        if (cmd->m_Entries[i].component != m_Entries[i].component)
        {
            return false;
        }
    }

    return true;
}

void ModifyMultipleComponentPropertyCommand::Merge(const Command* other)
{
    auto* cmd = dynamic_cast<const ModifyMultipleComponentPropertyCommand*>(other);
    if (cmd)
    {
        m_NewValue = cmd->m_NewValue;
    }
}

bool ModifyMultipleComponentPropertyCommand::ReferencesAny(const DeadMemory& dead) const
{
    for (const auto& entry : m_Entries)
    {
        if (CommandRefs::Has(dead, entry.component))
        {
            return true;
        }
    }
    return false;
}

void ModifyMultipleComponentPropertyCommand::ApplyValue(Deki::Component* comp, const nlohmann::json& value)
{
    if (m_PropertyType == Deki::PropertyType::Array)
    {
        Deki::PropertyInfo tempProp = {};
        tempProp.offset = m_PropertyOffset;
        tempProp.elementType = m_ElementType;
        ApplyArray(comp, tempProp, value);
    }
    else if (m_PropertyType == Deki::PropertyType::Enum)
    {
        ApplyEnumSized(comp, m_PropertyOffset, value, m_EnumSize);
    }
    else
    {
        const auto& handler = GetPropertyTypeHandler(m_PropertyType);
        handler.apply(comp, m_PropertyOffset, value);
    }
}

std::string ModifyAssetRefCommand::GetDescription() const
{
    return "Set " + m_PropertyName;
}

bool ModifyAssetRefCommand::ReferencesAny(const DeadMemory& dead) const
{
    return CommandRefs::Has(dead, m_Component);
}

Deki::AssetRefBase& ModifyAssetRefCommand::GetRef()
{
    return *reinterpret_cast<Deki::AssetRefBase*>(reinterpret_cast<char*>(m_Component) + m_PropertyOffset);
}

void ModifyAssetRefCommand::Apply(const std::string& guid, const std::string& source)
{
    auto& ref = GetRef();
    ref.guid = guid;
    ref.source = source;
    ref.ptr = nullptr;
    ref.loadAttempted = false;
    m_Component->ResetRefsResolved();
    m_Component->OnPropertyChanged(m_PropertyName.c_str());
}

}  // namespace DekiEditor
