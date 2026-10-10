#pragma once

// FirmwareBuildService: firmware build operations without any GUI.
//
// Owns the steps of a firmware build (load project -> find platform -> find
// builder -> prepare -> Build/Flash/Clean), so the Build panel, the CLI and
// MCP all run the same ones. An EditorApp is optional and only read: passing
// one lets the preparation export the boot payload and scan scenes for
// package defines. No ImGui state is touched, so everything here works
// headless.
//
// ITargetBuilder::Build/Flash/Clean run on a worker thread. The blocking entry
// points wait for that worker, so callers get a final result.

#include <string>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>

#include <deki-editor/build/TargetBuilder.h>  // ITargetBuilder, BuildOptions

namespace DekiEditor
{

class EditorApp;
struct PlatformConfig;

struct FirmwareOpResult
{
    bool success = false;
    std::string error;       // empty on success
    std::string platformId;  // platform the operation ran against
    std::string platformName;
};

/// Streamed build output. isError marks stderr / detected error lines.
using FirmwareLogCallback = std::function<void(const std::string& line, bool isError)>;

/// Called once when a started build or deploy ends: Completed, Failed (text is
/// the error) or Idle (cancelled; text is the status). On the build thread.
using FirmwareDoneCallback = std::function<void(BuildState state, const std::string& text)>;

class FirmwareBuildService
{
public:
    /// Preprocessor defines from the project's installed packages, like
    /// PackageManagerPanel::GetRequiredBuildDefines(): reads each
    /// <project>/project/packages/<pkg>/package.json for its buildDefine and
    /// each feature's buildDefine. Filesystem only, no GUI state.
    static std::vector<std::string> CollectPackageDefines(const std::string& projectPath,
                                                          const std::string& targetPlatformId = "");

    /// The platforms a project can use: built-ins, board packs, and the
    /// project's own project/platforms directory.
    /// Returns {"activePlatformId": "...", "platforms": [ {...}, ... ]}.
    static nlohmann::json ListPlatforms(const std::string& projectPath, std::string& outError);

    /// Toolchain component status for the platform's builder.
    /// Returns {"builder": "...", "installed": bool, "status": "...",
    ///          "components": [ {id, displayName, status, installedVersion, ...} ]}.
    static nlohmann::json ToolchainStatus(const std::string& projectPath, const std::string& platformId,
                                          std::string& outError);

    /// Everything between "a firmware build was asked for" and
    /// ITargetBuilder::Build: toolchain check, boot payload export, package
    /// defines, platform config, build options, build files.
    ///
    /// The only definition of preparing a firmware build; keep every build
    /// path going through it, or a path can miss a step (such as the boot
    /// payload: project_data.bin and the boot scene). The Build panel calls it
    /// and then drives the builder itself, for its own progress and cancel UI;
    /// Build/StartBuild below call it and drive the builder for you.
    ///
    /// `editor` may be null; then every installed package's define is used and
    /// no boot payload is written. A booted editor is the better input: its
    /// package panel scans the project's scenes and gives only the defines in
    /// use.
    static FirmwareOpResult PrepareBuild(const std::string& projectPath, const PlatformConfig& platform,
                                         ITargetBuilder& builder, EditorApp* editor, const BuildOptions& options,
                                         FirmwareLogCallback onLine);

    /// Writes the build files, then builds the firmware, and returns when done.
    /// An empty platformId means the project's activePlatformId. For the CLI,
    /// where blocking is what the caller wants.
    static FirmwareOpResult Build(const std::string& projectPath, const std::string& platformId,
                                  FirmwareLogCallback onLine = nullptr, EditorApp* editor = nullptr,
                                  const BuildOptions& options = {});

    /// Same preparation as Build(), but returns once the build thread is
    /// running. success==true means started, not finished; poll QueryStatus().
    /// Used by MCP, whose handlers run on the editor's frame loop and must not
    /// block for the minutes a firmware build takes.
    static FirmwareOpResult StartBuild(const std::string& projectPath, const std::string& platformId,
                                       FirmwareLogCallback onLine = nullptr, EditorApp* editor = nullptr,
                                       const BuildOptions& options = {}, FirmwareDoneCallback onDone = nullptr);

    /// Deploy() without waiting. success==true means started.
    static FirmwareOpResult StartDeploy(const std::string& projectPath, const std::string& platformId,
                                        const std::string& deployTargetId, FirmwareLogCallback onLine = nullptr,
                                        const BuildOptions& options = {}, FirmwareDoneCallback onDone = nullptr);

    /// InstallToolchainComponent() without waiting. success==true means
    /// started. QueryStatus() shows its progress as state "installing".
    static FirmwareOpResult StartInstallToolchainComponent(const std::string& projectPath,
                                                           const std::string& platformId,
                                                           const std::string& componentId,
                                                           FirmwareLogCallback onLine = nullptr);

    /// SetupToolchainComponent() without waiting. success==true means started.
    /// Runs the component's post-install step, such as the script that fetches
    /// the compilers of an SDK whose archive does not include them.
    static FirmwareOpResult StartSetupToolchainComponent(const std::string& projectPath, const std::string& platformId,
                                                         const std::string& componentId,
                                                         FirmwareLogCallback onLine = nullptr);

    /// Snapshot of the most recently started operation. Returns
    /// {"active": bool, "state": "...", "statusText": "...", "progress": f,
    ///  "error": "...", "platformId": "..."}.
    /// state is one of idle|building|deploying|installing|completed|failed.
    /// With outputLines, also "output": the operation's last lines (at most
    /// that many, of the last 500 kept; error lines start "error: "), and
    /// "outputLineCount", how many it has printed in all.
    static nlohmann::json QueryStatus(size_t outputLines = 0);

    /// Asks the running operation to cancel. Does nothing when idle.
    static bool CancelActive();

private:
    static nlohmann::json QueryState();

public:

    /// Deploys the last build, whatever that means for the platform's backend:
    /// flash it, run it, install it. deployTargetId is one of the backend's
    /// ITargetBuilder::EnumerateDeployTargets ids; empty lets it choose.
    /// Refused when the backend has no deploy step. With options.simulate it
    /// runs the simulated build in the backend's simulator.
    static FirmwareOpResult Deploy(const std::string& projectPath, const std::string& platformId,
                                   const std::string& deployTargetId, FirmwareLogCallback onLine = nullptr,
                                   const BuildOptions& options = {});

    /// Cleans the builder's build directory and returns when done.
    static FirmwareOpResult Clean(const std::string& projectPath, const std::string& platformId,
                                  FirmwareLogCallback onLine = nullptr);

    /// Downloads and extracts a toolchain component, then runs its postInstall
    /// step when it has one. Returns when done.
    static FirmwareOpResult InstallToolchainComponent(const std::string& projectPath, const std::string& platformId,
                                                      const std::string& componentId,
                                                      FirmwareLogCallback onLine = nullptr);
};

}  // namespace DekiEditor
