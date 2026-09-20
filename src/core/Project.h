#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <unordered_set>

// Settings types live in deki-engine/src/settings/. Including them here
// means call sites that read settings fields don't have to add the per-file
// includes - most editor code that touches settings already includes this
// header.
#include <deki/settings/DisplayProjectSettings.h>
#include <deki/settings/UnitProjectSettings.h>
#include <deki/settings/CoreProjectSettings.h>

// CompareVersions, for the ProjectNeedsNewerEditor rule below.
#include <deki-editor/VersionCompare.h>

namespace DekiEditor
{

// Free-function accessors. Return nullptr if the registry isn't populated yet
// (engine not initialized, or settings package not yet loaded). Hot-path
// readers that want a stable default should fall back: e.g.
//   auto* d = DekiEditor::GetDisplaySettings();
//   int w = d ? d->targetWidth : 320;
Deki::DisplayProjectSettings* GetDisplaySettings();
Deki::UnitProjectSettings* GetUnitSettings();
Deki::CoreProjectSettings* GetCoreSettings();

/// True when a project saved by editor `lastSavedBy` must not be opened by
/// editor `thisEditor`, because opening it would write its scenes back in the
/// older format and drop whatever the newer one stored.
///
/// `lastSavedBy` is deki.json's "lastSavedBy". An empty one means the project
/// predates the field, which is always older, so always allowed. It is a
/// separate key from the older "engineVersion" on purpose: that one was
/// stamped once at scaffold time and never updated, and its missing-macro
/// fallback wrote a literal "1.0.0" that outranks every real version, so no
/// threshold can tell a stale value there from a true one.
///
/// Inline so the rule can be tested without linking Project.cpp, which pulls
/// in the whole editor.
inline bool ProjectNeedsNewerEditor(const std::string& lastSavedBy,
                                    const std::string& thisEditor)
{
    if (lastSavedBy.empty())
        return false;
    return DekiEditor::CompareVersions(lastSavedBy, thisEditor) > 0;
}

/**
 * @brief Project file (deki.json) - paths and asset/build wiring.
 *
 * Numeric/typed settings (display, profiling, units, rendering, ...) live in
 * `Deki::SettingsRegistry`. Packages declare their settings types via
 * `DEKI_PROJECT_SETTINGS_SECTION` and the editor's Project Settings panel
 * renders them via reflection. Persistence round-trips through the
 * `"packageSettings"` block in deki.json - see Project::Save/Load.
 *
 * The fields that stay here are the asset-and-build references that don't
 * fit the generic registry model (startup scene path, render-pipeline GUID,
 * platform id, force-include / flash GUID sets).
 */
class Project
{
public:
    Project() = default;

    bool Load(const std::string& path);

    /// Why the last Load() returned false, when it failed for a reason worth
    /// showing rather than a missing file. Empty otherwise.
    const std::string& GetLoadError() const { return m_LoadError; }
    bool Save();

    // Settings are stored under settings/, one file per owner: package-owned
    // sections under settings/packages/<package>/, engine-core ones in core.json
    // (framebuffer.json for the device contract). Splitting them keeps deki.json
    // from growing with every installed package and stops unrelated settings edits
    // colliding in one file.
    // Returns the section names actually written, so the caller only drops those
    // from the legacy block and a section whose package is not loaded survives.
    std::vector<std::string> SaveSettingsFiles(const std::string& root) const;
    void LoadSettingsFiles(const std::string& root);

    const std::string& GetName() const { return m_Name; }
    const std::string& GetPath() const { return m_Path; }
    const std::string& GetAssetsPath() const { return m_AssetsPath; }
    const std::string& GetSrcPath() const { return m_SrcPath; }
    const std::string& GetBuildPath() const { return m_BuildPath; }
    const std::string& GetCachePath() const { return m_CachePath; }

    // Startup scene - the scene loaded when the game starts
    const std::string& GetStartupScene() const { return m_StartupScene; }
    void SetStartupScene(const std::string& scene)
    {
        // Asset paths are forward-slash keys everywhere else (asset table,
        // runtime lookup); keep the startup scene spelled the same way.
        m_StartupScene = scene;
        for (char& c : m_StartupScene)
            if (c == '\\') c = '/';
        MarkDirty();
    }

    // Render pipeline - GUID of the .rpipeline asset for this project
    const std::string& GetRenderPipelineGuid() const { return m_RenderPipelineGuid; }
    void SetRenderPipelineGuid(const std::string& guid)
    {
        m_RenderPipelineGuid = guid;
        MarkDirty();
    }

    // Active platform - the selected build target platform
    const std::string& GetActivePlatformId() const { return m_ActivePlatformId; }
    void SetActivePlatformId(const std::string& id)
    {
        m_ActivePlatformId = id;
        MarkDirty();
    }

    // Force-include GUIDs - assets to always deploy even if not referenced by scenes
    const std::unordered_set<std::string>& GetForceIncludeGuids() const { return m_ForceIncludeGuids; }
    void AddForceIncludeGuid(const std::string& guid)
    {
        m_ForceIncludeGuids.insert(guid);
        MarkDirty();
    }
    void RemoveForceIncludeGuid(const std::string& guid)
    {
        m_ForceIncludeGuids.erase(guid);
        MarkDirty();
    }
    bool IsForceIncluded(const std::string& guid) const { return m_ForceIncludeGuids.count(guid) > 0; }

    // Flash-targeted GUIDs - assets to deploy to flash instead of SD card
    const std::unordered_set<std::string>& GetFlashGuids() const { return m_FlashGuids; }
    void AddFlashGuid(const std::string& guid)
    {
        m_FlashGuids.insert(guid);
        MarkDirty();
    }
    void RemoveFlashGuid(const std::string& guid)
    {
        m_FlashGuids.erase(guid);
        MarkDirty();
    }
    bool IsFlashTargeted(const std::string& guid) const { return m_FlashGuids.count(guid) > 0; }

    // Mark project as modified (needs save)
    void MarkDirty() { m_IsDirty = true; }
    bool IsDirty() const { return m_IsDirty; }

private:
    std::string m_Name;
    std::string m_Path;                                   // Project root directory
    std::string m_LoadError;                              // see GetLoadError()
    std::string m_AssetsPath;                             // project/assets/
    std::string m_SrcPath;                                // project/src/
    std::string m_BuildPath;                              // build/
    std::string m_CachePath;                              // cache/
    std::string m_StructureVersion;                       // deki.json "structure"; gates layout migrations
    std::string m_StartupScene;                           // Path to startup scene (relative to assets)
    std::string m_RenderPipelineGuid;                     // GUID of .rpipeline asset
    std::string m_ActivePlatformId;                       // Selected build target platform
    std::unordered_set<std::string> m_ForceIncludeGuids;  // Assets to always deploy
    std::unordered_set<std::string> m_FlashGuids;         // Assets targeted for flash
    bool m_IsDirty = false;
};

}  // namespace DekiEditor
