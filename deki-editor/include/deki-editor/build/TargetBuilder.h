#pragma once

#include <string>
#include <functional>
#include <utility>
#include <vector>
#include <memory>

namespace DekiEditor
{

// Forward declarations
struct PlatformConfig;
enum class MCUFamily;
class EditorApp;

// ============================================================================
// Platform Editor UI - builders create custom editor UIs via this interface
// ============================================================================

/**
 * @brief Interface for builder-specific platform editor UI
 *
 * Each builder creates an implementation that draws its own ImGui widgets
 * for platform configuration. The PlatformEditorDialog owns the instance
 * and delegates rendering to it.
 */
class IPlatformEditorUI
{
public:
    virtual ~IPlatformEditorUI() = default;

    /// Draw the builder's platform editor UI (ImGui widgets)
    /// Draw this platform's builder-specific settings.
    ///
    /// Deliberately takes nothing: it used to take EditorApp*, an
    /// executable-only type, purely so two implementations could call
    /// app->Dpi(100) - which is just `x * ImGui::GetWindowDpiScale()`. Passing
    /// it would drag the editor's application class into an interface that
    /// third-party builders compile against.
    virtual void Draw() = 0;

    /// Write the edited values back into a PlatformConfig
    virtual void ApplyToConfig(PlatformConfig& config) const = 0;
};

/// Shared utility: draw an editable string list (defines, flags, etc.)
void DrawStringListEditor(const char* label, std::vector<std::string>& list, char* newItemBuffer, size_t bufSize);

// ============================================================================
// Toolchain component types (used by BuildPanel for generic toolchain UI)
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
    bool canInstall = false;  // Whether Install button should be shown
    bool canSetup = false;    // Post-install setup step (e.g., install.bat)
    bool setupDone = false;   // Whether setup has been completed
    std::string tooltip;      // Extra info shown on hover
};

// ============================================================================
// Shared types (used by all builders)
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
    // Build the platform's firmware to run in the backend's simulator (QEMU
    // for ESP32) instead of on the board: its own build directory, the board's
    // boot scene with the simulator's steps added, and deploy runs it. The
    // board's boot scene and its real build are not touched.
    bool simulate = false;
};

/// One thing a build can be deployed to, as the backend enumerates them.
struct DeployTarget
{
    std::string id;     // what Deploy() is given back: "COM5", an address, a serial number
    std::string label;  // what the selector shows; the id when empty
};

// Callback for real-time output lines
using BuildOutputCallback = std::function<void(const std::string& line, bool isError)>;
using BuildProgressCallback = std::function<void(const BuildProgress&)>;

// MCUFamily is defined in PlatformConfig.h - forward-declare the enum
// We include PlatformConfig.h in the .cpp files that need it

// ============================================================================
// ITargetBuilder - Abstract interface for platform-specific builders
// ============================================================================

/**
 * @brief A build backend: everything the editor knows about one kind of target
 *
 * One implementation per build framework - a microcontroller SDK, the desktop
 * the editor runs on, an app format for someone else's firmware, a console
 * SDK. It generates the target's build files, drives its toolchain and knows
 * what getting a build onto a device means there.
 *
 * The editor ships none. A backend arrives in a package's editor-side DLL
 * (see BuilderPlugin.h) or as a plugin under ~/.deki-editor/builders/, so a
 * target can be added - including one whose SDK cannot be published - without
 * an editor release.
 */
class ITargetBuilder
{
public:
    virtual ~ITargetBuilder() = default;

    // ========================================================================
    // Core operations
    // ========================================================================

    /// Build firmware for the given project
    virtual void Build(const std::string& projectPath, BuildOutputCallback outputCallback = nullptr,
                       BuildProgressCallback progressCallback = nullptr) = 0;

    /// Clean build directory
    virtual void Clean(const std::string& projectPath, BuildOutputCallback outputCallback = nullptr,
                       BuildProgressCallback progressCallback = nullptr) = 0;

