#pragma once

#include <deki-editor/Command.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace DekiEditor
{

/// Creates a folder as an undoable command. Undo removes the folder only
/// while it is still empty; if the user has put something inside since, it
/// stays and a warning is logged.
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

/// Deletes asset files and folders with undo.
///
/// Each deleted item is moved into a per-command trash directory and moved
/// back on undo. Three rules keep this correct:
///
///  - Origin and backup are stored as pairs. Parallel vectors drift as soon
///    as Execute skips a path, and undo then restores files under each
///    other's names.
///  - Backup names are unique per entry, not just `filename()`, so deleting
///    `ui/icon.png` and `hud/icon.png` together does not collide.
///  - The trash lives inside the project, not in %TEMP%:
///    std::filesystem::rename cannot cross volumes.
class DeleteAssetsCommand : public Command
{
public:
    /// `projectRoot` anchors the trash directory. Pass the project path so the
    /// move stays on one volume. When empty, the temp directory is used, which
    /// works only when project and temp share a drive.
    DeleteAssetsCommand(const std::vector<std::string>& paths, std::function<void()> onComplete = nullptr,
                        const std::string& projectRoot = {});
    ~DeleteAssetsCommand();

    std::string GetDescription() const override;

    /// True when Execute could not move everything it was asked to, so callers
    /// can report a partial delete.
    bool HadFailures() const { return m_HadFailures; }

    /// Removes deletes older than kTrashDays from the trash. Called on project
    /// open. Until then a deleted
    /// file can be taken back from generated/trash/ by hand, even after undo
    /// no longer can.
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

/// Rewrites an asset JSON file with undo. Stores the whole JSON before and
/// after the change.
class ModifyAssetCommand : public Command
{
public:
    ModifyAssetCommand(const std::string& assetPath, const std::string& assetGuid, const nlohmann::json& oldData,
                       const nlohmann::json& newData, std::function<void()> onApply = nullptr);

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
