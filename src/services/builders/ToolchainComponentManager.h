#pragma once

#include <deki-editor/build/BuilderDefinition.h>
#include <deki-editor/build/TargetBuilder.h>
#include "DownloadTypes.h"
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>

namespace DekiEditor
{

/**
 * @brief Runtime toolchain component manager driven by BuilderDefinition JSON
 *
 * Evaluates detection rules, downloads/installs components, checks for updates.
 * Each ITargetBuilder owns one of these, initialized from its builder JSON.
 */
class ToolchainComponentManager
{
public:
    ToolchainComponentManager();
    ~ToolchainComponentManager();

    /// Initialize from a parsed builder definition
    void Initialize(const BuilderDefinition& def);

    /// Whether this manager has been initialized with a valid definition
    bool IsInitialized() const { return !m_Definition.id.empty(); }

    /// Get the builder definition
    const BuilderDefinition& GetDefinition() const { return m_Definition; }

    /// Evaluate detection rules and produce ToolchainComponent list for UI
    std::vector<ToolchainComponent> GetComponents() const;

    /// Download + extract a component by id (async)
    void InstallComponent(const std::string& componentId, BuildProgressCallback cb = nullptr);

    /// Run the postInstall script for an already-installed component (async)
    void SetupComponent(const std::string& componentId, BuildProgressCallback cb = nullptr);

    /// Check for newer versions via GitHub API (async)
    void CheckForUpdates(std::function<void()> onComplete = nullptr);

    /// Whether any operation is in progress
    bool IsBusy() const { return m_IsBusy; }

private:
    BuilderDefinition m_Definition;
    std::string m_ToolchainsDir;  // ~/.deki-editor/toolchains/

    // Version cache per component
    mutable std::mutex m_VersionMutex;
    std::map<std::string, ToolchainVersionInfo> m_VersionCache;

    // Async state
    std::thread m_WorkerThread;
    std::atomic<bool> m_IsBusy{ false };

    // Progress tracking
    mutable std::mutex m_ProgressMutex;
    BuildProgress m_Progress;

    // Detection
    bool EvaluateDetection(const ComponentDef& comp) const;
    std::string ResolvePath(const std::string& tmpl, const ComponentDef& comp) const;
    bool MatchesGlob(const std::string& resolvedPath) const;

    // Install helpers
    void DoInstallComponent(const ComponentDef& comp, BuildProgressCallback cb);
    void DoInstallSubComponents(const ComponentDef& comp, BuildProgressCallback cb);
    void DoSetupComponent(const ComponentDef& comp, BuildProgressCallback cb);

    // Utility
    std::string GetToolchainsDir() const;
};

}  // namespace DekiEditor
