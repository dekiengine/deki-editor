#pragma once

// A third-party dependency a build needs, as read out of a package or engine
// descriptor.
//
// In its own header so a build backend can name the type without pulling in
// the editor's package manager.

#include <deki-editor/EditorExport.h>

#include <string>

// Global namespace, like the other package-manager types: the editor uses them
// unqualified, and moving them into DekiEditor would break every such use.

/// Where a dependency comes from.
enum class PackageSourceType
{
    GitHub,             // github.com
    FrameworkRegistry,  // the build framework's own component registry
    Custom              // user-defined URL
};

struct DEKI_EDITOR_API DependencyInfo
{
    std::string name;  // as the framework's registry or the repository names it
    std::string version;
    std::string gitUrl;  // empty for registry packages
    PackageSourceType source = PackageSourceType::FrameworkRegistry;
};
