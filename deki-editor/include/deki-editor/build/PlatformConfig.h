#pragma once

#include <deki-editor/EditorExport.h>

#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstdint>

namespace DekiEditor
{

// The family — "espressif", "native", "rp2040", a console's name — used to be
// a closed enum. It cannot be: the editor is target-agnostic, and an enum here
// means every new target patches the editor, which is exactly what a vendor
// under NDA cannot do. It is a free-form string, like `framework`, and the
// on-disk JSON has always stored it as text.
//
// It is DESCRIPTIVE ONLY: a capability source ("family:<x>") and a UI grouping.
// Builders are chosen by `framework`, never by this. See BuilderDispatch.h.

/**
 * @brief Memory region definition for partition layouts
 */
struct MemoryRegion
{
    std::string name;
    uint32_t start = 0;
    uint32_t size = 0;
    bool isFlash = false;
    bool isPSRAM = false;
};

/**
 * @brief Pin mapping for hardware configuration
 */
struct PinMapping
{
    std::string name;
    int gpio = -1;
    std::string function;  // "SPI_MOSI", "I2C_SDA", etc.
};

/**
 * @brief Complete platform configuration for firmware builds
 *
 * Contains all information needed to build firmware for a specific platform:
 * toolchain settings, memory layout, display configuration, compiler flags, etc.
 */
/// displayBus is an identifier: A-Z, 0-9 and '_' ("SPI", "PARALLEL_8BIT", "SOFTWARE").
DEKI_EDITOR_API bool IsValidDisplayBus(const std::string& bus);
DEKI_EDITOR_API bool IsValidCapabilityName(const std::string& name);

struct DEKI_EDITOR_API PlatformConfig
{
    // Identification
    std::string id;           // Unique identifier: "esp32s3_v_board"
    std::string displayName;  // Human-readable: "V-Board (ESP32-S3)"
    std::string family;   // "espressif", "native", ... — descriptive, see above
    std::string mcuChip;  // Specific chip: "esp32s3", "rp2040"

    // Framework
    std::string framework;     // "espidf", "pico-sdk"
    std::string boardVariant;  // Board variant name

    // Settings that belong to the build framework, as a JSON object in text
    // form. The editor round-trips it WITHOUT interpreting it; only the
    // builder claiming `framework` knows what is in there.
    //
    // This used to be a dozen named fields on this struct — idfTarget,
    // sdkconfig, partitionTable, uploadProtocol ("esptool"), useIramAttr,
    // psramMode ("quad"/"oct"), toolchainPrefix ("xtensa-esp32s3-elf-"),
    // debugInterface, flashFreqHz — which made a supposedly generic platform
    // type into an ESP-IDF config file. A console platform would have
    // inherited flash-frequency fields, and a new setting for any target
    // meant editing the editor.
    //
    // Text rather than a parsed object on purpose: this struct crosses a DLL
    // boundary to third-party builders, and a std::string has an ABI a JSON
    // library's type does not.
    std::string frameworkOptions;  // JSON object, "{}" when empty

    /// Read one value out of frameworkOptions. Missing or wrong-typed keys
    /// give the default, so a builder never has to guard every lookup.
    std::string Option(const std::string& key, const std::string& fallback = "") const;
    uint32_t OptionU32(const std::string& key, uint32_t fallback = 0) const;
    bool OptionBool(const std::string& key, bool fallback = false) const;
    std::vector<std::string> OptionList(const std::string& key) const;

    /// Set one value, creating the object if needed. For the platform editor
    /// and the MCP tool, which must not need to know what the keys mean.
    void SetOption(const std::string& key, const std::string& value);
    void SetOptionBool(const std::string& key, bool value);
    void SetOptionList(const std::string& key, const std::vector<std::string>& values);

    // Memory
    uint32_t flashSize = 0;   // Flash size in bytes
    uint32_t psramSize = 0;   // PSRAM size in bytes (0 if none)
    uint32_t maxAppSize = 0;  // Maximum application size (from partition table)
    std::vector<MemoryRegion> memoryMap;

    // Clocks
    uint32_t cpuFreqHz = 0;    // CPU frequency

    // Display
    int screenWidth = 0;
    int screenHeight = 0;
    std::string displayDriver;  // "ILI9341", "ST7789", etc.
    std::string displayBus;     // "SPI", "PARALLEL_8BIT", etc.
    std::string colorFormat;    // "RGB565", "RGB888", "RGBA8888" — maps to Deki::ColorFormat

    // Pin mappings
    std::vector<PinMapping> pins;

    // Compiler flags
    std::vector<std::string> defines;    // Preprocessor defines
    std::vector<std::string> cFlags;     // C-specific flags
    std::vector<std::string> cxxFlags;   // C++-specific flags
    std::vector<std::string> linkFlags;  // Linker flags

