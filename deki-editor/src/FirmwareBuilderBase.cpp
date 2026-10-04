#include <deki-editor/build/FirmwareBuilderBase.h>
#include <deki-editor/Paths.h>
#include <deki-editor/EditorSettings.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <set>
#include <map>
#include <deki-editor/VersionCompare.h>

namespace fs = std::filesystem;

namespace DekiEditor
{

// ============================================================================
// State & config
// ============================================================================

void FirmwareBuilderBase::Cancel()
{
    m_CancelRequested = true;
}

BuildState FirmwareBuilderBase::GetState() const
{
    std::lock_guard<std::mutex> lock(m_ProgressMutex);
    return m_Progress.state;
}

BuildProgress FirmwareBuilderBase::GetProgress() const
{
    std::lock_guard<std::mutex> lock(m_ProgressMutex);
    return m_Progress;
}

void FirmwareBuilderBase::SetPlatformConfig(const PlatformConfig& config)
{
    m_HasPlatformConfig = true;
    m_PlatformConfig = config;
}

void FirmwareBuilderBase::ClearPlatformConfig()
{
    m_HasPlatformConfig = false;
    m_PlatformConfig = PlatformConfig{};
}

std::string FirmwareBuilderBase::GetEnginePath(const std::string& projectPath) const
{
    return EditorSettings::GetEnginePath();
}

// ============================================================================
// Package compatibility
// ============================================================================

std::string FirmwareBuilderBase::GetPlatformKey() const
{
    return m_PlatformConfig.framework;
}

bool FirmwareBuilderBase::IsPackageCompatible(const std::string& metadataPath) const
{
    if (!fs::exists(metadataPath))
    {
        return true;  // no metadata: unknown, so assume compatible
    }

    try
    {
        std::ifstream f(metadataPath);
        if (!f.is_open())
        {
            return true;
        }

        nlohmann::json j;
        f >> j;

        if (!j.contains("dependencies") || !j["dependencies"].is_object())
        {
            return true;  // no dependencies: pure code, works everywhere
        }

        const auto& pd = j["dependencies"];
        std::string key = GetPlatformKey();
        return pd.contains(key) || pd.contains("default");
    }
    catch (...)
    {
        return true;
    }
}

std::vector<std::string> FirmwareBuilderBase::GetPackageSupportedPlatforms(const std::string& metadataPath) const
{
    if (!fs::exists(metadataPath))
    {
        return {};  // no file: unknown
    }

    try
    {
        std::ifstream f(metadataPath);
        if (!f.is_open())
        {
            return {};
        }

        nlohmann::json j;
        f >> j;

        if (!j.contains("dependencies") || !j["dependencies"].is_object())
        {
            return { "all" };  // no dependencies: pure code
        }

        std::vector<std::string> platforms;
        for (auto& [key, val] : j["dependencies"].items())
        {
            platforms.push_back(key == "default" ? "all" : key);
        }
        return platforms.empty() ? std::vector<std::string>{ "all" } : platforms;
    }
    catch (...)
    {
        return {};
    }
}

// ============================================================================
// Progress management
// ============================================================================

void FirmwareBuilderBase::SetProgress(BuildState state, const std::string& text, float progress)
{
    std::lock_guard<std::mutex> lock(m_ProgressMutex);
    m_Progress.state = state;
    m_Progress.statusText = text;
    m_Progress.progress = progress;
    m_Progress.error.clear();
}

void FirmwareBuilderBase::SetError(const std::string& error)
{
    std::lock_guard<std::mutex> lock(m_ProgressMutex);
    m_Progress.state = BuildState::Failed;
    m_Progress.statusText = "Error";
    m_Progress.error = error;
}

// ============================================================================
// Thread management helpers
// ============================================================================

bool FirmwareBuilderBase::RunOnBuildThread(std::function<void()> work)
{
    if (m_IsBuilding)
    {
        return false;
    }

    if (m_BuildThread.joinable())
    {
        m_BuildThread.join();
    }

    m_IsBuilding = true;
    m_CancelRequested = false;

    m_BuildThread = std::thread(
        [this, work = std::move(work)]()
        {
            work();
            m_IsBuilding = false;
        });

    return true;
}

// ============================================================================
// Toolchain component API (delegates to ToolchainComponentManager)
// ============================================================================

std::vector<ToolchainComponent> FirmwareBuilderBase::GetToolchainComponents() const
{
    if (m_ToolchainMgr.IsInitialized())
    {
        return m_ToolchainMgr.GetComponents();
    }
    return {};
}

void FirmwareBuilderBase::InstallToolchainComponent(const std::string& componentId, BuildProgressCallback cb)
{
    m_ToolchainMgr.InstallComponent(componentId, cb);
}

void FirmwareBuilderBase::SetupToolchainComponent(const std::string& componentId, BuildProgressCallback cb)
{
    m_ToolchainMgr.SetupComponent(componentId, cb);
}

void FirmwareBuilderBase::CheckForToolchainUpdates(std::function<void()> onComplete)
{
    m_ToolchainMgr.CheckForUpdates(onComplete);
}

bool FirmwareBuilderBase::IsToolchainBusy() const
{
    return m_ToolchainMgr.IsBusy();
}

// ============================================================================
// Package dep resolution
// ============================================================================

// Reads a platform-keyed field from a package.json object: platformKey first,
// then "default", else an empty array.
static nlohmann::json ResolvePlatformValue(const nlohmann::json& manifest, const std::string& field,
                                           const std::string& platformKey)
{
    if (!manifest.contains(field) || !manifest[field].is_object())
    {
        return nlohmann::json::array();
    }

    const auto& obj = manifest[field];
    if (obj.contains(platformKey))
    {
        return obj[platformKey];
    }
    if (obj.contains("default"))
    {
        return obj["default"];
    }
    return nlohmann::json::array();
}

std::vector<DependencyInfo> FirmwareBuilderBase::ReadEnginePackageDeps(const std::string& enginePath,
                                                                       const std::string& projectPath,
                                                                       const std::string& platformKey) const
{
    std::vector<DependencyInfo> result;

    // 1. Load the package.json of every project package.
    std::map<std::string, nlohmann::json> manifests;
    fs::path packagesDir = fs::path(GetPackagesDirectory(projectPath));
    if (!fs::exists(packagesDir))
    {
        return result;
    }

    try
    {
        for (const auto& entry : fs::directory_iterator(packagesDir))
        {
            if (!entry.is_directory())
            {
                continue;
            }
            fs::path manifestPath = entry.path() / "package.json";
            if (!fs::exists(manifestPath))
            {
                continue;
            }

            std::ifstream file(manifestPath);
            if (!file.is_open())
            {
                continue;
            }

            nlohmann::json j;
            file >> j;

            std::string id = j.value("id", "");
            if (!id.empty())
            {
                manifests[id] = j;
            }
        }
    }
    catch (const std::exception&)
    {
        return result;
    }

    // 2. Enabled packages, from the project's packages.json.
    std::set<std::string> enabledPackages;
    for (const auto& kv : manifests)
    {
        enabledPackages.insert(kv.first);  // all enabled by default
    }

    fs::path packagesJsonPath = fs::path(projectPath) / "packages.json";
    if (fs::exists(packagesJsonPath))
    {
        try
        {
            std::ifstream file(packagesJsonPath);
            nlohmann::json j;
            file >> j;

            if (j.contains("packages") && j["packages"].is_object())
            {
                for (const auto& item : j["packages"].items())
                {
                    if (item.value().contains("enabled") && !item.value()["enabled"].get<bool>())
                    {
                        enabledPackages.erase(item.key());
                    }
                }
            }
        }
        catch (const std::exception&)
        {
        }
    }

    // 3. Resolve deki-* package dependencies transitively (loop until nothing
    //    changes).
    bool changed = true;
    while (changed)
    {
        changed = false;
        std::set<std::string> toAdd;
        for (const auto& id : enabledPackages)
        {
            auto it = manifests.find(id);
            if (it == manifests.end())
            {
                continue;
            }

            auto deps = ResolvePlatformValue(it->second, "dependencies", platformKey);
            for (const auto& dep : deps)
            {
                // Package dependencies are entries whose "name" starts with "deki-".
                if (!dep.is_object())
                {
                    continue;
                }
                std::string name = dep.value("name", "");
                if (name.rfind("deki-", 0) != 0)
                {
                    continue;
                }
                if (enabledPackages.find(name) == enabledPackages.end())
                {
                    toAdd.insert(name);
                }
            }
        }
        for (const auto& id : toAdd)
        {
            enabledPackages.insert(id);
            changed = true;
        }
    }

    // 4. External dependencies (not deki-*), without duplicates: the highest
    //    version wins.
    for (const auto& id : enabledPackages)
    {
        auto it = manifests.find(id);
        if (it == manifests.end())
        {
            continue;
        }

        auto deps = ResolvePlatformValue(it->second, "dependencies", platformKey);
        for (const auto& depObj : deps)
        {
            if (!depObj.is_object())
            {
                continue;
            }

            DependencyInfo dep;
            dep.name = depObj.value("name", "");
            // deki-* entries were handled in step 3.
            if (dep.name.rfind("deki-", 0) == 0)
            {
                continue;
            }
            dep.version = depObj.value("version", "");
            dep.gitUrl = depObj.value("git", "");
            dep.source = dep.gitUrl.empty() ? PackageSourceType::FrameworkRegistry : PackageSourceType::GitHub;

            if (dep.name.empty() || dep.version.empty())
            {
                continue;
            }

            bool replaced = false;
            for (auto& existing : result)
            {
                if (existing.name == dep.name)
                {
                    if (DekiEditor::CompareVersions(dep.version, existing.version) > 0)
                    {
                        existing.version = dep.version;
                        existing.gitUrl = dep.gitUrl;
                        existing.source = dep.source;
                    }
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
            {
                result.push_back(dep);
            }
        }
    }

    return result;
}

}  // namespace DekiEditor
