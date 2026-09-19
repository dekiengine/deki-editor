#pragma once

// A third-party dependency a build needs, as read out of a package or engine
// descriptor.
//
// Split out of the editor's PackageManager so a build backend can name the
// type without dragging in the whole package-management service. Everything
// else in PackageManager is about installing and updating packages, which is
// the editor's business, not a backend's.

#include <deki-editor/EditorExport.h>

#include <string>

// Global namespace, matching where these lived in PackageManager.h: its
// types are not namespaced, and moving them into DekiEditor as a side effect
// of relocating one struct would break every unqualified use in the editor.
// A tidy-up for its own commit, not this one.

/// Where a dependency comes from.
enum class PackageSourceType
{
    GitHub,             // github.com
    FrameworkRegistry,  // the build framework's own component registry
    Custom              // user-defined URL
};

struct DEKI_EDITOR_API DependencyInfo
{
    std::string name;     // e.g. "LovyanGFX"
    std::string version;  // version string
    std::string gitUrl;   // empty for registry packages
    PackageSourceType source = PackageSourceType::FrameworkRegistry;
};
