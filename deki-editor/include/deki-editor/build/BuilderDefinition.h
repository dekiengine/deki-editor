#pragma once

#include <deki-editor/EditorExport.h>

#include <string>
#include <vector>
#include <map>

namespace DekiEditor
{

// ============================================================================
// Toolchain definition, parsed from JSON a build backend supplies
// ============================================================================

struct DEKI_EDITOR_API VersionCheckDef
{
    std::string type;          // "github_release" or "static"
    std::string repo;          // For github_release: "owner/name"
    std::string assetPattern;  // For github_release: asset filename pattern
    std::string version;       // For static: fixed version string
};

struct DEKI_EDITOR_API FallbackDef
{
    std::string version;
    std::string url;
};

struct DEKI_EDITOR_API PostInstallDef
{
    std::string windows;
    std::string unixLike;  // Linux and macOS
};

struct DEKI_EDITOR_API SubComponentDef
{
    std::string name;
    std::string version;
    std::string url;
    // Expected SHA-256 of the download, lowercase hex. Anything fetched here is
    // extracted and then run by the build, so an unpinned entry trusts the
    // network and the mirror. Empty is accepted but logged as unverified.
    std::string sha256;
    std::string extractedName;  // Name of extracted folder from zip
    std::string targetName;     // Desired folder name after rename
};

struct DEKI_EDITOR_API ComponentDef
{
    std::string id;
    std::string displayName;
    bool required = false;
    bool canInstall = false;
    bool canSetup = false;
    std::string installPath;  // Relative to ~/.deki-editor/toolchains/
    VersionCheckDef versionCheck;
    FallbackDef fallback;

    // Detection paths per platform. Key: "windows", "unix" or "any".
    // Values may contain {installPath} and * wildcards.
    std::map<std::string, std::string> detection;

    PostInstallDef postInstall;
    std::vector<SubComponentDef> subComponents;
    std::string extractedName;  // Name of extracted folder from zip (for rename)
    // Expected SHA-256 of this component's download, lowercase hex; see
    // SubComponentDef::sha256. Empty for components whose URL comes from a
    // GitHub release at run time, since those cannot be pinned this way.
    std::string sha256;
    std::string tooltip;
};

struct DEKI_EDITOR_API BuilderDefinition
{
    std::string id;      // the framework id the backend claims
    std::string name;    // display name
    std::string family;  // descriptive vendor family, never a dispatch key
    std::vector<std::string> supportedTargets;
    std::string buildDirectory;  // relative to the project, the backend's choice
    std::vector<ComponentDef> components;
};

/// Parses a toolchain definition from JSON text. On failure, says why in
/// outError.
///
/// Text, not a path, is the main form: a backend lives in a package whose DLL
/// is built into the project's runtime directory, far from its sources, so it
/// carries its definition inside itself.
DEKI_EDITOR_API bool ParseBuilderDefinition(const std::string& jsonText, BuilderDefinition& outDef,
                                            std::string& outError);

/// The same, from a file the backend locates itself.
DEKI_EDITOR_API bool LoadBuilderDefinition(const std::string& jsonPath, BuilderDefinition& outDef,
                                           std::string& outError);

}  // namespace DekiEditor
