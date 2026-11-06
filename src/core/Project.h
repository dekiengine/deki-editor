#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <unordered_set>
#include <nlohmann/json_fwd.hpp>

// Settings types live in deki-engine/src/settings/. Included here because most
// editor code that reads settings already includes this header.
#include <deki/settings/UnitProjectSettings.h>
#include <deki/settings/CoreProjectSettings.h>
#include <deki-editor/SceneView.h>

// CompareVersions, for the ProjectNeedsNewerEditor rule below.
#include <deki-editor/VersionCompare.h>

namespace DekiEditor
{

/// The project's settings sections. nullptr until the registry is filled (the
/// engine is not initialized or the settings package is not loaded). Hot paths
/// read the cached copy in Deki::EngineSettings::Global() instead.
Deki::UnitProjectSettings* GetUnitSettings();
Deki::CoreProjectSettings* GetCoreSettings();

/// Copies the Units settings into Deki::EngineSettings::Global(). Call after
/// loading settings and after any edit to them, so cameras and the inspector
/// see the new values.
void ApplySettingsToGlobalCache();

class Project;

/// The screen size of the project's active platform, in pixels. Renders with
/// no Play view to ask (--render-scenes, scene thumbnails) use it. 320 x 240
/// when there is no active platform or it has no screen.
void ActivePlatformScreen(const Project* project, int& width, int& height);

/// True when a project saved by editor `lastSavedBy` must not be opened by
/// editor `thisEditor`, because opening it would write its scenes back in the
/// older format and drop whatever the newer one stored.
///
/// `lastSavedBy` is deki.json's "lastSavedBy". An empty one means the project
/// predates the field, so it is older and always allowed. Must not read
/// "engineVersion" instead: it is set once when the project is created, and
/// can hold a literal "1.0.0" that outranks every real version.
///
/// Inline so tests can use it without linking Project.cpp, which pulls in the
/// whole editor.
inline bool ProjectNeedsNewerEditor(const std::string& lastSavedBy, const std::string& thisEditor)
{
    if (lastSavedBy.empty())
    {
        return false;
    }
    return DekiEditor::CompareVersions(lastSavedBy, thisEditor) > 0;
}

/// `name` as a project's name, without the spaces around it, in `cleaned`.
/// False, with `why`, for a name the editor's lists and a build's app label
/// cannot show: empty, longer than 64 characters, or holding a control
/// character (a line break, a tab).
///
/// Inline so tests can use it without linking Project.cpp.
inline bool CleanProjectName(const std::string& name, std::string& cleaned, std::string& why)
{
    const size_t first = name.find_first_not_of(" ");
    const size_t last = name.find_last_not_of(" ");
    cleaned = first == std::string::npos ? std::string() : name.substr(first, last - first + 1);
    if (cleaned.empty())
    {
        why = "A project needs a name.";
        return false;
    }
    if (cleaned.size() > 64)
    {
        why = "A project's name can be at most 64 characters.";
        return false;
    }
    for (unsigned char c : cleaned)
    {
        if (c < 0x20 || c == 0x7F)
        {
            why = "A project's name cannot hold a line break, a tab or another control character.";
            return false;
        }
    }
    return true;
}

/// The project file (deki.json): paths and asset/build wiring.
///
/// Typed settings (display, profiling, units, rendering, ...) live in
/// `Deki::SettingsRegistry`. Packages declare them with
/// `DEKI_PROJECT_SETTINGS_SECTION`, and the Project Settings panel draws them
/// through reflection. Save and Load store them as files under settings/.
///
/// This class holds the asset and build references that do not fit the
/// registry: startup scene path, render pipeline GUID, platform id and the
/// force-include GUIDs.
class Project
{
public:
    Project() = default;

    bool Load(const std::string& path);

    /// Why the last Load() returned false, when the reason is worth showing
    /// (not just a missing file). Empty otherwise.
    const std::string& GetLoadError() const { return m_LoadError; }
    bool Save();

    /// Settings are stored under settings/: package sections under
    /// settings/packages/<package>/, engine-core ones in core.json. This keeps
    /// deki.json from growing with every package and keeps unrelated edits out
    /// of one file.
    /// Returns the section names written. The caller drops only those from the
    /// legacy block, so a section whose package is not loaded survives.
    std::vector<std::string> SaveSettingsFiles(const std::string& root) const;
    void LoadSettingsFiles(const std::string& root);

    const std::string& GetName() const { return m_Name; }
    /// Renames the project and saves it: the name the editor and the builds
    /// show (a Miyoo app is labelled and named by it). The folder keeps its
    /// name: the editor finds a project by its folder, so nothing that points
    /// at it breaks. False, with `why`, for a name CleanProjectName refuses,
    /// or when deki.json cannot be written.
    bool SetName(const std::string& name, std::string& why);
    const std::string& GetPath() const { return m_Path; }
    const std::string& GetAssetsPath() const { return m_AssetsPath; }
    const std::string& GetSrcPath() const { return m_SrcPath; }
    const std::string& GetBuildPath() const { return m_BuildPath; }
    const std::string& GetCachePath() const { return m_CachePath; }

    // The scene loaded when the game starts.
    const std::string& GetStartupScene() const { return m_StartupScene; }
    void SetStartupScene(const std::string& scene)
    {
        // Asset paths use forward slashes everywhere else (asset table,
        // runtime lookup), so the startup scene must too.
        m_StartupScene = scene;
        for (char& c : m_StartupScene)
        {
            if (c == '\\')
            {
                c = '/';
            }
        }
        MarkDirty();
    }

    // GUID of the project's .rpipeline asset.
    const std::string& GetRenderPipelineGuid() const { return m_RenderPipelineGuid; }
    void SetRenderPipelineGuid(const std::string& guid)
    {
        m_RenderPipelineGuid = guid;
        MarkDirty();
    }

    // The selected build target platform.
    const std::string& GetActivePlatformId() const { return m_ActivePlatformId; }
    void SetActivePlatformId(const std::string& id)
    {
        m_ActivePlatformId = id;
        MarkDirty();
    }

    // Assets always deployed, even when no scene references them.
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

    // Marks the project as needing a save.
    void MarkDirty() { m_IsDirty = true; }
    bool IsDirty() const { return m_IsDirty; }

private:
    std::string m_Name;
    std::string m_Path;          // Project root directory
    std::string m_LoadError;     // see GetLoadError()
    std::string m_AssetsPath;    // project/assets/
    std::string m_SrcPath;       // project/src/
    std::string m_BuildPath;     // build/
    std::string m_CachePath;     // cache/
    std::string m_StartupScene;  // relative to assets
    std::string m_RenderPipelineGuid;
    std::string m_ActivePlatformId;
    std::unordered_set<std::string> m_ForceIncludeGuids;
    bool m_IsDirty = false;
};

}  // namespace DekiEditor
