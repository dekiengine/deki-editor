#pragma once

#include <string>
#include <functional>
#include <utility>
#include <vector>
#include <memory>

namespace DekiEditor
{

struct PlatformConfig;
enum class MCUFamily;
class EditorApp;

// ============================================================================
// Platform editor UI: each builder can supply its own
// ============================================================================

/// A builder's own settings UI in the platform editor. The builder draws its
/// own ImGui widgets; PlatformEditorDialog owns the instance and calls it.
class IPlatformEditorUI
{
public:
    virtual ~IPlatformEditorUI() = default;

    /// Draws this platform's builder-specific settings.
    ///
    /// Takes no EditorApp on purpose: third-party builders compile against this
    /// interface, and the editor's application class must stay out of it. For
    /// DPI, use `x * ImGui::GetWindowDpiScale()`.
    virtual void Draw() = 0;

    /// Writes the edited values back into `config`.
    virtual void ApplyToConfig(PlatformConfig& config) const = 0;
};

/// Draws an editable string list (defines, flags, etc.).
void DrawStringListEditor(const char* label, std::vector<std::string>& list, char* newItemBuffer, size_t bufSize);

// ============================================================================
// Toolchain component types, for BuildPanel's toolchain UI
// ============================================================================

enum class ToolchainComponentStatus
{
    Installed,
    NotInstalled,
    UpdateAvailable,
    Installing
};

struct ToolchainComponent
{
    std::string id;  // the backend's own id for it: an SDK, an interpreter, a flash tool
    std::string displayName;
    ToolchainComponentStatus status = ToolchainComponentStatus::NotInstalled;
    std::string installedVersion;
    std::string latestVersion;
    bool canInstall = false;  // Show the Install button
    bool canSetup = false;    // Has a post-install setup step (e.g. install.bat)
    bool setupDone = false;
    std::string tooltip;  // Extra info shown on hover
};

// ============================================================================
// Types shared by all builders
// ============================================================================

enum class BuildState
{
    Idle,
    Building,
    Deploying,
    Completed,
    Failed
};

struct BuildProgress
{
    BuildState state = BuildState::Idle;
    std::string statusText;
    float progress = 0.0f;
    std::string error;
};

/// Where a simulated build keeps its boot scene: this directory under the
/// builder's build directory, as boot.scene. The editor writes it; a backend
/// that computes its own strip plan counts the scenes in it.
constexpr const char* kSimulationSceneDir = "simulation";

/// What the simulated board's SD card holds, under kSimulationSceneDir: the
/// assets, exported as the SD Card Deploy panel exports them, for a target
/// that keeps them on the card; empty for one that keeps them inside. The
/// editor fills it on every simulated build; the backend makes the card.
constexpr const char* kSimulationCardDir = "sd_card";

struct BuildOptions
{
    bool enableLogging = true;           // Enable DEKI_LOG_* macros in firmware
    bool enableInternalLogging = false;  // Enable verbose engine-internal logs (DEKI_LOG_INTERNAL)
    // Build the platform's firmware for the backend's simulator (QEMU for
    // ESP32) instead of the board: its own build directory, the board's boot
    // scene with the simulator's steps added, and deploy runs it. The board's
    // boot scene and its real build are left alone.
    bool simulate = false;
};

/// One thing a build can be deployed to, as the backend enumerates them.
struct DeployTarget
{
    std::string id;     // what Deploy() is given back: "COM5", an address, a serial number
    std::string label;  // what the selector shows; the id when empty
};

// Receives build output one line at a time, as it is produced.
using BuildOutputCallback = std::function<void(const std::string& line, bool isError)>;
using BuildProgressCallback = std::function<void(const BuildProgress&)>;

// ============================================================================
// ITargetBuilder
// ============================================================================

/// A build backend: everything the editor knows about one kind of target.
///
/// One implementation per build framework: a microcontroller SDK, the desktop
/// the editor runs on, an app format for someone else's firmware, a console
/// SDK. It generates the target's build files, drives its toolchain and knows
/// what getting a build onto a device means there.
///
/// The editor ships none. A backend comes in a package's editor-side DLL (see
/// BuilderPlugin.h) or as a plugin under ~/.deki-editor/builders/, so a target
/// can be added without an editor release, even one whose SDK cannot be
/// published.
class ITargetBuilder
{
public:
    virtual ~ITargetBuilder() = default;

