#pragma once

#include <deki-editor/Command.h>
#include "CommandRefCheck.h"
#include "CommandText.h"
#include "HistoryGuards.h"
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace DekiEditor
{

/// The undoable edit the EditorUI *Field helpers produce.
///
/// Two modes:
///  - Pointer mode: `target` is non-null. Execute/Undo/Redo memcpy the new or
///    old bytes into it. Works for any trivially copyable value (int8/16/32,
///    unsigned, float, float[2/3/4], ...).
///  - Value mode: `target` is null. The value lives wherever the caller keeps
///    it, and `onUndo(old)` / `onRedo(new)` apply it. The old and new bytes are
///    still stored so the callbacks receive the value.
///
/// The optional `onUndo` / `onRedo` run after the bytes are restored or
/// reapplied, and get a pointer to the value's bytes (old on undo, new on
/// redo).
class ModifyValueCommand : public Command
{
public:
    using ValueCallback = std::function<void(const void* value)>;

    ModifyValueCommand(void* target, size_t size, const void* oldBytes, const void* newBytes, std::string description,
                       ValueCallback onUndo = nullptr, ValueCallback onRedo = nullptr)
        : m_Target(target),
          m_Old(static_cast<const uint8_t*>(oldBytes), static_cast<const uint8_t*>(oldBytes) + size),
          m_New(static_cast<const uint8_t*>(newBytes), static_cast<const uint8_t*>(newBytes) + size),
          m_Description(std::move(description)),
          m_OnUndo(std::move(onUndo)),
          m_OnRedo(std::move(onRedo)),
          m_Owners(HistoryGuards::CurrentEditOwners())
    {
    }

    // m_Description is the field's label, which the field helpers also
    // translate.
    std::string GetDescription() const override
    {
        return CommandText::Format(Tr("Change %s"), Tr(m_Description.c_str()));
    }

    // Initial commit: apply the new value (pointer mode), without firing onRedo.
    void Execute() override
    {
        if (m_Target)
        {
            std::memcpy(m_Target, m_New.data(), m_New.size());
        }
    }

    void Redo() override
    {
        if (m_Target)
        {
            std::memcpy(m_Target, m_New.data(), m_New.size());
        }
        if (m_OnRedo)
        {
            m_OnRedo(m_New.data());
        }
    }

    void Undo() override
    {
        if (m_Target)
        {
            std::memcpy(m_Target, m_Old.data(), m_Old.size());
        }
        if (m_OnUndo)
        {
            m_OnUndo(m_Old.data());
        }
    }

    bool CanMerge(const Command* other) const override
    {
        auto* cmd = dynamic_cast<const ModifyValueCommand*>(other);
        return cmd && cmd->m_Target == m_Target && cmd->m_New.size() == m_New.size();
    }

    void Merge(const Command* other) override
    {
        if (auto* cmd = dynamic_cast<const ModifyValueCommand*>(other))
        {
            m_New = cmd->m_New;
        }
    }

    bool ReferencesAny(const DeadMemory& dead) const override
    {
        // Pointer mode points into object or component storage. Value mode
        // applies through callbacks that capture the object or component, so
        // the owners recorded when the edit was made stand in for them
        // (HistoryGuards::EditOwnerScope).
        if (CommandRefs::Has(dead, m_Target))
        {
            return true;
        }
        for (const void* owner : m_Owners)
        {
            if (CommandRefs::Has(dead, owner))
            {
                return true;
            }
        }
        return false;
    }

private:
    void* m_Target;
    std::vector<uint8_t> m_Old;
    std::vector<uint8_t> m_New;
    std::string m_Description;
    ValueCallback m_OnUndo;
    ValueCallback m_OnRedo;
    // The objects/components open in an EditOwnerScope when the edit was made.
    std::vector<const void*> m_Owners;
};

}  // namespace DekiEditor
