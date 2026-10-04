#pragma once

#include <string>

namespace DekiEditor
{

class AssetPipeline;

/// Moving, renaming and copying assets *with their sidecars*.
///
/// Every asset has a `<file>.data` sidecar holding its GUID and import
/// settings. Scenes, prefabs and component references find assets by that
/// GUID, so moving an asset without its sidecar, or leaving two assets with one
/// GUID, quietly breaks references elsewhere in the project.
///
/// Kept apart from AssetBrowserPanel so the rules live in one place and can be
/// tested against a real directory.
namespace AssetFileOps
{

/// Moves or renames an asset together with its sidecar.
///
/// The sidecar moves when there is one; a missing one is fine, since not every
/// file in the tree is an imported asset. With a `pipeline`, the asset's index
/// entry moves too, so lookups by path stay right.
///
/// Returns false when the asset could not be moved; nothing changed then.
bool MoveWithData(const std::string& srcPath, const std::string& destPath, AssetPipeline* pipeline = nullptr,
                  const std::string& projectRoot = {});

/// Renames in place, keeping a file's extension. Refuses when the
/// destination already exists. Returns false if the rename did not happen.
bool RenameWithData(const std::string& oldPath, const std::string& newName, AssetPipeline* pipeline = nullptr,
                    const std::string& projectRoot = {});

/// Gives a freshly copied asset its own GUID.
///
/// A copy starts with the original's sidecar, so until this runs two assets
/// claim one GUID and references resolve to whichever was indexed last. If the
/// new sidecar cannot be written the copy is REMOVED: a duplicate GUID is
/// worse than a missing file, because it breaks references to the original.
void ReassignGuidForCopy(const std::string& srcPath, const std::string& destPath);

/// The same for a copied directory: rewrites the GUID in every sidecar under
/// `root` and leaves import settings alone.
void ReassignGuidsInTree(const std::string& root);

}  // namespace AssetFileOps
}  // namespace DekiEditor
