#include "PlatformConfig.h"
#include <deki/LogSystem.h>
#include <deki-editor/EditorPaths.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <cstdlib>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace DekiEditor
{

// ============================================================================
// PlatformRegistry Implementation
// ============================================================================

PlatformRegistry& PlatformRegistry::Instance()
{
    static PlatformRegistry instance;
    return instance;
}

void PlatformRegistry::RegisterPlatform(const PlatformConfig& config)
{
    m_Platforms[config.id] = config;
}

const PlatformConfig* PlatformRegistry::GetPlatform(const std::string& id) const
{
    auto it = m_Platforms.find(id);
    if (it != m_Platforms.end())
    {
        return &it->second;
    }
    return nullptr;
}

std::vector<std::string> PlatformRegistry::GetAvailablePlatforms() const
{
    std::vector<std::string> platforms;
    platforms.reserve(m_Platforms.size());
    for (const auto& [id, config] : m_Platforms)
    {
        platforms.push_back(id);
    }
    return platforms;
}

std::vector<std::pair<std::string, std::string>> PlatformRegistry::GetPlatformDisplayList() const
{
    std::vector<std::pair<std::string, std::string>> list;
    list.reserve(m_Platforms.size());
    for (const auto& [id, config] : m_Platforms)
    {
        list.emplace_back(id, config.displayName);
    }
    return list;
}

// Helper: parse a single platform config from JSON
const std::vector<std::string>& SupportedIdfTargets()
{
    static const std::vector<std::string> kTargets = { "esp32", "esp32s2", "esp32s3", "esp32c3", "esp32c6", "esp32h2" };
    return kTargets;
}

// The family when a platform did not name one. Only reached by JSON with no
// "family" and no legacy "type"; a capability of "family:custom" then says
// plainly that the platform declined to categorise itself.
static const char* kDefaultFamily = "custom";

std::set<std::string> PlatformConfig::Capabilities() const
{
    std::set<std::string> caps(provides.begin(), provides.end());
    // Derived, so the distinctions that already exist need no authoring: a
    // package tied to one toolchain requires "framework:espidf" and every
    // ESP-IDF board satisfies it without listing anything.
    if (!framework.empty())
        caps.insert("framework:" + framework);
    caps.insert("family:" + (family.empty() ? std::string(kDefaultFamily) : family));

    // Memory regions are capabilities named after the region, so a package
    // that allocates from Deki::Memory::Region("rtc") requires "memory:rtc" and the two
    // agree by construction rather than by someone keeping two lists in step.
    //
    // The prefix matches framework: and family: above, and keeps a region from
    // colliding with an unrelated capability that happens to share a word.
    //
    // Every platform has a heap, so memory:internal is universal. memory:external
    // comes from psramSize, the same field the firmware build reads to emit
    // CONFIG_SPIRAM — one source of truth, because when it was two they drifted:
    // a target created by copying a board without PSRAM and then given a
    // psramSize had the memory, booted with it detected, and still failed to
    // satisfy a package that asked for it.
    //
    // Regions beyond these two are a board's own business and it lists them in
    // `provides` ("memory:rtc"), because only that board knows it has them.
    caps.insert("memory:internal");
    if (psramSize > 0)
        caps.insert("memory:external");

    return caps;
}

// A capability is matched by exact string, so the only names that work are the
// ones everybody spells the same way. Deliberately narrow: lowercase only,
// because "WiFi" silently matching nothing is the failure this prevents.
bool IsValidCapabilityName(const std::string& name)
{
    if (name.empty() || name.size() > 64)
        return false;
    for (unsigned char c : name)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '-' || c == ':' || c == '_';
        if (!ok)
            return false;
    }
    return true;
}