    // ========================================================================
    // Deploy - getting a finished build onto something
    // ========================================================================
    //
    // This was Flash(projectPath, port): every target was assumed to be a
    // microcontroller on a serial port. The desktop backend implemented it as
    // "run the executable" and ignored the port; an app format for someone
    // else's firmware had to stub it. What the step IS, what it is called and
    // what it can be aimed at all belong to the backend.

    /// Whether this target has a deploy step at all. Pure: a backend says so
    /// either way, rather than inheriting a button that cannot work.
    virtual bool SupportsDeploy() const = 0;

    /// The verb for the button and the log: "Flash", "Run", "Install".
    virtual const char* GetDeployLabel() const { return "Deploy"; }

    /// What a deploy can be aimed at right now - serial ports, devkits on the
    /// network, paired devices. Empty means there is nothing to choose (the
    /// machine the editor runs on), and the UI shows no selector.
    virtual std::vector<DeployTarget> EnumerateDeployTargets() const { return {}; }

    /// Deploy the last build. deployTargetId is a DeployTarget::id from
    /// EnumerateDeployTargets, or empty to let the backend pick.
    virtual void Deploy(const std::string& projectPath, const std::string& deployTargetId,
                        BuildOutputCallback outputCallback = nullptr, BuildProgressCallback progressCallback = nullptr)
    {
        (void)projectPath;
        (void)deployTargetId;
        (void)outputCallback;
        (void)progressCallback;
    }

    /// Cancel current operation
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

    /// Set platform-specific hardware configuration (display, memory, etc.)
    virtual void SetPlatformConfig(const PlatformConfig& config) = 0;
    virtual void ClearPlatformConfig() = 0;

    /// Set package defines (auto-detected from scene scanning)
    virtual void SetPackageDefines(const std::vector<std::string>& defines) = 0;

    // ========================================================================
    // Toolchain management
    // ========================================================================

    /// Check if the required toolchain is installed
    virtual bool IsToolchainInstalled() const = 0;

    /// Get human-readable toolchain status description
    virtual std::string GetToolchainStatus() const = 0;

    // ========================================================================
    // Toolchain component API (generic, used by BuildPanel)
    // ========================================================================

    /// Get structured list of toolchain components and their status
    virtual std::vector<ToolchainComponent> GetToolchainComponents() const { return {}; }

    /// Install or reinstall a specific toolchain component by id
    virtual void InstallToolchainComponent(const std::string& componentId, BuildProgressCallback cb = nullptr)
    {
        (void)componentId;
        (void)cb;
    }

    /// Run the postInstall setup script for an already-installed component
    virtual void SetupToolchainComponent(const std::string& componentId, BuildProgressCallback cb = nullptr)
    {
        (void)componentId;
        (void)cb;
    }

    /// Check for toolchain updates (async). Calls onComplete when done.
    virtual void CheckForToolchainUpdates(std::function<void()> onComplete = nullptr)
    {
        if (onComplete)
        {
            onComplete();
        }
    }

    /// Whether any toolchain operation (install/update check) is in progress
    virtual bool IsToolchainBusy() const { return false; }

    /// Get the engine path for a project
    virtual std::string GetEnginePath(const std::string& projectPath) const = 0;

    // ========================================================================
    // Build file generation
    // ========================================================================

    /// Generate/regenerate build files for the given platform configuration.
    /// Called on platform switch and during new project creation.
    /// Each builder generates its own project structure (CMakeLists.txt, etc.)
    virtual bool GenerateBuildFiles(const std::string& projectPath, const PlatformConfig& config,
                                    const std::vector<std::string>& packageDefines) = 0;

    // ========================================================================
    // Platform Editor UI
    // ========================================================================

    /// Create a custom editor UI for this builder's platform configuration.
    /// The dialog owns the returned instance. Returns nullptr if no custom UI.
    virtual std::unique_ptr<IPlatformEditorUI> CreateEditorUI(const PlatformConfig& config) const { return nullptr; }

