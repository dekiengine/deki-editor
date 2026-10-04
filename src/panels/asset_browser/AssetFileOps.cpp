#include "AssetFileOps.h"

#include <deki-editor/AssetData.h>
#include <deki-editor/AssetPipeline.h>
#include <deki/Guid.h>
#include <deki/LogSystem.h>

#include <filesystem>
#include <system_error>

namespace DekiEditor
{
namespace AssetFileOps
{

namespace fs = std::filesystem;

namespace
{
/// Project-relative with forward slashes, the form the pipeline indexes by.
std::string ToPipelinePath(const std::string& absolute, const std::string& projectRoot)
{
    std::error_code ec;
    std::string rel = fs::relative(absolute, projectRoot, ec).string();
    if (ec)
    {
        return {};
    }
    for (char& c : rel)
    {
        if (c == '\\')
        {
            c = '/';
        }
    }
    return rel;
}
}  // namespace

bool MoveWithData(const std::string& srcPath, const std::string& destPath, AssetPipeline* pipeline,
                  const std::string& projectRoot)
{
    std::error_code ec;
    fs::rename(srcPath, destPath, ec);
    if (ec)
    {
        return false;
    }

    // The sidecar follows. Not every file is an imported asset, so a missing
    // one is normal. Failing to move one is logged but not fatal: the pipeline
    // makes a new sidecar, but with a new GUID, so references to the asset
    // break.
    const std::string srcData = srcPath + ".data";
    if (fs::exists(srcData))
    {
        std::error_code dataEc;
        fs::rename(srcData, destPath + ".data", dataEc);
        if (dataEc)
        {
            DEKI_LOG_WARNING("Asset move: '%s' moved but its sidecar did not (%s); "
                             "references to this asset may not resolve until reimport",
                             destPath.c_str(), dataEc.message().c_str());
        }
    }

    if (pipeline && !projectRoot.empty())
    {
        std::error_code dirEc;
        if (fs::is_directory(destPath, dirEc))
        {
            // A folder: every asset inside moved with it, so each index entry
            // moves too, or the asset picker looks for them at the old paths.
            for (const auto& item : fs::recursive_directory_iterator(destPath, dirEc))
            {
                if (!item.is_regular_file() || item.path().extension() == ".data")
                {
                    continue;
                }
                const fs::path inside = fs::relative(item.path(), destPath, dirEc);
                const std::string oldRel = ToPipelinePath((fs::path(srcPath) / inside).string(), projectRoot);
                const std::string newRel = ToPipelinePath(item.path().string(), projectRoot);
                if (!oldRel.empty())
                {
                    pipeline->RemoveAsset(oldRel);
                }
                if (!newRel.empty())
                {
                    pipeline->RefreshAsset(newRel);
                }
            }
        }
        else
        {
            const std::string oldRel = ToPipelinePath(srcPath, projectRoot);
            const std::string newRel = ToPipelinePath(destPath, projectRoot);
            if (!oldRel.empty())
            {
                pipeline->RemoveAsset(oldRel);
            }
            if (!newRel.empty())
            {
                pipeline->RefreshAsset(newRel);
            }
        }
    }
    return true;
}

bool RenameWithData(const std::string& oldPath, const std::string& newName, AssetPipeline* pipeline,
                    const std::string& projectRoot)
{
    if (newName.empty())
    {
        return false;
    }

    fs::path oldPathObj(oldPath);
    fs::path parentDir = oldPathObj.parent_path();

    // Directories rename wholesale; files keep their extension, so the user
    // types "Player", not "Player.png".
    std::error_code ec;
    const std::string newFileName = fs::is_directory(oldPath, ec) ? newName : newName + oldPathObj.extension().string();

    fs::path newPath = parentDir / newFileName;

    // Refuse rather than overwrite: replacing another asset would destroy it
    // and break every reference to it.
    if (fs::exists(newPath))
    {
        return false;
    }

    // A rename is a move within one directory.
    return MoveWithData(oldPath, newPath.string(), pipeline, projectRoot);
}

void ReassignGuidForCopy(const std::string& srcPath, const std::string& destPath)
{
    const std::string srcDataPath = srcPath + ".data";
    if (!fs::exists(srcDataPath))
    {
        return;  // not an imported asset; nothing claims a GUID
    }

    AssetData data;
    if (!LoadAssetData(srcDataPath, data))
    {
        return;
    }

    data.guid = GenerateGuid();
    if (!SaveAssetData(destPath + ".data", data))
    {
        DEKI_LOG_ERROR("Asset copy: could not write sidecar for '%s'; it would share "
                       "the source's GUID, so removing the copy",
                       destPath.c_str());
        std::error_code ec;
        fs::remove(destPath, ec);
    }
}

void ReassignGuidsInTree(const std::string& root)
{
    std::error_code ec;
    if (!fs::exists(root, ec))
    {
        return;
    }

    // The copy already has its sidecars, so only their guid field changes;
    // import settings stay.
    for (const auto& entry : fs::recursive_directory_iterator(root, ec))
    {
        if (ec)
        {
            break;
        }
        if (!entry.is_regular_file())
        {
            continue;
        }
        if (entry.path().extension() != ".data")
        {
            continue;
        }

        AssetData data;
        if (!LoadAssetData(entry.path().string(), data))
        {
            continue;
        }

        data.guid = GenerateGuid();
        if (!SaveAssetData(entry.path().string(), data))
        {
            DEKI_LOG_ERROR("Asset copy: could not rewrite '%s'; the copied asset still "
                           "shares the original's GUID",
                           entry.path().string().c_str());
        }
    }
}

}  // namespace AssetFileOps
}  // namespace DekiEditor