    // What this platform can offer a package, as free-form capability names
    // ("desktop-os", "sdl3", "posix-process", "wifi", "bluetooth", "spi").
    //
    // A package says what it needs rather than which platforms it likes, so a
    // platform nobody has heard of yet still works: it declares what it can
    // supply and the packages that need only those things build on it. The
    // allowlist this replaces got that backwards — a third-party board was
    // refused for the sole reason that its name was not on a list written
    // before it existed.
    //
    // Never read this directly; call Capabilities(), which adds the ones
    // derived from fields the platform already has.
    std::vector<std::string> provides;

    // provides, plus "framework:<framework>" and "family:<family>" derived from
    // this config. A package genuinely tied to one toolchain can require those
    // without anyone authoring a new list.
    std::set<std::string> Capabilities() const;

    // Packages this platform needs whether or not any scene names one of their
    // components — its hardware abstraction layer, typically. Package ids, e.g.
    // ["deki-esp32-integration"].
    //
    // The ESP-IDF builder used to force its own HAL on by name, which works for
    // exactly one family: a third-party framework would have had to patch the
    // editor to get the same. A board declares its own now, and one that
    // declares nothing gets nothing.
    std::vector<std::string> requiresPackages;


    // Libraries
    std::vector<std::string> requiredLibraries;

    // Debug settings

    // Upload settings
    int uploadBaudRate = 921600;

    // Partition table (CSV format)

    // Boot scene - loaded from flash (F:/) to initialize platform hardware
    // Contains PlatformSetupComponent that sets up SD card, etc.
    std::string bootScene;  // Path relative to assets, e.g., "scenes/boot.scene"
};

/**
 * @brief Registry for available platforms
 *
 * Singleton that manages platform configurations. Built-in platforms
 * are registered at startup, and custom platforms can be loaded from JSON.
 */
class DEKI_EDITOR_API PlatformRegistry
{
public:
    /**
     * @brief Get the singleton instance
     */
    static PlatformRegistry& Instance();

    /**
     * @brief Register a platform configuration
     * @param config The platform configuration to register
     */
    void RegisterPlatform(const PlatformConfig& config);

    /**
     * @brief Get a platform by ID
     * @param id Platform identifier
     * @return Pointer to platform config, or nullptr if not found
     */
    const PlatformConfig* GetPlatform(const std::string& id) const;

    /**
     * @brief Get list of all available platform IDs
     */
    std::vector<std::string> GetAvailablePlatforms() const;

    /**
     * @brief Get list of platform display names (for UI)
     */
    std::vector<std::pair<std::string, std::string>> GetPlatformDisplayList() const;

    /**
     * @brief Load custom platforms from a directory
     * @param path Directory containing platform JSON files
     * @return true if any platforms were loaded
     */
    bool LoadPlatformsFromDirectory(const std::string& path);

    /**
     * @brief Load platforms from installed board packs (~/.deki-editor/board-packs/)
     * @return true if any platforms were loaded
     *
     * Scans all board pack directories for platform JSON files.
     * Only registers platforms not already in the registry (project platforms take priority).
     */
    bool LoadPlatformsFromBoardPacks();

    /**
     * @brief Clear all registered platforms
     */
    void Clear();

    /**
     * @brief Save a platform configuration to a JSON file
     * @param config The platform configuration to save
     * @param directory Directory to save the JSON file in
     * @return true if saved successfully
     */
    bool SavePlatformToFile(const PlatformConfig& config, const std::string& directory);

    /**
     * @brief Delete a platform from the registry and its JSON file
     * @param id Platform identifier to delete
     * @param directory Directory containing the platform JSON files
     * @return true if deleted successfully
     */
    bool DeletePlatform(const std::string& id, const std::string& directory);

    /**
     * @brief Unregister a platform from the registry (without deleting file)
     * @param id Platform identifier to unregister
     */
    void UnregisterPlatform(const std::string& id);

private:
    PlatformRegistry() = default;
    ~PlatformRegistry() = default;
    PlatformRegistry(const PlatformRegistry&) = delete;
    PlatformRegistry& operator=(const PlatformRegistry&) = delete;

    std::map<std::string, PlatformConfig> m_Platforms;
};

/**
 * @brief Register built-in fallback platforms
 *
 * Called when no platforms are loaded from the project directory.
 * Only registers the Simulator as a fallback for legacy projects.
 */
DEKI_EDITOR_API void RegisterBuiltInPlatforms();

/**
 * @brief Create the desktop simulator platform configuration
 * @return Platform configuration for SDL3 simulator builds
 */
DEKI_EDITOR_API PlatformConfig CreateSimulatorPlatformConfig();

}  // namespace DekiEditor
