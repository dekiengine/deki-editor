// McpBuildTools — MCP tools for firmware build / flash / toolchain management.
//
// Handlers run on the editor's frame loop (McpServer::ProcessMessages), so the
// long-running operations are start-and-poll rather than blocking: build_firmware
// returns once the build thread is up, and build_status reports progress. Only
// the cheap, bounded operations (listing, status, cancel) answer synchronously.
//
// Toolchain installs download gigabytes, so install_toolchain_component is
// start-and-poll too, sharing build_status via the builder's progress state.

#include "McpTools.h"
#include "McpToolRegistry.h"
#include "../EditorApp.h"
#include "../core/Project.h"
#include "../services/FirmwareBuildService.h"
#include <deki-editor/build/PlatformConfig.h>
#include <deki-editor/build/BuilderRegistry.h>
#include <deki-editor/build/TargetBuilder.h>
#include <deki-editor/SafeNames.h>
#include "../services/PackageManager.h"
#include <deki-editor/Paths.h>

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
        return fromArgs;

    if (editor)
    {
        if (auto* project = editor->GetProject())
            return project->GetPath();
    }

    throw std::runtime_error("No project open and no project_path supplied.");
}

std::string ArgPlatform(const nlohmann::json& args)
{
    return args.value("platform_id", std::string());
}

nlohmann::json OpToJson(const FirmwareOpResult& r)
{
    nlohmann::json j;
    j["success"] = r.success;
    j["platformId"] = r.platformId;
    j["platformName"] = r.platformName;
    if (!r.error.empty())
        j["error"] = r.error;
    return j;
}

}  // namespace