    // ========================================================================
    // Identity
    // ========================================================================

    /// Human-readable backend name
    virtual const char* GetName() const = 0;

    /// What the Build panel shows about a platform aimed at this backend, as
    /// label/value rows: a chip and its memory, a devkit model, nothing at
    /// all. The editor used to print "Chip", "Flash" and "PSRAM" itself,
    /// which is one kind of target's idea of a summary.
    virtual std::vector<std::pair<std::string, std::string>> DescribePlatform(const PlatformConfig& config) const
    {
        (void)config;
        return {};
    }

    /// Problems with a platform aimed at this builder, empty when it is fine.
    ///
    /// The backend owns its own settings, so it owns validating them: the
    /// editor must not know which chip names a framework accepts. A new chip
    /// is then a backend update, not an editor release.
    virtual std::vector<std::string> ValidatePlatform(const PlatformConfig& config) const
    {
        (void)config;
        return {};
    }

    /// The build framework this backend serves, as a lowercase id. THE
    /// dispatch key: a platform names its framework and the registry hands it
    /// to whoever claims that id.
    ///
    /// Not the vendor family, which cannot distinguish two backends for the
    /// same silicon: an app format that runs on a chip and that chip's own SDK
    /// share a family and must not share a build. Not GetPlatformKey() either, which reads the
    /// platform config this builder was already given and so cannot be used to
    /// choose it.
    virtual std::string GetFrameworkId() const = 0;

    /// Optional self-description for the platform editor's builder picker, so
    /// a third-party builder is not anonymous in the UI.
    virtual const char* GetIcon() const { return nullptr; }
    virtual const char* GetDescription() const { return ""; }

    /// Get the builder's project directory for a given project path.
    virtual std::string GetBuildDirectory(const std::string& projectPath) const = 0;

    // ========================================================================
    // Simulation - running a board's firmware without the board
    // ========================================================================

    /// Whether this backend can build a platform's firmware for a simulator
    /// (BuildOptions::simulate). False by default.
    virtual bool SupportsSimulation() const { return false; }

    /// The simulator's name, for the UI and the log: "QEMU".
    virtual const char* GetSimulatorName() const { return ""; }

    /// The simulated build's boot scene. `scene` comes in as the board's boot
    /// scene (JSON text; empty when the platform has none) and goes out as
    /// the simulated build's: every board step kept, the simulator's own added
    /// (AddSimulationSteps), and a step changed where the simulator has to
    /// run it another way (a card on a bus the simulator does not have).
    /// `notes` says what was done, a line each, for the build log. The editor
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
    /// becomes of the directory is the backend's business - packed into a
    /// flash filesystem image, copied beside an executable, bundled into an
    /// app archive. Empty means this target takes no boot payload, and the
    /// editor writes none.
    ///
    /// Pure on purpose. The editor used to decide this itself, as
    /// "<build directory>/spiffs_data" - one target's partition name, which
    /// every backend then had to know to look for.
    virtual std::string GetBootPayloadDirectory(const std::string& projectPath) const = 0;

    // ========================================================================
    // Package compatibility
    // ========================================================================

    /// Platform key for resolving dependencies in package.json / .deki-package.json
    virtual std::string GetPlatformKey() const = 0;

    /// Check if a package is compatible with this builder given its metadata file.
    /// Reads dependencies from package.json or .deki-package.json.
    /// Returns true if dependencies contains this platform's key or "default",
    /// or if no dependencies exists (pure code, works everywhere).
    virtual bool IsPackageCompatible(const std::string& metadataPath) const { return true; }

    /// Get supported platform keys from a package's metadata file.
    /// Returns keys from dependencies (mapping "default" to "all").
    /// Returns empty if file doesn't exist or has no dependencies.
    virtual std::vector<std::string> GetPackageSupportedPlatforms(const std::string& metadataPath) const { return {}; }
};

}  // namespace DekiEditor
