#pragma once

#include <deki-editor/EditorExport.h>

// A project's choices for stripping its device builds, kept in
// settings/build.json beside the engine's own settings files. Two switches
// say whether unused things go; per-feature overrides pin a feature on (for
// something the scan cannot see) or force it off.

#include <filesystem>
#include <map>
#include <string>

namespace DekiEditor
{

struct DEKI_EDITOR_API BuildStripSettings
{
    // Leave out a feature when no scene or source names any of its components.
    bool stripUnusedComponents = true;
    // Leave out a package when no scene or source names any of its components
    // and no kept package requires it. Packages without component features
    // (drivers, protocols) are always kept: there is nothing to scan for.
    bool stripUnusedPackages = true;

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
