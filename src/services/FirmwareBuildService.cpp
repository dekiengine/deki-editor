#include "FirmwareBuildService.h"
#include <deki-editor/Paths.h>

#include "core/Project.h"
#include "core/PlatformConfig.h"
#include "builders/BuilderRegistry.h"
#include "builders/IFirmwareBuilder.h"
#include "EditorApp.h"
#include "panels/PackageManagerPanel.h"
#include "services/AssetExporter.h"
#include "services/FeatureResolver.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace DekiEditor
{

namespace
{

const char* FamilyName(MCUFamily family)
{
    switch (family)
    {
        case MCUFamily::Native: return "native";
        case MCUFamily::Espressif: return "espressif";
        case MCUFamily::RP2040: return "rp2040";
        case MCUFamily::SAMD: return "samd";
        case MCUFamily::NRF: return "nrf";
        default: return "custom";
    }
}

const char* StatusName(ToolchainComponentStatus s)
{
    switch (s)
    {
        case ToolchainComponentStatus::Installed: return "installed";
        case ToolchainComponentStatus::UpdateAvailable: return "update-available";
        case ToolchainComponentStatus::Installing: return "installing";
        default: return "not-installed";
    }
}

// Resolved project + platform + builder for one operation.
struct Ctx
{
    std::unique_ptr<Project> project;
    const PlatformConfig* platform = nullptr;
    IFirmwareBuilder* builder = nullptr;
    std::string platformId;
};

// Mirrors EditorProjectManager::OpenProject's registry setup so headless callers
// see exactly the same platform set the GUI would.
bool Prepare(const std::string& projectPath, const std::string& platformId,
             Ctx& ctx, std::string& err)
{
    if (!fs::exists(fs::path(projectPath) / "deki.json"))
    {
        err = "No deki.json found in " + projectPath + " — not a Deki project.";
        return false;
    }

    ctx.project = std::make_unique<Project>();
    if (!ctx.project->Load(projectPath))
    {
        err = "Failed to load project from " + projectPath;
        return false;
    }

    auto& platforms = PlatformRegistry::Instance();
    platforms.Clear();
    RegisterBuiltInPlatforms();
    platforms.LoadPlatformsFromBoardPacks();
    platforms.LoadPlatformsFromDirectory(GetPlatformsDirectory(projectPath));

    ctx.platformId = platformId.empty() ? ctx.project->GetActivePlatformId() : platformId;
    if (ctx.platformId.empty())
    {
        err = "No platform specified and the project has no activePlatformId.";
        return false;
    }

    ctx.platform = platforms.GetPlatform(ctx.platformId);
    if (!ctx.platform)
    {
        std::string avail;
        for (const auto& id : platforms.GetAvailablePlatforms())
        {
            if (!avail.empty()) avail += ", ";
            avail += id;
        }
        err = "Unknown platform '" + ctx.platformId + "'. Available: " +
              (avail.empty() ? "(none)" : avail);
        return false;
    }

    BuilderRegistry::Instance().RegisterBuiltInBuilders();
    ctx.builder = BuilderRegistry::Instance().GetBuilderForFamily(ctx.platform->family);
    if (!ctx.builder)
    {
        err = std::string("No firmware builder registered for family '") +
              FamilyName(ctx.platform->family) + "' (platform '" + ctx.platformId + "').";
        return false;
    }

    return true;
}

// The builder that owns the operation started most recently. Builders are owned
// by BuilderRegistry (a singleton), so the pointer stays valid for the process
// lifetime; this lets QueryStatus() report progress without re-running Prepare(),
// which would call PlatformRegistry::Clear() underneath a running build.
struct ActiveOp
{
    IFirmwareBuilder* builder = nullptr;
    std::string platformId;
    std::string platformName;
};

ActiveOp g_Active;

// Toolchain installs report through BuildProgressCallback rather than the
// builder's BuildState, so their latest status is mirrored here for QueryStatus().
std::mutex g_ToolchainMutex;
std::string g_ToolchainStatusText;
std::string g_ToolchainError;
float g_ToolchainProgress = 0.0f;

void ResetToolchainStatus()
{
    std::lock_guard<std::mutex> lock(g_ToolchainMutex);
    g_ToolchainStatusText.clear();
    g_ToolchainError.clear();
    g_ToolchainProgress = 0.0f;
}

// Progress sink shared by the blocking and non-blocking install paths.
BuildProgressCallback MakeToolchainProgressSink(FirmwareLogCallback onLine)
{
    return [onLine](const BuildProgress& p)
    {
        std::string toLog;
        {
            std::lock_guard<std::mutex> lock(g_ToolchainMutex);
            if (!p.statusText.empty() && p.statusText != g_ToolchainStatusText)
            {
                g_ToolchainStatusText = p.statusText;
                toLog = p.statusText;
            }
            g_ToolchainProgress = p.progress;
            if (!p.error.empty())
                g_ToolchainError = p.error;
        }

        if (!onLine) return;
        if (!toLog.empty())
            onLine(toLog, false);
        if (!p.error.empty())
            onLine(p.error, true);
    };
}

// Shared validation for both install entry points.
FirmwareOpResult ValidateComponent(const Ctx& ctx, const std::string& componentId)
{
    if (ctx.builder->IsToolchainBusy())
        return FirmwareOpResult{ false, "A toolchain operation is already in progress.", ctx.platformId, ctx.platform->displayName };

    // InstallComponent() silently no-ops on an unknown id, which would look like success.
    std::string ids;
    for (const auto& c : ctx.builder->GetToolchainComponents())
    {
        if (c.id == componentId)
            return FirmwareOpResult{ true, "", ctx.platformId, ctx.platform->displayName };
        if (!ids.empty()) ids += ", ";
        ids += c.id;
    }

    return FirmwareOpResult{ false,
                             "Unknown toolchain component '" + componentId + "'. Available: " +
                                 (ids.empty() ? "(none)" : ids),
                             ctx.platformId, ctx.platform->displayName };
}

// IFirmwareBuilder::Build/Flash/Clean return as soon as the worker starts.
// RunOnBuildThread sets m_IsBuilding before spawning, so polling cannot race.
FirmwareOpResult WaitForBuild(IFirmwareBuilder* builder, const Ctx& ctx)
{
    while (builder->IsBuilding())
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    FirmwareOpResult r;
    r.platformId = ctx.platformId;
    r.platformName = ctx.platform->displayName;

    const BuildProgress progress = builder->GetProgress();
    r.success = (builder->GetState() != BuildState::Failed);
    if (!r.success)
        r.error = progress.error.empty() ? "Build failed (no error reported)" : progress.error;

    return r;
}

FirmwareOpResult Failure(const std::string& err)
{
    FirmwareOpResult r;
    r.success = false;
    r.error = err;
    return r;
}

FirmwareOpResult Started(const Ctx& ctx)
{
    FirmwareOpResult r;
    r.success = true;
    r.platformId = ctx.platformId;
    r.platformName = ctx.platform->displayName;
    return r;
}

// Shared by Build() and StartBuild(): resolve, validate the toolchain,
// regenerate build files, and kick the build thread off.
FirmwareOpResult KickOffBuild(const std::string& projectPath, const std::string& platformId,
                              FirmwareLogCallback onLine, Ctx& ctx, EditorApp* editor)
{
    std::string err;
    if (!Prepare(projectPath, platformId, ctx, err))
        return Failure(err);

    if (ctx.builder->IsBuilding())
        return Failure("A build is already in progress for this builder.");

    // One definition of "prepare a firmware build", shared with the Build panel.
    FirmwareOpResult prepared = FirmwareBuildService::PrepareBuild(
        projectPath, *ctx.platform, *ctx.builder, editor, BuildOptions{}, onLine);
    if (!prepared.success)
        return prepared;

    g_Active = ActiveOp{ ctx.builder, ctx.platformId, ctx.platform->displayName };
    ctx.builder->Build(projectPath, onLine, nullptr);
    return Started(ctx);
}

}  // namespace

std::vector<std::string> FirmwareBuildService::CollectPackageDefines(const std::string& projectPath,
                                                                     const std::string& targetPlatformId)
{
    // The stripper decides what a device build defines (services/FeatureResolver):
    // the configured marker, every kept package, every kept feature. The target
    // scopes it: another platform's boot scene must not decide what this one
    // compiles.
    return ComputeStripPlan(projectPath, targetPlatformId).defines;
}

nlohmann::json FirmwareBuildService::ListPlatforms(const std::string& projectPath,
                                                   std::string& outError)
{
    Ctx ctx;
    std::string err;
    // Prepare() resolves a platform too; for listing we only need the registry,
    // so tolerate "no active platform" by probing with an explicit lookup below.
    if (!Prepare(projectPath, std::string(), ctx, err) && !ctx.project)
    {
        outError = err;
        return nlohmann::json::object();
    }

    nlohmann::json out;
    out["activePlatformId"] = ctx.project ? ctx.project->GetActivePlatformId() : "";

    nlohmann::json arr = nlohmann::json::array();
    auto& platforms = PlatformRegistry::Instance();
    BuilderRegistry::Instance().RegisterBuiltInBuilders();

    for (const auto& id : platforms.GetAvailablePlatforms())
    {
        const PlatformConfig* p = platforms.GetPlatform(id);
        if (!p) continue;

        IFirmwareBuilder* b = BuilderRegistry::Instance().GetBuilderForFamily(p->family);

        nlohmann::json j;
        j["id"] = p->id;
        j["displayName"] = p->displayName;
        j["family"] = FamilyName(p->family);
        j["mcuChip"] = p->mcuChip;
        j["framework"] = p->framework;
        j["screenWidth"] = p->screenWidth;
        j["screenHeight"] = p->screenHeight;
        j["builder"] = b ? b->GetName() : "";
        j["toolchainInstalled"] = b ? b->IsToolchainInstalled() : false;
        arr.push_back(j);
    }

    out["platforms"] = arr;
    return out;
}

nlohmann::json FirmwareBuildService::ToolchainStatus(const std::string& projectPath,
                                                     const std::string& platformId,
                                                     std::string& outError)
{
    Ctx ctx;
    if (!Prepare(projectPath, platformId, ctx, outError))
        return nlohmann::json::object();

    nlohmann::json out;
    out["platformId"] = ctx.platformId;
    out["platformName"] = ctx.platform->displayName;
    out["builder"] = ctx.builder->GetName();
    out["installed"] = ctx.builder->IsToolchainInstalled();
    out["status"] = ctx.builder->GetToolchainStatus();

    nlohmann::json comps = nlohmann::json::array();
    for (const auto& c : ctx.builder->GetToolchainComponents())
    {
        nlohmann::json j;
        j["id"] = c.id;
        j["displayName"] = c.displayName;
        j["status"] = StatusName(c.status);
        j["installedVersion"] = c.installedVersion;
        j["latestVersion"] = c.latestVersion;
        j["canInstall"] = c.canInstall;
        j["canSetup"] = c.canSetup;
        j["setupDone"] = c.setupDone;
        j["tooltip"] = c.tooltip;
        comps.push_back(j);
    }
    out["components"] = comps;

    return out;
}

FirmwareOpResult FirmwareBuildService::PrepareBuild(const std::string& projectPath,
                                                    const PlatformConfig& platform,
                                                    IFirmwareBuilder& builder,
                                                    EditorApp* editor,
                                                    const BuildOptions& options,
                                                    FirmwareLogCallback onLine)
{
    auto log = [&](const std::string& line, bool isError)
    {
        if (onLine) onLine(line, isError);
    };

    if (!builder.IsToolchainInstalled())
    {
        return Failure("Toolchain not installed for '" + platform.id + "': " +
                       builder.GetToolchainStatus() +
                       ". Install it with --install-toolchain or the Build Panel.");
    }

    // The builder learns its platform BEFORE the export below, not after.
    // GetBuildDirectory is per platform only once it knows one, and it used to
    // be told after the SPIFFS payload had already been written, so every
    // ESP-IDF board wrote into the same fallback directory and the last build
    // won. Two boards in one project overwrote each other's assets.
    builder.SetPlatformConfig(platform);

    // SPIFFS payload — dproject.bin plus the platform's boot scene — written
    // into the builder's build directory before the image is assembled.
    // Without it the device flashes with no assets at all, which is exactly
    // what every CLI firmware build produced while this step lived only in
    // BuildPanel::StartBuild.
    //
    // It needs an imported asset pipeline, which is why the CLI now boots the
    // editor rather than reaching for the builder directly.
    if (editor && editor->GetProject() && editor->GetAssetPipeline())
    {
        const std::string spiffsDataPath =
            builder.GetBuildDirectory(projectPath) + "/spiffs_data";

        // A platform may name its own boot scene (a splash that then loads
        // the game); without one the device boots the project's startup
        // scene, the one the editor opens. Nothing at all used to be
        // exported, and the engine's LoadBootScene failed on every board
        // whose platform file did not set bootScene.
        std::string bootScene = platform.bootScene;
        if (bootScene.empty())
        {
            bootScene = editor->GetProject()->GetStartupScene();
            std::replace(bootScene.begin(), bootScene.end(), '\\', '/');
        }

        AssetExporter exporter(editor->GetProject(), editor->GetAssetPipeline());
        // So a scene object that asks for something this board has not got is
        // left out of what gets flashed, rather than shipped and dead.
        exporter.SetTargetCapabilities(platform.Capabilities());
        // And the board's own screen, rather than whatever the project is set to.
        exporter.SetTargetDisplay(platform.screenWidth, platform.screenHeight, platform.colorFormat);
        if (exporter.ExportForSPIFFS(spiffsDataPath, bootScene))
        {
            log(std::string("Exported SPIFFS data: dproject.bin") +
                    (bootScene.empty() ? "" : ", boot.scene (" + bootScene + ")"),
                false);
        }
        else
        {
            log("Failed to export SPIFFS data", true);
        }
    }

    // Which package features the firmware is compiled with. The panel's scan
    // reads the project's scenes and contributes only what is actually used;
    // CollectPackageDefines is the weaker fallback for a caller with no editor,
    // taking every installed package's declared define.
    std::vector<std::string> defines;
    if (editor && editor->GetPackageManagerPanel())
    {
        auto* pkgPanel = editor->GetPackageManagerPanel();
        pkgPanel->ScanProjectForPackageUsage();
        defines = pkgPanel->GetRequiredBuildDefines(platform.id);
    }
    else
    {
        defines = CollectPackageDefines(projectPath, platform.id);
    }

    builder.SetBuildOptions(options);
    builder.SetPlatformConfig(platform);
    builder.SetPackageDefines(defines);

    // Regenerate before building: build files carry absolute engine/editor paths,
    // so a project cloned from another machine would otherwise build against
    // paths that do not exist here.
    if (!builder.GenerateBuildFiles(projectPath, platform, defines))
        return Failure("Failed to generate build files for '" + platform.id + "'.");

    FirmwareOpResult ok;
    ok.success = true;
    ok.platformId = platform.id;
    ok.platformName = platform.displayName;
    return ok;
}

FirmwareOpResult FirmwareBuildService::Build(const std::string& projectPath,
                                             const std::string& platformId,
                                             FirmwareLogCallback onLine,
                                             EditorApp* editor)
{
    Ctx ctx;
    FirmwareOpResult started = KickOffBuild(projectPath, platformId, onLine, ctx, editor);
    if (!started.success)
        return started;

    return WaitForBuild(ctx.builder, ctx);
}

FirmwareOpResult FirmwareBuildService::StartBuild(const std::string& projectPath,
                                                  const std::string& platformId,
                                                  FirmwareLogCallback onLine,
                                                  EditorApp* editor)
{
    Ctx ctx;
    return KickOffBuild(projectPath, platformId, onLine, ctx, editor);
}

FirmwareOpResult FirmwareBuildService::StartFlash(const std::string& projectPath,
                                                  const std::string& platformId,
                                                  const std::string& port,
                                                  FirmwareLogCallback onLine)
{
    Ctx ctx;
    std::string err;
    if (!Prepare(projectPath, platformId, ctx, err))
        return Failure(err);

    if (ctx.builder->IsBuilding())
        return Failure("A build or flash is already in progress for this builder.");

    ctx.builder->SetPlatformConfig(*ctx.platform);
    g_Active = ActiveOp{ ctx.builder, ctx.platformId, ctx.platform->displayName };
    ctx.builder->Flash(projectPath, port, onLine, nullptr);
    return Started(ctx);
}

FirmwareOpResult FirmwareBuildService::StartInstallToolchainComponent(
    const std::string& projectPath, const std::string& platformId,
    const std::string& componentId, FirmwareLogCallback onLine)
{
    Ctx ctx;
    std::string err;
    if (!Prepare(projectPath, platformId, ctx, err))
        return Failure(err);

    FirmwareOpResult check = ValidateComponent(ctx, componentId);
    if (!check.success)
        return check;

    ResetToolchainStatus();
    g_Active = ActiveOp{ ctx.builder, ctx.platformId, ctx.platform->displayName };
    ctx.builder->InstallToolchainComponent(componentId, MakeToolchainProgressSink(onLine));
    return Started(ctx);
}

FirmwareOpResult FirmwareBuildService::StartSetupToolchainComponent(
    const std::string& projectPath, const std::string& platformId,
    const std::string& componentId, FirmwareLogCallback onLine)
{
    Ctx ctx;
    std::string err;
    if (!Prepare(projectPath, platformId, ctx, err))
        return Failure(err);

    FirmwareOpResult check = ValidateComponent(ctx, componentId);
    if (!check.success)
        return check;

    ResetToolchainStatus();
    g_Active = ActiveOp{ ctx.builder, ctx.platformId, ctx.platform->displayName };
    ctx.builder->SetupToolchainComponent(componentId, MakeToolchainProgressSink(onLine));
    return Started(ctx);
}

nlohmann::json FirmwareBuildService::QueryStatus()
{
    nlohmann::json out;

    if (!g_Active.builder)
    {
        out["active"] = false;
        out["state"] = "idle";
        out["statusText"] = "";
        out["progress"] = 0.0f;
        out["error"] = "";
        out["platformId"] = "";
        return out;
    }

    // A toolchain install reports through its own progress channel, not
    // BuildState, so it takes precedence while the installer thread is live.
    if (g_Active.builder->IsToolchainBusy())
    {
        std::lock_guard<std::mutex> lock(g_ToolchainMutex);
        out["active"] = true;
        out["state"] = "installing";
        out["statusText"] = g_ToolchainStatusText;
        out["progress"] = g_ToolchainProgress;
        out["error"] = g_ToolchainError;
        out["platformId"] = g_Active.platformId;
        out["platformName"] = g_Active.platformName;
        return out;
    }

    const BuildProgress p = g_Active.builder->GetProgress();

    const char* state = "idle";
    switch (p.state)
    {
        case BuildState::Building: state = "building"; break;
        case BuildState::Flashing: state = "flashing"; break;
        case BuildState::Completed: state = "completed"; break;
        case BuildState::Failed: state = "failed"; break;
        default: state = "idle"; break;
    }

    out["active"] = g_Active.builder->IsBuilding();
    out["state"] = state;
    out["statusText"] = p.statusText;
    out["progress"] = p.progress;
    out["error"] = p.error;
    out["platformId"] = g_Active.platformId;
    out["platformName"] = g_Active.platformName;

    // Surface a finished install's error when no build has run since.
    if (p.state == BuildState::Idle)
    {
        std::lock_guard<std::mutex> lock(g_ToolchainMutex);
        if (!g_ToolchainError.empty())
        {
            out["state"] = "failed";
            out["error"] = g_ToolchainError;
        }
        else if (!g_ToolchainStatusText.empty())
        {
            out["statusText"] = g_ToolchainStatusText;
        }
    }

    return out;
}

bool FirmwareBuildService::CancelActive()
{
    if (!g_Active.builder || !g_Active.builder->IsBuilding())
        return false;

    g_Active.builder->Cancel();
    return true;
}

FirmwareOpResult FirmwareBuildService::Flash(const std::string& projectPath,
                                             const std::string& platformId,
                                             const std::string& port,
                                             FirmwareLogCallback onLine)
{
    Ctx ctx;
    std::string err;
    if (!Prepare(projectPath, platformId, ctx, err))
        return Failure(err);

    ctx.builder->SetPlatformConfig(*ctx.platform);
    g_Active = ActiveOp{ ctx.builder, ctx.platformId, ctx.platform->displayName };
    ctx.builder->Flash(projectPath, port, onLine, nullptr);
    return WaitForBuild(ctx.builder, ctx);
}

FirmwareOpResult FirmwareBuildService::Clean(const std::string& projectPath,
                                             const std::string& platformId,
                                             FirmwareLogCallback onLine)
{
    Ctx ctx;
    std::string err;
    if (!Prepare(projectPath, platformId, ctx, err))
        return Failure(err);

    ctx.builder->SetPlatformConfig(*ctx.platform);
    g_Active = ActiveOp{ ctx.builder, ctx.platformId, ctx.platform->displayName };
    ctx.builder->Clean(projectPath, onLine, nullptr);
    return WaitForBuild(ctx.builder, ctx);
}

FirmwareOpResult FirmwareBuildService::InstallToolchainComponent(const std::string& projectPath,
                                                                 const std::string& platformId,
                                                                 const std::string& componentId,
                                                                 FirmwareLogCallback onLine)
{
    Ctx ctx;
    std::string err;
    if (!Prepare(projectPath, platformId, ctx, err))
        return Failure(err);

    FirmwareOpResult check = ValidateComponent(ctx, componentId);
    if (!check.success)
        return check;

    ResetToolchainStatus();
    g_Active = ActiveOp{ ctx.builder, ctx.platformId, ctx.platform->displayName };
    ctx.builder->InstallToolchainComponent(componentId, MakeToolchainProgressSink(onLine));

    while (ctx.builder->IsToolchainBusy())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    FirmwareOpResult r;
    r.platformId = ctx.platformId;
    r.platformName = ctx.platform->displayName;

    // Re-query rather than trusting the progress callback: the component list is
    // rebuilt from disk detection, so this reflects what actually landed.
    for (const auto& c : ctx.builder->GetToolchainComponents())
    {
        if (c.id != componentId) continue;
        r.success = (c.status == ToolchainComponentStatus::Installed ||
                     c.status == ToolchainComponentStatus::UpdateAvailable);
        if (!r.success)
            r.error = "Component '" + componentId + "' is still reported as " +
                      StatusName(c.status) + " after install.";
        return r;
    }

    r.success = false;
    r.error = "Component '" + componentId + "' disappeared from the component list.";
    return r;
}

}  // namespace DekiEditor
