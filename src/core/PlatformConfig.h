#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstdint>

namespace DekiEditor
{

/**
 * @brief MCU family enumeration for supported target platforms
 *
 * This is a broad category that groups similar MCUs together.
 * The specific chip variant is stored in PlatformConfig::mcuChip.
 */
enum class MCUFamily
{
    Native,     // Desktop simulator (SDL3)
    Espressif,  // ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6, ESP32-H2
    RP2040,     // Raspberry Pi Pico, Pico W
    SAMD,       // SAMD21, SAMD51 (Adafruit M0/M4)
    NRF,        // Nordic nRF52840
    Custom      // User-defined / other
};

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
/// The ESP-IDF chips idf.py accepts as set-target. One list, used by the
/// platform loader (a platform JSON naming anything else is refused), the
/// ESP-IDF builder and the toolchain descriptor.
const std::vector<std::string>& SupportedIdfTargets();

/// displayBus is an identifier: A-Z, 0-9 and '_' ("SPI", "PARALLEL_8BIT", "SOFTWARE").
bool IsValidDisplayBus(const std::string& bus);
bool IsValidCapabilityName(const std::string& name);

struct PlatformConfig
{
    // Identification
    std::string id;           // Unique identifier: "esp32s3_v_board"
    std::string displayName;  // Human-readable: "V-Board (ESP32-S3)"
    MCUFamily family = MCUFamily::Custom;
    std::string mcuChip;  // Specific chip: "esp32s3", "rp2040"

    // Framework
    std::string framework;     // "espidf", "pico-sdk"
    std::string boardVariant;  // Board variant name

    // Toolchain
    std::string toolchainPrefix;  // "xtensa-esp32s3-elf-" (auto-detected based on family)
    std::string idfTarget;        // ESP-specific: "esp32s3", "esp32c3", etc.

    // Memory
    uint32_t flashSize = 0;   // Flash size in bytes
    uint32_t psramSize = 0;   // PSRAM size in bytes (0 if none)
    std::string psramMode;    // "quad" (default) or "oct" — SPI mode for PSRAM
    uint32_t maxAppSize = 0;  // Maximum application size (from partition table)
    std::vector<MemoryRegion> memoryMap;

    // Clocks
    uint32_t cpuFreqHz = 0;    // CPU frequency
    uint32_t flashFreqHz = 0;  // Flash SPI frequency

    // Display
    int screenWidth = 0;
    int screenHeight = 0;
    std::string displayDriver;  // "ILI9341", "ST7789", etc.
    std::string displayBus;     // "SPI", "PARALLEL_8BIT", etc.
    bool useIramAttr = true;    // Map DEKI_FAST_ATTR to IRAM_ATTR for performance-critical functions
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

    // Extra ESP-IDF options for this board, as "CONFIG_X=y" lines appended to
    // the generated sdkconfig.defaults.
    //
    // The generated file is rewritten on every build, so a board previously had
    // no way to say anything the editor did not already know how to emit, and
    // hand-editing it lasted exactly one build. Boards differ in ways the
    // editor should not have to enumerate: flash mode for an emulator, a
    // peripheral its driver needs, a stack size for a heavy component.
    //
    // Last word: these are appended after everything generated, so a board can
    // override what the editor chose.
    std::vector<std::string> sdkconfig;

    // Libraries
    std::vector<std::string> requiredLibraries;

    // Debug settings
    std::string debugInterface;  // "builtin-jtag", "ftdi", "esp-prog", "stlink", "cmsis-dap", "jlink"

    // Upload settings
    int uploadBaudRate = 921600;
    std::string uploadProtocol;  // "esptool"

    // Partition table (CSV format)
    std::string partitionTable;

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
class PlatformRegistry
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
void RegisterBuiltInPlatforms();

/**
 * @brief Create the desktop simulator platform configuration
 * @return Platform configuration for SDL3 simulator builds
 */
PlatformConfig CreateSimulatorPlatformConfig();

}  // namespace DekiEditor