void RegisterBuildTools(McpToolRegistry& registry, EditorApp* editor)
{
    // =========================================================================
    // Discovery
    // =========================================================================

    registry.Register({ "list_platforms",
                        "List build platforms available to the project, with each platform's "
                        "builder and whether its toolchain is installed. Also reports the "
                        "project's active platform.",
                        { { "type", "object" }, { "properties", { { "project_path", { { "type", "string" }, { "description", "Project directory (default: the open project)" } } } } } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            const std::string path = ResolveProjectPath(editor, args);
                            std::string err;
                            nlohmann::json result = FirmwareBuildService::ListPlatforms(path, err);
                            if (!err.empty())
                                throw std::runtime_error(err);
                            return result;
                        } });

    registry.Register({ "wait_for_operation",
                        "Block until the running firmware operation finishes, and report how it "
                        "went. build_firmware, deploy_build and the toolchain tools all start "
                        "in the background and return at once, which suits a GUI polling for "
                        "progress but leaves a script with nothing to wait on: the process exits "
                        "and takes the work with it.",
                        { { "type", "object" },
                          { "properties",
                            { { "timeout_seconds", { { "type", "integer" }, { "description", "Give up after this long (default 3600). Giving up does not cancel the operation." } } },
                              { "quiet", { { "type", "boolean" }, { "description", "Do not print progress lines while waiting" } } } } } },
                        [](const nlohmann::json& args) -> nlohmann::json
                        {
                            const int timeoutSeconds = args.value("timeout_seconds", 3600);
                            const bool quiet = args.value("quiet", false);

                            const auto started = std::chrono::steady_clock::now();
                            std::string lastText;
                            nlohmann::json status;

                            while (true)
                            {
                                status = FirmwareBuildService::QueryStatus();
                                const std::string state = status.value("state", "idle");
                                const std::string text = status.value("statusText", "");

                                if (!quiet && !text.empty() && text != lastText)
                                {
                                    // stderr: stdout carries the result, and a
                                    // caller parsing it should not have to strip
                                    // progress out of the middle.
                                    fprintf(stderr, "  %s\n", text.c_str());
                                    lastText = text;
                                }

                                if (!status.value("active", false) || state == "completed" ||
                                    state == "failed" || state == "idle")
                                    break;

                                const auto elapsed = std::chrono::steady_clock::now() - started;
                                if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >=
                                    timeoutSeconds)
                                {
                                    status["timedOut"] = true;
                                    break;
                                }
                                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                            }

                            if (status.value("state", "") == "failed")
                                throw std::runtime_error("operation failed: " +
                                                         status.value("error", "(no detail)"));
                            return status;
                        } });

    registry.Register({ "setup_toolchain_component",
                        "Run a toolchain component's post-install step. Downloading a component "
                        "is not always enough to use it: the ESP-IDF archive carries no compilers, "
                        "and its install script fetches them. Starts in the background; poll "
                        "build_status. Only components whose status reports canSetup have one.",
                        { { "type", "object" },
                          { "required", nlohmann::json::array({ "component_id" }) },
                          { "properties",
                            { { "component_id", { { "type", "string" }, { "description", "Component id, e.g. esp-idf" } } },
                              { "platform_id", { { "type", "string" }, { "description", "Platform id (default: the project's activePlatformId)" } } },
                              { "project_path", { { "type", "string" }, { "description", "Project directory (default: the open project)" } } } } } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            const std::string path = ResolveProjectPath(editor, args);
                            const std::string component = args.value("component_id", "");
                            if (component.empty())
                                throw std::runtime_error("setup_toolchain_component needs component_id");

                            FirmwareOpResult r = FirmwareBuildService::StartSetupToolchainComponent(
                                path, ArgPlatform(args), component, nullptr);
                            if (!r.success)
                                throw std::runtime_error(r.error);
                            return { { "started", true }, { "component", component } };
                        } });

    registry.Register({ "create_platform",
                        "Add a build target to the project. Copies an existing platform (its own, "
                        "or one from an installed board pack) under a new id, so a board can be "
                        "adapted without writing platform JSON by hand. Fields given here override "
                        "the copy. Returns the platform as saved.",
                        { { "type", "object" },
                          { "required", nlohmann::json::array({ "id" }) },
                          { "properties",
                            { { "id", { { "type", "string" }, { "description", "Id for the new platform" } } },
                              { "from", { { "type", "string" }, { "description", "Platform id to copy (optional; without it the platform is created bare)" } } },
                              { "display_name", { { "type", "string" }, { "description", "Human-readable name (default: the id)" } } },
                              { "screen_width", { { "type", "integer" } } },
                              { "screen_height", { { "type", "integer" } } },
                              { "family", { { "type", "string" }, { "description", "Vendor family, e.g. espressif or native" } } },
                              { "framework", { { "type", "string" }, { "description", "Build framework id, as claimed by an installed build backend. Any value is accepted; the backend validates it." } } },
                              { "mcu_chip", { { "type", "string" }, { "description", "Chip id, e.g. esp32s3 or rp2040" } } },
                              { "cpu_freq_hz", { { "type", "integer" } } },
                              { "flash_size", { { "type", "integer" }, { "description", "Flash in bytes" } } },
                              { "psram_size", { { "type", "integer" }, { "description", "PSRAM in bytes, 0 for none" } } },
                              { "psram_mode", { { "type", "string" }, { "description", "quad or oct" } } },
                              { "defines", { { "type", "array" }, { "items", { { "type", "string" } } }, { "description", "Preprocessor defines for the firmware build" } } },
                              { "provides", { { "type", "array" }, { "items", { { "type", "string" } } }, { "description", "Capabilities this board offers packages, e.g. wifi, spi, sdl3" } } },
                              { "requires_packages", { { "type", "array" }, { "items", { { "type", "string" } } }, { "description", "Packages this board always needs, e.g. its HAL" } } },
                              { "boot_scene", { { "type", "string" }, { "description", "Scene the device boots, relative to the project" } } },
                              { "project_path", { { "type", "string" }, { "description", "Project directory (default: the open project)" } } } } } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            const std::string path = ResolveProjectPath(editor, args);
                            if (path.empty())
                                throw std::runtime_error("no project");

                            const std::string id = args.value("id", "");
                            if (id.empty())
                                throw std::runtime_error("create_platform needs an id");
                            std::string why;
                            if (!SafeNames::IsSafeName(id, why))
                                throw std::runtime_error("'" + id + "' is not a usable platform id: " + why);

                            // Board packs as well as the project's own, so a
                            // supported board can be adopted by name.
                            auto& registryRef = PlatformRegistry::Instance();
                            registryRef.LoadPlatformsFromDirectory(GetPlatformsDirectory(path));
                            registryRef.LoadPlatformsFromBoardPacks();

                            if (registryRef.GetPlatform(id))
                                throw std::runtime_error("a platform called '" + id + "' already exists");

                            PlatformConfig config;
                            const std::string from = args.value("from", "");
                            if (!from.empty())
                            {
                                const PlatformConfig* source = registryRef.GetPlatform(from);
                                if (!source)
                                    throw std::runtime_error("no platform '" + from + "' to copy");
                                config = *source;
                            }

                            config.id = id;
                            config.displayName = args.value("display_name", config.displayName.empty() ? id : config.displayName);
                            if (args.contains("requires_packages") && args["requires_packages"].is_array())
                            {
                                config.requiresPackages.clear();
                                for (const auto& r : args["requires_packages"])
                                    if (r.is_string())
                                        config.requiresPackages.push_back(r.get<std::string>());
                            }
                            if (args.contains("provides") && args["provides"].is_array())
                            {
                                config.provides.clear();
                                for (const auto& cap : args["provides"])
                                    if (cap.is_string())
                                        config.provides.push_back(cap.get<std::string>());
                            }
                            if (args.contains("screen_width"))
                                config.screenWidth = args["screen_width"].get<int32_t>();
                            if (args.contains("screen_height"))
                                config.screenHeight = args["screen_height"].get<int32_t>();
                            // family is an enum, spelled as a string everywhere a
                            // platform is written, so it is translated here rather
                            // than assigned.
                            if (args.contains("family") && args["family"].is_string())
                            {
                                // Any spelling: the family is descriptive and
                                // the editor has no list of the world's targets.
                                // A capability name must still be matchable, so
                                // it is validated as one.
                                const std::string family = args["family"].get<std::string>();
                                if (!IsValidCapabilityName(family))
                                    throw std::runtime_error("invalid family '" + family +
                                        "'; lowercase letters, digits, '.', '-', ':' and '_' only");
                                config.family = family;
                            }
                            config.framework = args.value("framework", config.framework);
                            config.mcuChip = args.value("mcu_chip", config.mcuChip);
                            // Framework settings pass through untouched: the
                            // editor does not know what any of them mean. The
                            // two named ones are kept as conveniences that
                            // write into the same blob.
                            if (args.contains("framework_options") &&
                                args["framework_options"].is_object())
                                config.frameworkOptions = args["framework_options"].dump();
                            // No idf_target parameter: naming one framework's
                            // setting in a generic tool is what framework_options
                            // exists to avoid. Pass {"idfTarget": "esp32s3"} in it.
                            if (args.contains("psram_mode"))
                                config.SetOption("psramMode", args.value("psram_mode", std::string()));
                            if (args.contains("cpu_freq_hz"))
                                config.cpuFreqHz = args["cpu_freq_hz"].get<uint32_t>();
                            if (args.contains("flash_size"))
                                config.flashSize = args["flash_size"].get<uint32_t>();
                            if (args.contains("psram_size"))
                                config.psramSize = args["psram_size"].get<uint32_t>();
                            if (args.contains("defines") && args["defines"].is_array())
                            {
                                config.defines.clear();
                                for (const auto& define : args["defines"])
                                    if (define.is_string())
                                        config.defines.push_back(define.get<std::string>());
                            }
                            config.bootScene = args.value("boot_scene", config.bootScene);

                            // Judged by the builder that owns the framework,
                            // not here: this tool must not know what "espidf"
                            // means. Missing builder is not fatal - a platform
                            // for a target whose package is not installed yet
                            // is still worth saving.
                            BuilderRegistry::Instance().EnsureLoaded();
                            if (auto* b = BuilderRegistry::Instance().GetBuilderForPlatform(config))
                            {
                                const auto problems = b->ValidatePlatform(config);
                                if (!problems.empty())
                                {
                                    std::string msg = "platform is not valid for the " +
                                                      b->GetFrameworkId() + " builder:";
                                    for (const auto& pr : problems)
                                        msg += "\n  " + pr;
                                    throw std::runtime_error(msg);
                                }
                            }

                            // SavePlatformToFile makes the per-platform folder itself,
                            // so it wants platforms/, not platforms/<id>.
                            const std::string platformsDir = GetPlatformsDirectory(path);
                            std::error_code ec;
                            std::filesystem::create_directories(platformsDir, ec);
                            if (!registryRef.SavePlatformToFile(config, platformsDir))
                                throw std::runtime_error("could not write the platform under " + platformsDir);
                            const std::string directory = platformsDir + "/" + id;

                            // A copied platform names the boot scene of the
                            // platform it came from, which belongs to that one.
                            // Carry the file across so the new target boots.
                            if (!from.empty() && !config.bootScene.empty() && !args.contains("boot_scene"))
                            {
                                const std::filesystem::path sourceScene =
                                    std::filesystem::path(path) / config.bootScene;
                                const std::filesystem::path ours =
                                    std::filesystem::path(directory) / sourceScene.filename();
                                if (std::filesystem::exists(sourceScene, ec) &&
                                    !std::filesystem::equivalent(sourceScene, ours, ec))
                                {
                                    std::filesystem::copy_file(
                                        sourceScene, ours,
                                        std::filesystem::copy_options::overwrite_existing, ec);
                                    config.bootScene = GetPlatformsDirectoryRelative() + "/" + id +
                                                       "/" + sourceScene.filename().string();
                                    registryRef.SavePlatformToFile(config, directory);
                                }
                            }

                            return { { "id", config.id },
                                     { "displayName", config.displayName },
                                     { "framework", config.framework },
                                     { "frameworkOptions", config.frameworkOptions },
                                     { "screenWidth", config.screenWidth },
                                     { "screenHeight", config.screenHeight },
                                     { "bootScene", config.bootScene },
                                     { "directory", directory } };
                        } });

    registry.Register({ "set_active_platform",
                        "Choose which platform the project builds for by default, the same "
                        "setting the Build panel's target dropdown writes.",
                        { { "type", "object" },
                          { "required", nlohmann::json::array({ "platform_id" }) },
                          { "properties",
                            { { "platform_id", { { "type", "string" }, { "description", "Platform id, or \"\" to leave the project with no default" } } } } } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            auto* project = editor ? editor->GetProject() : nullptr;
                            if (!project)
                                throw std::runtime_error("no project open");

                            const std::string id = args.value("platform_id", "");
                            if (!id.empty())
                            {
                                auto& registryRef = PlatformRegistry::Instance();
                                registryRef.LoadPlatformsFromDirectory(
                                    GetPlatformsDirectory(project->GetPath()));
                                registryRef.LoadPlatformsFromBoardPacks();
                                if (!registryRef.GetPlatform(id))
                                    throw std::runtime_error("no platform '" + id + "' in this project");
                            }

                            project->SetActivePlatformId(id);
                            if (!project->Save())
                                throw std::runtime_error("could not save the project");

                            return { { "activePlatformId", id } };
                        } });

    // The CLI equivalent of the Package Manager's "Editable (I am developing
    // this package)" checkbox. Without it, a package directory belongs to the
    // editor: PackageIntegrity stashes local edits and puts the checkout back
    // on the pinned version every time the project opens, which on the command
    // line happens on every single verb.
    registry.Register({ "set_package_editable",
                        "Mark a package as one this project is DEVELOPING rather than consuming. "
                        "Editable packages are left alone; non-editable ones have local edits "
                        "stashed and their checkout reset to the pinned version on every project "
                        "open. Persisted as \"editable\" in deki-packages.json.",
                        { { "type", "object" },
                          { "required", nlohmann::json::array({ "package_id" }) },
                          { "properties",
                            { { "package_id", { { "type", "string" }, { "description", "Package id, e.g. \"deki-sdl3\"" } } },
                              { "editable", { { "type", "boolean" }, { "description", "true to develop it here (default), false to hand it back to the editor" } } } } } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            auto* project = editor ? editor->GetProject() : nullptr;
                            if (!project)
                                throw std::runtime_error("no project open");

                            const std::string id = args.value("package_id", "");
                            if (id.empty())
                                throw std::runtime_error("package_id is required");
                            const bool editable = args.value("editable", true);

                            PackageManager manager(editor);
                            manager.LoadPackages(project->GetPath());

                            // Packages are named inconsistently across manifests:
                            // some carry a bare directory name, others "owner/repo".
                            // Accept either, plus the id and display name, rather
                            // than making the caller guess which one this package
                            // happens to use.
                            PackageEntry* found = nullptr;
                            std::vector<std::string> known;
                            for (auto& pkg : manager.GetPackages())
                            {
                                known.push_back(pkg.name);

                                std::string leaf = pkg.name;
                                const auto slash = leaf.find_last_of('/');
                                if (slash != std::string::npos) leaf = leaf.substr(slash + 1);

                                if (pkg.name == id || pkg.id == id || leaf == id ||
                                    pkg.displayName == id)
                                {
                                    found = &pkg;
                                    break;
                                }
                            }
                            if (!found)
                            {
                                std::string msg = "no package '" + id + "' in this project; it has:";
                                for (const auto& k : known) msg += " " + k;
                                throw std::runtime_error(msg);
                            }

                            const std::string resolvedId = found->id;
                            found->editable = editable;
                            manager.SavePackages(project->GetPath());

                            // Read it back rather than trusting the write. The
                            // manifest keeps only some of what it is handed, so
                            // a flag can be dropped on save; reporting success
                            // anyway is how a caller ends up believing a package
                            // is protected while the restore pass still stashes
                            // its edits.
                            PackageManager check(editor);
                            check.LoadPackages(project->GetPath());
                            for (const auto& pkg : check.GetPackages())
                            {
                                if (pkg.id != resolvedId) continue;
                                if (pkg.editable != editable)
                                    throw std::runtime_error(
                                        "'" + resolvedId + "' did not keep editable=" +
                                        (editable ? "true" : "false") + " across a reload");
                                break;
                            }

                            return { { "packageId", id }, { "editable", editable } };
                        } });

    // Capabilities are how a package decides whether it can build for a target,
    // so they have to be settable without opening platform JSON by hand.
    registry.Register({ "set_platform_provides",
                        "Set what a platform can offer packages, as capability names "
                        "(\"desktop-os\", \"sdl3\", \"posix-process\", \"wifi\", \"spi\"). A package "
                        "declares what it needs in its platformRequires and builds for this target "
                        "only when everything it needs is offered here. \"framework:<x>\" and "
                        "\"family:<x>\" are added automatically and need not be listed.",
                        { { "type", "object" },
                          { "required", nlohmann::json::array({ "platform_id", "provides" }) },
                          { "properties",
                            { { "platform_id", { { "type", "string" }, { "description", "Platform to change" } } },
                              { "provides", { { "type", "array" }, { "items", { { "type", "string" } } }, { "description", "The complete capability list; replaces what was there" } } },
                              { "requires_packages", { { "type", "array" }, { "items", { { "type", "string" } } }, { "description", "Packages this board always needs, e.g. its HAL. Replaces what was there when given." } } },
                              { "sdkconfig", { { "type", "array" }, { "items", { { "type", "string" } } }, { "description", "Extra ESP-IDF options as \"CONFIG_X=y\" lines, appended last so they win" } } },
                              { "project_path", { { "type", "string" }, { "description", "Project directory (default: the open project)" } } } } } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            const std::string path = ResolveProjectPath(editor, args);
                            if (path.empty())
                                throw std::runtime_error("no project");

                            const std::string id = args.value("platform_id", "");
                            if (id.empty())
                                throw std::runtime_error("set_platform_provides needs a platform_id");

                            auto& registryRef = PlatformRegistry::Instance();
                            registryRef.LoadPlatformsFromDirectory(GetPlatformsDirectory(path));
                            registryRef.LoadPlatformsFromBoardPacks();

                            const PlatformConfig* existing = registryRef.GetPlatform(id);
                            if (!existing)
                                throw std::runtime_error("no platform '" + id + "' in this project");

                            PlatformConfig config = *existing;
                            config.provides.clear();
                            if (args.contains("provides") && args["provides"].is_array())
                            {
                                for (const auto& cap : args["provides"])
                                    if (cap.is_string())
                                        config.provides.push_back(cap.get<std::string>());
                            }

                            if (args.contains("requires_packages") && args["requires_packages"].is_array())
                            {
                                config.requiresPackages.clear();
                                for (const auto& r : args["requires_packages"])
                                    if (r.is_string())
                                        config.requiresPackages.push_back(r.get<std::string>());
                            }
                            if (args.contains("sdkconfig") && args["sdkconfig"].is_array())
                            {
                                std::vector<std::string> lines;
                                for (const auto& line : args["sdkconfig"])
                                    if (line.is_string())
                                        lines.push_back(line.get<std::string>());
                                config.SetOptionList("sdkconfig", lines);
                            }

                            registryRef.RegisterPlatform(config);
                            if (!registryRef.SavePlatformToFile(config, GetPlatformsDirectory(path)))
                                throw std::runtime_error("could not write the platform file for '" + id + "'");

                            nlohmann::json caps = nlohmann::json::array();
                            for (const auto& c : config.Capabilities())
                                caps.push_back(c);
                            return { { "platformId", id },
                                     { "provides", config.provides },
                                     { "requiresPackages", config.requiresPackages },
                                     { "capabilities", caps } };
                        } });

    // Per-target content. "requires" is a BUILD-time key: the exporter drops
    // the object for targets that cannot satisfy it and the runtime never sees
    // it, which is why this edits the scene file rather than the loaded scene.
    registry.Register({ "set_object_requires",
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
                              { "requires", { { "type", "array" }, { "items", { { "type", "string" } } }, { "description", "Capabilities the object needs; empty clears" } } },
                              { "project_path", { { "type", "string" }, { "description", "Project directory (default: the open project)" } } } } } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            namespace fs = std::filesystem;
                            const std::string path = ResolveProjectPath(editor, args);
                            const std::string rel = args.value("scene_path", "");
                            if (rel.empty())
                                throw std::runtime_error("set_object_requires needs a scene_path");

                            fs::path scenePath = fs::path(path) / rel;
                            if (!fs::exists(scenePath))
                                throw std::runtime_error("no scene at " + scenePath.string());

                            nlohmann::json scene;
                            {
                                std::ifstream in(scenePath);
                                in >> scene;
                            }

                            const int64_t wantedId = args.value("object_id", (int64_t)-1);
                            std::vector<std::string> needs;
                            if (args.contains("requires") && args["requires"].is_array())
                                for (const auto& n : args["requires"])
                                    if (n.is_string())
                                        needs.push_back(n.get<std::string>());

                            // Objects nest through "children", so this walks.
                            bool found = false;
                            std::function<void(nlohmann::json&)> visit = [&](nlohmann::json& node)
                            {
                                if (found) return;
                                if (node.is_array())
                                {
                                    for (auto& item : node) visit(item);
                                    return;
                                }
                                if (!node.is_object()) return;
                                // Components carry an "id" as well, so an object
                                // is the node that holds components or children.
                                const bool isObject =
                                    node.contains("components") || node.contains("children");
                                if (isObject && node.contains("id") && node["id"].is_number_integer() &&
                                    node["id"].get<int64_t>() == wantedId)
                                {
                                    if (needs.empty())
                                        node.erase("requires");
                                    else
                                        node["requires"] = needs;
                                    found = true;
                                    return;
                                }
                                for (auto& entry : node.items()) visit(entry.value());
                            };
                            visit(scene);

                            if (!found)
                                throw std::runtime_error("no object with id " + std::to_string(wantedId) +
                                                         " in " + rel);

                            {
                                std::ofstream out(scenePath);
                                out << scene.dump(2) << "\n";
                            }

                            return { { "scene", rel }, { "objectId", wantedId }, { "requires", needs } };
                        } });

    registry.Register({ "toolchain_status",
                        "Report toolchain components for a platform's builder (e.g. esp-idf, "
                        "python, esptool), including install state and versions.",
                        { { "type", "object" }, { "properties", { { "project_path", { { "type", "string" }, { "description", "Project directory (default: the open project)" } } }, { "platform_id", { { "type", "string" }, { "description", "Platform id (default: the project's activePlatformId)" } } } } } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            const std::string path = ResolveProjectPath(editor, args);
                            std::string err;
                            nlohmann::json result = FirmwareBuildService::ToolchainStatus(path, ArgPlatform(args), err);
                            if (!err.empty())
                                throw std::runtime_error(err);
                            return result;
                        } });

    // =========================================================================
    // Long-running operations (start-and-poll)
    // =========================================================================

    registry.Register({ "build_firmware",
                        "Start a firmware build for the project. Regenerates build files first. "
                        "Returns as soon as the build starts — poll build_status for progress "
                        "and the final result. Fails immediately if the toolchain is missing.",
                        { { "type", "object" }, { "properties", { { "project_path", { { "type", "string" }, { "description", "Project directory (default: the open project)" } } }, { "platform_id", { { "type", "string" }, { "description", "Platform id (default: the project's activePlatformId)" } } } } } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            const std::string path = ResolveProjectPath(editor, args);
                            FirmwareOpResult r = FirmwareBuildService::StartBuild(path, ArgPlatform(args), nullptr);
                            if (!r.success)
                                throw std::runtime_error(r.error);

                            nlohmann::json j = OpToJson(r);
                            j["started"] = true;
                            j["note"] = "Build running in background — poll build_status.";
                            return j;
                        } });

    registry.Register({ "build_status",
                        "Progress of the most recently started build, flash, or toolchain "
                        "install. state is idle|building|flashing|completed|failed; active is "
                        "true while the worker thread is running.",
                        { { "type", "object" }, { "properties", nlohmann::json::object() } },
                        [](const nlohmann::json&) -> nlohmann::json
                        {
                            return FirmwareBuildService::QueryStatus();
                        } });

    registry.Register({ "cancel_build",
                        "Request cancellation of the in-flight build or flash. Returns "
                        "cancelled=false when nothing is running.",
                        { { "type", "object" }, { "properties", nlohmann::json::object() } },
                        [](const nlohmann::json&) -> nlohmann::json
                        {
                            return nlohmann::json{ { "cancelled", FirmwareBuildService::CancelActive() } };
                        } });

    registry.Register({ "install_toolchain_component",
                        "Download and install a toolchain component by id (see toolchain_status "
                        "for valid ids, e.g. esp-idf). Runs the component's post-install step "
                        "when it declares one. This downloads gigabytes and can take a long time.",
                        { { "type", "object" }, { "properties", { { "component_id", { { "type", "string" }, { "description", "Component id, e.g. \"esp-idf\"" } } }, { "project_path", { { "type", "string" }, { "description", "Project directory (default: the open project)" } } }, { "platform_id", { { "type", "string" }, { "description", "Platform id (default: the project's activePlatformId)" } } } } }, { "required", nlohmann::json::array({ "component_id" }) } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            const std::string path = ResolveProjectPath(editor, args);
                            const std::string componentId = args["component_id"].get<std::string>();

                            // Blocking would stall the editor's frame loop for the length of a
                            // multi-GB download, so this starts the installer and returns.
                            FirmwareOpResult r = FirmwareBuildService::StartInstallToolchainComponent(
                                path, ArgPlatform(args), componentId, nullptr);
                            if (!r.success)
                                throw std::runtime_error(r.error);

                            nlohmann::json j = OpToJson(r);
                            j["componentId"] = componentId;
                            j["started"] = true;
                            j["note"] = "Install running in background — poll build_status "
                                        "(state \"installing\").";
                            return j;
                        } });

    // =========================================================================
    // Flash / clean
    // =========================================================================

    registry.Register({ "deploy_build",
                        "Deploy the last build the way the platform's backend defines it: flash a "
                        "device, run the executable, install an app. Refused when the backend has "
                        "no deploy step. Returns once it starts — poll build_status for the result.",
                        { { "type", "object" }, { "properties", { { "project_path", { { "type", "string" }, { "description", "Project directory (default: the open project)" } } }, { "platform_id", { { "type", "string" }, { "description", "Platform id (default: the project's activePlatformId)" } } }, { "target", { { "type", "string" }, { "description", "What to deploy to, in the backend's own terms (a serial port for a board that is flashed); omit to let the backend choose" } } } } } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            const std::string path = ResolveProjectPath(editor, args);
                            const std::string target = args.value("target", std::string());
                            FirmwareOpResult r = FirmwareBuildService::StartDeploy(path, ArgPlatform(args), target, nullptr);
                            if (!r.success)
                                throw std::runtime_error(r.error);

                            nlohmann::json j = OpToJson(r);
                            j["started"] = true;
                            j["note"] = "Deploy running in background — poll build_status.";
                            return j;
                        } });

    registry.Register({ "clean_build",
                        "Delete the builder's build directory for the platform, forcing a full "
                        "rebuild next time.",
                        { { "type", "object" }, { "properties", { { "project_path", { { "type", "string" }, { "description", "Project directory (default: the open project)" } } }, { "platform_id", { { "type", "string" }, { "description", "Platform id (default: the project's activePlatformId)" } } } } } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            const std::string path = ResolveProjectPath(editor, args);
                            FirmwareOpResult r = FirmwareBuildService::Clean(path, ArgPlatform(args), nullptr);
                            if (!r.success)
                                throw std::runtime_error(r.error);
                            return OpToJson(r);
                        } });
}

}  // namespace DekiEditor