bool IsValidDisplayBus(const std::string& bus)
{
    if (bus.empty() || bus.size() > 32) return false;
    for (unsigned char c : bus)
    {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

static bool LoadPlatformFromJson(const json& j, PlatformConfig& config)
{
    config.id = j.value("id", "");
    if (config.id.empty())
        return false;

    config.displayName = j.value("displayName", config.id);

    // Load family (new format) or type (legacy format). Any spelling is
    // accepted: the editor does not have a list of the world's targets.
    if (j.contains("family"))
    {
        config.family = j.value("family", kDefaultFamily);
        if (config.family.empty())
            config.family = kDefaultFamily;
    }
    else if (j.contains("type"))
    {
        // Legacy format - convert type to family + mcuChip
        std::string typeStr = j.value("type", "custom");
        if (typeStr == "esp32s3")
        {
            config.family = "espressif";
            config.mcuChip = "esp32s3";
        }
        else if (typeStr == "esp32")
        {
            config.family = "espressif";
            config.mcuChip = "esp32";
        }
        else if (typeStr == "esp32c3")
        {
            config.family = "espressif";
            config.mcuChip = "esp32c3";
        }
        else if (typeStr == "simulator")
        {
            config.family = "native";
        }
        else
        {
            config.family = kDefaultFamily;
        }
    }

    // Load mcuChip if not already set by legacy conversion
    if (config.mcuChip.empty())
        config.mcuChip = j.value("mcuChip", "");
    config.boardVariant = j.value("boardVariant", "");

    config.toolchainPrefix = j.value("toolchainPrefix", "");
    config.idfTarget = j.value("idfTarget", "");
    config.framework = j.value("framework", "espidf");
    // idfTarget and displayBus end up inside the shell line that runs idf.py
    // (ESPIDFToolchain::ExecuteIDF) and in generated CMake; a platform JSON
    // can arrive in a board pack, so both are allowlisted here and an invalid
    // value is dropped with an error rather than passed along.
    if (!config.idfTarget.empty())
    {
        const auto& targets = SupportedIdfTargets();
        if (std::find(targets.begin(), targets.end(), config.idfTarget) == targets.end())
        {
            DEKI_LOG_ERROR("Platform '%s': idfTarget '%s' is not a supported ESP-IDF target; ignoring it",
                           config.id.c_str(), config.idfTarget.c_str());
            config.idfTarget.clear();
        }
    }

    config.flashSize = j.value("flashSize", 0u);
    config.psramSize = j.value("psramSize", 0u);
    config.psramMode = j.value("psramMode", "quad");
    config.cpuFreqHz = j.value("cpuFreqHz", 0u);

    config.screenWidth = j.value("screenWidth", 0);
    config.screenHeight = j.value("screenHeight", 0);
    config.displayDriver = j.value("displayDriver", "");
    config.displayBus = j.value("displayBus", "");
    if (!config.displayBus.empty() && !IsValidDisplayBus(config.displayBus))
    {
        DEKI_LOG_ERROR("Platform '%s': displayBus '%s' is not an identifier (A-Z, 0-9, '_'); ignoring it",
                       config.id.c_str(), config.displayBus.c_str());
        config.displayBus.clear();
    }
    config.useIramAttr = j.value("useIramAttr", true);
    config.colorFormat = j.value("colorFormat", "RGB565");

    config.maxAppSize = j.value("maxAppSize", 0u);
    config.flashFreqHz = j.value("flashFreqHz", 0u);

    config.debugInterface = j.value("debugInterface", "");
    config.uploadBaudRate = j.value("uploadBaudRate", 0u);
    config.uploadProtocol = j.value("uploadProtocol", "");
    config.partitionTable = j.value("partitionTable", "");

    if (j.contains("sdkconfig") && j["sdkconfig"].is_array())
        for (const auto& line : j["sdkconfig"])
            if (line.is_string())
                config.sdkconfig.push_back(line.get<std::string>());

    if (j.contains("requiresPackages") && j["requiresPackages"].is_array())
        for (const auto& id : j["requiresPackages"])
            if (id.is_string())
                config.requiresPackages.push_back(id.get<std::string>());

    if (j.contains("provides") && j["provides"].is_array())
        for (const auto& cap : j["provides"])
            if (cap.is_string())
            {
                std::string name = cap.get<std::string>();
                // Capabilities are matched by exact string against what a
                // package requires, so a name that cannot match anything is
                // worth saying out loud at load rather than leaving as a
                // package mysteriously refused on a board that has the
                // hardware. "WiFi" and "wifi " are the two that bite.
                if (!IsValidCapabilityName(name))
                    DEKI_LOG_WARNING("Platform '%s': capability '%s' is not a valid name "
                                     "(lowercase letters, digits and . - : _ only), so no "
                                     "package requirement can match it",
                                     config.id.c_str(), name.c_str());
                config.provides.push_back(std::move(name));
            }

    if (j.contains("defines") && j["defines"].is_array())
        for (const auto& def : j["defines"])
        {
            if (!def.is_string())
                continue;
            const std::string d = def.get<std::string>();

            // Which packages and features a build has is the resolver's answer
            // (FeatureResolver, ResolveActivePackages): a platform asserting
            // one here says a package is present without anything having put
            // it in the build, and the two are then free to disagree. A board
            // that needs a package says so in requiresPackages.
            if (d.rfind("DEKI_PACKAGE_", 0) == 0 || d.rfind("DEKI_FEATURE_", 0) == 0)
            {
                DEKI_LOG_ERROR("Platform '%s': define '%s' names a package or feature, which is "
                               "the build's decision, not the board's; ignoring it. Use "
                               "requiresPackages for a package the board always needs.",
                               config.id.c_str(), d.c_str());
                continue;
            }
            config.defines.push_back(d);
        }

    if (j.contains("cxxFlags") && j["cxxFlags"].is_array())
        for (const auto& flag : j["cxxFlags"])
            config.cxxFlags.push_back(flag.get<std::string>());

    if (j.contains("cFlags") && j["cFlags"].is_array())
        for (const auto& flag : j["cFlags"])
            config.cFlags.push_back(flag.get<std::string>());

    if (j.contains("linkFlags") && j["linkFlags"].is_array())
        for (const auto& flag : j["linkFlags"])
            config.linkFlags.push_back(flag.get<std::string>());

    if (j.contains("requiredLibraries") && j["requiredLibraries"].is_array())
        for (const auto& lib : j["requiredLibraries"])
            config.requiredLibraries.push_back(lib.get<std::string>());

    if (j.contains("pins") && j["pins"].is_array())
    {
        for (const auto& pinObj : j["pins"])
        {
            PinMapping pin;
            pin.name = pinObj.value("name", "");
            pin.gpio = pinObj.value("gpio", -1);
            pin.function = pinObj.value("function", "");
            config.pins.push_back(pin);
        }
    }

    config.bootScene = j.value("bootScene", "");
    return true;
}

bool PlatformRegistry::LoadPlatformsFromDirectory(const std::string& path)
{
    if (!fs::exists(path) || !fs::is_directory(path))
        return false;

    bool loaded = false;
    for (const auto& entry : fs::directory_iterator(path))
    {
        if (!entry.is_directory())
            continue;

        // Each platform lives in its own subdirectory: <id>/<id>.json
        fs::path jsonFile = entry.path() / (entry.path().filename().string() + ".json");
        if (!fs::exists(jsonFile))
            continue;

        try
        {
            std::ifstream file(jsonFile);
            json j;
            file >> j;

            PlatformConfig config;
            if (LoadPlatformFromJson(j, config))
            {
                RegisterPlatform(config);
                loaded = true;
            }
        }
        catch (const std::exception&)
        {
            // Skip invalid JSON files
        }
    }

    return loaded;
}

bool PlatformRegistry::LoadPlatformsFromBoardPacks()
{
    // Get board packs directory: ~/.deki-editor/board-packs/
    std::string boardPacksDir = DekiEditor::EditorPaths::GetBoardPacksDir();

    if (boardPacksDir.empty() || !fs::exists(boardPacksDir))
        return false;

    bool loaded = false;
    try
    {
        for (const auto& packEntry : fs::directory_iterator(boardPacksDir))
        {
            if (!packEntry.is_directory())
                continue;

            // Each board pack directory contains platform JSON files
            for (const auto& platformFile : fs::directory_iterator(packEntry.path()))
            {
                if (platformFile.path().extension() != ".json")
                    continue;
                if (platformFile.path().filename() == "pack.json")
                    continue;

                try
                {
                    std::ifstream file(platformFile.path());
                    json j;
                    file >> j;

                    PlatformConfig config;
                    if (LoadPlatformFromJson(j, config) && !GetPlatform(config.id))
                    {
                        RegisterPlatform(config);
                        loaded = true;
                    }
                }
                catch (const std::exception&)
                {
                    // Skip invalid JSON files
                }
            }
        }
    }
    catch (const std::exception&)
    {
        // Skip errors during directory iteration
    }

    return loaded;
}

void PlatformRegistry::Clear()
{
    m_Platforms.clear();
}

void PlatformRegistry::UnregisterPlatform(const std::string& id)
{
    m_Platforms.erase(id);
}

bool PlatformRegistry::SavePlatformToFile(const PlatformConfig& config, const std::string& directory)
{
    try
    {
        // Ensure platform subdirectory exists: <dir>/<id>/
        fs::path platformDir = fs::path(directory) / config.id;
        fs::create_directories(platformDir);

        // Build JSON — only write non-default values to keep files clean
        json j;
        j["id"] = config.id;
        j["displayName"] = config.displayName;

        j["family"] = config.family.empty() ? kDefaultFamily : config.family;

        if (!config.mcuChip.empty()) j["mcuChip"] = config.mcuChip;
        if (!config.boardVariant.empty()) j["boardVariant"] = config.boardVariant;
        if (!config.toolchainPrefix.empty()) j["toolchainPrefix"] = config.toolchainPrefix;
        if (!config.idfTarget.empty()) j["idfTarget"] = config.idfTarget;
        j["framework"] = config.framework;

        if (config.flashSize > 0) j["flashSize"] = config.flashSize;
        if (config.psramSize > 0) j["psramSize"] = config.psramSize;
        if (!config.psramMode.empty() && config.psramMode != "quad") j["psramMode"] = config.psramMode;
        if (config.maxAppSize > 0) j["maxAppSize"] = config.maxAppSize;
        if (config.cpuFreqHz > 0) j["cpuFreqHz"] = config.cpuFreqHz;
        if (config.flashFreqHz > 0) j["flashFreqHz"] = config.flashFreqHz;

        if (config.screenWidth > 0) j["screenWidth"] = config.screenWidth;
        if (config.screenHeight > 0) j["screenHeight"] = config.screenHeight;
        if (!config.displayDriver.empty()) j["displayDriver"] = config.displayDriver;
        if (!config.displayBus.empty()) j["displayBus"] = config.displayBus;
        if (!config.useIramAttr) j["useIramAttr"] = false;
        if (!config.colorFormat.empty()) j["colorFormat"] = config.colorFormat;

        if (!config.provides.empty()) j["provides"] = config.provides;
        if (!config.requiresPackages.empty()) j["requiresPackages"] = config.requiresPackages;
        if (!config.sdkconfig.empty()) j["sdkconfig"] = config.sdkconfig;
        if (!config.defines.empty()) j["defines"] = config.defines;
        if (!config.cFlags.empty()) j["cFlags"] = config.cFlags;
        if (!config.cxxFlags.empty()) j["cxxFlags"] = config.cxxFlags;
        if (!config.linkFlags.empty()) j["linkFlags"] = config.linkFlags;
        if (!config.requiredLibraries.empty()) j["requiredLibraries"] = config.requiredLibraries;

        if (!config.debugInterface.empty()) j["debugInterface"] = config.debugInterface;
        if (config.uploadBaudRate > 0 && config.uploadBaudRate != 921600)
            j["uploadBaudRate"] = config.uploadBaudRate;
        if (!config.uploadProtocol.empty()) j["uploadProtocol"] = config.uploadProtocol;
        if (!config.partitionTable.empty()) j["partitionTable"] = config.partitionTable;

        if (!config.bootScene.empty()) j["bootScene"] = config.bootScene;

        if (!config.pins.empty())
        {
            json pinsArray = json::array();
            for (const auto& pin : config.pins)
            {
                json pinObj;
                pinObj["name"] = pin.name;
                pinObj["gpio"] = pin.gpio;
                pinObj["function"] = pin.function;
                pinsArray.push_back(pinObj);
            }
            j["pins"] = pinsArray;
        }

        // Write to file: <dir>/<id>/<id>.json
        fs::path filePath = platformDir / (config.id + ".json");
        std::ofstream file(filePath);
        file << j.dump(2);
        file.close();

        // Also register in memory
        RegisterPlatform(config);

        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool PlatformRegistry::DeletePlatform(const std::string& id, const std::string& directory)
{
    try
    {
        // Remove from registry
        UnregisterPlatform(id);

        // Delete entire platform subdirectory: <dir>/<id>/
        fs::path platformDir = fs::path(directory) / id;
        if (fs::exists(platformDir))
        {
            fs::remove_all(platformDir);
        }

        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

// ============================================================================
// Built-in Platform Configurations (Fallback for legacy projects)
// ============================================================================

PlatformConfig CreateSimulatorPlatformConfig()
{
    PlatformConfig config;

    // Identification
    config.id = "native_simulator";
    config.displayName = "Desktop Simulator (SDL3)";
    config.family = "native";
    config.mcuChip = "";

    // Toolchain (native)
    config.toolchainPrefix = "";
    config.idfTarget = "";
    config.framework = "native";

    // What a desktop can offer a package. "sdl3" is here because the simulator
    // is the SDL3 backend, which is how deki-sdl3 ends up in this build and out
    // of a board's; "posix-process" is what the curl HTTP backend needs to fork.
    config.provides = { "desktop-os", "posix-process", "filesystem", "threads", "sdl3" };

    // Memory (unlimited for desktop)
    config.flashSize = 0;
    config.psramSize = 0;
    config.maxAppSize = 0;

    // Display
    config.screenWidth = 320;
    config.screenHeight = 240;
    config.displayDriver = "SDL3";
    config.displayBus = "SOFTWARE";

    // Compiler defines. DEKI_PACKAGE_SDL3 used to be here, asserting a package
    // the resolver had not necessarily put in the build; deki-sdl3 is chosen
    // now because it needs "sdl3" and this platform provides it.
    config.defines = {
        "SIMULATOR",
        "DEBUG",
        "DEKI_LOG_ENABLED"
    };

    // C++ flags. No "-std=" here: the standard is the build's decision
    // (NativeBuilder writes CMAKE_CXX_STANDARD 23, matching the editor-side
    // package build and what ESP-IDF forces for a device). This used to say
    // "-std=c++17", which landed after CMake's own flag and quietly won, so
    // the simulator could not compile the engine once its headers reached
    // C++20. NativeBuilder now drops any "-std=" it finds here, which also
    // rescues the platform JSON already written into existing projects.
    config.cxxFlags = {
        "-O0",
        "-g"
    };

    return config;
}

void RegisterBuiltInPlatforms()
{
    PlatformRegistry& registry = PlatformRegistry::Instance();

    // Only register Simulator as fallback for legacy projects
    registry.RegisterPlatform(CreateSimulatorPlatformConfig());
}

}  // namespace DekiEditor
