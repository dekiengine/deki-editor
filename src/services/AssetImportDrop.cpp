#include "AssetImportDrop.h"
#include "../panels/asset_browser/AssetFileOps.h"

#include <deki-editor/AssetData.h>
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/AssetTypeRegistry.h>

#include <cctype>
#include <filesystem>

namespace DekiEditor
{
namespace AssetImportDrop
{

namespace
{
// Which dropped files get a .data sidecar (GUID) and a cache entry: the
// package-owned texture/animation/scene categories, plus BDF fonts.
bool NeedsGuid(const std::string& extension)
{
    const AssetCategory category = AssetTypeRegistry::Instance().GetCategoryForExtension(extension);
    if (category == AssetCategory::Texture || category == AssetCategory::Animation || category == AssetCategory::Scene)
        return true;
    std::string ext = extension;
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".bdf";
}

void RegisterAsset(const std::filesystem::path& path, AssetPipeline* pipeline)
{
    if (!NeedsGuid(path.extension().string()))
        return;
    GetOrCreateAssetGuid(path.string());
    // Compile to the cache (the pipeline wants a project-relative path)
    if (pipeline)
        pipeline->RefreshAsset(pipeline->MakeProjectRelative(path.string()));
}
}  // namespace

bool Import(const std::string& droppedPath, const std::string& targetDir,
            AssetPipeline* pipeline, std::string& error)
{
    namespace fs = std::filesystem;

    const fs::path sourcePath(droppedPath);
    std::error_code ec;
    if (!fs::exists(sourcePath, ec))
    {
        error = "'" + droppedPath + "' does not exist";
        return false;
    }

    const fs::path destPath = fs::path(targetDir) / sourcePath.filename();
    if (fs::exists(destPath, ec))
    {
        error = "'" + sourcePath.filename().string() + "' already exists in '" + targetDir + "'";
        return false;
    }

    try
    {
        if (fs::is_directory(sourcePath))
        {
            fs::copy(sourcePath, destPath, fs::copy_options::recursive);
            // Re-mint every sidecar GUID before anything adopts them.
            AssetFileOps::ReassignGuidsInTree(destPath.string());
            for (const auto& entry : fs::recursive_directory_iterator(destPath))
            {
                if (entry.is_regular_file())
                    RegisterAsset(entry.path(), pipeline);
            }
        }
        else
        {
            fs::copy_file(sourcePath, destPath);
            AssetFileOps::ReassignGuidForCopy(sourcePath.string(), destPath.string());
            RegisterAsset(destPath, pipeline);
        }
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }
    return true;
}

}  // namespace AssetImportDrop
}  // namespace DekiEditor
