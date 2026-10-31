// MCP tools for firmware build, flash and toolchain management.
//
// Handlers run on the editor's frame loop (McpServer::ProcessMessages), so long
// operations start and are polled rather than blocking: build_firmware returns
// once the build thread is up, and build_status reports progress. Only the
// quick operations (listing, status, cancel) answer directly.
//
// Toolchain installs download gigabytes, so install_toolchain_component is
// polled too, through build_status and the builder's progress state.

#include "McpTools.h"
#include <deki-editor/build/PlatformTemplates.h>
#include "McpToolRegistry.h"
#include "../EditorApp.h"
#include "../core/Project.h"
#include "../services/FirmwareBuildService.h"
#include "../panels/ActivityPanel.h"
#include "../services/DeviceScreenshot.h"
#include "../services/MemoryReport.h"
#include "../services/SceneMemoryEstimate.h"
#include "../panels/MemoryPanel.h"
#include "McpCompletion.h"
#include <deki-editor/SerialPorts.h>
#include "stb_image_write.h"
#include <deki-editor/build/PlatformConfig.h>
#include <deki-editor/build/BuilderRegistry.h>
#include <deki-editor/build/TargetBuilder.h>
#include <deki-editor/SafeNames.h>
#include "../services/PackageManager.h"
#include <deki-editor/Paths.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <filesystem>
#include <chrono>
#include <cstdio>
#include <thread>
#include <fstream>
#include <functional>
#include <vector>

#include <stdexcept>
#include <string>

