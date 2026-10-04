#pragma once

namespace DekiEditor
{

class EditorApp;

/// One parsed command line.
struct CliOptions
{
    const char* projectPath = nullptr;

    bool help = false;
    bool version = false;  // -V / --editor-version: print the build's versions and exit
    bool badArgs = false;

    // Verbs that need a booted editor
    bool build = false;
    bool exportAssets = false;
    bool buildCode = false;
    bool verifyScenes = false;
    bool renderScenes = false;
    bool stripPlan = false;                 // --strip-plan: what a device build leaves out, and why
    bool platformCompat = false;            // --platform-compat: which packages build for which target
    bool buildAllTargets = false;           // --build-all-targets: build every platform the project declares
    bool listTools = false;                 // --list-tools: every scriptable tool and its schema
    const char* toolName = nullptr;         // --tool <name>: run one tool
    const char* toolArgs = nullptr;         // --args <json>: its arguments
    const char* scriptPath = nullptr;       // --script <file>: run a list of them in order
    const char* dumpMetadataOut = nullptr;  // --dump-metadata <file>: reflection metadata as JSON
    bool includeProject = false;            // --include-project: the project's own components too
    bool packagesRestore = false;
    const char* packagesAddUrl = nullptr;
    const char* packagesRemoveId = nullptr;
    const char* packagesUpdateId = nullptr;  // package id, repo name, or "all"
    const char* packageVersion = nullptr;
    bool firmwareBuild = false;
    bool deploy = false;
    bool simulate = false;  // --simulate: build for / run in the backend's simulator (QEMU for ESP32)
    bool clean = false;

    // Verbs that answer from files on disk
    const char* newProjectName = nullptr;  // created under projectPath (the parent directory)
    bool packagesList = false;
    bool listPlatforms = false;
    bool toolchainStatus = false;
    const char* installToolchainId = nullptr;

    // Modifiers
    const char* platformId = nullptr;
    const char* deployTarget = nullptr;

    // GUI options
    bool mcp = false;
    int mcpPort = 3000;
};

/// Fills `out`. Returns false when the line is malformed; `out.badArgs` says
/// the same thing, and the specific complaint has already been printed.
bool ParseArgs(int argc, char* argv[], CliOptions& out);

void PrintUsage(const char* program);

/// True when the line asks for a verb rather than for the editor window.
/// `--mcp` is not one: it modifies the GUI run, it does not replace it.
bool HasCommand(const CliOptions& o);

/// True for the verbs that read the project directory and nothing else.
///
/// Everything else boots the editor, because everything else depends on state
/// the editor produces (restored packages, generated build files, a compiled
/// and promoted plugin, imported assets). These few do not, and booting for
/// them would turn a sub-second command into a multi-minute one. The list is
/// explicit: a verb is light only when it is added here.
bool IsLightCommand(const CliOptions& o);

/// Run a verb from `IsLightCommand`. Precondition: IsLightCommand(o).
int RunLightCommand(const CliOptions& o);

/// Run a verb against a booted editor whose project is open.
int RunCommand(const CliOptions& o, EditorApp& editor);

/// Windows: the editor links as a GUI-subsystem binary (WIN32_EXECUTABLE), so
/// a process launched from a terminal starts with no console attached and
/// every printf goes nowhere. Reattach to the parent's console for CLI runs.
/// No-op elsewhere, and no-op when output is already redirected to a file or
/// a pipe - that handle is the one the caller wants.
void AttachParentConsole();

}  // namespace DekiEditor
