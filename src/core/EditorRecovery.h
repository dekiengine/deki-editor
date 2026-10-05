#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "SceneStats.h"  // SceneOutlineNode, CountSceneObjects, BuildSceneOutline

namespace Deki
{
class Scene;
}

namespace DekiEditor
{

/// One recovery snapshot found on disk, with enough metadata to decide whether
/// it is worth restoring without opening it. An untitled snapshot with 1 object
/// is almost always safe to discard; one with 200 objects from this morning is
/// not.
struct RecoveryEntry
{
    std::string recoveryPath;  // full path to <recovery dir>/foo.scene
    std::string displayName;   // foo.scene
    std::string originalPath;  // restored from sidecar; "" if untitled at crash
    std::time_t savedAt = 0;   // sidecar value, else the file's mtime
    std::uintmax_t fileSize = 0;
    int objectCount = -1;  // -1 when the scene could not be parsed
};

/// Crash-recovery snapshots and the autosave timer that writes them.
///
/// Has no UI and does not include EditorApp.h: the dialogs live in
/// EditorApp.cpp and call in here. This keeps it testable in DekiEditorTests
/// without an editor.
///
/// Takes the scene and paths as parameters instead of holding an EditorApp
/// pointer. The caller decides when a snapshot is wanted (play mode, dirty
/// state); this class owns the files and the clock.
class EditorRecovery
{
public:
    /// Recovery directory for a project, or "" when there is no editor data
    /// directory to write into.
    ///
    /// Each project gets its own folder: the readable project name plus a hash
    /// of the normalised full path, so two projects with the same name in
    /// different places stay separate.
    ///
    /// The folder is under ~/.deki-editor/Recovery, not in the project (it would
    /// show in git status, one `git add .` from being committed) and not in the
    /// OS temp folder (cleared on reboot, which often follows a crash).
    static std::string RecoveryDirFor(const std::string& projectPath);

    /// Deletes the whole recovery directory. Call only once every entry is
    /// resolved.
    static void RemoveRecoveryDir(const std::string& projectPath);

    // -- discovery ----------------------------------------------------------

    /// Fills the entry list from the project's recovery directory, newest
    /// first, and resets the selection. A missing directory gives an empty list.
    void ScanProject(const std::string& projectPath);

    const std::vector<RecoveryEntry>& Entries() const { return m_Files; }
    bool HasPending() const { return !m_Files.empty(); }
    const std::string& ScannedProjectPath() const { return m_ProjectPath; }

    int SelectedIndex() const { return m_Selected; }
    void SetSelectedIndex(int index);
    /// nullptr when the list is empty or the index is stale.
    const RecoveryEntry* Selected() const;

    /// Set by ScanProject when anything was found; the dialog owns clearing it.
    bool DialogRequested() const { return m_DialogRequested; }
    void SetDialogRequested(bool value) { m_DialogRequested = value; }

    // -- writing ------------------------------------------------------------

    /// Writes `scene` to the project's recovery directory with a metadata
    /// sidecar that records `scenePath`, so a restore can save back to it.
    /// Returns false if there was nothing to write or the write failed. Never
    /// throws: this also runs from the crash handler.
    ///
    /// `outWrittenPath`, when given, receives the file written, for the crash
    /// dialog to show. The crash handler lives in the editor executable (SDL,
    /// ImGui), so this class only hands the path back.
    bool WriteSnapshot(Deki::Scene* scene, const std::string& scenePath, const std::string& projectPath,
                       std::string* outWrittenPath = nullptr);

    /// Advances the autosave clock. Returns true on the ticks where a snapshot
    /// is due; the caller then calls WriteSnapshot.
    ///
    /// `ready` says whether a snapshot would be useful now: a project and scene
    /// are open, play mode is off, and there are unsaved changes. While it is
    /// false the timer stays at zero, so the first change after a save waits a
    /// full interval.
    bool TickAutosaveTimer(float deltaSeconds, bool ready);

    static constexpr float kAutosaveIntervalSeconds = 90.0f;

    // -- preview ------------------------------------------------------------

    /// Parses an entry's scene into the preview outline. A truncated file gives
    /// an empty outline, which the popup reports as unreadable rather than as
    /// an empty scene.
    void BuildPreview(const RecoveryEntry& entry);
    const std::vector<SceneOutlineNode>& Preview() const { return m_Preview; }
    const std::string& PreviewName() const { return m_PreviewName; }
    void ClearPreview();

    // -- consumption --------------------------------------------------------

    /// Deletes one entry's files and drops it from the list. Returns false if
    /// the index is out of range. Must delete only this entry, not the whole
    /// directory: other crashed scenes in it have not been offered yet.
    bool ConsumeEntry(int index);

    /// Deletes every remaining recovery and the directory itself.
    void DiscardAll();

    /// Deletes the snapshots this session wrote once they are obsolete: the
    /// scene was saved or replaced, or the editor is quitting normally. A crash
    /// never reaches this, so its snapshot survives for the next open. Without
    /// it, every later open would offer to recover work that was saved or
    /// discarded on purpose.
    ///
    /// Recoveries that were pending when the project opened (a crash postponed
    /// with Later) are kept, even when an autosave has since written to the
    /// same file.
    void DiscardSessionSnapshots();

private:
    std::vector<RecoveryEntry> m_Files;
    std::vector<std::string> m_SessionSnapshots;  // written by this process
    std::string m_ProjectPath;
    int m_Selected = 0;
    bool m_DialogRequested = false;

    // Built on demand and kept until the popup closes, so the file is parsed
    // once, not every frame.
    std::vector<SceneOutlineNode> m_Preview;
    std::string m_PreviewName;

    float m_AutosaveTimer = 0.0f;
};

}  // namespace DekiEditor
