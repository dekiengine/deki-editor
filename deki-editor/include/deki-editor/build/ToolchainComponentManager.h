#pragma once

#include <deki-editor/EditorExport.h>

#include <deki-editor/build/BuilderDefinition.h>
#include <deki-editor/build/TargetBuilder.h>
#include <deki-editor/build/DownloadTypes.h>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>

namespace DekiEditor
{

/// Manages a builder's toolchain components as its BuilderDefinition describes
/// them: detects them, downloads and installs them, and checks for updates.
/// Each ITargetBuilder owns one, initialised from its builder JSON.
class DEKI_EDITOR_API ToolchainComponentManager
{
public:
    ToolchainComponentManager();
    ~ToolchainComponentManager();

    void Initialize(const BuilderDefinition& def);

    /// True once initialised with a valid definition.
    bool IsInitialized() const { return !m_Definition.id.empty(); }

    const BuilderDefinition& GetDefinition() const { return m_Definition; }

    /// Runs the detection rules and lists the components for the UI.
    std::vector<ToolchainComponent> GetComponents() const;

    /// Downloads and extracts a component, asynchronously.
    void InstallComponent(const std::string& componentId, BuildProgressCallback cb = nullptr);

    /// Runs the postInstall script of an installed component, asynchronously.
    void SetupComponent(const std::string& componentId, BuildProgressCallback cb = nullptr);

    /// Checks the GitHub API for newer versions, asynchronously.
    void CheckForUpdates(std::function<void()> onComplete = nullptr);

    /// Whether an operation is in progress.
    bool IsBusy() const { return m_IsBusy; }

    /// The PATH entries (ComponentDef::pathEntries) of every installed
    /// component, absolute and in definition order, for this platform.
    std::vector<std::string> InstalledPathEntries() const;

private:
    BuilderDefinition m_Definition;
    std::string m_ToolchainsDir;  // ~/.deki-editor/toolchains/

    // Latest known version per component
    mutable std::mutex m_VersionMutex;
    std::map<std::string, ToolchainVersionInfo> m_VersionCache;

    std::thread m_WorkerThread;
    std::atomic<bool> m_IsBusy{ false };

    mutable std::mutex m_ProgressMutex;
    BuildProgress m_Progress;

    bool EvaluateDetection(const ComponentDef& comp) const;
    std::string ResolvePath(const std::string& tmpl, const ComponentDef& comp) const;
    bool MatchesGlob(const std::string& resolvedPath) const;

    void DoInstallComponent(const ComponentDef& comp, BuildProgressCallback cb);
    void DoInstallSubComponents(const ComponentDef& comp, BuildProgressCallback cb);
    void DoSetupComponent(const ComponentDef& comp, BuildProgressCallback cb);

    std::string GetToolchainsDir() const;
};

}  // namespace DekiEditor
