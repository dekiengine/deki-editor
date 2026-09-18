#pragma once

#include <deki-editor/EditorExport.h>

#include <deki-editor/build/DependencyInfo.h>

#include <deki-editor/build/TargetBuilder.h>
#include <deki-editor/build/ToolchainComponentManager.h>
#include <deki-editor/build/PlatformConfig.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>

namespace DekiEditor
{

class DEKI_EDITOR_API FirmwareBuilderBase : public ITargetBuilder
{
public:
    ~FirmwareBuilderBase() override = default;

    void Cancel() override;

    // State queries
    bool IsBuilding() const override { return m_IsBuilding; }
    BuildState GetState() const override;
    BuildProgress GetProgress() const override;

    // Configuration
    void SetBuildOptions(const BuildOptions& options) override { m_BuildOptions = options; }
    const BuildOptions& GetBuildOptions() const override { return m_BuildOptions; }
    void SetPlatformConfig(const PlatformConfig& config) override;
    void ClearPlatformConfig() override;
    void SetPackageDefines(const std::vector<std::string>& defines) override { m_PackageDefines = defines; }

    // Engine path
    std::string GetEnginePath(const std::string& projectPath) const override;

    // Package compatibility (reads dependencies from metadata files)
    std::string GetPlatformKey() const override;
    bool IsPackageCompatible(const std::string& metadataPath) const override;
    std::vector<std::string> GetPackageSupportedPlatforms(const std::string& metadataPath) const override;

    // Toolchain component API (delegates to m_ToolchainMgr)
    std::vector<ToolchainComponent> GetToolchainComponents() const override;
    void InstallToolchainComponent(const std::string& componentId,
                                   BuildProgressCallback cb = nullptr) override;
    void SetupToolchainComponent(const std::string& componentId,
                                 BuildProgressCallback cb = nullptr) override;
    void CheckForToolchainUpdates(std::function<void()> onComplete = nullptr) override;
    bool IsToolchainBusy() const override;

protected:
    // Thread management helpers — subclasses call from their overrides
    bool RunOnBuildThread(std::function<void()> work);
    // Progress management
    void SetProgress(BuildState state, const std::string& text, float progress);
    void SetError(const std::string& error);

    // Package dep resolution
    std::vector<DependencyInfo> ReadEnginePackageDeps(const std::string& enginePath,
                                                      const std::string& projectPath,
                                                      const std::string& platformKey) const;

    // Shared state
    std::thread m_BuildThread;
    std::atomic<bool> m_IsBuilding{ false };
    std::atomic<bool> m_CancelRequested{ false };

    mutable std::mutex m_ProgressMutex;
    BuildProgress m_Progress;
    BuildOptions m_BuildOptions;

    // Platform config
    bool m_HasPlatformConfig = false;
    PlatformConfig m_PlatformConfig;

    // Package defines
    std::vector<std::string> m_PackageDefines;

    // Data-driven toolchain management
    ToolchainComponentManager m_ToolchainMgr;
};

}  // namespace DekiEditor
