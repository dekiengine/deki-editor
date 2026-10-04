#pragma once

#include <string>
#include <vector>
#include <filesystem>

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

struct DEKI_EDITOR_API ImportResult
{
    bool success = false;
    std::string errorMessage;
    std::string outputPath;  // the cache file written
};

/// Base interface for asset importers.
class DEKI_EDITOR_API IAssetImporter
{
public:
    virtual ~IAssetImporter() = default;

    /// The extensions it handles, e.g. {".png", ".jpg", ".bmp"}.
    virtual std::vector<std::string> GetSupportedExtensions() const = 0;

    /// A name for logs.
    virtual std::string GetImporterName() const = 0;

    /// Imports the source file into the cache.
    virtual ImportResult Import(const std::string& sourcePath, const std::string& cachePath) = 0;

    /// Whether the asset needs importing again. By default, when there is no
    /// cache file.
    virtual bool NeedsReimport(const std::string& sourcePath, const std::string& cachePath) const
    {
        return !std::filesystem::exists(cachePath);
    }
};

}  // namespace DekiEditor
