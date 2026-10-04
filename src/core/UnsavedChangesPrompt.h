#pragma once

#include <string>

namespace DekiEditor
{

/// What the user was trying to do when the unsaved-changes prompt interrupted.
enum class PendingSceneAction
{
    None,
    NewScene,
    OpenScene,
    CloseProject,
    OpenProject,  // path = the project directory to open once the scene is dealt with
    Quit          // the window close button, Alt+F4 and File > Exit
};

/// A programmatic answer to the prompt, for callers with no mouse (MCP
/// automation, tests). Applied on the dialog's next frame, since ImGui popups
/// can only be closed from inside their own draw scope.
enum class DialogAnswer
{
    None,
    Save,
    Discard,
    Cancel
};

/// The state machine behind "you have unsaved changes".
///
/// Decides whether to interrupt, remembers what was interrupted, and holds the
/// user's answer. EditorApp carries out the deferred NewScene / OpenScene /
/// CloseProject. This keeps the class free of EditorApp.h and ImGui, so tests
/// can drive it with their own recorder in place of the editor.
class UnsavedChangesPrompt
{
public:
    /// Asks to interrupt `action`. Returns true when the prompt was raised; the
    /// caller must then not proceed, because the action is now deferred.
    ///
    /// `sceneIsDirty` comes from the caller: a scene is open and has unsaved
    /// changes.
    bool Request(PendingSceneAction action, const std::string& path, bool sceneIsDirty);

    /// True while an action is being held. Scripted callers should check it:
    /// until the prompt is answered, the editor stays on the guarded scene even
    /// though later commands appear to succeed.
    bool IsOpen() const { return m_DialogVisible || m_PendingAction != PendingSceneAction::None; }

    /// True on the frames the modal should be drawn.
    bool DialogVisible() const { return m_DialogVisible; }
    void SetDialogVisible(bool visible) { m_DialogVisible = visible; }

    PendingSceneAction PendingAction() const { return m_PendingAction; }
    const std::string& PendingPath() const { return m_PendingPath; }

    /// Hands the deferred action to the caller and clears it. Returns None when
    /// nothing is pending. Cleared before the caller acts, so a re-entrant call
    /// (a failed open that shows its own dialog) cannot see it again.
    PendingSceneAction TakePending(std::string& outPath);

    /// Drops the deferred action (the user chose Cancel).
    void Cancel();

    // -- programmatic answering ---------------------------------------------

    void Answer(DialogAnswer answer) { m_PendingAnswer = answer; }
    /// Takes the waiting programmatic answer, or None.
    DialogAnswer TakeAnswer();

    // -- re-entrancy --------------------------------------------------------

    /// Set while the caller carries out the answered action through the real
    /// NewScene/OpenScene, so the prompt does not open again. While set,
    /// Request() always returns false.
    void SetSuppressed(bool suppressed) { m_Suppressed = suppressed; }
    bool Suppressed() const { return m_Suppressed; }

private:
    bool m_DialogVisible = false;
    PendingSceneAction m_PendingAction = PendingSceneAction::None;
    std::string m_PendingPath;
    bool m_Suppressed = false;
    DialogAnswer m_PendingAnswer = DialogAnswer::None;
};

}  // namespace DekiEditor
