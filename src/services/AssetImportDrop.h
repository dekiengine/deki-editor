#pragma once

#include <string>

namespace DekiEditor
{

class AssetPipeline;

namespace AssetImportDrop
{

/// Copies a file or folder dropped from the OS into `targetDir`, gives its GUID
/// sidecars new GUIDs (they may come from another project) and registers and
/// compiles the assets through `pipeline`, which may be null. Returns false,
/// with `error` set, when the destination exists or the copy fails.
bool Import(const std::string& droppedPath, const std::string& targetDir, AssetPipeline* pipeline, std::string& error);

}  // namespace AssetImportDrop
}  // namespace DekiEditor
