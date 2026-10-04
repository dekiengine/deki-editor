#include <deki-editor/CommandHistory.h>
#include <deki/LogSystem.h>

namespace DekiEditor
{

CommandHistory& CommandHistory::Instance()
{
    static CommandHistory s_Instance;
    return s_Instance;
}

void CommandHistory::Execute(std::unique_ptr<Command> command)
{
    auto now = std::chrono::steady_clock::now();

    if (!m_UndoStack.empty())
    {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_LastCommandTime);
        if (elapsed < m_MergeWindow && m_UndoStack.back()->CanMerge(command.get()))
        {
            m_UndoStack.back()->Merge(command.get());
            command->Execute();
            m_LastCommandTime = now;
            NotifyChanged();
            return;
        }
    }

    DEKI_LOG_DEBUG("[Undo] Execute: %s", command->GetDescription().c_str());
    command->Execute();
    m_UndoStack.push_back(std::move(command));

    // A new action makes the redo history invalid.
    m_RedoStack.clear();

    TrimHistory();

    m_LastCommandTime = now;
    NotifyChanged();
}

void CommandHistory::ExecuteNoMerge(std::unique_ptr<Command> command)
{
    DEKI_LOG_DEBUG("[Undo] Execute (no merge): %s", command->GetDescription().c_str());
    command->Execute();
    m_UndoStack.push_back(std::move(command));

    m_RedoStack.clear();
    TrimHistory();

    // Ends the merge window.
    m_LastCommandTime = std::chrono::steady_clock::time_point{};
    NotifyChanged();
}

void CommandHistory::Undo()
{
    if (m_UndoStack.empty())
    {
        return;
    }

    auto command = std::move(m_UndoStack.back());
    m_UndoStack.pop_back();

    DEKI_LOG_DEBUG("[Undo] Undo: %s", command->GetDescription().c_str());
    command->Undo();

    m_RedoStack.push_back(std::move(command));
    NotifyChanged();
}

void CommandHistory::Redo()
{
    if (m_RedoStack.empty())
    {
        return;
    }

    auto command = std::move(m_RedoStack.back());
    m_RedoStack.pop_back();

    DEKI_LOG_DEBUG("[Undo] Redo: %s", command->GetDescription().c_str());
    command->Redo();

    m_UndoStack.push_back(std::move(command));
    NotifyChanged();
}

void CommandHistory::Clear()
{
    m_UndoStack.clear();
    m_RedoStack.clear();
    m_SavePointIndex = 0;
    NotifyChanged();
}

std::string CommandHistory::GetUndoDescription() const
{
    if (m_UndoStack.empty())
    {
        return "";
    }
    return m_UndoStack.back()->GetDescription();
}

std::string CommandHistory::GetRedoDescription() const
{
    if (m_RedoStack.empty())
    {
        return "";
    }
    return m_RedoStack.back()->GetDescription();
}

void CommandHistory::TrimHistory()
{
    while (m_UndoStack.size() > m_MaxHistory)
    {
        m_UndoStack.erase(m_UndoStack.begin());

        // Shift the save point. If it pointed at a trimmed command, set it to
        // SIZE_MAX, so IsDirty() always returns true.
        if (m_SavePointIndex > 0)
        {
            m_SavePointIndex--;
        }
        else
        {
            m_SavePointIndex = SIZE_MAX;  // the save point was trimmed away
        }
    }
}

void CommandHistory::NotifyChanged()
{
    ++m_Generation;
    if (m_OnHistoryChanged)
    {
        m_OnHistoryChanged();
    }
}

void CommandHistory::SaveAndClear()
{
    m_SavedUndoStack = std::move(m_UndoStack);
    m_SavedRedoStack = std::move(m_RedoStack);
    m_SavedSavePointIndex = m_SavePointIndex;
    m_UndoStack.clear();
    m_RedoStack.clear();
    m_SavePointIndex = 0;
    NotifyChanged();
}

void CommandHistory::RestoreFromSave()
{
    m_UndoStack = std::move(m_SavedUndoStack);
    m_RedoStack = std::move(m_SavedRedoStack);
    m_SavePointIndex = m_SavedSavePointIndex;
    m_SavedUndoStack.clear();
    m_SavedRedoStack.clear();
    m_SavedSavePointIndex = 0;
    NotifyChanged();
}

void CommandHistory::MarkSavePoint()
{
    m_SavePointIndex = m_UndoStack.size();
}

void CommandHistory::MarkDirty()
{
    // The same sentinel TrimHistory uses: SIZE_MAX never equals
    // m_UndoStack.size(), so IsDirty() is always true.
    m_SavePointIndex = SIZE_MAX;
    NotifyChanged();
}

bool CommandHistory::IsDirty() const
{
    return m_UndoStack.size() != m_SavePointIndex;
}

bool CommandHistory::IsSavedAsideDirty() const
{
    return m_SavedUndoStack.size() != m_SavedSavePointIndex;
}

namespace
{
// Drop everything up to and including the newest command that references a
// dying pointer. Both stacks are replayed from the back, so an unreachable
// entry at index k also seals off everything older than it. Returns how many
// entries were removed from the front.
size_t TruncateAtDead(std::vector<std::unique_ptr<Command>>& stack, const DeadMemory& dead)
{
    size_t cutoff = 0;  // number of entries to erase from the front
    bool found = false;

    for (size_t i = 0; i < stack.size(); ++i)
    {
        if (stack[i] && stack[i]->ReferencesAny(dead))
        {
            cutoff = i + 1;
            found = true;
        }
    }

    if (!found)
    {
        return 0;
    }

    stack.erase(stack.begin(), stack.begin() + static_cast<std::ptrdiff_t>(cutoff));
    return cutoff;
}
}  // namespace

void CommandHistory::DiscardCommandsReferencing(const DeadMemory& dead)
{
    if (dead.Empty())
    {
        return;
    }

    const size_t undoDropped = TruncateAtDead(m_UndoStack, dead);
    const size_t redoDropped = TruncateAtDead(m_RedoStack, dead);

    // Play mode keeps the edit stacks aside. They point at the same edit-mode
    // objects, so they are truncated too, or Stop() would restore dangling
    // commands.
    TruncateAtDead(m_SavedUndoStack, dead);
    TruncateAtDead(m_SavedRedoStack, dead);

    if (undoDropped == 0 && redoDropped == 0)
    {
        return;
    }

    DEKI_LOG_DEBUG("[Undo] Dropped %zu undo / %zu redo entries referencing destroyed objects", undoDropped,
                   redoDropped);

    // The save point is an index into the undo stack, so it shifts with it.
    // If the point was inside the dropped range there is no known clean state
    // left, so the history counts as dirty.
    if (m_SavePointIndex != SIZE_MAX)
    {
        if (m_SavePointIndex >= undoDropped)
        {
            m_SavePointIndex -= undoDropped;
        }
        else
        {
            m_SavePointIndex = SIZE_MAX;
        }
    }

    NotifyChanged();
}

}  // namespace DekiEditor
