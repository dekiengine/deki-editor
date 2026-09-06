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

    registry.Register({ "flash_firmware",
                        "Flash previously built firmware to a connected device. Returns once "
                        "flashing starts — poll build_status for the result.",
                        { { "type", "object" }, { "properties", { { "project_path", { { "type", "string" }, { "description", "Project directory (default: the open project)" } } }, { "platform_id", { { "type", "string" }, { "description", "Platform id (default: the project's activePlatformId)" } } }, { "port", { { "type", "string" }, { "description", "Serial port, e.g. \"COM5\" (default: auto-detect)" } } } } } },
                        [editor](const nlohmann::json& args) -> nlohmann::json
                        {
                            const std::string path = ResolveProjectPath(editor, args);
                            const std::string port = args.value("port", std::string());
                            FirmwareOpResult r = FirmwareBuildService::StartFlash(path, ArgPlatform(args), port, nullptr);
                            if (!r.success)
                                throw std::runtime_error(r.error);

                            nlohmann::json j = OpToJson(r);
                            j["started"] = true;
                            j["note"] = "Flash running in background — poll build_status.";
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
