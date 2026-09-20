#pragma once

#include <string>

namespace DekiEditor
{

/**
 * @brief Which package owns each project-settings section.
 *
 * The settings registry knows a section's name and whether it came from a plugin,
 * but not *which* package registered it - it never needed to. Settings are stored
 * per-package on disk, so that link has to exist somewhere.
 *
 * It is derived rather than declared: a settings section is an ordinary
 * Deki::Component, so it already appears in its package's exported component
 * metadata. PackageLoader intersects the settings registry against each package's
 * registeredTypeIds after load and records the result here. Packages need no new
 * export and the registry needs no new field - which matters, because that struct
 * crosses the DLL boundary and changing its layout would break every built package.
 *
 * Lives in core/ so project persistence can read it without depending on
 * services/PackageLoader.
 */
namespace SettingsOwnership
{

/** Record that @p packageId registered @p sectionName. */
void SetOwner(const std::string& sectionName, const std::string& packageId);

/** Owning package id, or an empty string for engine-core sections. */
std::string OwnerOf(const std::string& sectionName);

/** Forget one package's sections (package unload). */
void ClearPackage(const std::string& packageId);

/** Forget everything (project close). */
void Clear();

}  // namespace SettingsOwnership
}  // namespace DekiEditor
