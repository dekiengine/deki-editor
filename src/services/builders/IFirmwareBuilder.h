#pragma once

#include <string>
#include <functional>
#include <vector>
#include <memory>

namespace DekiEditor
{

// Forward declarations
struct PlatformConfig;
enum class MCUFamily;
class EditorApp;

// ============================================================================
// Platform Editor UI — builders create custom editor UIs via this interface
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
    virtual void Draw(EditorApp* app) = 0;

    /// Write the edited values back into a PlatformConfig
    virtual void ApplyToConfig(PlatformConfig& config) const = 0;
};

/// Shared utility: draw an editable string list (defines, flags, etc.)
void DrawStringListEditor(const char* label, std::vector<std::string>& list,
                          char* newItemBuffer, size_t bufSize);

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
    std::string id;           // e.g., "esp-idf", "python", "gcc"
    std::string displayName;  // e.g., "ESP-IDF SDK"
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
    Flashing,
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

struct BuildOptions
{
    bool enableLogging = true;           // Enable DEKI_LOG_* macros in firmware
    bool enableInternalLogging = false;  // Enable verbose engine-internal logs (DEKI_LOG_INTERNAL)
};

// Callback for real-time output lines
using BuildOutputCallback = std::function<void(const std::string& line, bool isError)>;
using BuildProgressCallback = std::function<void(const BuildProgress&)>;

// MCUFamily is defined in PlatformConfig.h — forward-declare the enum
// We include PlatformConfig.h in the .cpp files that need it

// ============================================================================
// IFirmwareBuilder — Abstract interface for platform-specific builders
// ============================================================================

/**
 * @brief Abstract interface for firmware builders
 *
 * Each MCU family (ESP-IDF, Pico SDK, etc.) implements this
 * interface to provide platform-specific build, flash, and toolchain
 * management. Builders can be built-in (linked) or loaded as DLL plugins
 * from ~/.deki-editor/builders/.
 */
class IFirmwareBuilder
{
public:
    virtual ~IFirmwareBuilder() = default;

    // ========================================================================
    // Core operations
    // ========================================================================

    /// Build firmware for the given project
    virtual void Build(const std::string& projectPath,
                       BuildOutputCallback outputCallback = nullptr,
                       BuildProgressCallback progressCallback = nullptr) = 0;

    /// Flash firmware to device
    virtual void Flash(const std::string& projectPath, const std::string& port,
                       BuildOutputCallback outputCallback = nullptr,
                       BuildProgressCallback progressCallback = nullptr) = 0;

    /// Clean build directory
    virtual void Clean(const std::string& projectPath,
                       BuildOutputCallback outputCallback = nullptr,
                       BuildProgressCallback progressCallback = nullptr) = 0;

    /// Set target chip within the builder's family
    virtual void SetTarget(const std::string& projectPath, const std::string& target,
                           BuildOutputCallback outputCallback = nullptr,
                           BuildProgressCallback progressCallback = nullptr) = 0;

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
    virtual void InstallToolchainComponent(const std::string& componentId,
                                           BuildProgressCallback cb = nullptr)
    {
        (void)componentId;
        (void)cb;
    }

    /// Run the postInstall setup script for an already-installed component
    virtual void SetupToolchainComponent(const std::string& componentId,
                                         BuildProgressCallback cb = nullptr)
    {
        (void)componentId;
        (void)cb;
    }

    /// Check for toolchain updates (async). Calls onComplete when done.
    virtual void CheckForToolchainUpdates(std::function<void()> onComplete = nullptr)
    {
        if (onComplete) onComplete();
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
    virtual bool GenerateBuildFiles(const std::string& projectPath,
                                    const PlatformConfig& config,
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

    /// Human-readable builder name (e.g., "ESP-IDF", "Pico SDK")
    virtual const char* GetName() const = 0;

    /// Problems with a platform aimed at this builder, empty when it is fine.
    ///
    /// The builder owns its own settings, so it owns validating them: the
    /// editor must not know that an ESP-IDF target has to be one of six chip
    /// names. A new Espressif chip is then a builder update, not an editor
    /// release.
    virtual std::vector<std::string> ValidatePlatform(const PlatformConfig& config) const
    {
        (void)config;
        return {};
    }

    /// The build framework this builder serves: "espidf", "native",
    /// "tactility". THE dispatch key — a platform names its framework and the
    /// registry hands it to whoever claims that id.
    ///
    /// Not the MCU family, which cannot distinguish two builders for the same
    /// silicon: a Tactility board IS an ESP32-S3, so a family key would hand
    /// its build to ESP-IDF. Not GetPlatformKey() either, which reads the
    /// platform config this builder was already given and so cannot be used to
    /// choose it.
    virtual std::string GetFrameworkId() const = 0;

    /// Optional self-description for the platform editor's builder picker, so
    /// a third-party builder is not anonymous in the UI.
    virtual const char* GetIcon() const { return nullptr; }
    virtual const char* GetDescription() const { return ""; }

    /// List of supported chip targets (e.g., {"esp32", "esp32s3", "esp32c3"})
    virtual std::vector<std::string> GetSupportedTargets() const = 0;

    /// Get the builder's project directory for a given project path.
    /// e.g., "projectPath/builders/esp-idf" for ESP-IDF
    virtual std::string GetBuildDirectory(const std::string& projectPath) const = 0;

    // ========================================================================
    // Package compatibility
    // ========================================================================

    /// Platform key for resolving dependencies in package.json / .deki-package.json
    /// e.g., "espidf", "pico-sdk", "devkitpro"
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
