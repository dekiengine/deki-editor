#pragma once

// One undo stack shared by the editor app, deki-editor.dll and every package
// tool window, so a package's edits interleave correctly with scene edits.
#include <deki-editor/EditorExport.h>
#include <string>
#include <memory>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <algorithm>

namespace DekiEditor
{

/// Memory that is about to be freed, described as byte ranges.
///
/// Ranges rather than a set of base pointers, because commands do not only
/// store objects - the EditorUI field helpers store *interior* pointers such as
/// `&obj->x` or the address of a field inside a component. A base-pointer set
/// would miss every one of those and leave exactly the dangling writes this
/// mechanism exists to prevent.
///
/// Populated with the dying DekiObjects and every Deki::Component they own (a
/// component dies with its object). Type-erased to bytes so Command.h stays
/// free of engine headers.
class DeadMemory
{
public:
    void Add(const void* base, size_t size)
    {
        if (!base || size == 0)
        {
            return;
        }
        const auto* b = static_cast<const uint8_t*>(base);
        m_Ranges.emplace_back(b, b + size);
    }

    bool Empty() const { return m_Ranges.empty(); }

    /// True if `ptr` falls inside any dying range.
    bool Contains(const void* ptr) const
    {
        if (!ptr)
        {
            return false;
        }
        const auto* p = static_cast<const uint8_t*>(ptr);
        return std::any_of(m_Ranges.begin(), m_Ranges.end(),
                           [p](const Range& r) { return p >= r.first && p < r.second; });
    }

    /// True if any element of a container of pointers falls inside a dying range.
    template <typename Container>
    bool ContainsAnyIn(const Container& items) const
    {
        for (const auto* item : items)
        {
            if (Contains(static_cast<const void*>(item)))
            {
                return true;
            }
        }
        return false;
    }

private:
    using Range = std::pair<const uint8_t*, const uint8_t*>;
    std::vector<Range> m_Ranges;
};

/**
 * Base interface for undoable commands
 */
class DEKI_EDITOR_API Command
{
public:
    virtual ~Command() = default;

    /// Human-readable description for UI display
    virtual std::string GetDescription() const = 0;

    /// Execute the command
    virtual void Execute() = 0;

    /// Undo the command
    virtual void Undo() = 0;

    /// Redo the command (default: calls Execute)
    virtual void Redo() { Execute(); }

    /// Check if this command can merge with another
    virtual bool CanMerge(const Command* other) const { return false; }

    /// Merge another command into this one
    virtual void Merge(const Command* other) {}

    /// Report whether this command holds a raw pointer to anything in `dead`.
    ///
    /// CommandHistory calls this when the scene is about to free objects, and
    /// discards any command that answers true - running Undo()/Redo() on a
    /// command whose target has been freed is a use-after-free.
    ///
    /// The default is false, which is correct for commands that address their
    /// target by value or by id. Any command storing a Deki::Object*,
    /// Deki::Component*, or Scene* MUST override this; see the implementations
    /// in src/core/commands/ for the pattern.
    virtual bool ReferencesAny(const DeadMemory& dead) const { return false; }
};

/**
 * Groups multiple commands into a single undoable operation
 */
class DEKI_EDITOR_API CompoundCommand : public Command
{
public:
    explicit CompoundCommand(const std::string& description, std::vector<std::unique_ptr<Command>> commands)
        : m_Description(description),
          m_Commands(std::move(commands))
    {
    }

    // dllexport instantiates every implicit member, and the implicit copy
    // ctor/assignment are ill-formed with a vector<unique_ptr>. Deleting them
    // explicitly keeps them out of the export set.
    CompoundCommand(const CompoundCommand&) = delete;
    CompoundCommand& operator=(const CompoundCommand&) = delete;

    std::string GetDescription() const override { return m_Description; }

    void Execute() override
    {
        for (auto& cmd : m_Commands)
        {
            cmd->Execute();
        }
    }

    void Undo() override
    {
        for (auto it = m_Commands.rbegin(); it != m_Commands.rend(); ++it)
        {
            (*it)->Undo();
        }
    }

    void Redo() override
    {
        for (auto& cmd : m_Commands)
        {
            cmd->Redo();
        }
    }

    bool ReferencesAny(const DeadMemory& dead) const override
    {
        for (const auto& cmd : m_Commands)
        {
            if (cmd->ReferencesAny(dead))
            {
                return true;
            }
        }
        return false;
    }

private:
    std::string m_Description;
    std::vector<std::unique_ptr<Command>> m_Commands;
};

}  // namespace DekiEditor
