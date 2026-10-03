#pragma once

#include <deki-editor/EditorExport.h>

#include <deki-editor/Command.h>
#include <vector>
#include <memory>
#include <chrono>
#include <functional>
#include <cstdint>

namespace DekiEditor
{

/**
 * Manages undo/redo command history with support for command merging
 */
class DEKI_EDITOR_API CommandHistory
{
public:
    static CommandHistory& Instance();

    /// Execute a command and add it to history
    void Execute(std::unique_ptr<Command> command);

    /// Execute a command without merge possibility
    void ExecuteNoMerge(std::unique_ptr<Command> command);

    /// Undo the last command
    void Undo();

    /// Redo the last undone command
    void Redo();

    /// Clear all history
    void Clear();

    /// Check if undo is available
    bool CanUndo() const { return !m_UndoStack.empty(); }

    /// Check if redo is available
    bool CanRedo() const { return !m_RedoStack.empty(); }

    /// Get count of undo commands
    size_t GetUndoCount() const { return m_UndoStack.size(); }

    /// Get count of redo commands
    size_t GetRedoCount() const { return m_RedoStack.size(); }

    /// Monotonic counter bumped on every state-changing operation (Execute,
    /// ExecuteNoMerge, Undo, Redo, Clear, MarkDirty, SaveAndClear,
    /// RestoreFromSave). Polled by panels that need to invalidate cached
    /// derived state (e.g. SceneView's edit-mode buffer) when the scene
    /// content changes through the command system.
    uint64_t GetGeneration() const { return m_Generation; }

    /// Get description of next undo command
    std::string GetUndoDescription() const;

    /// Get description of next redo command
    std::string GetRedoDescription() const;

    /// Set callback for history changes
    void SetOnHistoryChanged(std::function<void()> callback) { m_OnHistoryChanged = callback; }

    /// Save current stacks aside and clear for play mode isolation
    void SaveAndClear();

    /// Restore saved stacks, discarding current (play mode) stacks
    void RestoreFromSave();

    /// Set maximum history size
    void SetMaxHistory(size_t max) { m_MaxHistory = max; }

    /// Mark current state as saved (for dirty tracking)
    void MarkSavePoint();

    /// Force the history into a "dirty vs disk" state with no undoable commands.
    /// Used after loading content (e.g. crash recovery) that differs from the
    /// file currently at m_CurrentScenePath, so the user is prompted to save.
    void MarkDirty();

    /// Check if there are unsaved changes since last save point
    bool IsDirty() const;

    /// While Play has the edit history set aside (SaveAndClear), whether THAT
    /// history has unsaved changes: the edit scene's state. Play's own edits
    /// are thrown away on Stop and say nothing about the file.
    bool IsSavedAsideDirty() const;

    /// Discard history that can no longer be safely replayed because the
    /// objects it points at are being freed.
    ///
    /// Hook this to Scene's on_objects_about_to_be_destroyed. `dead` must
    /// contain the dying objects and all of their components.
    ///
    /// Truncation, not surgical removal: undo walks the stack newest-to-oldest,
    /// so a dead command at index k makes everything at or below k unreachable
    /// without stepping through it. Those entries are dropped and the remaining
    /// suffix stays consistent.
    void DiscardCommandsReferencing(const DeadMemory& dead);

private:
    CommandHistory() = default;
    ~CommandHistory() = default;

    // No copying
    CommandHistory(const CommandHistory&) = delete;
    CommandHistory& operator=(const CommandHistory&) = delete;

    void TrimHistory();
    void NotifyChanged();

    std::vector<std::unique_ptr<Command>> m_UndoStack;
    std::vector<std::unique_ptr<Command>> m_RedoStack;

    size_t m_MaxHistory = 100;

    // Merge window - commands within this time can be merged
    std::chrono::steady_clock::time_point m_LastCommandTime;
    std::chrono::milliseconds m_MergeWindow{ 500 };

    std::function<void()> m_OnHistoryChanged;

    // Save point tracking - index of undo stack when last saved
    size_t m_SavePointIndex = 0;

    // Saved stacks for play mode isolation
    std::vector<std::unique_ptr<Command>> m_SavedUndoStack;
    std::vector<std::unique_ptr<Command>> m_SavedRedoStack;
    size_t m_SavedSavePointIndex = 0;

    // Bumped on every state change. Read via GetGeneration() to detect that
    // *something* changed without inspecting the stacks themselves.
    uint64_t m_Generation = 0;
};

}  // namespace DekiEditor
