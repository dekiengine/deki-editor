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
/// Project-relative, forward-slashed — the form the pipeline indexes by.
std::string ToPipelinePath(const std::string& absolute, const std::string& projectRoot)
{
    std::error_code ec;
    std::string rel = fs::relative(absolute, projectRoot, ec).string();
    if (ec) return {};
    for (char& c : rel)
    {
        if (c == '\\') c = '/';
    }
    return rel;
}
}  // namespace

bool MoveWithData(const std::string& srcPath, const std::string& destPath,
                  AssetPipeline* pipeline, const std::string& projectRoot)
{
    std::error_code ec;
    fs::rename(srcPath, destPath, ec);
    if (ec)
        return false;

    // The sidecar follows. Not every file is an imported asset, so its absence
    // is normal; a failure to move an existing one is not fatal either, since
    // the pipeline can regenerate it — but it is worth a line, because the
    // asset comes back with a fresh GUID and references to it will not resolve.
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
        const std::string oldRel = ToPipelinePath(srcPath, projectRoot);
        const std::string newRel = ToPipelinePath(destPath, projectRoot);
        if (!oldRel.empty()) pipeline->RemoveAsset(oldRel);
        if (!newRel.empty()) pipeline->RefreshAsset(newRel);
    }
    return true;
}

bool RenameWithData(const std::string& oldPath, const std::string& newName,
                    AssetPipeline* pipeline, const std::string& projectRoot)
{
    if (newName.empty())
        return false;

    fs::path oldPathObj(oldPath);
    fs::path parentDir = oldPathObj.parent_path();

    // Directories rename wholesale; files keep their extension, so the user
    // types "Player", not "Player.png".
    std::error_code ec;
    const std::string newFileName = fs::is_directory(oldPath, ec)
                                        ? newName
                                        : newName + oldPathObj.extension().string();

    fs::path newPath = parentDir / newFileName;

    // Refuse rather than overwrite: silently replacing another asset would
    // destroy it and orphan every reference to it.
    if (fs::exists(newPath))
        return false;

    // A rename is a move within one directory.
    return MoveWithData(oldPath, newPath.string(), pipeline, projectRoot);
}

void ReassignGuidForCopy(const std::string& srcPath, const std::string& destPath)
{
    const std::string srcDataPath = srcPath + ".data";
    if (!fs::exists(srcDataPath))
        return;  // not an imported asset; nothing claims a GUID

    AssetData data;
    if (!LoadAssetData(srcDataPath, data))
        return;

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
        return;

    // Rewrite every sidecar in place. The copy already contains them, so this
    // only needs to change the guid field and leave import settings alone.
    for (const auto& entry : fs::recursive_directory_iterator(root, ec))
    {
        if (ec) break;
        if (!entry.is_regular_file())
            continue;
        if (entry.path().extension() != ".data")
            continue;

        AssetData data;
        if (!LoadAssetData(entry.path().string(), data))
            continue;

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
