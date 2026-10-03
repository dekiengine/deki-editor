#pragma once

#include <deki-editor/Command.h>
#include "CommandRefCheck.h"
#include "HistoryGuards.h"
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace DekiEditor
{

/**
 * Generic undoable edit produced by the EditorUI *Field helpers.
 *
 * Two modes:
 *  - Pointer mode: `target` is non-null. Execute/Undo/Redo memcpy the new/old
 *    bytes into it (works for any trivially-copyable value: int8/16/32, unsigned,
 *    float, float[2/3/4], etc.).
 *  - Value mode: `target` is null. Nothing is memcpy'd; the value lives wherever
 *    the caller keeps it, and `onUndo(old)` / `onRedo(new)` apply it. The old/new
 *    byte snapshots are still stored so the callbacks receive the value.
 *
 * `onUndo` / `onRedo` (optional) fire after the bytes are restored/reapplied,
 * receiving a pointer to the value's bytes (old on undo, new on redo).
 */
class ModifyValueCommand : public Command
{
public:
    using ValueCallback = std::function<void(const void* value)>;

    ModifyValueCommand(void* target, size_t size,
                       const void* oldBytes, const void* newBytes,
                       std::string description,
                       ValueCallback onUndo = nullptr,
                       ValueCallback onRedo = nullptr)
        : m_Target(target), m_Old(static_cast<const uint8_t*>(oldBytes),
                                  static_cast<const uint8_t*>(oldBytes) + size),
          m_New(static_cast<const uint8_t*>(newBytes),
                static_cast<const uint8_t*>(newBytes) + size),
          m_Description(std::move(description)), m_OnUndo(std::move(onUndo)), m_OnRedo(std::move(onRedo)),
          m_Owners(HistoryGuards::CurrentEditOwners())
    {
    }

    std::string GetDescription() const override { return "Change " + m_Description; }

    // Initial commit: apply the new value (pointer mode), without firing onRedo.
    void Execute() override
    {
        if (m_Target) std::memcpy(m_Target, m_New.data(), m_New.size());
    }

    void Redo() override
    {
        if (m_Target) std::memcpy(m_Target, m_New.data(), m_New.size());
        if (m_OnRedo) m_OnRedo(m_New.data());
    }

    void Undo() override
    {
        if (m_Target) std::memcpy(m_Target, m_Old.data(), m_Old.size());
        if (m_OnUndo) m_OnUndo(m_Old.data());
    }

    bool CanMerge(const Command* other) const override
    {
        auto* cmd = dynamic_cast<const ModifyValueCommand*>(other);
        return cmd && cmd->m_Target == m_Target && cmd->m_New.size() == m_New.size();
    }

    void Merge(const Command* other) override
    {
        if (auto* cmd = dynamic_cast<const ModifyValueCommand*>(other))
            m_New = cmd->m_New;
    }

    bool ReferencesAny(const DeadMemory& dead) const override
    {
        // Pointer mode aims into component/object storage. Value mode has a null
        // target and applies through callbacks that capture the object or
        // component; the owners recorded when the edit was made stand in for
        // those (HistoryGuards::EditOwnerScope).
        if (CommandRefs::Has(dead, m_Target))
            return true;
        for (const void* owner : m_Owners)
            if (CommandRefs::Has(dead, owner))
                return true;
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
