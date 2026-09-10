#pragma once

// What a project has said about stripping its device builds.
//
// Lives in settings/build.json beside the engine's own settings files. Two
// switches say whether unused things go, and per-feature overrides pin a
// feature on (for something the scan cannot see) or force it off.

#include <filesystem>
#include <map>
#include <string>

namespace DekiEditor
{

struct BuildStripSettings
{
    // Leave out a feature none of whose components any scene or source names.
    bool stripUnusedComponents = true;
    // Leave out a package all of whose component features are off and that
    // no kept package requires. Packages with no component features (drivers,
    // protocols) are never touched by this; they have nothing to scan for.
    bool stripUnusedPackages = false;

    // package id -> feature id -> "on" | "off". Absent means automatic.
    std::map<std::string, std::map<std::string, std::string>> featureOverrides;

    // "" when automatic.
    std::string Override(const std::string& packageId, const std::string& featureId) const;
    // "" clears the override.
    void SetOverride(const std::string& packageId, const std::string& featureId, const std::string& value);

    static std::filesystem::path FilePath(const std::string& projectPath);
    // Defaults when the file is missing or unreadable.
    static BuildStripSettings Load(const std::string& projectPath);
    bool Save(const std::string& projectPath) const;
};

}  // namespace DekiEditor
