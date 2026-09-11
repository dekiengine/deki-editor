#pragma once

// FirmwareBuildService — GUI-free firmware build operations.
//
// Owns the sequence a firmware build goes through (load project -> resolve
// platform -> resolve builder -> prepare -> Build/Flash/Clean) so the Build
// panel, the CLI and MCP all run the same one. An EditorApp is optional and
// only ever an input: passing one lets the preparation export the SPIFFS
// payload and scan scenes for package defines. No ImGui state is touched
// either way, so every entry point here works headless.
//
// IFirmwareBuilder::Build/Flash/Clean run on a worker thread. Every Run* entry
// point here blocks until that worker finishes, so callers get a final result.

#include <string>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>

#include "services/builders/IFirmwareBuilder.h"  // IFirmwareBuilder, BuildOptions

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

class FirmwareBuildService
{
public:
    /// Preprocessor defines contributed by installed project packages.
    /// Mirrors PackageManagerPanel::GetRequiredBuildDefines() — reads
    /// <project>/project/packages/<pkg>/package.json for buildDefine plus each
    /// feature's buildDefine. Pure filesystem work, no GUI state.
    static std::vector<std::string> CollectPackageDefines(const std::string& projectPath,
                                                         const std::string& targetPlatformId = "");

    /// Platforms visible for a project: built-ins, board packs, and the
    /// project's own project/platforms directory.
    /// Returns {"activePlatformId": "...", "platforms": [ {...}, ... ]}.
    static nlohmann::json ListPlatforms(const std::string& projectPath, std::string& outError);

    /// Toolchain component status for the platform's builder.
    /// Returns {"builder": "...", "installed": bool, "status": "...",
    ///          "components": [ {id, displayName, status, installedVersion, ...} ]}.
    static nlohmann::json ToolchainStatus(const std::string& projectPath,
                                          const std::string& platformId,
                                          std::string& outError);

    /// Everything that has to happen between "a firmware build was asked for"
    /// and IFirmwareBuilder::Build: toolchain check, SPIFFS payload export,
    /// package defines, platform config, build options, build-file regeneration.
    ///
    /// This is the single definition of what preparing a firmware build means.
    /// The Build panel calls it and then drives the builder itself (so it can
    /// wire its own progress and cancel UI); Build/StartBuild below call it and
    /// drive the builder for you. Before it existed the two had drifted, and a
    /// firmware image built from the CLI shipped with no SPIFFS data at all —
    /// no dproject.bin, no boot scene — because that step lived only in the
    /// panel.
    ///
    /// `editor` may be null, in which case the defines fall back to every
    /// installed package's declared define and no SPIFFS payload is written.
    /// A booted editor is always the better input: its package panel scans the
    /// project's scenes and contributes only the defines actually used.
    static FirmwareOpResult PrepareBuild(const std::string& projectPath,
                                         const PlatformConfig& platform,
                                         IFirmwareBuilder& builder,
                                         EditorApp* editor,
                                         const BuildOptions& options,
                                         FirmwareLogCallback onLine);

    /// Regenerate build files, then build firmware. Blocks until complete.
    /// platformId empty => the project's activePlatformId.
    /// Intended for the CLI, where blocking is what the caller wants.
    static FirmwareOpResult Build(const std::string& projectPath,
                                  const std::string& platformId,
                                  FirmwareLogCallback onLine = nullptr,
                                  EditorApp* editor = nullptr);

    /// Same preparation as Build(), but returns as soon as the build thread is
    /// running. success==true means "started", not "finished" — poll QueryStatus().
    /// Used by MCP, whose handlers run on the editor's frame loop and must not
    /// block for the minutes a firmware build takes.
    static FirmwareOpResult StartBuild(const std::string& projectPath,
                                       const std::string& platformId,
                                       FirmwareLogCallback onLine = nullptr,
                                       EditorApp* editor = nullptr);

    /// Non-blocking Flash(). success==true means "started".
    static FirmwareOpResult StartFlash(const std::string& projectPath,
                                       const std::string& platformId,
                                       const std::string& port,
                                       FirmwareLogCallback onLine = nullptr);

    /// Non-blocking InstallToolchainComponent(). success==true means "started".
    /// Progress surfaces through QueryStatus() as state "installing".
    static FirmwareOpResult StartInstallToolchainComponent(const std::string& projectPath,
                                                           const std::string& platformId,
                                                           const std::string& componentId,
                                                           FirmwareLogCallback onLine = nullptr);

    /// Non-blocking SetupToolchainComponent(). success==true means "started".
    /// Runs the component's post-install step, which for ESP-IDF is the
    /// install script that fetches the compilers the SDK archive omits.
    static FirmwareOpResult StartSetupToolchainComponent(const std::string& projectPath,
                                                         const std::string& platformId,
                                                         const std::string& componentId,
                                                         FirmwareLogCallback onLine = nullptr);

    /// Snapshot of the most recently started operation. Returns
    /// {"active": bool, "state": "...", "statusText": "...", "progress": f,
    ///  "error": "...", "platformId": "..."}.
    /// state is one of idle|building|flashing|installing|completed|failed.
    static nlohmann::json QueryStatus();

    /// Request cancellation of the in-flight operation. No-op when idle.
    static bool CancelActive();

    /// Flash previously built firmware. port empty => builder auto-detect.
    static FirmwareOpResult Flash(const std::string& projectPath,
                                  const std::string& platformId,
                                  const std::string& port,
                                  FirmwareLogCallback onLine = nullptr);

    /// Clean the builder's build directory. Blocks until complete.
    static FirmwareOpResult Clean(const std::string& projectPath,
                                  const std::string& platformId,
                                  FirmwareLogCallback onLine = nullptr);

    /// Download/extract a toolchain component, then run its postInstall step
    /// when the component declares one. Blocks until complete.
    static FirmwareOpResult InstallToolchainComponent(const std::string& projectPath,
                                                      const std::string& platformId,
                                                      const std::string& componentId,
                                                      FirmwareLogCallback onLine = nullptr);
};

}  // namespace DekiEditor