    // ========================================================================
    // Core operations
    // ========================================================================

    /// Builds firmware for the project.
    virtual void Build(const std::string& projectPath, BuildOutputCallback outputCallback = nullptr,
                       BuildProgressCallback progressCallback = nullptr) = 0;

    /// Cleans the build directory.
    virtual void Clean(const std::string& projectPath, BuildOutputCallback outputCallback = nullptr,
                       BuildProgressCallback progressCallback = nullptr) = 0;

    // ========================================================================
    // Deploy: getting a finished build onto something
    // ========================================================================
    //
    // What the step is (flash over serial, run the executable, install an
    // app), what it is called and what it can be aimed at all belong to the
    // backend.

    /// Whether this target has a deploy step at all. Pure virtual, so every
    /// backend says so either way rather than inheriting a button that cannot
    /// work.
    virtual bool SupportsDeploy() const = 0;

    /// The verb for the button and the log: "Flash", "Run", "Install".
    virtual const char* GetDeployLabel() const { return "Deploy"; }

    /// What a deploy can be aimed at right now: serial ports, devkits on the
    /// network, paired devices. Empty means there is nothing to choose (the
    /// machine the editor runs on), and the UI shows no selector.
    virtual std::vector<DeployTarget> EnumerateDeployTargets() const { return {}; }

    /// Deploys the last build. deployTargetId is a DeployTarget::id from
    /// EnumerateDeployTargets, or empty to let the backend pick.
    virtual void Deploy(const std::string& projectPath, const std::string& deployTargetId,
                        BuildOutputCallback outputCallback = nullptr, BuildProgressCallback progressCallback = nullptr)
    {
        (void)projectPath;
        (void)deployTargetId;
        (void)outputCallback;
        (void)progressCallback;
    }

    /// Cancels the current operation.
    virtual void Cancel() = 0;

    // ========================================================================
    // State queries
    // ========================================================================

    virtual bool IsBuilding() const = 0;
    virtual BuildState GetState() const = 0;
    virtual BuildProgress GetProgress() const = 0;

    // ========================================================================
    // Configuration
    // ========================================================================

    virtual void SetBuildOptions(const BuildOptions& options) = 0;
    virtual const BuildOptions& GetBuildOptions() const = 0;

    /// Sets the platform's hardware configuration (display, memory, etc.).
    virtual void SetPlatformConfig(const PlatformConfig& config) = 0;
    virtual void ClearPlatformConfig() = 0;

    /// Sets the package defines, found by scanning the scenes.
    virtual void SetPackageDefines(const std::vector<std::string>& defines) = 0;

    // ========================================================================
    // Toolchain management
    // ========================================================================

    /// True when the required toolchain is installed.
    virtual bool IsToolchainInstalled() const = 0;

    /// The toolchain's status, as text for the user.
    virtual std::string GetToolchainStatus() const = 0;

    // ========================================================================
    // Toolchain components, shown by BuildPanel
    // ========================================================================

    /// The toolchain's components and their status.
    virtual std::vector<ToolchainComponent> GetToolchainComponents() const { return {}; }

    /// Installs or reinstalls one toolchain component.
    virtual void InstallToolchainComponent(const std::string& componentId, BuildProgressCallback cb = nullptr)
    {
        (void)componentId;
        (void)cb;
    }

    /// Runs the postInstall setup script of an installed component.
    virtual void SetupToolchainComponent(const std::string& componentId, BuildProgressCallback cb = nullptr)
    {
        (void)componentId;
        (void)cb;
    }

    /// Checks for toolchain updates asynchronously and calls onComplete when done.
    virtual void CheckForToolchainUpdates(std::function<void()> onComplete = nullptr)
    {
        if (onComplete)
        {
            onComplete();
        }
    }

    /// Whether a toolchain install or update check is in progress.
    virtual bool IsToolchainBusy() const { return false; }

    /// The engine path for a project.
    virtual std::string GetEnginePath(const std::string& projectPath) const = 0;

    // ========================================================================
    // Build file generation
    // ========================================================================

    /// Generates the build files (CMakeLists.txt, etc.) for `config`, in the
    /// builder's own project structure. Called on a platform switch and when a
    /// project is created.
    virtual bool GenerateBuildFiles(const std::string& projectPath, const PlatformConfig& config,
                                    const std::vector<std::string>& packageDefines) = 0;

    // ========================================================================
    // Platform editor UI
    // ========================================================================

