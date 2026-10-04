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

/// The undo/redo history. Commands that arrive close together can merge.
class DEKI_EDITOR_API CommandHistory
{
public:
    static CommandHistory& Instance();

    /// Runs a command and adds it to the history, merging it with the last
    /// one when it can.
    void Execute(std::unique_ptr<Command> command);

    /// Runs a command and adds it to the history without merging.
    void ExecuteNoMerge(std::unique_ptr<Command> command);

    void Undo();

    void Redo();

    void Clear();

    bool CanUndo() const { return !m_UndoStack.empty(); }

    bool CanRedo() const { return !m_RedoStack.empty(); }

    size_t GetUndoCount() const { return m_UndoStack.size(); }

    size_t GetRedoCount() const { return m_RedoStack.size(); }

    /// A counter bumped on every change (Execute, ExecuteNoMerge, Undo, Redo,
    /// Clear, MarkDirty, SaveAndClear, RestoreFromSave). Panels poll it to
    /// drop cached state (e.g. SceneView's edit-mode buffer) when the scene
    /// changes through commands.
    uint64_t GetGeneration() const { return m_Generation; }

    /// Description of the command Undo would undo.
    std::string GetUndoDescription() const;

    /// Description of the command Redo would redo.
    std::string GetRedoDescription() const;

    void SetOnHistoryChanged(std::function<void()> callback) { m_OnHistoryChanged = callback; }

    /// Sets the stacks aside and starts empty ones, for play mode.
    void SaveAndClear();

    /// Brings back the stacks set aside, dropping the play mode ones.
    void RestoreFromSave();

    void SetMaxHistory(size_t max) { m_MaxHistory = max; }

    /// Marks the current state as saved, for IsDirty.
    void MarkSavePoint();

    /// Marks the scene as differing from its file, with nothing to undo. Used
    /// after loading content (e.g. crash recovery) that differs from the file
    /// at m_CurrentScenePath, so the user is asked to save.
    void MarkDirty();

    /// Whether there are unsaved changes since the save point.
    bool IsDirty() const;

    /// While Play has the edit history set aside (SaveAndClear), whether THAT
    /// history has unsaved changes: the edit scene's state. Play's own edits
    /// are thrown away on Stop and say nothing about the file.
    bool IsSavedAsideDirty() const;

    /// Drops history that points at objects about to be freed.
    ///
    /// Hook this to Scene's on_objects_about_to_be_destroyed. `dead` must
    /// contain the dying objects and all of their components.
    ///
    /// Truncates rather than removing single commands: undo walks the stack
    /// newest to oldest, so a dead command at index k makes everything at or
    /// below k unreachable. Those entries are dropped and the rest stays
    /// consistent.
    void DiscardCommandsReferencing(const DeadMemory& dead);

private:
    CommandHistory() = default;
    ~CommandHistory() = default;

    CommandHistory(const CommandHistory&) = delete;
    CommandHistory& operator=(const CommandHistory&) = delete;

    void TrimHistory();
    void NotifyChanged();

    std::vector<std::unique_ptr<Command>> m_UndoStack;
    std::vector<std::unique_ptr<Command>> m_RedoStack;

    size_t m_MaxHistory = 100;

    // Commands within this time of each other can merge.
    std::chrono::steady_clock::time_point m_LastCommandTime;
    std::chrono::milliseconds m_MergeWindow{ 500 };

    std::function<void()> m_OnHistoryChanged;

    // Size of the undo stack at the last save.
    size_t m_SavePointIndex = 0;

    // Set aside during play mode.
    std::vector<std::unique_ptr<Command>> m_SavedUndoStack;
    std::vector<std::unique_ptr<Command>> m_SavedRedoStack;
    size_t m_SavedSavePointIndex = 0;

    // Bumped on every change; see GetGeneration().
    uint64_t m_Generation = 0;
};

}  // namespace DekiEditor
