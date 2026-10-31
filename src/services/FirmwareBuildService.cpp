#include "FirmwareBuildService.h"
#include <deki-editor/build/PlatformTemplates.h>
#include <deki-editor/Paths.h>

#include "core/Project.h"
#include <deki-editor/build/PlatformConfig.h>
#include <deki-editor/build/BuilderRegistry.h>
#include <deki-editor/build/TargetBuilder.h>
#include "EditorApp.h"
#include "panels/PackageManagerPanel.h"
#include "services/AssetExporter.h"
#include "services/ExportTarget.h"
#include <deki-editor/FeatureResolver.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace DekiEditor
{

namespace
{

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
    ITargetBuilder* builder = nullptr;
    std::string platformId;
};

// Sets up the registries as EditorProjectManager::OpenProject does, so headless
// callers see the same platforms the GUI would.
bool Prepare(const std::string& projectPath, const std::string& platformId, Ctx& ctx, std::string& err)
{
    if (!fs::exists(fs::path(projectPath) / "deki.json"))
    {
        err = "No deki.json found in " + projectPath + " - not a Deki project.";
        return false;
    }

    ctx.project = std::make_unique<Project>();
    if (!ctx.project->Load(projectPath))
    {
        err = "Failed to load project from " + projectPath;
        return false;
    }

    auto& platforms = PlatformRegistry::Instance();
    ReloadProjectPlatforms(projectPath);

    ctx.platformId = platformId.empty() ? ctx.project->GetActivePlatformId() : platformId;
    if (ctx.platformId.empty())
    {
        err = "No platform specified and the project has no activePlatformId.";
        return false;
    }

    // A platform a package only offers becomes the project's own before it is
    // built for: its boot scene path is relative to the project.
    if (!EnsureProjectPlatform(projectPath, ctx.platformId, err))
    {
        return false;
    }

    ctx.platform = platforms.GetPlatform(ctx.platformId);
    if (!ctx.platform)
    {
        std::string avail;
        for (const auto& id : platforms.GetAvailablePlatforms())
        {
            if (!avail.empty())
            {
                avail += ", ";
            }
            avail += id;
        }
        err = "Unknown platform '" + ctx.platformId + "'. Available: " + (avail.empty() ? "(none)" : avail);
        return false;
    }

    BuilderRegistry::Instance().EnsureLoaded();
    ctx.builder = BuilderRegistry::Instance().GetBuilderForPlatform(*ctx.platform);
    if (!ctx.builder)
    {
        err =
            BuilderRegistry::Instance().DescribeMissingBuilder(*ctx.platform) + " (platform '" + ctx.platformId + "')";
        return false;
    }

    // The builder owns its own settings, so it owns judging them. The editor
    // does not know what an ESP-IDF target is.
    const std::vector<std::string> problems = ctx.builder->ValidatePlatform(*ctx.platform);
    if (!problems.empty())
    {
        err = "Platform '" + ctx.platformId + "' is not valid for the " + ctx.builder->GetFrameworkId() + " builder:";
        for (const auto& problem : problems)
        {
            err += "\n  " + problem;
        }
        return false;
    }

    // Told its platform before anything is asked of it. One backend serves
    // every platform of its framework, and its answers (which toolchain
    // components, whether they are installed, whether it can deploy) can
    // depend on which one: a Tactility device app needs ESP-IDF, its simulator
    // does not. Never during a running operation, which reads the platform it
    // was started with.
    if (!ctx.builder->IsBuilding())
    {
        ctx.builder->SetPlatformConfig(*ctx.platform);
    }

    return true;
}

// The builder that owns the operation started most recently. This lets
// QueryStatus() report progress without running Prepare() again, which would
// call PlatformRegistry::Clear() under a running build. Builders come from
// packages and are destroyed when those unload, so the pointer is checked
// against the registry (ActiveBuilder) before every use.
struct ActiveOp
{
    ITargetBuilder* builder = nullptr;
    std::string platformId;
    std::string platformName;
};

ActiveOp g_Active;

// g_Active's builder, if it still exists. A package reload destroys the
// builders it brought, and status and cancel must not call into freed memory.
// Compared by address only, never dereferenced until found.
ITargetBuilder* ActiveBuilder()
{
    if (!g_Active.builder)
    {
        return nullptr;
    }
    for (ITargetBuilder* b : BuilderRegistry::Instance().GetAllBuilders())
    {
        if (b == g_Active.builder)
        {
            return b;
        }
    }
    g_Active = ActiveOp{};
    return nullptr;
}

// Toolchain installs report through BuildProgressCallback rather than the
// builder's BuildState, so their latest status is mirrored here for QueryStatus().
std::mutex g_ToolchainMutex;
std::string g_ToolchainStatusText;
std::string g_ToolchainError;
float g_ToolchainProgress = 0.0f;

BuildState g_ToolchainState = BuildState::Idle;  // Completed or Failed once it ends

// Whether the operation started last is a toolchain install or setup, whose
// result is the mirror above rather than the builder's BuildState.
bool g_LastOpIsToolchain = false;

void ResetToolchainStatus()
{
    std::lock_guard<std::mutex> lock(g_ToolchainMutex);
    g_ToolchainStatusText.clear();
    g_ToolchainError.clear();
    g_ToolchainProgress = 0.0f;
    g_ToolchainState = BuildState::Idle;
}

// The output of the operation started last, for QueryStatus(). One started
// from MCP has nobody reading its lines as they come, and why a build failed
// is in them (the compiler's error), not in its status.
constexpr size_t kOutputLines = 500;
std::mutex g_OutputMutex;
std::deque<std::string> g_Output;
size_t g_OutputTotal = 0;

void ResetOutput()
{
    std::lock_guard<std::mutex> lock(g_OutputMutex);
    g_Output.clear();
    g_OutputTotal = 0;
}

// onLine, with each line also kept in g_Output. Error lines are kept with an
// "error: " prefix, the only place their flag survives.
FirmwareLogCallback Recording(FirmwareLogCallback onLine)
{
    return [onLine](const std::string& line, bool isError)
    {
        {
            std::lock_guard<std::mutex> lock(g_OutputMutex);
            g_Output.push_back(isError ? "error: " + line : line);
            if (g_Output.size() > kOutputLines)
            {
                g_Output.pop_front();
            }
            ++g_OutputTotal;
        }
        if (onLine)
        {
            onLine(line, isError);
        }
    };
}

// A build or deploy's progress, reduced to the one call that says it ended:
// Completed, Failed, or Idle after it ran (cancelled). Null without onDone.
BuildProgressCallback DoneSink(FirmwareDoneCallback onDone)
{
    if (!onDone)
    {
        return nullptr;
    }
    auto ran = std::make_shared<std::atomic<bool>>(false);
    auto ended = std::make_shared<std::atomic<bool>>(false);
    return [onDone, ran, ended](const BuildProgress& p)
    {
        if (p.state == BuildState::Building || p.state == BuildState::Deploying)
        {
            *ran = true;
            return;
        }
        const bool end = p.state == BuildState::Completed || p.state == BuildState::Failed ||
                         (p.state == BuildState::Idle && *ran);
        if (end && !ended->exchange(true))
        {
            onDone(p.state, p.state == BuildState::Failed ? p.error : p.statusText);
        }
    };
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
            {
                g_ToolchainError = p.error;
            }
            if (p.state == BuildState::Completed || p.state == BuildState::Failed)
            {
                g_ToolchainState = p.state;
            }
        }

        if (!onLine)
        {
            return;
        }
        if (!toLog.empty())
        {
            onLine(toLog, false);
        }
        if (!p.error.empty())
        {
            onLine(p.error, true);
        }
    };
}

