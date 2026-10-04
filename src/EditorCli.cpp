// EditorCli - argument parsing and verb dispatch.
//
// The dispatch here has exactly two shapes. A verb either runs against a
// booted editor (RunCommand), which is the default and is what makes a CLI run
// equivalent to opening the project in the GUI, or it reads the project
// directory and nothing else (RunLightCommand). IsLightCommand is the whole
// list of the second kind; there is no third kind, and no verb bootstraps
// engine state of its own any more.

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
    printf("  --packages-update <id|all> Update installed package(s) to the newest version, then rebuild\n");
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
    printf("                        stopping at the first failure\n");
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
    printf("                        (--flash is accepted as the same thing)\n");
    printf("  --simulate            With --build-firmware or --deploy: the board's firmware in its\n");
    printf("                        simulator (QEMU for ESP32); the board's boot scene is not changed\n");
    printf("  --clean               Clean the firmware build directory\n");
    printf("  --list-platforms      List platforms available to the project (JSON)\n");
    printf("  --toolchain-status    Show toolchain component status (JSON)\n");
    printf("  --install-toolchain <id>  Install a toolchain component (ids: --toolchain-status)\n");
    printf("  --platform <id>       Override the platform (default: activePlatformId)\n");
    printf("  --deploy-target <id>  What to deploy to, e.g. a serial port (default: the backend chooses;\n");
    printf("                        --port is accepted as the same thing)\n");
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
    // Options that consume the following token. Guard against a missing value so
    // "--platform" at the end doesn't swallow the project path or read past argv.
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
        else if (strcmp(argv[i], "--deploy") == 0 || strcmp(argv[i], "--flash") == 0)
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
        else if (strcmp(argv[i], "--deploy-target") == 0 || strcmp(argv[i], "--port") == 0)
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
            // A mistyped verb used to be ignored, and the editor opened its
            // window instead of running the command: a script waited forever.
            printf("Error: unknown option %s\n", argv[i]);
            o.badArgs = true;
        }
    }

    // The verbs that read the project directory and nothing else run without
    // booting, so they cannot share a line with one that boots: the other
    // verbs used to be dropped without a word. --new-project's path is the
    // parent directory, not a project, so it stands alone too.
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
    return o.build || o.exportAssets || o.buildCode || o.verifyScenes || o.renderScenes || o.stripPlan ||
           o.platformCompat || o.buildAllTargets || o.listTools || o.toolName || o.scriptPath || o.dumpMetadataOut ||
           o.packagesRestore || o.packagesAddUrl || o.packagesRemoveId || o.packagesUpdateId || o.firmwareBuild ||
           o.deploy || o.clean || o.packagesList || o.listPlatforms || o.toolchainStatus || o.installToolchainId ||
           o.newProjectName;
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
    // packs'. A project open changes neither.
    //
    // --toolchain-status and --install-toolchain used to be here too, on the
    // reasoning that a toolchain describes the machine. It describes a build
    // BACKEND, and backends arrive from the project's packages now - so without
    // the open there is nobody to ask, and both verbs answered "No build
    // backends are registered at all" for every project.
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
    // Every verb on the line runs, in this order, stopping at the first one
    // that fails. This was an if-chain that returned from the first verb it
    // matched: `--export --verify-scenes` verified, exported nothing and
    // exited 0, and `--clean --build-firmware` only cleaned.
    //
    // The order is the one the work depends on: package changes, then the
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
    // Redirect only what has nowhere to go. A caller that redirected stdout to
    // a file or a pipe already handed us a valid handle, and reopening CONOUT$
    // over it would throw their output on the screen instead.
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

    // Line-buffered would still hold output across a crash inside a package
    // DLL's static initialiser, which is exactly when the last line printed is
    // the diagnostic. Unbuffered costs nothing at CLI output volumes.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
#endif
}

}  // namespace DekiEditor
