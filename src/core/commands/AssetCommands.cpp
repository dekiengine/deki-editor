#include "AssetCommands.h"

#include <deki-editor/FileIO.h>
#include <deki/LogSystem.h>

#include <chrono>
#include <system_error>
#include <vector>

namespace DekiEditor
{

namespace fs = std::filesystem;

// ----------------------------------------------------------------------------
// CreateFolderCommand
// ----------------------------------------------------------------------------

CreateFolderCommand::CreateFolderCommand(const std::string& path, std::function<void()> onComplete)
    : m_Path(path),
      m_OnComplete(std::move(onComplete))
{
}

std::string CreateFolderCommand::GetDescription() const
{
    return "Create folder " + fs::path(m_Path).filename().string();
}

void CreateFolderCommand::Execute()
{
    std::error_code ec;
    m_Created = !fs::exists(m_Path, ec) && fs::create_directories(m_Path, ec);
    if (ec)
    {
        DEKI_LOG_ERROR("Create folder '%s' failed: %s", m_Path.c_str(), ec.message().c_str());
    }
    if (m_OnComplete)
    {
        m_OnComplete();
    }
}

void CreateFolderCommand::Undo()
{
    if (!m_Created)
    {
        return;
    }
    std::error_code ec;
    if (fs::exists(m_Path, ec) && !fs::is_empty(m_Path, ec))
    {
        DEKI_LOG_WARNING("Undo create folder: '%s' is no longer empty, leaving it in place", m_Path.c_str());
        return;
    }
    fs::remove(m_Path, ec);
    if (ec)
    {
        DEKI_LOG_ERROR("Undo create folder '%s' failed: %s", m_Path.c_str(), ec.message().c_str());
    }
    if (m_OnComplete)
    {
        m_OnComplete();
    }
}

// ----------------------------------------------------------------------------
// DeleteAssetsCommand
// ----------------------------------------------------------------------------

DeleteAssetsCommand::DeleteAssetsCommand(const std::vector<std::string>& paths, std::function<void()> onComplete,
                                         const std::string& projectRoot)
    : m_OriginalPaths(paths),
      m_OnComplete(std::move(onComplete)),
      m_Executed(false)
{
    std::error_code ec;
    fs::path trashRoot =
        projectRoot.empty() ? fs::temp_directory_path(ec) / "deki_editor_backup" : TrashRoot(projectRoot);

    // Timestamp keeps concurrent delete commands from sharing a directory. The
    // directory itself is created by Execute: constructing the command (which
    // the confirm dialog does before the user has answered) must not touch the
    // project.
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    m_BackupDir = trashRoot / ("delete_" + std::to_string(ms));
}

DeleteAssetsCommand::~DeleteAssetsCommand()
{
    // The deleted files stay in the trash when the command leaves the undo
    // history (opening another scene clears it): deleting them here made a
    // mistaken delete permanent a few clicks later. SweepStaleTrash removes
    // them after kTrashDays. An undone delete leaves the folder empty.
    std::error_code ec;
    if (fs::is_directory(m_BackupDir, ec) && fs::is_empty(m_BackupDir, ec))
    {
        fs::remove(m_BackupDir, ec);
    }
}

fs::path DeleteAssetsCommand::TrashRoot(const std::string& projectRoot)
{
    // Under generated/, which projects keep out of version control: the old
    // <project>/.trash was not in the scaffold's .gitignore and got committed.
    return fs::path(projectRoot) / "generated" / "trash";
}

std::string DeleteAssetsCommand::GetDescription() const
{
    if (m_OriginalPaths.size() == 1)
    {
        return "Delete " + fs::path(m_OriginalPaths[0]).filename().string();
    }
    return "Delete " + std::to_string(m_OriginalPaths.size()) + " items";
}

void DeleteAssetsCommand::SweepStaleTrash(const std::string& projectRoot)
{
    if (projectRoot.empty())
    {
        return;
    }

    // Only deletes older than kTrashDays go. The whole trash used to be
    // emptied on every open, so a delete followed by a crash (or just closing
    // the editor) could not be taken back by any means.
    // Aged by the time in the folder's name (delete_<ms since epoch>): a
    // folder's own modification time moves when an undo takes files back out.
    const auto nowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    const long long maxAgeMs = 1000LL * 60 * 60 * 24 * kTrashDays;
    for (const fs::path& root : { TrashRoot(projectRoot), fs::path(projectRoot) / ".trash" })
    {
        std::error_code ec;
        if (!fs::is_directory(root, ec))
        {
            continue;
        }
        std::vector<fs::path> old;
        for (const auto& entry : fs::directory_iterator(root, ec))
        {
            const std::string name = entry.path().filename().string();
            long long ms = 0;
            if (name.rfind("delete_", 0) == 0)
            {
                try
                {
                    ms = std::stoll(name.substr(7));
                }
                catch (...)
                {
                    ms = 0;
                }
            }
            if (ms == 0 || nowMs - ms > maxAgeMs)  // unreadable names are not kept forever
            {
                old.push_back(entry.path());
            }
        }
        for (const fs::path& p : old)
        {
            fs::remove_all(p, ec);
        }
        if (fs::is_empty(root, ec))
        {
            fs::remove(root, ec);
        }
    }
}

void DeleteAssetsCommand::Execute()
{
    m_Moves.clear();
    m_HadFailures = false;

    std::error_code dirEc;
    fs::create_directories(m_BackupDir, dirEc);
    if (dirEc)
    {
        DEKI_LOG_ERROR("Delete: cannot create trash directory '%s': %s", m_BackupDir.string().c_str(),
                       dirEc.message().c_str());
        m_HadFailures = true;
        m_Executed = true;
        if (m_OnComplete)
        {
            m_OnComplete();
        }
        return;
    }

    // Index prefix guarantees a unique backup name even when several
    // selected paths share a filename.
    size_t slot = 0;
    for (const auto& path : m_OriginalPaths)
    {
        const size_t mySlot = slot++;

        fs::path src(path);
        std::error_code existsEc;
        if (!fs::exists(src, existsEc))
        {
            continue;  // already gone; nothing to move or restore
        }

        fs::path backupPath = m_BackupDir / (std::to_string(mySlot) + "_" + src.filename().string());

        std::error_code ec;
        fs::rename(src, backupPath, ec);
        if (ec)
        {
            // Do not record a move that did not happen - Undo would then try
            // to restore a file that is still in place.
            m_HadFailures = true;
            continue;
        }

        m_Moves.push_back({ path, backupPath.string() });

        // The .data sidecar carries the asset's GUID, so it has to travel
        // with the asset and come back with it.
        fs::path dataPath(path + ".data");
        if (fs::exists(dataPath, existsEc))
        {
            fs::path backupDataPath = m_BackupDir / (std::to_string(mySlot) + "_" + dataPath.filename().string());
            std::error_code ecData;
            fs::rename(dataPath, backupDataPath, ecData);
            if (!ecData)
            {
                m_Moves.push_back({ dataPath.string(), backupDataPath.string() });
            }
            else
            {
                m_HadFailures = true;
            }
        }
    }

    m_Executed = true;
    if (m_OnComplete)
    {
        m_OnComplete();
    }
}

void DeleteAssetsCommand::Undo()
{
    // Reverse order so a directory is restored before anything that lived
    // inside it is put back.
    for (auto it = m_Moves.rbegin(); it != m_Moves.rend(); ++it)
    {
        fs::path src(it->backup);
        fs::path dest(it->original);

        std::error_code ec;
        if (!fs::exists(src, ec))
        {
            continue;
        }
        if (fs::exists(dest, ec))
        {
            continue;  // something else occupies the path; leave it alone
        }

        if (dest.has_parent_path())
        {
            fs::create_directories(dest.parent_path(), ec);
        }

        fs::rename(src, dest, ec);
    }

    m_Moves.clear();
    m_Executed = false;
    if (m_OnComplete)
    {
        m_OnComplete();
    }
}

void DeleteAssetsCommand::Redo()
{
    Execute();
}

// ----------------------------------------------------------------------------
// ModifyAssetCommand
// ----------------------------------------------------------------------------

ModifyAssetCommand::ModifyAssetCommand(const std::string& assetPath, const std::string& assetGuid,
                                       const nlohmann::json& oldData, const nlohmann::json& newData,
                                       std::function<void()> onApply)
    : m_AssetPath(assetPath),
      m_AssetGuid(assetGuid),
      m_OldData(oldData),
      m_NewData(newData),
      m_OnApply(std::move(onApply)),
      m_Executed(false)
{
    // Extract asset type for description
    m_AssetType = newData.value("type", "Asset");
}

std::string ModifyAssetCommand::GetDescription() const
{
    return "Modify " + m_AssetType;
}

void ModifyAssetCommand::Execute()
{
    ApplyData(m_NewData);
    m_Executed = true;
}

void ModifyAssetCommand::Undo()
{
    ApplyData(m_OldData);
}

bool ModifyAssetCommand::CanMerge(const Command* other) const
{
    if (!m_Executed)
    {
        return false;
    }
    auto* cmd = dynamic_cast<const ModifyAssetCommand*>(other);
    return cmd && cmd->m_AssetPath == m_AssetPath;
}

void ModifyAssetCommand::Merge(const Command* other)
{
    auto* cmd = dynamic_cast<const ModifyAssetCommand*>(other);
    if (cmd)
    {
        m_NewData = cmd->m_NewData;
    }
}

void ModifyAssetCommand::ApplyData(const nlohmann::json& data)
{
    // Atomic: this overwrites a user-authored .asset file. Streaming into it
    // truncates first, so an interrupted write left the asset destroyed -
    // the same failure the scene save had.
    std::string err;
    if (!AtomicWriteFile(m_AssetPath, data.dump(2), err))
    {
        DEKI_LOG_ERROR("ModifyAssetCommand: failed to write '%s': %s", m_AssetPath.c_str(), err.c_str());
        return;  // no callback: nothing changed on disk
    }

    // Call the apply callback (triggers rebuild and texture invalidation)
    if (m_OnApply)
    {
        m_OnApply();
    }
}

}  // namespace DekiEditor
