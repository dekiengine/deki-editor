#pragma once

// deki-packages.json: the project's declared packages (user intent only).
//
// Two writers need it, the Package Manager when it saves and the project
// scaffold when it creates a project, so the JSON shape lives here once.

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// PackageEntry lives at global scope in PackageManager.h.
struct PackageEntry;

namespace DekiEditor
{

/// Manifest JSON for a set of package entries: `schemaVersion`, `packages`
/// keyed by id (version, and source/url for non-registry packages, `editable`
/// when set), plus `dependencies` for version overrides. Only entries the
/// project declares, or that were installed by hand, are written.
nlohmann::json BuildPackageManifest(const std::vector<PackageEntry>& packages,
                                    const std::map<std::string, std::string>& versionOverrides);

/// Manifest JSON declaring the given registry package ids at the newest
/// version ("*"). What a new project starts from.
nlohmann::json BuildPackageManifestForIds(const std::vector<std::string>& packageIds);

/// How PackageEntry::source is spelled in the manifest and the lock.
///
/// "github" and "custom" are the editor's own. Anything else is the id of the
/// installer whose framework registry the package comes from, so the file
/// names the installer and the editor needs no table of frameworks ("espidf"
/// is the ESP-IDF installer's id).
///
/// An empty spelling means Custom in the manifest (where the key is omitted
/// for registry packages) and "leave it alone" in the lock.
void ReadPackageSource(const std::string& spelling, bool emptyIsCustom, PackageEntry& pkg);
std::string PackageSourceToString(const PackageEntry& pkg);

/// Atomic write of a manifest to `path`.
bool WritePackageManifest(const std::string& path, const nlohmann::json& manifest, std::string& err);

}  // namespace DekiEditor
