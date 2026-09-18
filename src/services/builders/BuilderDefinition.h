#pragma once

#include <string>
#include <vector>
#include <map>

namespace DekiEditor
{

enum class MCUFamily;

// ============================================================================
// Builder Definition structs — parsed from resources/builders/*.json
// ============================================================================

struct VersionCheckDef
{
    std::string type;          // "github_release" or "static"
    std::string repo;          // For github_release: "espressif/esp-idf"
    std::string assetPattern;  // For github_release: asset filename pattern
    std::string version;       // For static: fixed version string
};

struct FallbackDef
{
    std::string version;
    std::string url;
};

struct PostInstallDef
{
    std::string windows;
    std::string unix_;  // "unix" is not a keyword but avoid confusion
};

struct SubComponentDef
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

struct ComponentDef
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

struct BuilderDefinition
{
    std::string id;      // "espidf"
    std::string name;    // "ESP-IDF"
    std::string family;  // "espressif" — maps to MCUFamily
    std::vector<std::string> supportedTargets;
    std::string buildDirectory;  // e.g., "builders/esp-idf"
    std::vector<ComponentDef> components;
};

/// Parse a single builder JSON file into a BuilderDefinition
bool LoadBuilderDefinition(const std::string& jsonPath, BuilderDefinition& outDef);

/// Load all builder definitions from a directory
std::vector<BuilderDefinition> LoadAllBuilderDefinitions(const std::string& directory);

/// Convert family string ("espressif", "rp2040", etc.) to MCUFamily enum
std::string ParseMCUFamily(const std::string& familyStr);

}  // namespace DekiEditor
