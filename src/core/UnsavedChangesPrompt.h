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
/// Owns *when* - whether to interrupt, what was interrupted, and what the user
/// answered. It deliberately does not own *what*: carrying out the deferred
/// NewScene / OpenScene / CloseProject stays with EditorApp, which is the thing
/// that can actually do them. That split is what keeps this free of EditorApp.h
/// and ImGui, and therefore testable - the test supplies its own recorder in
/// place of the editor and asserts on the sequence.
class UnsavedChangesPrompt
{
public:
    /// Ask to interrupt @p action. Returns true when the prompt was raised, in
    /// which case the caller must NOT proceed - the action is now deferred.
    ///
    /// @p sceneIsDirty is the caller's verdict (a scene is open and the command
    /// history is dirty); this class does not reach for that state itself.
    bool Request(PendingSceneAction action, const std::string& path, bool sceneIsDirty);

    /// Is an operation being held? A scripted caller that does not check this
    /// can keep issuing commands that appear to succeed while the editor is
    /// still on the scene the prompt is guarding.
    bool IsOpen() const { return m_DialogVisible || m_PendingAction != PendingSceneAction::None; }

    /// True on the frames the modal should be drawn.
    bool DialogVisible() const { return m_DialogVisible; }
    void SetDialogVisible(bool visible) { m_DialogVisible = visible; }

    PendingSceneAction PendingAction() const { return m_PendingAction; }
    const std::string& PendingPath() const { return m_PendingPath; }

    /// Hand the deferred action to the caller and clear it. Cleared up front so
    /// a re-entrant call - a failed open that surfaces its own dialog - cannot
    /// see a stale pending action. Returns None when nothing is pending.
    PendingSceneAction TakePending(std::string& outPath);

    /// Abandon the deferred action entirely (the user chose Cancel).
    void Cancel();

    // -- programmatic answering ---------------------------------------------

    void Answer(DialogAnswer answer) { m_PendingAnswer = answer; }
    /// Consume a programmatic answer, if one is waiting.
    DialogAnswer TakeAnswer();

    // -- re-entrancy --------------------------------------------------------

    /// Set while the caller replays the user's answer through the real
    /// NewScene/OpenScene. The dialog has already been answered by then, so
    /// re-asking would put us straight back into it. While set, Request() always
    /// returns false.
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
