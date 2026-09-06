#pragma once

#include <string>

namespace DekiEditor
{

class AssetPipeline;

namespace AssetImportDrop
{

// Copies a file or folder dropped from the OS into `targetDir`, re-mints the
// GUID sidecars it carries (a sidecar from another project would otherwise be
// adopted wholesale) and registers/compiles the assets through `pipeline`
// (may be null). False, with `error` set, when the destination exists or the
// copy fails.
bool Import(const std::string& droppedPath, const std::string& targetDir,
            AssetPipeline* pipeline, std::string& error);

}  // namespace AssetImportDrop
}  // namespace DekiEditor
