// EditorCli: argument parsing and verb dispatch.
//
// A verb runs in one of two ways. By default it runs against a booted editor
// (RunCommand), so a CLI run is the same as opening the project in the GUI.
// Otherwise it reads the project directory and nothing else
// (RunLightCommand); IsLightCommand lists those. No verb sets up engine state
// of its own.

#include "EditorCli.h"
#include "EditorHeadless.h"
#include "EditorVersion.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace DekiEditor
{

void PrintUsage(const char* program)
{
    printf("%s - Scene and Asset Editor\n", EditorVersionLine().c_str());
    printf("\n");
    printf("Usage: %s [options] [project_path]\n", program);
    printf("\n");
    printf("Every option below other than --help opens the project exactly as the\n");
    printf("GUI does - packages restored, build files generated, plugin compiled and\n");
    printf("promoted, assets imported - and then runs the requested operation without\n");
    printf("a window. A first run on a fresh checkout therefore takes as long as the\n");
    printf("editor's first open, because it is the same work.\n");
    printf("\n");
    printf("The exceptions, which read the project directory and nothing else, are\n");
    printf("--new-project, --packages-list and --list-platforms.\n");
    printf("\n");
    printf("Options:\n");
    printf("  --help, -h            Show this help message\n");
    printf("  --editor-version, -V  Print the editor, engine and reflection ABI versions\n");
    printf("  --new-project <name>  Create <project_path>/<name>: the desktop packages declared,\n");
    printf("                        a starter scene, the simulator platform. Open it to build.\n");
    printf("  --build               Compile all assets to cache/ (headless)\n");
    printf("  --export              Build and export assets to storage/, stored for the\n");
    printf("                        active platform, or for --platform <id>\n");
    printf("  --packages-list        List the project's packages and their install state\n");
    printf("  --packages-restore     Install every declared package that is missing on disk\n");
    printf("  --packages-add <url>   Add a package by repo URL or registry id, and install it\n");
    printf("  --packages-remove <id> Remove a package and any dependencies only it needed\n");
    printf("  --packages-update <id|all> Update the project's package(s) to the newest version, then rebuild\n");
    printf("  --version <ver>       Version/tag for --packages-add (or branch:<name>)\n");
    printf("  --build-code          Regenerate build files and compile packages + plugin\n");
    printf("  --verify-scenes       Report scene property keys no component recognises\n");
    printf("  --strip-plan          Print what a device build leaves out, feature by feature, and why\n");
    printf(
        "                        Answers for --platform <id>; default the active one, \"all\" pools every platform\n");
    printf("  --build-all-targets   Build every platform the project declares, not just one\n");
    printf("  --platform-compat     Which packages build for which of the project's targets, and\n");
    printf("                        what a package needs that a target does not provide\n");
    printf("\n");
    printf("Scripting (the same tools the MCP server exposes):\n");
    printf("  --list-tools          List every tool, with its JSON schema\n");
    printf("  --tool <name>         Run one tool and print its result as JSON\n");
    printf("  --args <json>         Arguments for --tool (default: {})\n");
    printf("  --script <file>       Run a JSON array of {\"tool\", \"args\"} in order,\n");
    printf("                        stopping at the first failure. A step with \"as\": \"name\" saves\n");
    printf("                        its result; later steps use it as \"${name.id}\"\n");
    printf("  --upgrade-project     Re-save every scene, boot scene, platform file and sprite sheet in\n");
    printf("                        the current format; writes nothing unless all of it loads back the same\n");
    printf("  --render-scenes       Render every scene headless and print a hash per scene\n");
    printf("                        (DEKI_RENDER_FRAMES=n, DEKI_RENDER_OUT=<dir> writes .ppm files,\n");
    printf("                        DEKI_RENDER_MODE=buffer|system|tracked: scene-view path, the\n");
    printf("                        engine's render system, or that with dirty tracking forced on;\n");
    printf("                        DEKI_RENDER_TICK_MS=<ms> runs the scene like play mode, advancing\n");
    printf("                        a fixed clock per frame, and writes every frame to DEKI_RENDER_OUT)\n");
    printf("  --dump-metadata <file> Write every loaded package's components, properties and\n");
    printf("                        settings sections (with defaults) as JSON; the docs build's input\n");
    printf("  --include-project     With --dump-metadata: the project's own components too\n");
    printf("\n");
    printf("Firmware (headless):\n");
    printf("  --build-firmware      Build firmware for the active platform\n");
    printf("  --deploy              Deploy the last build as its backend defines: flash, run, install\n");
    printf("  --simulate            With --build-firmware or --deploy: the board's firmware in its\n");
    printf("                        simulator (QEMU for ESP32); the board's boot scene is not changed\n");
    printf("  --clean               Clean the firmware build directory\n");
    printf("  --list-platforms      List platforms available to the project (JSON)\n");
    printf("  --toolchain-status    Show toolchain component status (JSON)\n");
    printf("  --install-toolchain <id>  Install a toolchain component (ids: --toolchain-status)\n");
    printf("  --platform <id>       Override the platform (default: activePlatformId)\n");
    printf("  --deploy-target <id>  What to deploy to, e.g. a serial port (default: the backend chooses)\n");
    printf("\n");
    printf("MCP:\n");
    printf("  --mcp                 Start the MCP server (SSE, http://localhost:3000/sse)\n");
    printf("                        when the editor opens, instead of toggling it in the\n");
    printf("                        Activity panel\n");
    printf("  --mcp-port <port>     Port for the MCP server (implies --mcp)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s                                    Open editor with last project\n", program);
    printf("  %s /path/to/project                   Open editor with specified project\n", program);
    printf("  %s --new-project my-game /path/to/parent  Create /path/to/parent/my-game\n", program);
    printf("  %s --build /path/to/project           Build assets without GUI\n", program);
    printf("  %s --export /path/to/project          Build and export to storage/\n", program);
    printf("  %s --packages-list /path/to/project    Show package status\n", program);
    printf("  %s --packages-restore /path/to/project Install missing packages\n", program);
    printf("  %s --packages-add https://github.com/dekiengine/deki-2d /path/to/project\n", program);
    printf("  %s --list-platforms /path/to/project\n", program);
    printf("  %s --toolchain-status --platform <platform-id> /path/to/project\n", program);
    printf("  %s --build-firmware /path/to/project\n", program);
    printf("  %s --build-firmware --platform <platform-id> /path/to/project\n", program);
    printf("  %s --deploy --deploy-target COM5 /path/to/project\n", program);
    printf("  %s --mcp /path/to/project             Open editor with MCP server running\n", program);
}

bool ParseArgs(int argc, char* argv[], CliOptions& o)
{
    // Options that take the next token as their value. A missing value is an
    // error, so "--platform" at the end neither swallows the project path nor
    // reads past argv.
    auto takeValue = [&](int& i, const char* flag) -> const char*
    {
        if (i + 1 >= argc || argv[i + 1][0] == '-')
        {
            printf("Error: %s requires a value\n", flag);
            o.badArgs = true;
            return nullptr;
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            o.help = true;
        }
        // Not "--version": that is the package tag argument of --packages-add.
        else if (strcmp(argv[i], "--editor-version") == 0 || strcmp(argv[i], "-V") == 0)
        {
            o.version = true;
        }
        else if (strcmp(argv[i], "--new-project") == 0)
        {
            o.newProjectName = takeValue(i, "--new-project");
        }
        else if (strcmp(argv[i], "--build") == 0)
        {
            o.build = true;
        }
        else if (strcmp(argv[i], "--export") == 0)
        {
            o.exportAssets = true;
        }
        else if (strcmp(argv[i], "--build-firmware") == 0)
        {
            o.firmwareBuild = true;
        }
        else if (strcmp(argv[i], "--deploy") == 0)
        {
            o.deploy = true;
        }
        else if (strcmp(argv[i], "--simulate") == 0)
        {
            o.simulate = true;
        }
        else if (strcmp(argv[i], "--clean") == 0)
        {
            o.clean = true;
        }
        else if (strcmp(argv[i], "--list-platforms") == 0)
        {
            o.listPlatforms = true;
        }
        else if (strcmp(argv[i], "--toolchain-status") == 0)
        {
            o.toolchainStatus = true;
        }
        else if (strcmp(argv[i], "--install-toolchain") == 0)
        {
            o.installToolchainId = takeValue(i, "--install-toolchain");
        }
        else if (strcmp(argv[i], "--platform") == 0)
        {
            o.platformId = takeValue(i, "--platform");
        }
        else if (strcmp(argv[i], "--deploy-target") == 0)
        {
            o.deployTarget = takeValue(i, argv[i]);
        }
        else if (strcmp(argv[i], "--mcp") == 0)
        {
            o.mcp = true;
        }
        else if (strcmp(argv[i], "--mcp-port") == 0)
        {
            if (const char* v = takeValue(i, "--mcp-port"))
            {
                o.mcpPort = atoi(v);
                o.mcp = true;
            }
        }
        else if (strcmp(argv[i], "--build-code") == 0)
        {
            o.buildCode = true;
        }
        else if (strcmp(argv[i], "--verify-scenes") == 0)
        {
            o.verifyScenes = true;
        }
        else if (strcmp(argv[i], "--render-scenes") == 0)
        {
            o.renderScenes = true;
        }
        else if (strcmp(argv[i], "--upgrade-project") == 0)
        {
            o.upgradeProject = true;
        }
        else if (strcmp(argv[i], "--strip-plan") == 0)
        {
            o.stripPlan = true;
        }
        else if (strcmp(argv[i], "--platform-compat") == 0)
        {
            o.platformCompat = true;
        }
        else if (strcmp(argv[i], "--build-all-targets") == 0)
        {
            o.buildAllTargets = true;
        }
        else if (strcmp(argv[i], "--list-tools") == 0)
        {
            o.listTools = true;
        }
        else if (strcmp(argv[i], "--tool") == 0)
        {
            o.toolName = takeValue(i, "--tool");
        }
        else if (strcmp(argv[i], "--args") == 0)
        {
            o.toolArgs = takeValue(i, "--args");
        }
        else if (strcmp(argv[i], "--script") == 0)
        {
            o.scriptPath = takeValue(i, "--script");
        }
        else if (strcmp(argv[i], "--dump-metadata") == 0)
        {
            o.dumpMetadataOut = takeValue(i, "--dump-metadata");
        }
        else if (strcmp(argv[i], "--include-project") == 0)
        {
            o.includeProject = true;
        }
        else if (strcmp(argv[i], "--packages-list") == 0)
        {
            o.packagesList = true;
        }
        else if (strcmp(argv[i], "--packages-restore") == 0)
        {
            o.packagesRestore = true;
        }
        else if (strcmp(argv[i], "--packages-add") == 0)
        {
            o.packagesAddUrl = takeValue(i, "--packages-add");
        }
        else if (strcmp(argv[i], "--packages-update") == 0)
        {
            o.packagesUpdateId = takeValue(i, "--packages-update");
        }
        else if (strcmp(argv[i], "--packages-remove") == 0)
        {
            o.packagesRemoveId = takeValue(i, "--packages-remove");
        }
        else if (strcmp(argv[i], "--version") == 0)
        {
            o.packageVersion = takeValue(i, "--version");
        }
        else if (argv[i][0] != '-')
        {
            o.projectPath = argv[i];
        }
        else
        {
            // An unknown option must be an error: otherwise the editor opens
            // its window instead of running the command, and a script waits
            // forever.
            printf("Error: unknown option %s\n", argv[i]);
            o.badArgs = true;
        }
    }

    // The verbs that only read the project directory run without booting, so
    // they cannot share a command line with one that boots; that is an error
    // rather than a silently dropped verb. --new-project's path is the parent
    // directory, not a project, so it stands alone too.
    CliOptions booting = o;
    booting.newProjectName = nullptr;
    booting.packagesList = false;
    booting.listPlatforms = false;
    const int light = (o.newProjectName ? 1 : 0) + (o.packagesList ? 1 : 0) + (o.listPlatforms ? 1 : 0);
    if (light > 0 && HasCommand(booting))
    {
        printf("Error: %s cannot be combined with other commands; run it on its own\n",
               o.newProjectName ? "--new-project" : (o.packagesList ? "--packages-list" : "--list-platforms"));
        o.badArgs = true;
    }
    else if (o.newProjectName && light > 1)
    {
        printf("Error: --new-project cannot be combined with other commands; run it on its own\n");
        o.badArgs = true;
    }

    return !o.badArgs;
}

bool HasCommand(const CliOptions& o)
{
    return o.build || o.exportAssets || o.buildCode || o.verifyScenes || o.renderScenes || o.upgradeProject ||
           o.stripPlan || o.platformCompat || o.buildAllTargets || o.listTools || o.toolName || o.scriptPath ||
           o.dumpMetadataOut || o.packagesRestore || o.packagesAddUrl || o.packagesRemoveId || o.packagesUpdateId ||
           o.firmwareBuild || o.deploy || o.clean || o.packagesList || o.listPlatforms || o.toolchainStatus ||
           o.installToolchainId || o.newProjectName;
}

bool IsLightCommand(const CliOptions& o)
{
    // --new-project writes files into a directory that is not a project yet;
    // there is nothing to boot. The first open (GUI or any other verb) does
    // the restore and build.
    if (o.newProjectName)
    {
        return true;
    }

    // --packages-list reports what the manifest declares and what is on disk;
    // booting would install the missing ones first and there would be nothing
    // left to report.
    //
    // --list-platforms reads platform files: the project's and the board
    // packs'. Opening the project changes neither.
    //
    // --toolchain-status and --install-toolchain must boot: a toolchain belongs
    // to a build backend, and backends come from the project's packages, so
    // without opening the project there is no backend to ask.
    return o.packagesList || o.listPlatforms;
}

int RunLightCommand(const CliOptions& o)
{
    if (o.newProjectName)
    {
        return CmdNewProject(o.projectPath, o.newProjectName);
    }

    // ParseArgs allows these two together and nothing else with them.
    int rc = 0;
    if (o.packagesList)
    {
        rc = CmdPackagesList(o.projectPath);
    }
    if (rc == 0 && o.listPlatforms)
    {
        rc = CmdListPlatforms(o.projectPath);
    }
    return rc;
}

int RunCommand(const CliOptions& o, EditorApp& editor)
{
    // Every verb on the command line runs, in this order, stopping at the
    // first one that fails. So `--export --verify-scenes` does both, and
    // `--clean --build-firmware` cleans and then builds.
    //
    // The order follows what the work depends on: package changes, then the
    // code build, then checks that read the built project, then what ships.
    int rc = 0;
    auto run = [&rc](bool wanted, const auto& verb)
    {
        if (rc == 0 && wanted)
        {
            rc = verb();
        }
    };

    run(o.packagesAddUrl != nullptr, [&] { return CmdPackagesAdd(editor, o.packagesAddUrl, o.packageVersion); });
    run(o.packagesRemoveId != nullptr, [&] { return CmdPackagesRemove(editor, o.packagesRemoveId); });
    run(o.packagesUpdateId != nullptr, [&] { return CmdPackagesUpdate(editor, o.packagesUpdateId); });
    run(o.packagesRestore, [&] { return CmdPackagesRestore(editor); });
    run(o.buildCode, [&] { return CmdBuildCode(editor); });
    run(o.build, [&] { return CmdBuildAssets(editor); });

    run(o.dumpMetadataOut != nullptr, [&] { return CmdDumpMetadata(editor, o.dumpMetadataOut, o.includeProject); });
    run(o.verifyScenes, [&] { return CmdVerifyScenes(editor); });
    run(o.upgradeProject, [&] { return CmdUpgradeProject(editor); });
    run(o.renderScenes, [&] { return CmdRenderScenes(editor, o.platformId); });
    run(o.platformCompat, [&] { return CmdPlatformCompat(editor); });
    run(o.stripPlan, [&] { return CmdStripPlan(editor, o.platformId); });
    run(o.listTools, [&] { return CmdListTools(editor); });
    run(o.toolchainStatus, [&] { return CmdToolchainStatus(o.projectPath, o.platformId); });
    run(o.installToolchainId != nullptr,
        [&] { return CmdInstallToolchain(o.projectPath, o.platformId, o.installToolchainId); });
    run(o.toolName != nullptr, [&] { return CmdRunTool(editor, o.toolName, o.toolArgs); });
    run(o.scriptPath != nullptr, [&] { return CmdRunScript(editor, o.scriptPath); });

    run(o.exportAssets, [&] { return CmdExportAssets(editor, o.platformId); });
    run(o.clean, [&] { return CmdClean(editor, o.platformId); });
    run(o.firmwareBuild, [&] { return CmdFirmwareBuild(editor, o.platformId, o.simulate); });
    run(o.buildAllTargets, [&] { return CmdBuildAllTargets(editor); });
    run(o.deploy, [&] { return CmdDeploy(editor, o.platformId, o.deployTarget, o.simulate); });

    return rc;
}

void AttachParentConsole()
{
#ifdef _WIN32
    // Redirect only what has nowhere to go. A caller that sent stdout to a file
    // or a pipe already gave us a valid handle, and reopening CONOUT$ over it
    // would put their output on the screen instead.
    auto orphaned = [](DWORD which)
    {
        HANDLE h = GetStdHandle(which);
        return h == nullptr || h == INVALID_HANDLE_VALUE;
    };

    const bool needOut = orphaned(STD_OUTPUT_HANDLE);
    const bool needErr = orphaned(STD_ERROR_HANDLE);
    if (!needOut && !needErr)
    {
        return;
    }

    // Fails when there is no parent console at all (launched from Explorer, or
    // from a detached service). Nothing to attach to, and nothing to do.
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
    {
        return;
    }

    FILE* f = nullptr;
    if (needOut)
    {
        freopen_s(&f, "CONOUT$", "w", stdout);
    }
    if (needErr)
    {
        freopen_s(&f, "CONOUT$", "w", stderr);
    }

    // Unbuffered, not line-buffered: a crash inside a package DLL's static
    // initialiser would lose buffered output, and then the last line printed
    // is the diagnostic. It costs nothing at CLI output volumes.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
#endif
}

}  // namespace DekiEditor
