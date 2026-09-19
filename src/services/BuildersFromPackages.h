#pragma once

// Finding build backends and package installers among the packages a project
// has loaded.
//
// The editor ships neither. ESP-IDF and native builders live in
// deki-esp32-integration and deki-desktop-integration, and the ESP-IDF
// component installer with them; all arrive through the same plugin exports a
// third-party or NDA'd backend uses. That way the path is the only path,
// exercised on every build, and cannot quietly rot.
//
// Here rather than in deki-editor.dll because the registries are in the DLL
// and PackageLoader is in the application: the DLL cannot look into the exe.
// The knowledge is generic either way - it probes for exported symbols and
// registers whatever answers, naming no target.

namespace DekiEditor
{
class PackageLoader;
}

// PackageManager is in the global namespace, with the package types it belongs to.
class PackageManager;

namespace DekiEditor
{

/// Probe loaded packages for builder-plugin exports and register what they
/// provide. Safe to call repeatedly; frameworks already claimed are kept.
void RegisterBuildersFromPackages(PackageLoader& loader);

/// Probe loaded packages for installer-plugin exports and register them.
/// Safe to call repeatedly.
void RegisterInstallersFromPackages(PackageLoader& loader, ::PackageManager& manager);

}  // namespace DekiEditor