namespace DekiEditor
{

namespace
{

// Project path from the argument when given, else the editor's open project.
std::string ResolveProjectPath(EditorApp* editor, const nlohmann::json& args)
{
    const std::string fromArgs = args.value("project_path", std::string());
    if (!fromArgs.empty())
    {
        return fromArgs;
    }

    if (editor)
    {
        if (auto* project = editor->GetProject())
        {
            return project->GetPath();
        }
    }

    throw std::runtime_error("No project open and no project_path supplied.");
}

std::string ArgPlatform(const nlohmann::json& args)
{
    return args.value("platform_id", std::string());
}

// The Activity panel's process for a deploy: the backend's own verb ("Flash"),
// as the Build panel names it.
std::string DeployVerb(EditorApp* editor, const nlohmann::json& args)
{
    std::string id = ArgPlatform(args);
    if (id.empty() && editor && editor->GetProject())
    {
        id = editor->GetProject()->GetActivePlatformId();
    }
    const PlatformConfig* platform = PlatformRegistry::Instance().GetPlatform(id);
    ITargetBuilder* builder = platform ? BuilderRegistry::Instance().GetBuilderForPlatform(*platform) : nullptr;
    return builder ? builder->GetDeployLabel() : "Deploy";
}

// A memory report as MCP returns it: the regions, then every call site,
// biggest first, with the category it counts under. -1 figures are left out.
nlohmann::json MemoryReportJson(const MemoryReport& r)
{
    nlohmann::json out{ { "internalUsed", r.internalUsed },
                        { "externalUsed", r.externalUsed },
                        { "internalPeak", r.internalPeak },
                        { "externalPeak", r.externalPeak } };
    auto optional = [&out](const char* name, int64_t value)
    {
        if (value >= 0)
        {
            out[name] = value;
        }
    };
    optional("internalFree", r.internalFree);
    optional("externalFree", r.externalFree);
    optional("internalMinFree", r.internalMinFree);
    optional("externalMinFree", r.externalMinFree);
    optional("internalTotal", r.internalTotal);
    optional("externalTotal", r.externalTotal);

    std::vector<const MemoryReportSite*> sites;
    for (const MemoryReportSite& s : r.sites)
    {
        sites.push_back(&s);
    }
    std::sort(sites.begin(), sites.end(),
              [](const MemoryReportSite* a, const MemoryReportSite* b) { return a->Bytes() > b->Bytes(); });
    std::map<std::string, nlohmann::json> categories;
    nlohmann::json list = nlohmann::json::array();
    for (const MemoryReportSite* s : sites)
    {
        const char* category = MemoryCategory(*s);
        list.push_back({ { "category", category },
                         { "function", s->function },
                         { "at", s->file + ":" + std::to_string(s->line) },
                         { "internal", s->internalBytes },
                         { "external", s->externalBytes },
                         { "peak", s->peakBytes },
                         { "blocks", s->blocks } });
        nlohmann::json& c = categories[category];
        if (c.is_null())
        {
            c = { { "bytes", 0 }, { "peak", 0 } };
        }
        c["bytes"] = c.value("bytes", uint64_t{ 0 }) + s->Bytes();
        c["peak"] = c.value("peak", uint64_t{ 0 }) + s->peakBytes;
    }
    out["categories"] = categories;
    out["sites"] = list;
    return out;
}

// The serial port a board tool talks to: `port` from the arguments, else the
// only one there is.
std::string BoardPort(const nlohmann::json& args)
{
    std::string port = args.value("port", std::string());
    if (port.empty())
    {
        const std::vector<std::string> ports = EnumerateSerialPorts();
        if (ports.empty())
        {
            throw std::runtime_error("No serial port: connect the board with a USB data cable");
        }
        if (ports.size() > 1)
        {
            std::string list;
            for (const std::string& p : ports)
            {
                list += (list.empty() ? "" : ", ") + p;
            }
            throw std::runtime_error("Several serial ports (" + list + "): name the board's with port");
        }
        port = ports.front();
    }
    std::string reason;
    if (!SafeNames::IsSafeSerialPort(port, reason))
    {
        throw std::runtime_error("Not a serial port: " + reason + " ('" + port + "')");
    }
    return port;
}

// A build or deploy started from MCP shows in the Activity panel like one
// started from the Build panel: its output as it comes, and how it ended.
// Without this its output went nowhere, so a failed build could not be read
// in the editor.
FirmwareLogCallback ActivityLines(const std::string& process)
{
    ActivityPanel::Log(process, process + " (started over MCP)...");
    return [process](const std::string& line, bool isError)
    {
        if (isError)
        {
            ActivityPanel::LogError(process, line);
        }
        else
        {
            ActivityPanel::Log(process, line);
        }
    };
}

FirmwareDoneCallback ActivityDone(const std::string& process)
{
    return [process](BuildState state, const std::string& text)
    {
        if (state == BuildState::Completed)
        {
            ActivityPanel::CompleteProcess(process);
        }
        else if (state == BuildState::Failed)
        {
            ActivityPanel::FailProcess(process, text);
        }
        else
        {
            ActivityPanel::CancelProcess(process);
        }
    };
}

nlohmann::json OpToJson(const FirmwareOpResult& r)
{
    nlohmann::json j;
    j["success"] = r.success;
    j["platformId"] = r.platformId;
    j["platformName"] = r.platformName;
    if (!r.error.empty())
    {
        j["error"] = r.error;
    }
    return j;
}

}  // namespace

void RegisterBuildTools(McpToolRegistry& registry, EditorApp* editor)
{
    // =========================================================================
    // Discovery
    // =========================================================================

    registry.Register(
        { "list_platforms",
          "List build platforms available to the project, with each platform's "
          "builder and whether its toolchain is installed. Also reports the "
          "project's active platform.",
          { { "type", "object" },
            { "properties",
              { { "project_path",
                  { { "type", "string" }, { "description", "Project directory (default: the open project)" } } } } } },
          [editor](const nlohmann::json& args) -> nlohmann::json
          {
              const std::string path = ResolveProjectPath(editor, args);
              std::string err;
              nlohmann::json result = FirmwareBuildService::ListPlatforms(path, err);
              if (!err.empty())
              {
                  throw std::runtime_error(err);
              }
              return result;
          } });

    registry.Register(
        { "wait_for_operation",
          "Return once the running firmware operation finishes, and report how it "
          "went. build_firmware, deploy_build and the toolchain tools all start "
          "in the background and return at once, which suits a GUI polling for "
          "progress but leaves a script with nothing to wait on: the process exits "
          "and takes the work with it. In a running editor the editor keeps "
          "drawing while this waits; a wait past 14 minutes there returns with "
          "timedOut, so call it again.",
          { { "type", "object" },
            { "properties",
              { { "timeout_seconds",
                  { { "type", "integer" },
                    { "description",
                      "Give up after this long (default 3600). Giving up does not cancel the operation." } } },
                { "quiet",
                  { { "type", "boolean" }, { "description", "Do not print progress lines while waiting" } } } } } },
          [](const nlohmann::json& args) -> nlohmann::json
          {
              const int timeoutSeconds = args.value("timeout_seconds", 3600);
              const bool quiet = args.value("quiet", false);

              // The status once the operation is over, else nullopt. A
              // failure fails the call.
              auto finished = [](nlohmann::json status) -> std::optional<nlohmann::json>
              {
                  const std::string state = status.value("state", "idle");
                  if (status.value("active", false) && state != "completed" && state != "failed" && state != "idle")
                  {
                      return std::nullopt;
                  }
                  if (state == "failed")
                  {
                      throw std::runtime_error("operation failed: " + status.value("error", "(no detail)"));
                  }
                  return status;
              };

              // In the live editor the reply waits, not the main thread, so
              // the editor keeps drawing through a build or a flash. The
              // server's HTTP request gives up after 15 minutes, so a longer
              // wait answers timedOut first and is called again.
              if (McpCompletion::CanDefer())
              {
                  constexpr int kLiveWaitLimitSeconds = 840;
                  const int limit = std::min(timeoutSeconds, kLiveWaitLimitSeconds);
                  const auto started = std::chrono::steady_clock::now();
                  McpCompletion::Defer(
                      [finished, started, limit]() -> std::optional<nlohmann::json>
                      {
                          nlohmann::json status = FirmwareBuildService::QueryStatus();
                          if (std::optional<nlohmann::json> done = finished(status))
                          {
                              return done;
                          }
                          const auto elapsed = std::chrono::steady_clock::now() - started;
                          if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= limit)
                          {
                              status["timedOut"] = true;
                              return status;
                          }
                          return std::nullopt;
                      },
                      std::chrono::seconds(limit + 30));
                  return nlohmann::json::object();
              }

              const auto started = std::chrono::steady_clock::now();
              std::string lastText;
              while (true)
              {
                  nlohmann::json status = FirmwareBuildService::QueryStatus();
                  const std::string text = status.value("statusText", "");
                  if (!quiet && !text.empty() && text != lastText)
                  {
                      // stderr: stdout carries the result, and a
                      // caller parsing it should not have to strip
                      // progress out of the middle.
                      fprintf(stderr, "  %s\n", text.c_str());
                      lastText = text;
                  }

                  if (std::optional<nlohmann::json> done = finished(status))
                  {
                      return *done;
                  }

                  const auto elapsed = std::chrono::steady_clock::now() - started;
                  if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= timeoutSeconds)
                  {
                      status["timedOut"] = true;
                      return status;
                  }
                  std::this_thread::sleep_for(std::chrono::milliseconds(500));
              }
          } });

    registry.Register(
        { "setup_toolchain_component",
          "Run a toolchain component's post-install step. Downloading a component "
          "is not always enough to use it: an SDK archive may carry no compilers, "
          "and its install script fetches them. Starts in the background; poll "
          "build_status. Only components whose status reports canSetup have one.",
          { { "type", "object" },
            { "required", nlohmann::json::array({ "component_id" }) },
            { "properties",
              { { "component_id", { { "type", "string" }, { "description", "Component id, from toolchain_status" } } },
                { "platform_id",
                  { { "type", "string" },
                    { "description", "Platform id (default: the project's activePlatformId)" } } },
                { "project_path",
                  { { "type", "string" }, { "description", "Project directory (default: the open project)" } } } } } },
          [editor](const nlohmann::json& args) -> nlohmann::json
          {
              const std::string path = ResolveProjectPath(editor, args);
              const std::string component = args.value("component_id", "");
              if (component.empty())
              {
                  throw std::runtime_error("setup_toolchain_component needs component_id");
              }

              FirmwareOpResult r =
                  FirmwareBuildService::StartSetupToolchainComponent(path, ArgPlatform(args), component, nullptr);
              if (!r.success)
              {
                  throw std::runtime_error(r.error);
              }
              return { { "started", true }, { "component", component } };
          } });

    registry.Register(
        { "create_platform",
          "Add a build target to the project. Copies an existing platform (its own, "
          "or one from an installed board pack) under a new id, so a board can be "
          "adapted without writing platform JSON by hand. Fields given here override "
          "the copy. Returns the platform as saved.",
          { { "type", "object" },
            { "required", nlohmann::json::array({ "id" }) },
            { "properties",
              { { "id", { { "type", "string" }, { "description", "Id for the new platform" } } },
                { "from",
                  { { "type", "string" },
                    { "description", "Platform id to copy (optional; without it the platform is created bare)" } } },
                { "display_name",
                  { { "type", "string" }, { "description", "Human-readable name (default: the id)" } } },
                { "screen_width", { { "type", "integer" } } },
                { "screen_height", { { "type", "integer" } } },
                { "screen_diagonal_inches",
                  { { "type", "number" },
                    { "description",
                      "The screen's diagonal in inches, as the board's maker gives it; 0 for unknown" } } },
                { "family",
                  { { "type", "string" },
                    { "description", "Vendor family; descriptive only, never used to choose a backend" } } },
                { "framework",
                  { { "type", "string" },
                    { "description", "Build framework id, as claimed by an installed build backend. Any value is "
                                     "accepted; the backend validates it." } } },
                { "external_memory_size",
                  { { "type", "integer" },
                    { "description",
                      "Bytes of memory beyond the internal heap that the engine may allocate from "
                      "(Deki::Memory::External), 0 for none. Grants packages the memory:external capability." } } },
                { "defines",
                  { { "type", "array" },
                    { "items", { { "type", "string" } } },
                    { "description", "Preprocessor defines for the firmware build" } } },
                { "provides",
                  { { "type", "array" },
                    { "items", { { "type", "string" } } },
                    { "description", "Capabilities this board offers packages, e.g. wifi, spi, sdl3" } } },
                { "requires_packages",
                  { { "type", "array" },
                    { "items", { { "type", "string" } } },
                    { "description", "Packages this board always needs, e.g. its HAL" } } },
                { "boot_scene",
                  { { "type", "string" }, { "description", "Scene the device boots, relative to the project" } } },
                { "project_path",
                  { { "type", "string" }, { "description", "Project directory (default: the open project)" } } } } } },
          [editor](const nlohmann::json& args) -> nlohmann::json
          {
              const std::string path = ResolveProjectPath(editor, args);
              if (path.empty())
              {
                  throw std::runtime_error("no project");
              }

              const std::string id = args.value("id", "");
              if (id.empty())
              {
                  throw std::runtime_error("create_platform needs an id");
              }
              std::string why;
              if (!SafeNames::IsSafeName(id, why))
              {
                  throw std::runtime_error("'" + id + "' is not a usable platform id: " + why);
              }

              // Board packs as well as the project's own, so a
              // supported board can be adopted by name.
              auto& registryRef = PlatformRegistry::Instance();
              registryRef.LoadPlatformsFromDirectory(GetPlatformsDirectory(path));
              registryRef.LoadPlatformsFromBoardPacks();

              if (registryRef.GetPlatform(id))
              {
                  throw std::runtime_error("a platform called '" + id + "' already exists");
              }

              PlatformConfig config;
              const std::string from = args.value("from", "");
              if (!from.empty())
              {
                  const PlatformConfig* source = registryRef.GetPlatform(from);
                  if (!source)
                  {
                      throw std::runtime_error("no platform '" + from + "' to copy");
                  }
                  config = *source;
              }

              config.id = id;
              config.displayName = args.value("display_name", config.displayName.empty() ? id : config.displayName);
              if (args.contains("requires_packages") && args["requires_packages"].is_array())
              {
                  config.requiresPackages.clear();
                  for (const auto& r : args["requires_packages"])
                  {
                      if (r.is_string())
                      {
                          config.requiresPackages.push_back(r.get<std::string>());
                      }
                  }
              }
              if (args.contains("provides") && args["provides"].is_array())
              {
                  config.provides.clear();
                  for (const auto& cap : args["provides"])
                  {
                      if (cap.is_string())
                      {
                          config.provides.push_back(cap.get<std::string>());
                      }
                  }
              }
              if (args.contains("screen_width"))
              {
                  config.screenWidth = args["screen_width"].get<int32_t>();
              }
              if (args.contains("screen_height"))
              {
                  config.screenHeight = args["screen_height"].get<int32_t>();
              }
              if (args.contains("screen_diagonal_inches") && args["screen_diagonal_inches"].is_number())
              {
                  config.screenDiagonalInches = (std::max)(0.0f, args["screen_diagonal_inches"].get<float>());
              }
              // family is written as a string everywhere a platform
              // is saved, so it is converted here rather than assigned.
              if (args.contains("family") && args["family"].is_string())
              {
                  // Any name: the family is descriptive and the editor
                  // has no list of every target. It is still checked
                  // like a capability name, so it can be matched.
                  const std::string family = args["family"].get<std::string>();
                  if (!IsValidCapabilityName(family))
                  {
                      throw std::runtime_error("invalid family '" + family +
                                               "'; lowercase letters, digits, '.', '-', ':' and '_' only");
                  }
                  config.family = family;
              }
              config.framework = args.value("framework", config.framework);
              // Framework settings pass through untouched: the
              // editor does not know what any of them mean, and
              // must not model one kind of target (chip, flash,
              // clock) as every platform.
              if (args.contains("framework_options") && args["framework_options"].is_object())
              {
                  config.frameworkOptions = args["framework_options"].dump();
              }
              if (args.contains("external_memory_size"))
              {
                  config.externalMemorySize = args["external_memory_size"].get<uint32_t>();
              }
              if (args.contains("defines") && args["defines"].is_array())
              {
                  config.defines.clear();
                  for (const auto& define : args["defines"])
                  {
                      if (define.is_string())
                      {
                          config.defines.push_back(define.get<std::string>());
                      }
                  }
              }
              config.bootScene = args.value("boot_scene", config.bootScene);

              // Checked by the builder that owns the framework,
              // not here: this tool must not know what "espidf"
              // means. A missing builder is not fatal: a platform
              // for a target whose package is not installed yet
              // is still worth saving.
              BuilderRegistry::Instance().EnsureLoaded();
              if (auto* b = BuilderRegistry::Instance().GetBuilderForPlatform(config))
              {
                  const auto problems = b->ValidatePlatform(config);
                  if (!problems.empty())
                  {
                      std::string msg = "platform is not valid for the " + b->GetFrameworkId() + " builder:";
                      for (const auto& pr : problems)
                      {
                          msg += "\n  " + pr;
                      }
                      throw std::runtime_error(msg);
                  }
              }

              // SavePlatformToFile makes the per-platform folder itself,
              // so it wants platforms/, not platforms/<id>.
              const std::string platformsDir = GetPlatformsDirectory(path);
              std::error_code ec;
              std::filesystem::create_directories(platformsDir, ec);
              if (!registryRef.SavePlatformToFile(config, platformsDir))
              {
                  throw std::runtime_error("could not write the platform under " + platformsDir);
              }
              const std::string directory = platformsDir + "/" + id;

              // A copied platform names the boot scene of the
              // platform it came from, which belongs to that one.
              // Carry the file across so the new target boots.
              if (!from.empty() && !config.bootScene.empty() && !args.contains("boot_scene"))
              {
                  const std::filesystem::path sourceScene = std::filesystem::path(path) / config.bootScene;
                  const std::filesystem::path ours = std::filesystem::path(directory) / sourceScene.filename();
                  if (std::filesystem::exists(sourceScene, ec) && !std::filesystem::equivalent(sourceScene, ours, ec))
                  {
                      std::filesystem::copy_file(sourceScene, ours, std::filesystem::copy_options::overwrite_existing,
                                                 ec);
                      config.bootScene =
                          GetPlatformsDirectoryRelative() + "/" + id + "/" + sourceScene.filename().string();
                      registryRef.SavePlatformToFile(config, directory);
                  }
              }

              return { { "id", config.id },
                       { "displayName", config.displayName },
                       { "framework", config.framework },
                       { "frameworkOptions", config.frameworkOptions },
                       { "screenWidth", config.screenWidth },
                       { "screenHeight", config.screenHeight },
                       { "screenDiagonalInches", config.screenDiagonalInches },
                       { "bootScene", config.bootScene },
                       { "directory", directory } };
          } });

    registry.Register(
        { "set_active_platform",
          "Choose which platform the project builds for by default, the same "
          "setting the Build panel's target dropdown writes.",
          { { "type", "object" },
            { "required", nlohmann::json::array({ "platform_id" }) },
            { "properties",
              { { "platform_id",
                  { { "type", "string" },
                    { "description", "Platform id, or \"\" to leave the project with no default" } } } } } },
          [editor](const nlohmann::json& args) -> nlohmann::json
          {
              auto* project = editor ? editor->GetProject() : nullptr;
              if (!project)
              {
                  throw std::runtime_error("no project open");
              }

              const std::string id = args.value("platform_id", "");
              if (!id.empty())
              {
                  auto& registryRef = PlatformRegistry::Instance();
                  registryRef.LoadPlatformsFromDirectory(GetPlatformsDirectory(project->GetPath()));
                  registryRef.LoadPlatformsFromBoardPacks();
                  RegisterPlatformTemplates(project->GetPath());
                  std::string adoptErr;
                  if (!EnsureProjectPlatform(project->GetPath(), id, adoptErr))
                  {
                      throw std::runtime_error(adoptErr);
                  }
                  if (!registryRef.GetPlatform(id))
                  {
                      throw std::runtime_error("no platform '" + id +
                                               "' in this project, its "
                                               "packages or the installed board packs");
                  }
              }

              project->SetActivePlatformId(id);
              if (!project->Save())
              {
                  throw std::runtime_error("could not save the project");
              }
              editor->ApplyEditorTarget();

              return { { "activePlatformId", id } };
          } });

    // A platform adopted from a package is the project's copy, and remembers
    // what it was copied from (build/PlatformTemplates). list_platforms reports
    // that under "origin"; these two act on it.
    auto syncPlatform = [editor](const nlohmann::json& args, const char* tool, bool restore) -> nlohmann::json
    {
        const std::string path = ResolveProjectPath(editor, args);
        const std::string id = ArgPlatform(args);
        if (id.empty())
        {
            throw std::runtime_error(std::string(tool) + " needs a platform_id");
        }

        const PlatformSyncResult r =
            restore ? RestorePlatformFromPackage(path, id) : UpdatePlatformFromPackage(path, id);
        if (!r.ok)
        {
            throw std::runtime_error(r.error);
        }

        return { { "platformId", id }, { "taken", r.taken }, { "kept", r.kept } };
    };

    registry.Register(
        { "update_platform",
          "Bring a platform adopted from a package up to date with what the package "
          "offers now. Settings and files only the package changed are taken; what "
          "the project changed is kept. Where both changed the same thing the "
          "project's is kept and listed under \"kept\". list_platforms says when an "
          "update is available (origin.canUpdate).",
          { { "type", "object" },
            { "required", nlohmann::json::array({ "platform_id" }) },
            { "properties",
              { { "platform_id", { { "type", "string" }, { "description", "Platform to update" } } },
                { "project_path",
                  { { "type", "string" }, { "description", "Project directory (default: the open project)" } } } } } },
          [syncPlatform](const nlohmann::json& args) -> nlohmann::json
          { return syncPlatform(args, "update_platform", false); } });

    registry.Register(
        { "restore_platform",
          "Replace a platform adopted from a package with the package's version as "
          "it is now: its settings and its files, the boot scene included. The "
          "project's changes to them are lost; files only the project has are left. "
          "\"taken\" lists what changed.",
          { { "type", "object" },
            { "required", nlohmann::json::array({ "platform_id" }) },
            { "properties",
              { { "platform_id", { { "type", "string" }, { "description", "Platform to restore" } } },
                { "project_path",
                  { { "type", "string" }, { "description", "Project directory (default: the open project)" } } } } } },
          [syncPlatform](const nlohmann::json& args) -> nlohmann::json
          { return syncPlatform(args, "restore_platform", true); } });

    // The CLI version of the Package Manager's "Editable (I am developing this
    // package)" checkbox. Without it, a package directory belongs to the
    // editor: PackageIntegrity stashes local edits and resets the checkout to
    // the pinned version every time the project opens, which on the command
    // line is every verb.
    registry.Register(
        { "set_package_editable",
          "Mark a package as one this project is DEVELOPING rather than consuming. "
          "Editable packages are left alone; non-editable ones have local edits "
          "stashed and their checkout reset to the pinned version on every project "
          "open. Persisted as \"editable\" in deki-packages.json.",
          { { "type", "object" },
            { "required", nlohmann::json::array({ "package_id" }) },
            { "properties",
              { { "package_id", { { "type", "string" }, { "description", "Package id" } } },
                { "editable",
                  { { "type", "boolean" },
                    { "description",
                      "true to develop it here (default), false to hand it back to the editor" } } } } } },
          [editor](const nlohmann::json& args) -> nlohmann::json
          {
              auto* project = editor ? editor->GetProject() : nullptr;
              if (!project)
              {
                  throw std::runtime_error("no project open");
              }

              const std::string id = args.value("package_id", "");
              if (id.empty())
              {
                  throw std::runtime_error("package_id is required");
              }
              const bool editable = args.value("editable", true);

              PackageManager manager(editor);
              manager.LoadPackages(project->GetPath());

              // Manifests name packages differently: some by bare
              // directory name, others by "owner/repo". Accept either,
              // plus the id and display name, so the caller does not
              // have to guess.
              PackageEntry* found = nullptr;
              std::vector<std::string> known;
              for (auto& pkg : manager.GetPackages())
              {
                  known.push_back(pkg.name);

                  std::string leaf = pkg.name;
                  const auto slash = leaf.find_last_of('/');
                  if (slash != std::string::npos)
                  {
                      leaf = leaf.substr(slash + 1);
                  }

                  if (pkg.name == id || pkg.id == id || leaf == id || pkg.displayName == id)
                  {
                      found = &pkg;
                      break;
                  }
              }
              if (!found)
              {
                  std::string msg = "no package '" + id + "' in this project; it has:";
                  for (const auto& k : known)
                  {
                      msg += " " + k;
                  }
                  throw std::runtime_error(msg);
              }

              const std::string resolvedId = found->id;
              found->editable = editable;
              manager.SavePackages(project->GetPath());

              // Read it back rather than trust the write. The
              // manifest keeps only some of what it is given, so a
              // flag can be dropped on save, and the caller would
              // then believe a package is protected while the
              // restore pass still stashes its edits.
              PackageManager check(editor);
              check.LoadPackages(project->GetPath());
              for (const auto& pkg : check.GetPackages())
              {
                  if (pkg.id != resolvedId)
                  {
                      continue;
                  }
                  if (pkg.editable != editable)
                  {
                      throw std::runtime_error("'" + resolvedId + "' did not keep editable=" +
                                               (editable ? "true" : "false") + " across a reload");
                  }
                  break;
              }

              return { { "packageId", id }, { "editable", editable } };
          } });

    // Capabilities are how a package decides whether it can build for a target,
    // so they have to be settable without opening platform JSON by hand.
    registry.Register(
        { "set_platform_provides",
          "Set what a platform can offer packages, as capability names "
          "(\"desktop-os\", \"sdl3\", \"posix-process\", \"wifi\", \"spi\"). A package "
          "declares what it needs in its platformRequires and builds for this target "
          "only when everything it needs is offered here. \"framework:<x>\" and "
          "\"family:<x>\" are added automatically and need not be listed.",
          { { "type", "object" },
            { "required", nlohmann::json::array({ "platform_id", "provides" }) },
            { "properties",
              { { "platform_id", { { "type", "string" }, { "description", "Platform to change" } } },
                { "provides",
                  { { "type", "array" },
                    { "items", { { "type", "string" } } },
                    { "description", "The complete capability list; replaces what was there" } } },
                { "requires_packages",
                  { { "type", "array" },
                    { "items", { { "type", "string" } } },
                    { "description",
                      "Packages this board always needs, e.g. its HAL. Replaces what was there when given." } } },
                { "external_memory_size",
                  { { "type", "integer" },
                    { "description",
                      "Bytes of memory beyond the internal heap the engine may allocate from (Deki::Memory::External), "
                      "0 for none. Grants packages the memory:external capability. Unchanged when omitted." } } },
                { "display_name",
                  { { "type", "string" }, { "description", "Human-readable name. Unchanged when omitted." } } },
                { "screen_width",
                  { { "type", "integer" },
                    { "description", "Screen width in pixels, the size the game renders and exports assets at. Give "
                                     "with screen_height. Unchanged when omitted." } } },
                { "screen_height",
                  { { "type", "integer" },
                    { "description", "Screen height in pixels. Give with screen_width. Unchanged when omitted." } } },
                { "screen_diagonal_inches",
                  { { "type", "number" },
                    { "description", "The screen's diagonal in inches, as the board's maker gives it; 0 for unknown. "
                                     "Scenes use it to lay out for small screens. Unchanged when omitted." } } },
                { "framework_options",
                  { { "type", "object" },
                    { "description",
                      "Settings for the platform's build backend, merged key by key into what it already has. The "
                      "editor does not interpret them; the backend validates them when it builds." } } },
                { "cxx_flags",
                  { { "type", "array" },
                    { "items", { { "type", "string" } } },
                    { "description", "C++ compiler flags for this platform's build, e.g. \"-O0\". Replaces what was "
                                     "there when given." } } },
                { "asset_storage",
                  { { "type", "string" },
                    { "enum", nlohmann::json::array({ "internal", "external" }) },
                    { "description", "Where builds keep the assets: internal (flashed with the game) or external (an "
                                     "SD card; needs the external-storage capability). Unchanged when omitted." } } },
                { "project_path",
                  { { "type", "string" }, { "description", "Project directory (default: the open project)" } } } } } },
          [editor](const nlohmann::json& args) -> nlohmann::json
          {
              const std::string path = ResolveProjectPath(editor, args);
              if (path.empty())
              {
                  throw std::runtime_error("no project");
              }

              const std::string id = args.value("platform_id", "");
              if (id.empty())
              {
                  throw std::runtime_error("set_platform_provides needs a platform_id");
              }

              auto& registryRef = PlatformRegistry::Instance();
              registryRef.LoadPlatformsFromDirectory(GetPlatformsDirectory(path));
              registryRef.LoadPlatformsFromBoardPacks();

              const PlatformConfig* existing = registryRef.GetPlatform(id);
              if (!existing)
              {
                  throw std::runtime_error("no platform '" + id + "' in this project");
              }

              PlatformConfig config = *existing;
              config.provides.clear();
              if (args.contains("provides") && args["provides"].is_array())
              {
                  for (const auto& cap : args["provides"])
                  {
                      if (cap.is_string())
                      {
                          config.provides.push_back(cap.get<std::string>());
                      }
                  }
              }

              if (args.contains("requires_packages") && args["requires_packages"].is_array())
              {
                  config.requiresPackages.clear();
                  for (const auto& r : args["requires_packages"])
                  {
                      if (r.is_string())
                      {
                          config.requiresPackages.push_back(r.get<std::string>());
                      }
                  }
              }
              // The other settings create_platform takes, so an existing
              // platform can be changed through the CLI instead of by
              // editing its JSON. create_platform only makes new ones.
              if (args.contains("external_memory_size"))
              {
                  if (!args["external_memory_size"].is_number_unsigned() &&
                      !(args["external_memory_size"].is_number_integer() &&
                        args["external_memory_size"].get<long long>() >= 0))
                  {
                      throw std::runtime_error("external_memory_size must be a non-negative byte count");
                  }
                  config.externalMemorySize = args["external_memory_size"].get<uint32_t>();
              }
              if (args.contains("display_name") && args["display_name"].is_string() &&
                  !args["display_name"].get<std::string>().empty())
              {
                  config.displayName = args["display_name"].get<std::string>();
              }
              if (args.contains("screen_width") || args.contains("screen_height"))
              {
                  // Both or neither: half a resolution change leaves a
                  // platform that matches no screen.
                  auto dimension = [&args](const char* key) -> int32_t
                  {
                      if (!args.contains(key) || !args[key].is_number_integer() || args[key].get<long long>() <= 0 ||
                          args[key].get<long long>() > 16384)
                      {
                          throw std::runtime_error("screen_width and screen_height are given together, as pixel counts "
                                                   "from 1 to 16384");
                      }
                      return args[key].get<int32_t>();
                  };
                  config.screenWidth = dimension("screen_width");
                  config.screenHeight = dimension("screen_height");
              }
              if (args.contains("screen_diagonal_inches"))
              {
                  if (!args["screen_diagonal_inches"].is_number() || args["screen_diagonal_inches"].get<double>() < 0.0)
                  {
                      throw std::runtime_error("screen_diagonal_inches is a number of inches, 0 for unknown");
                  }
                  config.screenDiagonalInches = args["screen_diagonal_inches"].get<float>();
              }
              if (args.contains("framework_options") && args["framework_options"].is_object())
              {
                  nlohmann::json options = nlohmann::json::parse(
                      config.frameworkOptions.empty() ? std::string("{}") : config.frameworkOptions, nullptr,
                      /*allow_exceptions*/ false);
                  if (!options.is_object())
                  {
                      options = nlohmann::json::object();
                  }
                  for (auto it = args["framework_options"].begin(); it != args["framework_options"].end(); ++it)
                  {
                      options[it.key()] = it.value();
                  }
                  config.frameworkOptions = options.dump();
              }
              if (args.contains("cxx_flags"))
              {
                  if (!args["cxx_flags"].is_array())
                  {
                      throw std::runtime_error("cxx_flags is a list of flags");
                  }
                  config.cxxFlags.clear();
                  for (const auto& flag : args["cxx_flags"])
                  {
                      if (!flag.is_string())
                      {
                          throw std::runtime_error("cxx_flags is a list of flags");
                      }
                      config.cxxFlags.push_back(flag.get<std::string>());
                  }
              }
              if (args.contains("asset_storage"))
              {
                  const std::string storage = args.value("asset_storage", "");
                  if (storage != "internal" && storage != "external")
                  {
                      throw std::runtime_error("asset_storage is internal or external");
                  }
                  config.assetStorage = storage;
              }
              if (config.assetStorage == "external" && config.Capabilities().count("external-storage") == 0)
              {
                  throw std::runtime_error("'" + id +
                                           "' keeps its assets on external storage, so it "
                                           "must provide \"external-storage\"");
              }

              registryRef.RegisterPlatform(config);
              if (!registryRef.SavePlatformToFile(config, GetPlatformsDirectory(path)))
              {
                  throw std::runtime_error("could not write the platform file for '" + id + "'");
              }

              nlohmann::json caps = nlohmann::json::array();
              for (const auto& c : config.Capabilities())
              {
                  caps.push_back(c);
              }
              return { { "platformId", id },
                       { "provides", config.provides },
                       { "requiresPackages", config.requiresPackages },
                       { "capabilities", caps },
                       { "screenWidth", config.screenWidth },
                       { "screenHeight", config.screenHeight },
                       { "screenDiagonalInches", config.screenDiagonalInches },
                       { "cxxFlags", config.cxxFlags },
                       { "assetStorage", config.assetStorage } };
          } });

    // Per-target content. "requires" is read at build time: the exporter drops
    // the object for targets that cannot satisfy it, and the runtime never
    // sees it. So this edits the scene file, not the loaded scene.
    registry.Register(
        { "set_object_requires",
          "Say which platform capabilities an object needs, so it exists only on "
          "targets that offer them (\"wifi\", \"desktop-os\"). One project can then "
          "serve a desktop and a board, with an object present on one and absent "
          "from the other, instead of the build failing or shipping something dead. "
          "An empty list clears it, putting the object back on every target.",
          { { "type", "object" },
            { "required", nlohmann::json::array({ "scene_path", "object_id", "requires" }) },
            { "properties",
              { { "scene_path", { { "type", "string" }, { "description", "Scene file, relative to the project" } } },
                { "object_id", { { "type", "integer" }, { "description", "Object id, as shown by list_objects" } } },
                { "requires",
                  { { "type", "array" },
                    { "items", { { "type", "string" } } },
                    { "description", "Capabilities the object needs; empty clears" } } },
                { "project_path",
                  { { "type", "string" }, { "description", "Project directory (default: the open project)" } } } } } },
          [editor](const nlohmann::json& args) -> nlohmann::json
          {
              namespace fs = std::filesystem;
              const std::string path = ResolveProjectPath(editor, args);
              const std::string rel = args.value("scene_path", "");
              if (rel.empty())
              {
                  throw std::runtime_error("set_object_requires needs a scene_path");
              }

              fs::path scenePath = fs::path(path) / rel;
              if (!fs::exists(scenePath))
              {
                  throw std::runtime_error("no scene at " + scenePath.string());
              }

              nlohmann::json scene;
              {
                  std::ifstream in(scenePath);
                  in >> scene;
              }

              const int64_t wantedId = args.value("object_id", (int64_t)-1);
              std::vector<std::string> needs;
              if (args.contains("requires") && args["requires"].is_array())
              {
                  for (const auto& n : args["requires"])
                  {
                      if (n.is_string())
                      {
                          needs.push_back(n.get<std::string>());
                      }
                  }
              }

              // Objects nest through "children", so this walks.
              bool found = false;
              std::function<void(nlohmann::json&)> visit = [&](nlohmann::json& node)
              {
                  if (found)
                  {
                      return;
                  }
                  if (node.is_array())
                  {
                      for (auto& item : node)
                      {
                          visit(item);
                      }
                      return;
                  }
                  if (!node.is_object())
                  {
                      return;
                  }
                  // Components carry an "id" as well, so an object
                  // is the node that holds components or children.
                  const bool isObject = node.contains("components") || node.contains("children");
                  if (isObject && node.contains("id") && node["id"].is_number_integer() &&
                      node["id"].get<int64_t>() == wantedId)
                  {
                      if (needs.empty())
                      {
                          node.erase("requires");
                      }
                      else
                      {
                          node["requires"] = needs;
                      }
                      found = true;
                      return;
                  }
                  for (auto& entry : node.items())
                  {
                      visit(entry.value());
                  }
              };
              visit(scene);

              if (!found)
              {
                  throw std::runtime_error("no object with id " + std::to_string(wantedId) + " in " + rel);
              }

              {
                  std::ofstream out(scenePath);
                  out << scene.dump(2) << "\n";
              }

              return { { "scene", rel }, { "objectId", wantedId }, { "requires", needs } };
          } });

    registry.Register(
        { "toolchain_status",
          "Report the toolchain components of a platform's build backend, "
          "including install state and versions.",
          { { "type", "object" },
            { "properties",
              { { "project_path",
                  { { "type", "string" }, { "description", "Project directory (default: the open project)" } } },
                { "platform_id",
                  { { "type", "string" },
                    { "description", "Platform id (default: the project's activePlatformId)" } } } } } },
          [editor](const nlohmann::json& args) -> nlohmann::json
          {
              const std::string path = ResolveProjectPath(editor, args);
              std::string err;
              nlohmann::json result = FirmwareBuildService::ToolchainStatus(path, ArgPlatform(args), err);
              if (!err.empty())
              {
                  throw std::runtime_error(err);
              }
              return result;
          } });

    // =========================================================================
    // Long operations (start, then poll)
    // =========================================================================

    registry.Register(
        { "build_firmware",
          "Start a firmware build for the project. Regenerates build files first. "
          "Returns as soon as the build starts - poll build_status for progress "
          "and the final result. Fails immediately if the toolchain is missing.",
          { { "type", "object" },
            { "properties",
              { { "project_path",
                  { { "type", "string" }, { "description", "Project directory (default: the open project)" } } },
                { "platform_id",
                  { { "type", "string" },
                    { "description", "Platform id (default: the project's activePlatformId)" } } },
                { "simulate",
                  { { "type", "boolean" },
                    { "description", "Build the board's firmware for its backend's simulator (QEMU for ESP32), in its "
                                     "own build directory; the board's boot scene is not changed" } } } } } },
          [editor](const nlohmann::json& args) -> nlohmann::json
          {
              const std::string path = ResolveProjectPath(editor, args);
              BuildOptions options;
              options.simulate = args.value("simulate", false);
              // The editor, so the build gets its boot payload and the
              // scene-scanned defines, as the CLI and the Build panel do.
              FirmwareOpResult r = FirmwareBuildService::StartBuild(path, ArgPlatform(args), ActivityLines("Build"),
                                                                    editor, options, ActivityDone("Build"));
              if (!r.success)
              {
                  throw std::runtime_error(r.error);
              }

              nlohmann::json j = OpToJson(r);
              j["started"] = true;
              j["note"] = "Build running in background - poll build_status.";
              return j;
          } });

    registry.Register(
        { "build_status",
          "Progress of the most recently started build, flash, or toolchain install or setup. state is "
          "idle|building|deploying|installing|completed|failed; active is true while the worker thread is "
          "running. output is the operation's last lines (error lines start \"error: \"): a failed build's "
          "reason, such as the compiler error, is there.",
          { { "type", "object" },
            { "properties",
              { { "output_lines",
                  { { "type", "integer" },
                    { "description", "How many of the last output lines to return (default 20, at most 500; 0 for "
                                     "none)" } } } } } },
          [](const nlohmann::json& args) -> nlohmann::json
          {
              const int lines = args.value("output_lines", 20);
              return FirmwareBuildService::QueryStatus(static_cast<size_t>(std::clamp(lines, 0, 500)));
          } });

    registry.Register({ "cancel_build",
                        "Request cancellation of the in-flight build or flash. Returns "
                        "cancelled=false when nothing is running.",
                        { { "type", "object" }, { "properties", nlohmann::json::object() } },
                        [](const nlohmann::json&) -> nlohmann::json
                        { return nlohmann::json{ { "cancelled", FirmwareBuildService::CancelActive() } }; } });

    registry.Register(
        { "install_toolchain_component",
          "Download and install a toolchain component by id (see toolchain_status "
          "for valid ids). Runs the component's post-install step "
          "when it declares one. This downloads gigabytes and can take a long time.",
          { { "type", "object" },
            { "properties",
              { { "component_id", { { "type", "string" }, { "description", "Component id, from toolchain_status" } } },
                { "project_path",
                  { { "type", "string" }, { "description", "Project directory (default: the open project)" } } },
                { "platform_id",
                  { { "type", "string" },
                    { "description", "Platform id (default: the project's activePlatformId)" } } } } },
            { "required", nlohmann::json::array({ "component_id" }) } },
          [editor](const nlohmann::json& args) -> nlohmann::json
          {
              const std::string path = ResolveProjectPath(editor, args);
              const std::string componentId = args["component_id"].get<std::string>();

              // Blocking would stall the editor's frame loop for the length of a
              // multi-GB download, so this starts the installer and returns.
              FirmwareOpResult r =
                  FirmwareBuildService::StartInstallToolchainComponent(path, ArgPlatform(args), componentId, nullptr);
              if (!r.success)
              {
                  throw std::runtime_error(r.error);
              }

              nlohmann::json j = OpToJson(r);
              j["componentId"] = componentId;
              j["started"] = true;
              j["note"] = "Install running in background - poll build_status "
                          "(state \"installing\").";
              return j;
          } });

    // =========================================================================
    // Flash / clean
    // =========================================================================

    registry.Register(
        { "deploy_build",
          "Deploy the last build the way the platform's backend defines it: flash a "
          "device, run the executable, install an app. Refused when the backend has "
          "no deploy step. Returns once it starts - poll build_status for the result.",
          { { "type", "object" },
            { "properties",
              { { "project_path",
                  { { "type", "string" }, { "description", "Project directory (default: the open project)" } } },
                { "platform_id",
                  { { "type", "string" },
                    { "description", "Platform id (default: the project's activePlatformId)" } } },
                { "target",
                  { { "type", "string" },
                    { "description", "What to deploy to, in the backend's own terms (a serial port for a board that is "
                                     "flashed); omit to let the backend choose" } } },
                { "simulate",
                  { { "type", "boolean" },
                    { "description",
                      "Run the simulated build (build_firmware with simulate) in the backend's simulator" } } } } } },
          [editor](const nlohmann::json& args) -> nlohmann::json
          {
              const std::string path = ResolveProjectPath(editor, args);
              const std::string target = args.value("target", std::string());
              BuildOptions options;
              options.simulate = args.value("simulate", false);
              const std::string verb = DeployVerb(editor, args);
              FirmwareOpResult r = FirmwareBuildService::StartDeploy(path, ArgPlatform(args), target,
                                                                     ActivityLines(verb), options, ActivityDone(verb));
              if (!r.success)
              {
                  throw std::runtime_error(r.error);
              }

              nlohmann::json j = OpToJson(r);
              j["started"] = true;
              j["note"] = "Deploy running in background - poll build_status.";
              return j;
          } });

    // What the board itself shows, fetched over its serial port: the one way
    // to see what the hardware drew (DeviceScreenshot.h has the protocol).
    registry.Register(
        { "device_screenshot",
          "Save the frame a connected board is showing to a PNG, fetched over its serial port. The firmware must "
          "answer the screen capture request (the ESP32 HAL does, from the build that added it). Returns once the "
          "file is written, about a second for 320x240. Close the Serial Monitor first if it has the port open.",
          { { "type", "object" },
            { "required", nlohmann::json::array({ "path" }) },
            { "properties",
              { { "path", { { "type", "string" }, { "description", "PNG file to write" } } },
                { "port",
                  { { "type", "string" },
                    { "description", "Serial port (COM3, /dev/ttyACM0); default: the only one there is" } } },
                { "timeout_ms",
                  { { "type", "integer" },
                    { "description", "How long to wait for the frame (default 15000)" } } } } } },
          [](const nlohmann::json& args) -> nlohmann::json
          {
              const std::string path = args.value("path", std::string());
              if (path.empty())
              {
                  throw std::runtime_error("device_screenshot needs a path");
              }
              const std::string port = BoardPort(args);
              const int timeoutMs = std::clamp(args.value("timeout_ms", 15000), 1000, 55000);

              // The serial exchange blocks for about a second, so it runs on
              // its own thread and the call is answered once it is done.
              struct Job
              {
                  std::mutex mutex;
                  bool done = false;
                  bool ok = false;
                  std::string error;
                  DeviceFrame frame;
                  double seconds = 0.0;
              };
              auto job = std::make_shared<Job>();
              std::thread(
                  [job, port, timeoutMs]()
                  {
                      const auto t0 = std::chrono::steady_clock::now();
                      DeviceFrame frame;
                      std::string error;
                      const bool ok = CaptureDeviceScreenshot(port, timeoutMs, frame, error);
                      std::lock_guard<std::mutex> lock(job->mutex);
                      job->ok = ok;
                      job->error = error;
                      job->frame = std::move(frame);
                      job->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                      job->done = true;
                  })
                  .detach();

              McpCompletion::Defer(
                  [job, path, port]() -> std::optional<nlohmann::json>
                  {
                      std::lock_guard<std::mutex> lock(job->mutex);
                      if (!job->done)
                      {
                          return std::nullopt;
                      }
                      if (!job->ok)
                      {
                          throw std::runtime_error(job->error);
                      }
                      std::error_code ec;
                      const std::filesystem::path parent = std::filesystem::path(path).parent_path();
                      if (!parent.empty())
                      {
                          std::filesystem::create_directories(parent, ec);
                      }
                      const DeviceFrame& f = job->frame;
                      if (!stbi_write_png(path.c_str(), f.width, f.height, 4, f.rgba.data(), f.width * 4))
                      {
                          throw std::runtime_error("could not write " + path);
                      }
                      return nlohmann::json{
                          { "path", path },       { "port", port },       { "width", f.width },
                          { "height", f.height }, { "format", f.format }, { "seconds", job->seconds }
                      };
                  });
              return { { "status", "fetching" }, { "port", port } };
          } });

    registry.Register(
        { "device_stats",
          "Frame statistics from a connected board, averaged over the last second: fps, frameMs, and the engine's "
          "split of a frame (updateMs, renderMs, presentMs, whole milliseconds averaged), free internal and "
          "external memory, and how frames reach the screen: frameMode \"whole\" or \"strips\" with bandRows, "
          "the strip height (the Rendering setting frameMode; Auto picks whole frames when the framebuffer fits "
          "in internal RAM, strips when not). The firmware must answer the request (the ESP32 HAL does, from the build "
          "that added "
          "it). Close the Serial Monitor first if it has the port open.",
          { { "type", "object" },
            { "properties",
              { { "port",
                  { { "type", "string" },
                    { "description", "Serial port (COM3, /dev/ttyACM0); default: the only one there is" } } } } } },
          [](const nlohmann::json& args) -> nlohmann::json
          {
              const std::string port = BoardPort(args);
              struct Job
              {
                  std::mutex mutex;
                  bool done = false;
                  bool ok = false;
                  std::string error;
                  std::map<std::string, std::string> stats;
              };
              auto job = std::make_shared<Job>();
              std::thread(
                  [job, port]()
                  {
                      std::map<std::string, std::string> stats;
                      std::string error;
                      const bool ok = QueryDeviceStats(port, 5000, stats, error);
                      std::lock_guard<std::mutex> lock(job->mutex);
                      job->ok = ok;
                      job->error = error;
                      job->stats = std::move(stats);
                      job->done = true;
                  })
                  .detach();
              McpCompletion::Defer(
                  [job, port]() -> std::optional<nlohmann::json>
                  {
                      std::lock_guard<std::mutex> lock(job->mutex);
                      if (!job->done)
                      {
                          return std::nullopt;
                      }
                      if (!job->ok)
                      {
                          throw std::runtime_error(job->error);
                      }
                      nlohmann::json out{ { "port", port } };
                      for (const auto& [name, value] : job->stats)
                      {
                          // Numbers as numbers.
                          char* end = nullptr;
                          const double number = std::strtod(value.c_str(), &end);
                          if (end && *end == 0 && !value.empty())
                          {
                              out[name] = number;
                          }
                          else
                          {
                              out[name] = value;
                          }
                      }
                      return out;
                  });
              return { { "status", "asking" }, { "port", port } };
          } });

    registry.Register(
        { "device_memory",
          "Where a connected board's memory is, measured: internal and external RAM in use by the engine, their "
          "peaks, the heap's free space, lowest free space since boot and size, and every call site that allocated "
          "through the engine with its bytes now, peak and blocks, grouped into categories (frame buffer, display "
          "driver, images, fonts, tilemaps, ...). The firmware must answer DEKI:MEMORY (the ESP32 HAL does from "
          "0.22). Close the Serial Monitor first if it has the port open.",
          { { "type", "object" },
            { "properties",
              { { "port",
                  { { "type", "string" },
                    { "description", "Serial port (COM3, /dev/ttyACM0); default: the only one there is" } } } } } },
          [](const nlohmann::json& args) -> nlohmann::json
          {
              const std::string port = BoardPort(args);
              struct Job
              {
                  std::mutex mutex;
                  bool done = false;
                  bool ok = false;
                  std::string error;
                  MemoryReport report;
              };
              auto job = std::make_shared<Job>();
              std::thread(
                  [job, port]()
                  {
                      MemoryReport report;
                      std::string error;
                      const bool ok = QueryDeviceMemory(port, 5000, report, error);
                      std::lock_guard<std::mutex> lock(job->mutex);
                      job->ok = ok;
                      job->error = error;
                      job->report = std::move(report);
                      job->done = true;
                  })
                  .detach();
              McpCompletion::Defer(
                  [job, port]() -> std::optional<nlohmann::json>
                  {
                      std::lock_guard<std::mutex> lock(job->mutex);
                      if (!job->done)
                      {
                          return std::nullopt;
                      }
                      if (!job->ok)
                      {
                          throw std::runtime_error(job->error);
                      }
                      nlohmann::json out = MemoryReportJson(job->report);
                      out["port"] = port;
                      return out;
                  });
              return { { "status", "asking" }, { "port", port } };
          } });

    registry.Register(
        { "memory_report",
          "What memory the open scene takes. source \"scene\" (default) estimates it on a target without running "
          "it: the frame buffer or render strip, every asset its components name (an image at the size and format "
          "it is stored in there, other assets at their built file's size) and every object, against the target's "
          "RAM. source \"play\" measures this editor's own process by call site, as Play mode runs it, with what "
          "was held when Play started taken off when since_play is true.",
          { { "type", "object" },
            { "properties",
              { { "source", { { "type", "string" }, { "enum", { "scene", "play" } } } },
                { "target",
                  { { "type", "string" },
                    { "description", "Platform id for the estimate; default: the project's active one, \"\" for "
                                     "the editor's desktop" } } },
                { "since_play", { { "type", "boolean" } } },
                { "limit",
                  { { "type", "integer" }, { "description", "Rows per list, biggest first; default 20" } } } } } },
          [editor](const nlohmann::json& args) -> nlohmann::json
          {
              const size_t limit = static_cast<size_t>(std::max(1, args.value("limit", 20)));
              if (args.value("source", std::string("scene")) == "play")
              {
                  MemoryReport report = CaptureLocalMemory();
                  const MemoryReport* start = PlayStartMemory();
                  if (args.value("since_play", true) && editor->IsPlaying() && start)
                  {
                      report = SincePlayStart(report, *start);
                  }
                  nlohmann::json out = MemoryReportJson(report);
                  out["playing"] = editor->IsPlaying();
                  if (out["sites"].size() > limit)
                  {
                      out["sites"].erase(out["sites"].begin() + static_cast<long>(limit), out["sites"].end());
                  }
                  return out;
              }

              Project* project = editor->GetProject();
              if (!project)
              {
                  throw std::runtime_error("No project is open");
              }
              const std::string targetId =
                  args.contains("target") ? args["target"].get<std::string>() : project->GetActivePlatformId();
              const PlatformConfig* platform =
                  targetId.empty() ? nullptr : PlatformRegistry::Instance().GetPlatform(targetId);
              if (!targetId.empty() && !platform)
              {
                  throw std::runtime_error("No platform '" + targetId + "'");
              }
              const MemoryPanel* panel = editor->GetMemoryPanel();
              const MemoryReport* board = panel ? panel->LastDeviceReport() : nullptr;
              const SceneMemoryEstimate e =
                  EstimateSceneMemory(project->GetPath(), editor->GetAssetPipeline(), editor->GetCurrentScene(),
                                      platform, board ? board->internalTotal : -1, board ? board->internalFree : -1);

              auto item = [](const MemoryEstimateItem& i)
              {
                  return nlohmann::json{ { "name", i.name },   { "kind", i.kind },         { "detail", i.detail },
                                         { "bytes", i.bytes }, { "external", i.external }, { "fileSize", i.fileSize },
                                         { "users", i.users } };
              };
              nlohmann::json rendering = nlohmann::json::array();
              for (const auto& r : e.rendering)
              {
                  rendering.push_back(item(r));
              }
              nlohmann::json assets = nlohmann::json::array();
              for (size_t i = 0; i < e.assets.size() && i < limit; ++i)
              {
                  assets.push_back(item(e.assets[i]));
              }
              nlohmann::json objects = nlohmann::json::array();
              for (size_t i = 0; i < e.objects.size() && i < limit; ++i)
              {
                  const auto& o = e.objects[i];
                  objects.push_back({ { "path", o.path },
                                      { "ownBytes", o.ownBytes },
                                      { "assetBytes", o.assetBytes },
                                      { "components", o.components } });
              }
              return { { "estimate", true },
                       { "target", e.targetId },
                       { "targetName", e.targetName },
                       { "frameMode", e.frameModeNote },
                       { "internalBytes", e.internalBytes },
                       { "externalBytes", e.externalBytes },
                       { "internalBudget", e.internalBudget },
                       { "internalBudgetMeasured", e.internalBudgetMeasured },
                       { "externalBudget", e.externalBudget },
                       { "objectBytes", e.objectBytes },
                       { "objectCount", e.objects.size() },
                       { "assetCount", e.assets.size() },
                       { "rendering", rendering },
                       { "assets", assets },
                       { "objects", objects } };
          } });

    registry.Register(
        { "clean_build",
          "Delete the builder's build directory for the platform, forcing a full "
          "rebuild next time.",
          { { "type", "object" },
            { "properties",
              { { "project_path",
                  { { "type", "string" }, { "description", "Project directory (default: the open project)" } } },
                { "platform_id",
                  { { "type", "string" },
                    { "description", "Platform id (default: the project's activePlatformId)" } } } } } },
          [editor](const nlohmann::json& args) -> nlohmann::json
          {
              const std::string path = ResolveProjectPath(editor, args);
              FirmwareOpResult r = FirmwareBuildService::Clean(path, ArgPlatform(args), nullptr);
              if (!r.success)
              {
                  throw std::runtime_error(r.error);
              }
              return OpToJson(r);
          } });
}

}  // namespace DekiEditor