    /// Creates this builder's settings UI for the platform editor, or nullptr
    /// if it has none. The dialog owns the result.
    virtual std::unique_ptr<IPlatformEditorUI> CreateEditorUI(const PlatformConfig& config) const { return nullptr; }

    // ========================================================================
    // Identity
    // ========================================================================

    /// The backend's name, for the user.
    virtual const char* GetName() const = 0;

    /// What the Build panel shows about a platform aimed at this backend, as
    /// label/value rows: a chip and its memory, a devkit model, or nothing.
    virtual std::vector<std::pair<std::string, std::string>> DescribePlatform(const PlatformConfig& config) const
    {
        (void)config;
        return {};
    }

    /// Problems with a platform aimed at this builder; empty when it is fine.
    /// The backend owns its settings, so it validates them: the editor does
    /// not know which chip names a framework accepts, and a new chip is a
    /// backend update, not an editor release.
    virtual std::vector<std::string> ValidatePlatform(const PlatformConfig& config) const
    {
        (void)config;
        return {};
    }

    /// The build framework this backend serves, as a lowercase id. This is the
    /// dispatch key: a platform names its framework and the registry hands it
    /// to whoever claims that id.
    ///
    /// Not the vendor family, which cannot tell apart two backends for the
    /// same silicon (an app format that runs on a chip, and that chip's own
    /// SDK). Not GetPlatformKey() either, which reads the platform config this
    /// builder was already given, so it cannot be used to choose the builder.
    virtual std::string GetFrameworkId() const = 0;

    /// Optional self-description for the platform editor's builder picker, so
    /// a third-party builder is not anonymous in the UI.
    virtual const char* GetIcon() const { return nullptr; }
    virtual const char* GetDescription() const { return ""; }

    /// The builder's build directory for a project.
    virtual std::string GetBuildDirectory(const std::string& projectPath) const = 0;

    // ========================================================================
    // Simulation: running a board's firmware without the board
    // ========================================================================

    /// Whether this backend can build a platform's firmware for a simulator
    /// (BuildOptions::simulate). False by default.
    virtual bool SupportsSimulation() const { return false; }

    /// The simulator's name, for the UI and the log: "QEMU".
    virtual const char* GetSimulatorName() const { return ""; }

    /// Makes the simulated build's boot scene. `scene` comes in as the board's
    /// boot scene (JSON text; empty when the platform has none) and goes out
    /// as the simulated build's: every board step kept, the simulator's own
    /// added (AddSimulationSteps), and a step changed where the simulator must
    /// run it another way (a card on a bus the simulator does not have).
    /// `notes` says what was done, one line each, for the build log. The editor
    /// writes the result under kSimulationSceneDir and the build compiles it;
    /// the board's boot scene is never changed, and a real build never sees
    /// any of it. False, with `error` set, when it cannot be made.
    virtual bool MakeSimulationBootScene(std::string& scene, const PlatformConfig& config,
                                         std::vector<std::string>& notes, std::string& error) const
    {
        (void)scene;
        (void)config;
        (void)notes;
        error = "this backend has no simulator";
        return false;
    }

    /// Where the editor writes the boot payload before Build(): the compiled
    /// project settings (dproject.bin) and the platform's boot scene. What
    /// becomes of the directory is up to the backend: packed into a flash
    /// filesystem image, copied beside an executable, bundled into an app
    /// archive. Empty means this target takes no boot payload, and the editor
    /// writes none. Pure virtual on purpose: only the backend knows.
    virtual std::string GetBootPayloadDirectory(const std::string& projectPath) const = 0;

    // ========================================================================
    // Package compatibility
    // ========================================================================

    /// Platform key for resolving dependencies in package.json / .deki-package.json.
    virtual std::string GetPlatformKey() const = 0;

    /// Whether a package works with this builder, from the dependencies in its
    /// metadata file (package.json or .deki-package.json). True when they name
    /// this platform's key or "default", or when there are none (pure code
    /// works everywhere).
    virtual bool IsPackageCompatible(const std::string& metadataPath) const { return true; }

    /// The platform keys a package's metadata file lists under dependencies,
    /// with "default" reported as "all". Empty if the file is missing or has
    /// no dependencies.
    virtual std::vector<std::string> GetPackageSupportedPlatforms(const std::string& metadataPath) const { return {}; }
};

}  // namespace DekiEditor