// Shared validation for both install entry points.
FirmwareOpResult ValidateComponent(const Ctx& ctx, const std::string& componentId)
{
    if (ctx.builder->IsToolchainBusy())
    {
        return FirmwareOpResult{ false, "A toolchain operation is already in progress.", ctx.platformId,
                                 ctx.platform->displayName };
    }

    // InstallComponent() does nothing for an unknown id, which would look like success.
    std::string ids;
    for (const auto& c : ctx.builder->GetToolchainComponents())
    {
        if (c.id == componentId)
        {
            return FirmwareOpResult{ true, "", ctx.platformId, ctx.platform->displayName };
        }
        if (!ids.empty())
        {
            ids += ", ";
        }
        ids += c.id;
    }

    return FirmwareOpResult{ false,
                             "Unknown toolchain component '" + componentId +
                                 "'. Available: " + (ids.empty() ? "(none)" : ids),
                             ctx.platformId, ctx.platform->displayName };
}

// ITargetBuilder::Build/Flash/Clean return as soon as the worker starts.
// RunOnBuildThread sets m_IsBuilding before spawning, so polling cannot race.
FirmwareOpResult WaitForBuild(ITargetBuilder* builder, const Ctx& ctx)
{
    while (builder->IsBuilding())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    FirmwareOpResult r;
    r.platformId = ctx.platformId;
    r.platformName = ctx.platform->displayName;

    const BuildProgress progress = builder->GetProgress();
    r.success = (builder->GetState() != BuildState::Failed);
    if (!r.success)
    {
        r.error = progress.error.empty() ? "Build failed (no error reported)" : progress.error;
    }

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

// Makes ctx's builder the one QueryStatus() reports on, with a fresh output.
void BeginOp(const Ctx& ctx, bool toolchain)
{
    g_Active = ActiveOp{ ctx.builder, ctx.platformId, ctx.platform->displayName };
    g_LastOpIsToolchain = toolchain;
    ResetOutput();
}

// Shared by Build() and StartBuild(): resolves, checks the toolchain, writes
// the build files, and starts the build thread.
FirmwareOpResult KickOffBuild(const std::string& projectPath, const std::string& platformId, FirmwareLogCallback onLine,
                              Ctx& ctx, EditorApp* editor, const BuildOptions& options,
                              FirmwareDoneCallback onDone = nullptr)
{
    std::string err;
    if (!Prepare(projectPath, platformId, ctx, err))
    {
        return Failure(err);
    }

    if (ctx.builder->IsBuilding())
    {
        return Failure("A build is already in progress for this builder.");
    }

    ResetOutput();
    const FirmwareLogCallback lines = Recording(onLine);

    // One definition of "prepare a firmware build", shared with the Build panel.
    FirmwareOpResult prepared =
        FirmwareBuildService::PrepareBuild(projectPath, *ctx.platform, *ctx.builder, editor, options, lines);
    if (!prepared.success)
    {
        return prepared;
    }

    g_Active = ActiveOp{ ctx.builder, ctx.platformId, ctx.platform->displayName };
    g_LastOpIsToolchain = false;
    ctx.builder->Build(projectPath, lines, DoneSink(onDone));
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

nlohmann::json FirmwareBuildService::ListPlatforms(const std::string& projectPath, std::string& outError)
{
    Ctx ctx;
    std::string err;
    // Prepare() also resolves a platform; listing only needs the registry, so
    // "no active platform" is not an error here.
    if (!Prepare(projectPath, std::string(), ctx, err) && !ctx.project)
    {
        outError = err;
        return nlohmann::json::object();
    }

    nlohmann::json out;
    out["activePlatformId"] = ctx.project ? ctx.project->GetActivePlatformId() : "";

    nlohmann::json arr = nlohmann::json::array();
    auto& platforms = PlatformRegistry::Instance();
    BuilderRegistry::Instance().EnsureLoaded();

    for (const auto& id : platforms.GetAvailablePlatforms())
    {
        const PlatformConfig* p = platforms.GetPlatform(id);
        if (!p)
        {
            continue;
        }

        ITargetBuilder* b = BuilderRegistry::Instance().GetBuilderForPlatform(*p);

        nlohmann::json j;
        j["id"] = p->id;
        j["displayName"] = p->displayName;
        j["family"] = p->family;
        j["framework"] = p->framework;
        j["screenWidth"] = p->screenWidth;
        j["screenHeight"] = p->screenHeight;
        j["builder"] = b ? b->GetName() : "";
        j["toolchainInstalled"] = b ? b->IsToolchainInstalled() : false;

        // Where it came from, for the ones a package offers
        const PlatformOriginStatus origin = GetPlatformOrigin(projectPath, p->id);
        if (origin.fromPackage)
        {
            nlohmann::json o;
            o["package"] = origin.packageId;
            o["packageInstalled"] = origin.packageInstalled;
            if (!origin.adoptedVersion.empty())
            {
                o["adoptedVersion"] = origin.adoptedVersion;
            }
            if (!origin.offeredVersion.empty())
            {
                o["offeredVersion"] = origin.offeredVersion;
            }
            o["modified"] = origin.modifiedItems;
            o["updateAvailable"] = origin.updatedItems;
            o["canUpdate"] = origin.CanUpdate();
            o["canRestore"] = origin.CanRestore();
            j["origin"] = o;
        }
        arr.push_back(j);
    }

    out["platforms"] = arr;
    return out;
}

nlohmann::json FirmwareBuildService::ToolchainStatus(const std::string& projectPath, const std::string& platformId,
                                                     std::string& outError)
{
    Ctx ctx;
    if (!Prepare(projectPath, platformId, ctx, outError))
    {
        return nlohmann::json::object();
    }

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

FirmwareOpResult FirmwareBuildService::PrepareBuild(const std::string& projectPath, const PlatformConfig& platform,
                                                    ITargetBuilder& builder, EditorApp* editor,
                                                    const BuildOptions& options, FirmwareLogCallback onLine)
{
    auto log = [&](const std::string& line, bool isError)
    {
        if (onLine)
        {
            onLine(line, isError);
        }
    };

    if (!builder.IsToolchainInstalled())
    {
        return Failure("Toolchain not installed for '" + platform.id + "': " + builder.GetToolchainStatus() +
                       ". Install it with --install-toolchain or the Build Panel.");
    }

    // The builder must learn its platform before the export below.
    // GetBuildDirectory is per platform only once it knows one; otherwise every
    // board of one framework writes into the same fallback directory, and two
    // boards in one project overwrite each other's assets.
    builder.SetPlatformConfig(platform);
    // The options too: a simulated build has its own build directory, and the
    // payload below goes into it.
    builder.SetBuildOptions(options);
    if (options.simulate && !builder.SupportsSimulation())
    {
        return Failure("The '" + std::string(builder.GetName()) + "' backend cannot build '" + platform.id +
                       "' for a simulator.");
    }
    if (options.simulate && !editor)
    {
        return Failure("A simulated build needs the editor, which writes its boot scene.");
    }
    // Where a simulated build's boot scene goes; its scenes count for the
    // strip plan like the board's own.
    std::string simulationSceneDir;
    if (options.simulate)
    {
        simulationSceneDir =
            (std::filesystem::path(builder.GetBuildDirectory(projectPath)) / kSimulationSceneDir).string();
    }

    // The boot payload (project_data.bin plus the platform's boot scene),
    // written where the backend asks for it, before it assembles anything.
    // Without it a device comes up with no assets at all.
    //
    // It needs an imported asset pipeline, which is why the CLI boots the
    // editor rather than calling the builder directly.
    const std::string bootPayloadDir = builder.GetBootPayloadDirectory(projectPath);
    // Packages that did not build leave their components out of every scene
    // the export reads, and the firmware is compiled from the same sources.
    if (editor && editor->GetHotReloadService() && editor->GetHotReloadService()->HasBuildErrors())
    {
        return Failure("The project's code did not build; fix the errors in the Console first.");
    }
    if (editor && editor->GetProject() && editor->GetAssetPipeline() && !bootPayloadDir.empty())
    {
        // A platform may name its own boot scene (a splash that then loads
        // the game); without one the device boots the project's startup
        // scene, the one the editor opens. Something must always be exported,
        // or the engine's LoadBootScene fails.
        std::string bootScene = platform.bootScene;
        if (bootScene.empty())
        {
            bootScene = editor->GetProject()->GetStartupScene();
            std::replace(bootScene.begin(), bootScene.end(), '\\', '/');
        }

        // Simulated: the backend's copy of the board's boot scene, in the
        // simulated build's own directory. The board's boot scene is only read.
        if (options.simulate)
        {
            std::string scene;  // empty: the platform has no boot scene of its own
            if (!platform.bootScene.empty())
            {
                std::ifstream in(std::filesystem::path(projectPath) / platform.bootScene, std::ios::binary);
                if (!in)
                {
                    return Failure("Cannot read the boot scene " + platform.bootScene);
                }
                scene.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            std::vector<std::string> notes;
            std::string why;
            if (!builder.MakeSimulationBootScene(scene, platform, notes, why))
            {
                return Failure("Cannot make the simulated boot scene: " + why);
            }
            std::error_code ec;
            std::filesystem::create_directories(simulationSceneDir, ec);
            const std::filesystem::path simulated = std::filesystem::path(simulationSceneDir) / "boot.scene";
            std::ofstream out(simulated, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                return Failure("Cannot write " + simulated.string());
            }
            out << scene;
            out.close();
            bootScene = simulated.generic_string();
            log(std::string("Simulating in ") + builder.GetSimulatorName() + ", from a copy of the boot scene (" +
                    simulated.generic_string() + ")",
                false);
            for (const std::string& note : notes)
            {
                log("  " + note, false);
            }
        }

        AssetExportTarget target;
        if (!ExportTargetForPlatform(platform.id, target))
        {
            return Failure("Cannot export for '" + platform.id + "'; the log says why.");
        }

        AssetExporter exporter(editor->GetProject(), editor->GetAssetPipeline(), editor->GetPackageLoader());
        exporter.SetTarget(target);
        // A scene object that needs something this board lacks is left out
        // of what gets flashed, rather than shipped and broken.
        exporter.SetTargetCapabilities(platform.Capabilities());
        if (exporter.ExportBootPayload(bootPayloadDir, bootScene))
        {
            log(std::string("Exported boot payload: project_data.bin") +
                    (bootScene.empty() ? "" : ", boot.scene (" + bootScene + ")"),
                false);
        }
        else
        {
            // Must fail the build: a board that cannot load its first scene
            // shows a blank screen.
            std::string why;
            for (const std::string& problem : exporter.GetProblems())
            {
                why += (why.empty() ? "" : "; ") + problem;
            }
            return Failure("Failed to export the boot payload" + (why.empty() ? std::string(".") : ": " + why));
        }

        // Internal storage: the assets ship in the build, in the payload's
        // assets/ folder (F:/assets/ on the device). External storage gets
        // them from the SD Card Deploy panel instead.
        if (target.assetStorage == Deki::Storage::Internal)
        {
            if (!exporter.ExportAssets((std::filesystem::path(bootPayloadDir) / "assets").string()))
            {
                // Each problem in the build's output and in the error, not
                // only the Console: a build started from MCP or the CLI has
                // no Console to read.
                std::string why;
                for (const std::string& problem : exporter.GetProblems())
                {
                    log(problem, true);
                    why += (why.empty() ? "" : "; ") + problem;
                }
                return Failure("Failed to export the assets into the build" +
                               (why.empty() ? std::string("; the log lists what is wrong.") : ": " + why));
            }
            log("Exported the assets into the build (internal storage)", false);
        }

        // The simulated board's SD card, made fresh: what the SD Card Deploy
        // panel would copy to a real one, or nothing.
        if (options.simulate)
        {
            const std::filesystem::path cardDir = std::filesystem::path(simulationSceneDir) / kSimulationCardDir;
            std::error_code ec;
            std::filesystem::remove_all(cardDir, ec);
            std::filesystem::create_directories(cardDir, ec);
            if (ec)
            {
                return Failure("Cannot make " + cardDir.string() + ": " + ec.message());
            }
            if (target.assetStorage == Deki::Storage::External)
            {
                if (!exporter.ExportAssets(cardDir.string()))
                {
                    return Failure("Failed to export the assets onto the simulated SD card");
                }
                log("Exported the assets onto the simulated SD card (external storage)", false);
            }
        }
    }

    // Which package features the firmware is compiled with. The panel's scan
    // reads the project's scenes and gives only what is used;
    // CollectPackageDefines is the weaker fallback for a caller with no editor,
    // taking every installed package's declared define.
    std::vector<std::string> defines;
    if (options.simulate)
    {
        // The simulated boot scene's steps are used too.
        defines = ComputeStripPlan(projectPath, platform.id, { simulationSceneDir }).defines;
    }
    else if (editor && editor->GetPackageManagerPanel())
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

    // Write the build files again before building: they hold absolute engine
    // and editor paths, so a project cloned from another machine would
    // otherwise build against paths that do not exist here.
    if (!builder.GenerateBuildFiles(projectPath, platform, defines))
    {
        return Failure("Failed to generate build files for '" + platform.id + "'.");
    }

    FirmwareOpResult ok;
    ok.success = true;
    ok.platformId = platform.id;
    ok.platformName = platform.displayName;
    return ok;
}

FirmwareOpResult FirmwareBuildService::Build(const std::string& projectPath, const std::string& platformId,
                                             FirmwareLogCallback onLine, EditorApp* editor, const BuildOptions& options)
{
    Ctx ctx;
    FirmwareOpResult started = KickOffBuild(projectPath, platformId, onLine, ctx, editor, options);
    if (!started.success)
    {
        return started;
    }

    return WaitForBuild(ctx.builder, ctx);
}

FirmwareOpResult FirmwareBuildService::StartBuild(const std::string& projectPath, const std::string& platformId,
                                                  FirmwareLogCallback onLine, EditorApp* editor,
                                                  const BuildOptions& options, FirmwareDoneCallback onDone)
{
    Ctx ctx;
    return KickOffBuild(projectPath, platformId, onLine, ctx, editor, options, onDone);
}

namespace
{
// The error for a backend with no deploy step. Its Flash() does nothing, so
// calling it would wrongly report that a deploy started.
std::string NoDeployStep(const ITargetBuilder& builder, const PlatformConfig& platform)
{
    return std::string("The '") + builder.GetName() + "' backend has no deploy step for platform '" + platform.id +
           "'.";
}
}  // namespace

FirmwareOpResult FirmwareBuildService::StartDeploy(const std::string& projectPath, const std::string& platformId,
                                                   const std::string& deployTargetId, FirmwareLogCallback onLine,
                                                   const BuildOptions& options, FirmwareDoneCallback onDone)
{
    Ctx ctx;
    std::string err;
    if (!Prepare(projectPath, platformId, ctx, err))
    {
        return Failure(err);
    }

    if (ctx.builder->IsBuilding())
    {
        return Failure("A build or deploy is already in progress for this builder.");
    }

    // Told its platform before it is asked whether it can deploy: one backend
    // serves every platform of its framework, and whether there is a deploy
    // step can differ between them (a device app vs. its simulator).
    ctx.builder->SetPlatformConfig(*ctx.platform);
    ctx.builder->SetBuildOptions(options);  // a simulated build is run, not flashed
    if (!ctx.builder->SupportsDeploy())
    {
        return Failure(NoDeployStep(*ctx.builder, *ctx.platform));
    }
    BeginOp(ctx, false);
    ctx.builder->Deploy(projectPath, deployTargetId, Recording(onLine), DoneSink(onDone));
    return Started(ctx);
}

FirmwareOpResult FirmwareBuildService::StartInstallToolchainComponent(const std::string& projectPath,
                                                                      const std::string& platformId,
                                                                      const std::string& componentId,
                                                                      FirmwareLogCallback onLine)
{
    Ctx ctx;
    std::string err;
    if (!Prepare(projectPath, platformId, ctx, err))
    {
        return Failure(err);
    }

    FirmwareOpResult check = ValidateComponent(ctx, componentId);
    if (!check.success)
    {
        return check;
    }

    ResetToolchainStatus();
    BeginOp(ctx, true);
    ctx.builder->InstallToolchainComponent(componentId, MakeToolchainProgressSink(Recording(onLine)));
    return Started(ctx);
}

FirmwareOpResult FirmwareBuildService::StartSetupToolchainComponent(const std::string& projectPath,
                                                                    const std::string& platformId,
                                                                    const std::string& componentId,
                                                                    FirmwareLogCallback onLine)
{
    Ctx ctx;
    std::string err;
    if (!Prepare(projectPath, platformId, ctx, err))
    {
        return Failure(err);
    }

    FirmwareOpResult check = ValidateComponent(ctx, componentId);
    if (!check.success)
    {
        return check;
    }

    ResetToolchainStatus();
    BeginOp(ctx, true);
    ctx.builder->SetupToolchainComponent(componentId, MakeToolchainProgressSink(Recording(onLine)));
    return Started(ctx);
}

nlohmann::json FirmwareBuildService::QueryStatus(size_t outputLines)
{
    nlohmann::json out = QueryState();
    if (outputLines > 0)
    {
        std::lock_guard<std::mutex> lock(g_OutputMutex);
        const size_t n = std::min(outputLines, g_Output.size());
        out["output"] = std::vector<std::string>(g_Output.end() - static_cast<std::ptrdiff_t>(n), g_Output.end());
        out["outputLineCount"] = g_OutputTotal;
    }
    return out;
}

nlohmann::json FirmwareBuildService::QueryState()
{
    nlohmann::json out;

    if (!ActiveBuilder())
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
        case BuildState::Deploying: state = "deploying"; break;
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

    // A finished install or setup reports its own result: the builder's
    // BuildState is still whatever the last build left.
    if (g_LastOpIsToolchain)
    {
        std::lock_guard<std::mutex> lock(g_ToolchainMutex);
        const bool failed = g_ToolchainState == BuildState::Failed || !g_ToolchainError.empty();
        out["state"] = failed ? "failed" : g_ToolchainState == BuildState::Completed ? "completed" : "idle";
        out["statusText"] = g_ToolchainStatusText;
        out["progress"] = g_ToolchainProgress;
        out["error"] = g_ToolchainError;
    }

    return out;
}

bool FirmwareBuildService::CancelActive()
{
    if (!ActiveBuilder() || !g_Active.builder->IsBuilding())
    {
        return false;
    }

    g_Active.builder->Cancel();
    return true;
}

FirmwareOpResult FirmwareBuildService::Deploy(const std::string& projectPath, const std::string& platformId,
                                              const std::string& deployTargetId, FirmwareLogCallback onLine,
                                              const BuildOptions& options)
{
    Ctx ctx;
    std::string err;
    if (!Prepare(projectPath, platformId, ctx, err))
    {
        return Failure(err);
    }

    ctx.builder->SetPlatformConfig(*ctx.platform);
    ctx.builder->SetBuildOptions(options);  // a simulated build is run, not flashed
    if (!ctx.builder->SupportsDeploy())
    {
        return Failure(NoDeployStep(*ctx.builder, *ctx.platform));
    }

    BeginOp(ctx, false);
    ctx.builder->Deploy(projectPath, deployTargetId, Recording(onLine), nullptr);
    return WaitForBuild(ctx.builder, ctx);
}

FirmwareOpResult FirmwareBuildService::Clean(const std::string& projectPath, const std::string& platformId,
                                             FirmwareLogCallback onLine)
{
    Ctx ctx;
    std::string err;
    if (!Prepare(projectPath, platformId, ctx, err))
    {
        return Failure(err);
    }

    ctx.builder->SetPlatformConfig(*ctx.platform);
    BeginOp(ctx, false);
    ctx.builder->Clean(projectPath, Recording(onLine), nullptr);
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
    {
        return Failure(err);
    }

    FirmwareOpResult check = ValidateComponent(ctx, componentId);
    if (!check.success)
    {
        return check;
    }

    ResetToolchainStatus();
    BeginOp(ctx, true);
    ctx.builder->InstallToolchainComponent(componentId, MakeToolchainProgressSink(Recording(onLine)));

    while (ctx.builder->IsToolchainBusy())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    FirmwareOpResult r;
    r.platformId = ctx.platformId;
    r.platformName = ctx.platform->displayName;

    // Ask again rather than trust the progress callback: the component list is
    // rebuilt by looking at the disk, so this shows what was really installed.
    for (const auto& c : ctx.builder->GetToolchainComponents())
    {
        if (c.id != componentId)
        {
            continue;
        }
        r.success =
            (c.status == ToolchainComponentStatus::Installed || c.status == ToolchainComponentStatus::UpdateAvailable);
        if (!r.success)
        {
            r.error =
                "Component '" + componentId + "' is still reported as " + StatusName(c.status) + " after install.";
        }
        return r;
    }

    r.success = false;
    r.error = "Component '" + componentId + "' disappeared from the component list.";
    return r;
}

}  // namespace DekiEditor
