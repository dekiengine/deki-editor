#pragma once

#include <deki-editor/build/FirmwareBuilderBase.h>
#include "ESPIDFToolchain.h"
#include <thread>

namespace DekiEditor
{

/**
 * @brief ESP-IDF firmware builder implementation
 *
 * Builds firmware using the Espressif ESP-IDF framework and idf.py tool.
 * Supports ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6, ESP32-H2 targets.
 */
class ESPIDFBuilder : public FirmwareBuilderBase
{
public:
    ESPIDFBuilder();
    ~ESPIDFBuilder() override;

    // Core operations — use RunOnBuildThread()
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

    // Toolchain — ESP-IDF specific
    bool IsToolchainInstalled() const override;
    std::string GetToolchainStatus() const override;
    // Build file generation
    bool GenerateBuildFiles(const std::string& projectPath,
                            const PlatformConfig& config,
                            const std::vector<std::string>& packageDefines) override;

    // Identity
    const char* GetName() const override { return "ESP-IDF"; }
    std::string GetFrameworkId() const override { return "espidf"; }
    std::vector<std::string> ValidatePlatform(const PlatformConfig& config) const override;
    std::vector<std::string> GetSupportedTargets() const override;
    std::string GetBuildDirectory(const std::string& projectPath) const override;

    // Platform editor UI
    std::unique_ptr<IPlatformEditorUI> CreateEditorUI(const PlatformConfig& config) const override;

    // ESP-IDF specific public methods
    std::string GetIDFPath() const;

private:
    ESPIDFToolchain m_Toolchain;

    // Build execution context helper
    ESPIDFExecContext MakeExecContext();

    // Internal worker functions
    void DoBuild(const std::string& projectPath, BuildOutputCallback outputCallback,
                 BuildProgressCallback progressCallback);
    void DoFlash(const std::string& projectPath, const std::string& port,
                 BuildOutputCallback outputCallback, BuildProgressCallback progressCallback);
    void DoClean(const std::string& projectPath, BuildOutputCallback outputCallback,
                 BuildProgressCallback progressCallback);
    void DoSetTarget(const std::string& projectPath, const std::string& target,
                     BuildOutputCallback outputCallback, BuildProgressCallback progressCallback);

    // Build file generation helpers
    bool GenerateRootCMakeLists(const std::string& espIdfPath, const PlatformConfig& config);
    bool GenerateMainCMakeLists(const std::string& mainPath, const std::string& projectPath,
                                const PlatformConfig& config,
                                const std::vector<std::string>& packageDefines);
    bool GenerateSdkConfigDefaults(const std::string& boardPath, const PlatformConfig& config);
    bool GeneratePartitionsCsv(const std::string& boardPath, const PlatformConfig& config);
};

}  // namespace DekiEditor
