#pragma once

// Finds build backends and package installers among the packages a project
// has loaded.
//
// All of them come from packages: the ESP-IDF and native builders live in
// deki-esp32-integration and deki-desktop-integration, with the ESP-IDF
// component installer, and arrive through the same plugin exports a
// third-party backend uses. So this one path is exercised on every build.
//
// This lives in the application, not deki-editor.dll, because PackageLoader is
// in the application and the DLL cannot reach into the exe. It names no
// target: it probes for exported symbols and registers whatever answers.

namespace DekiEditor
{
class PackageLoader;
}

// PackageManager is in the global namespace, with the package types it belongs to.
class PackageManager;

namespace DekiEditor
{

/// Probes loaded packages for builder-plugin exports and registers what they
/// provide. Safe to call repeatedly; frameworks already claimed are kept.
void RegisterBuildersFromPackages(PackageLoader& loader);

/// Probes loaded packages for installer-plugin exports and registers them.
/// Safe to call repeatedly.
void RegisterInstallersFromPackages(PackageLoader& loader, ::PackageManager& manager);

}  // namespace DekiEditor
