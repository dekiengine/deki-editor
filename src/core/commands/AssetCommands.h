#pragma once

#include <deki-editor/Command.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace DekiEditor
{

/**
 * Command for creating a folder, so that folder creation is undoable like
 * folder deletion beside it. Undo removes the folder only while it is still
 * empty; anything the user put inside since is left alone (and said so).
 */
class CreateFolderCommand : public Command
{
public:
    CreateFolderCommand(const std::string& path, std::function<void()> onComplete = nullptr);

    std::string GetDescription() const override;
    void Execute() override;
    void Undo() override;

private:
    std::string m_Path;
    std::function<void()> m_OnComplete;
    bool m_Created = false;
};

/**
 * Command for deleting asset files/folders with undo support.
 *
 * Each deleted item is moved into a per-command trash directory and moved back
 * on undo. Three things matter for correctness here, and all three used to be
 * wrong:
 *
 *  - Origin and backup are stored as *pairs*. Two parallel vectors indexed in
 *    lockstep drifted the moment Execute skipped a path (missing file, failed
 *    move), and undo then restored files under each other's names.
 *  - Backup names are unique per entry, not just `filename()`. Deleting
 *    `ui/icon.png` and `hud/icon.png` in one action collided on a single backup
 *    path and destroyed one of them outright.
 *  - The trash lives inside the project, not in %TEMP%. std::filesystem::rename
 *    cannot cross volumes, so a project on another drive failed every move and
 *    the delete silently did nothing.
 */
class DeleteAssetsCommand : public Command
{
public:
    /// `projectRoot` anchors the trash directory. Pass the project path so the
    /// move stays on one volume; when empty the temp directory is used, which
    /// still works whenever project and temp share a drive.
    DeleteAssetsCommand(const std::vector<std::string>& paths,
                        std::function<void()> onComplete = nullptr,
                        const std::string& projectRoot = {});
    ~DeleteAssetsCommand();

    std::string GetDescription() const override;

    /// True when Execute could not move everything it was asked to. Callers can
    /// surface this instead of reporting a delete that partly did not happen.
    bool HadFailures() const { return m_HadFailures; }

    /// Remove deletes older than kTrashDays from the trash (and from the
    /// <project>/.trash older editors used). Called on project open. Until
    /// then a deleted file can be taken back from generated/trash/ by hand,
    /// after undo no longer can.
    static void SweepStaleTrash(const std::string& projectRoot);

    /// Where deleted assets wait: <project>/generated/trash/delete_<ms>/.
    static std::filesystem::path TrashRoot(const std::string& projectRoot);
    /// How long they wait there before SweepStaleTrash removes them.
    static constexpr int kTrashDays = 7;

    void Execute() override;
    void Undo() override;
    void Redo() override;

private:
    struct Move
    {
        std::string original;  // where the file came from
        std::string backup;    // where it is parked while deleted
    };

    std::vector<std::string> m_OriginalPaths;
    std::vector<Move> m_Moves;
    std::filesystem::path m_BackupDir;
    std::function<void()> m_OnComplete;
    bool m_Executed;
    bool m_HadFailures = false;
};

/**
 * Command for modifying an asset JSON file with undo support
 * Stores the complete JSON state before and after modification
 */
class ModifyAssetCommand : public Command
{
public:
    ModifyAssetCommand(const std::string& assetPath, const std::string& assetGuid,
                       const nlohmann::json& oldData, const nlohmann::json& newData,
                       std::function<void()> onApply = nullptr);

    std::string GetDescription() const override;
    void Execute() override;
    void Undo() override;
    bool CanMerge(const Command* other) const override;
    void Merge(const Command* other) override;

private:
    void ApplyData(const nlohmann::json& data);

    std::string m_AssetPath;
    std::string m_AssetGuid;
    std::string m_AssetType;
    nlohmann::json m_OldData;
    nlohmann::json m_NewData;
    std::function<void()> m_OnApply;
    bool m_Executed;
};

}  // namespace DekiEditor
