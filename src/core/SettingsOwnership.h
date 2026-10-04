#pragma once

#include <string>

namespace DekiEditor
{

/// Which package owns each project-settings section.
///
/// The settings registry knows a section's name but not which package
/// registered it. Settings are stored per package on disk, so this map holds
/// that link.
///
/// It is worked out, not declared: a settings section is an ordinary
/// Deki::Component, so it already appears in its package's exported component
/// metadata. After a package loads, PackageLoader matches the settings registry
/// against the package's registeredTypeIds and records the result here. This
/// keeps the registry struct unchanged; it crosses the DLL boundary, and
/// changing its layout would break every built package.
///
/// Lives in core/ so project saving and loading can read it without depending
/// on services/PackageLoader.
namespace SettingsOwnership
{

/// Records that `packageId` registered `sectionName`.
void SetOwner(const std::string& sectionName, const std::string& packageId);

/// The owning package id, or an empty string for engine-core sections.
std::string OwnerOf(const std::string& sectionName);

/// Forgets one package's sections, when the package unloads.
void ClearPackage(const std::string& packageId);

/// Forgets everything, when the project closes.
void Clear();

}  // namespace SettingsOwnership
}  // namespace DekiEditor
