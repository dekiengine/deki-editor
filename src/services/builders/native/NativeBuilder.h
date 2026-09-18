#pragma once

#include <deki-editor/build/FirmwareBuilderBase.h>
#include "NativeToolchain.h"
#include <map>
#include <string>
#include <thread>

namespace DekiEditor
{

/**
 * @brief Native (Desktop) firmware builder implementation
 *
 * Builds standalone desktop executables using CMake + system C++ compiler.
 * Generates a CMakeLists.txt that includes the Deki engine, SDL3, and game sources.
 * Delegates compiler detection and command execution to NativeToolchain.
 */
class NativeBuilder : public FirmwareBuilderBase
{
public:
    NativeBuilder();
    ~NativeBuilder() override;

    // Core operations
    void Build(const std::string& projectPath, BuildOutputCallback outputCallback = nullptr,
               BuildProgressCallback progressCallback = nullptr) override;
    void Flash(const std::string& projectPath, const std::string& port,
               BuildOutputCallback outputCallback = nullptr,
               BuildProgressCallback progressCallback = nullptr) override;
    void Clean(const std::string& projectPath, BuildOutputCallback outputCallback = nullptr,
               BuildProgressCallback progressCallback = nullptr) override;
    void SetTarget(const std::string& projectPath, const std::string& target,
                   BuildOutputCallback outputCallback = nullptr,
                   BuildProgressCallback progressCallback = nullptr) override;

    // Toolchain
    bool IsToolchainInstalled() const override;
    std::string GetToolchainStatus() const override;
    // Toolchain component API — reports system compilers
    std::vector<ToolchainComponent> GetToolchainComponents() const override;

    // Build file generation
    bool GenerateBuildFiles(const std::string& projectPath,
                            const PlatformConfig& config,
                            const std::vector<std::string>& packageDefines) override;

    // Identity
    const char* GetName() const override { return "Native (Desktop)"; }
    std::string GetFrameworkId() const override { return "native"; }
    std::vector<std::string> GetSupportedTargets() const override;
    std::string GetBuildDirectory(const std::string& projectPath) const override;

    // Platform editor UI
    std::unique_ptr<IPlatformEditorUI> CreateEditorUI(const PlatformConfig& config) const override;

private:
    NativeToolchain m_Toolchain;

    // A third-party library the active packages declared for this build,
    // resolved by PrepareNativeDependencies(): either a prebuilt archive
    // unpacked under <project>/generated/deps (cmakeDir set) or a source
    // build at release-<version> from git.
    struct ResolvedNativeDependency
    {
        std::string version;
        std::string git;
        std::string cmakeDir;    // absolute, holds <Name>Config.cmake; empty = build from source
        std::string includeDir;  // absolute public headers of the prebuilt
    };
    std::map<std::string, ResolvedNativeDependency> m_NativeDeps;

    static const char* HostPrebuiltKey();
    bool PrepareNativeDependencies(const std::string& projectPath, BuildOutputCallback outputCallback);

    // Internal worker functions
    void DoBuild(const std::string& projectPath, BuildOutputCallback outputCallback,
                 BuildProgressCallback progressCallback);
    void DoClean(const std::string& projectPath, BuildOutputCallback outputCallback,
                 BuildProgressCallback progressCallback);

    // Build file generation helpers
    bool DeployPartitions(const std::string& projectPath, const std::string& buildDir,
                          BuildOutputCallback outputCallback);
    bool GenerateCMakeLists(const std::string& projectPath, const std::string& buildDir, const PlatformConfig& config,
                            const std::vector<std::string>& packageDefines);
};

}  // namespace DekiEditor
