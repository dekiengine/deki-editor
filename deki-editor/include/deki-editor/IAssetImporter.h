#pragma once

#include <string>
#include <vector>
#include <filesystem>

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

// Result of an import operation
struct DEKI_EDITOR_API ImportResult
{
    bool success = false;
    std::string errorMessage;
    std::string outputPath;  // Path to generated cache file
};

// Base interface for all asset importers
class DEKI_EDITOR_API IAssetImporter
{
public:
    virtual ~IAssetImporter() = default;

    // Get supported file extensions (e.g., {".png", ".jpg", ".bmp"})
    virtual std::vector<std::string> GetSupportedExtensions() const = 0;

    // Get importer name for logging/debugging
    virtual std::string GetImporterName() const = 0;

    // Import asset from source to cache
    virtual ImportResult Import(const std::string& sourcePath, const std::string& cachePath) = 0;

    // Check if asset needs re-import (for incremental builds)
    virtual bool NeedsReimport(const std::string& sourcePath, const std::string& cachePath) const
    {
        return !std::filesystem::exists(cachePath);
    }
};

}  // namespace DekiEditor
