#pragma once

#include <deki-editor/EditorExport.h>

#include <string>
#include <vector>
#include <map>

namespace DekiEditor
{

// ============================================================================
// Toolchain definition structs — parsed from JSON a build backend supplies
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
    std::string unix_;  // "unix" is not a keyword but avoid confusion
};

struct DEKI_EDITOR_API SubComponentDef
{
    std::string name;
    std::string version;
    std::string url;
    // Expected SHA-256 of the download, lowercase hex. Anything fetched here is
    // extracted and then executed by the build, so an unpinned entry means the
    // network and the mirror are both trusted. Empty is accepted but logged as
    // unverified rather than passing silently.
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

    // Detection paths per platform. Key: "windows", "unix", or "any"
    // Values may contain {installPath} and * wildcards
    std::map<std::string, std::string> detection;

    PostInstallDef postInstall;
    std::vector<SubComponentDef> subComponents;
    std::string extractedName;  // Name of extracted folder from zip (for rename)
    // Expected SHA-256 of this component's download, lowercase hex. See the
    // note on SubComponentDef::sha256. Components whose URL is resolved at run
    // time from a GitHub release cannot be pinned this way and stay empty.
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

/// Parse a toolchain definition from JSON text.
///
/// Text, not a path, is the primary form: a build backend lives in a package,
/// and the package DLL is built into the project's runtime directory, nowhere
/// near its own sources - so a backend carries its definition inside itself
/// rather than hoping a file sits beside the editor. It used to: the ESP-IDF
/// backend read resources/builders/espidf.json next to DekiEditor.exe, which
/// made the editor ship one framework's recipe after the backend had left.
///
/// A failure says why in outError. There is no silent "no toolchain" state.
DEKI_EDITOR_API bool ParseBuilderDefinition(const std::string& jsonText, BuilderDefinition& outDef,
                                            std::string& outError);

/// The same, from a file a backend locates itself.
DEKI_EDITOR_API bool LoadBuilderDefinition(const std::string& jsonPath, BuilderDefinition& outDef,
                                           std::string& outError);

}  // namespace DekiEditor
