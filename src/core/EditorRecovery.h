#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "SceneStats.h"  // SceneOutlineNode, CountSceneObjects, BuildSceneOutline

namespace Deki { class Scene; }

namespace DekiEditor
{

/// One recovery snapshot found on disk, plus enough metadata to decide whether
/// it is worth taking without opening it. "recovered.scene, untitled, 1 object"
/// is almost always a crash before anything was built and safe to discard; the
/// same line with 200 objects and this morning's timestamp is not.
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
/// Deliberately free of EditorApp.h and of any UI: the dialogs that present
/// this state still live in EditorApp.cpp and call in here. That is what lets
/// this compile into DekiEditorTests, where the interesting behaviour - sidecar
/// parsing, newest-first ordering, per-entry consumption, timer policy - can be
/// exercised without standing up an editor.
///
/// The class deliberately takes the scene and paths it needs as parameters
/// rather than holding an EditorApp back-pointer. Snapshot policy that depends
/// on play mode or dirty state stays with the caller, which owns that state;
/// this owns the files and the clock.
class EditorRecovery
{
public:
    /// Recovery directory for a project, or "" when there is no editor data
    /// directory to write into.
    ///
    /// Keyed per project so two projects cannot see each other's recoveries.
    /// The folder name keeps the readable project name for anyone browsing the
    /// directory, plus a hash of the normalised full path so two projects of
    /// the same name in different places stay separate.
    static std::string RecoveryDirFor(const std::string& projectPath);

    /// Delete the whole recovery directory. Only once every entry is resolved.
    static void RemoveRecoveryDir(const std::string& projectPath);

    // -- discovery ----------------------------------------------------------

    /// Populate the entry list from the project's recovery directory, newest
    /// first. Resets the selection. Safe on a missing directory (finds nothing).
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

    /// Write @p scene to the project's recovery directory with its metadata
    /// sidecar, recording @p scenePath as the original location so a restore
    /// can rebind to it. Returns false if there was nothing to write or the
    /// write failed; never throws - this also runs from the crash handler.
    ///
    /// @param outWrittenPath optional; receives the file actually written. The
    ///        caller reports it (the crash dialog shows it) - this class stays
    ///        free of the crash handler, which lives in the editor executable
    ///        and drags in SDL and ImGui.
    bool WriteSnapshot(Deki::Scene* scene, const std::string& scenePath,
                       const std::string& projectPath,
                       std::string* outWrittenPath = nullptr);

    /// Advance the autosave clock. Returns true exactly on the ticks where a
    /// snapshot is due, at which point the caller should call WriteSnapshot.
    ///
    /// @p ready is the caller's "a snapshot would be meaningful right now"
    /// verdict - a project and scene are open, we are not in play mode, and the
    /// history is dirty. When it is false the timer parks at zero, so the first
    /// change after a save gets a full interval rather than an immediate write.
    bool TickAutosaveTimer(float deltaSeconds, bool ready);

    static constexpr float kAutosaveIntervalSeconds = 90.0f;

    // -- preview ------------------------------------------------------------

    /// Parse an entry's scene into the preview outline. Safe on a truncated
    /// file: the outline just comes back empty, which the popup reports as
    /// unreadable rather than as an empty scene.
    void BuildPreview(const RecoveryEntry& entry);
    const std::vector<SceneOutlineNode>& Preview() const { return m_Preview; }
    const std::string& PreviewName() const { return m_PreviewName; }
    void ClearPreview();

    // -- consumption --------------------------------------------------------

    /// Delete one entry's files and drop it from the list. Consuming only what
    /// was restored matters: this used to remove the whole directory, so with
    /// two crashed scenes the second was destroyed without ever being offered.
    /// Returns false if the index is out of range.
    bool ConsumeEntry(int index);

    /// Delete every remaining recovery and the directory itself.
    void DiscardAll();

    /// Delete the snapshots this session wrote, once they are obsolete: the
    /// scene was saved, replaced, or the editor is quitting normally. A crash
    /// never reaches this, so its snapshot survives for the next open.
    ///
    /// Without it, a scene left unsaved past one autosave interval kept its
    /// snapshot forever, and every later open offered a "recovery" of work that
    /// had been saved or discarded on purpose.
    ///
    /// Recoveries that were pending when the project opened (a crash postponed
    /// with Later) are never touched, even when an autosave has since written
    /// to the same file.
    void DiscardSessionSnapshots();

private:
    std::vector<RecoveryEntry> m_Files;
    std::vector<std::string> m_SessionSnapshots;  // written by this process
    std::string m_ProjectPath;
    int m_Selected = 0;
    bool m_DialogRequested = false;

    // Built on demand and held until the popup closes, so the file is parsed
    // once rather than on every frame the popup is open.
    std::vector<SceneOutlineNode> m_Preview;
    std::string m_PreviewName;

    float m_AutosaveTimer = 0.0f;
};

}  // namespace DekiEditor
