#pragma once

// One undo stack shared by the editor app, deki-editor.dll and every package
// tool window, so a package's edits interleave correctly with scene edits.
#include <deki-editor/EditorExport.h>
#include <deki-editor/Localization.h>
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
/// Ranges, not base pointers, because the EditorUI field helpers store
/// pointers *inside* objects, such as `&obj->x` or a field of a component. A
/// set of base pointers would miss those and leave the dangling writes this
/// exists to prevent.
///
/// Holds the dying DekiObjects and every Deki::Component they own (a
/// component dies with its object). Plain bytes so Command.h needs no engine
/// headers.
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

/// An undoable edit.
class DEKI_EDITOR_API Command
{
public:
    virtual ~Command() = default;

    /// Human-readable text shown in the UI, in the editor's current language.
    virtual std::string GetDescription() const = 0;

    virtual void Execute() = 0;

    virtual void Undo() = 0;

    /// Calls Execute by default.
    virtual void Redo() { Execute(); }

    /// Whether `other` can be folded into this command.
    virtual bool CanMerge(const Command* other) const { return false; }

    /// Folds `other` into this command.
    virtual void Merge(const Command* other) {}

    /// Whether this command holds a raw pointer to anything in `dead`.
    ///
    /// CommandHistory asks this when the scene is about to free objects and
    /// discards any command that answers true, since Undo()/Redo() on a freed
    /// target is a use-after-free.
    ///
    /// The default, false, is right for commands that address their target by
    /// value or by id. Any command storing a Deki::Object*, Deki::Component*
    /// or Scene* MUST override this; see src/core/commands/ for the pattern.
    virtual bool ReferencesAny(const DeadMemory& dead) const { return false; }
};

/// Several commands undone and redone as one.
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
    // keeps them out of the export set.
    CompoundCommand(const CompoundCommand&) = delete;
    CompoundCommand& operator=(const CompoundCommand&) = delete;

    // The caller's text, translated when the table has it.
    std::string GetDescription() const override { return Tr(m_Description.c_str()); }

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
