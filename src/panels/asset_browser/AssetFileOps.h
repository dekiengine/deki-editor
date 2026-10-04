#pragma once

#include <string>

namespace DekiEditor
{

class AssetPipeline;

/// Moving, renaming and copying assets *with their sidecars*.
///
/// Every asset carries a `<file>.data` sidecar holding its GUID and import
/// settings. Scene files, prefabs and component references all address assets
/// by that GUID, so an operation that moves the asset but not the sidecar, or
/// that leaves two assets sharing one GUID, silently breaks references
/// somewhere else in the project - usually noticed much later, in a scene that
/// has nothing obviously to do with the file that was moved.
///
/// Split out of AssetBrowserPanel so these rules live in one place and can be
/// tested against a real directory rather than only through the UI.
namespace AssetFileOps
{

/// Move or rename an asset together with its sidecar.
///
/// The sidecar follows silently if it exists; a missing one is not an error,
/// since not every file in the tree is an imported asset. When @p pipeline is
/// non-null the asset's index entry is moved with it, so lookups by path do not
/// go stale.
///
/// Returns false when the asset itself could not be moved, in which case
/// nothing was changed.
bool MoveWithData(const std::string& srcPath, const std::string& destPath, AssetPipeline* pipeline = nullptr,
                  const std::string& projectRoot = {});

/// Rename in place, keeping the file's extension (directories keep just the
/// name). Refuses rather than overwrites when the destination already exists.
///
/// Returns false if the rename did not happen.
bool RenameWithData(const std::string& oldPath, const std::string& newName, AssetPipeline* pipeline = nullptr,
                    const std::string& projectRoot = {});

/// Give a freshly copied asset its own GUID.
///
/// A copy starts out sharing the original's sidecar, so until this runs there
/// are two assets claiming one GUID and references resolve to whichever the
/// pipeline indexed last. If the new sidecar cannot be written the copy is
/// REMOVED - a duplicate GUID is worse than a missing file, because it corrupts
/// references to the original.
void ReassignGuidForCopy(const std::string& srcPath, const std::string& destPath);

/// Same, for a copied directory: rewrites the guid in every sidecar beneath
/// @p root, leaving import settings alone.
void ReassignGuidsInTree(const std::string& root);

}  // namespace AssetFileOps
}  // namespace DekiEditor
