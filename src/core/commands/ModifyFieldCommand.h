#pragma once

#include <deki-editor/Command.h>
#include "CommandRefCheck.h"
#include <functional>
#include <string>

namespace DekiEditor
{

/**
 * Templated command for modifying a single field of any type
 * Consolidates ModifyFloatCommand, ModifyIntCommand, ModifyBoolCommand, ModifyStringCommand
 */
template <typename T>
class ModifyFieldCommand : public Command
{
public:
    ModifyFieldCommand(T* target, const T& newValue, const std::string& propertyName,
                       std::function<void()> onChange = nullptr)
        : m_Target(target),
          m_OldValue(*target),
          m_NewValue(newValue),
          m_PropertyName(propertyName),
          m_OnChange(onChange),
          m_Executed(false)
    {
    }

    std::string GetDescription() const override { return "Change " + m_PropertyName; }

    void Execute() override
    {
        *m_Target = m_NewValue;
        m_Executed = true;
        if (m_OnChange)
        {
            m_OnChange();
        }
    }

    void Undo() override
    {
        *m_Target = m_OldValue;
        if (m_OnChange)
        {
            m_OnChange();
        }
    }

    bool CanMerge(const Command* other) const override
    {
        if (!m_Executed)
        {
            return false;
        }
        auto* cmd = dynamic_cast<const ModifyFieldCommand<T>*>(other);
        return cmd && cmd->m_Target == m_Target;
    }

    void Merge(const Command* other) override
    {
        auto* cmd = dynamic_cast<const ModifyFieldCommand<T>*>(other);
        if (cmd)
        {
            m_NewValue = cmd->m_NewValue;
        }
    }

    bool ReferencesAny(const DeadMemory& dead) const override
    {
        // m_Target points *into* an object or component (e.g. &obj->x), which is
        // why DeadMemory tracks ranges rather than base pointers.
        return CommandRefs::Has(dead, m_Target);
    }

private:
    T* m_Target;
    T m_OldValue;
    T m_NewValue;
    std::string m_PropertyName;
    std::function<void()> m_OnChange;
    bool m_Executed;
};

/**
 * Generic templated command for modifying a pair of fields of any type
 * Useful for resize operations (width/height), position (x/y), etc.
 */
template <typename T>
class ModifyFieldPairCommand : public Command
{
public:
    ModifyFieldPairCommand(T* target1, T* target2, T newValue1, T newValue2, const std::string& description)
        : m_Target1(target1),
          m_Target2(target2),
          m_OldValue1(*target1),
          m_OldValue2(*target2),
          m_NewValue1(newValue1),
          m_NewValue2(newValue2),
          m_Description(description)
    {
    }

    std::string GetDescription() const override { return m_Description; }

    void Execute() override
    {
        *m_Target1 = m_NewValue1;
        *m_Target2 = m_NewValue2;
    }

    void Undo() override
    {
        *m_Target1 = m_OldValue1;
        *m_Target2 = m_OldValue2;
    }

    bool CanMerge(const Command* other) const override
    {
        auto* cmd = dynamic_cast<const ModifyFieldPairCommand<T>*>(other);
        return cmd && cmd->m_Target1 == m_Target1 && cmd->m_Target2 == m_Target2;
    }

    void Merge(const Command* other) override
    {
        auto* cmd = dynamic_cast<const ModifyFieldPairCommand<T>*>(other);
        if (cmd)
        {
            m_NewValue1 = cmd->m_NewValue1;
            m_NewValue2 = cmd->m_NewValue2;
        }
    }

    bool ReferencesAny(const DeadMemory& dead) const override
    {
        return CommandRefs::HasAny(dead, m_Target1, m_Target2);
    }

private:
    T* m_Target1;
    T* m_Target2;
    T m_OldValue1;
    T m_OldValue2;
    T m_NewValue1;
    T m_NewValue2;
    std::string m_Description;
};

}  // namespace DekiEditor
