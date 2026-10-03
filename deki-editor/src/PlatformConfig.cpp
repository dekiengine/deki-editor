#include <deki-editor/build/PlatformConfig.h>
#include <deki-editor/build/PlatformTemplates.h>
#include <deki/LogSystem.h>
#include <deki-editor/EditorPaths.h>
#include <deki-editor/SafeNames.h>
#include <deki-editor/FileIO.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <set>

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

// The family when a platform did not name one. Only reached by JSON with no
// "family" and no legacy "type"; a capability of "family:custom" then says
// plainly that the platform declined to categorise itself.
static const char* kDefaultFamily = "custom";

// ---- frameworkOptions accessors -------------------------------------------
// The editor never interprets these; it only carries them. Every lookup is
// total: a missing key, a malformed blob or a wrong type gives the fallback,
// so a builder reads its settings without guarding each one.

static json ParseOptions(const std::string& text)
{
    if (text.empty())
        return json::object();
    json parsed = json::parse(text, nullptr, false);
    return parsed.is_object() ? parsed : json::object();
}

std::string PlatformConfig::Option(const std::string& key, const std::string& fallback) const
{
    const json o = ParseOptions(frameworkOptions);
    const auto it = o.find(key);
    return (it != o.end() && it->is_string()) ? it->get<std::string>() : fallback;
}

uint32_t PlatformConfig::OptionU32(const std::string& key, uint32_t fallback) const
{
    const json o = ParseOptions(frameworkOptions);
    const auto it = o.find(key);
    return (it != o.end() && it->is_number_unsigned()) ? it->get<uint32_t>() : fallback;
}

bool PlatformConfig::OptionBool(const std::string& key, bool fallback) const
{
    const json o = ParseOptions(frameworkOptions);
    const auto it = o.find(key);
    return (it != o.end() && it->is_boolean()) ? it->get<bool>() : fallback;
}

std::vector<std::string> PlatformConfig::OptionList(const std::string& key) const
{
    std::vector<std::string> out;
    const json o = ParseOptions(frameworkOptions);
    const auto it = o.find(key);
    if (it != o.end() && it->is_array())
        for (const auto& v : *it)
            if (v.is_string())
                out.push_back(v.get<std::string>());
    return out;
}

void PlatformConfig::SetOption(const std::string& key, const std::string& value)
{
    json o = ParseOptions(frameworkOptions);
    o[key] = value;
    frameworkOptions = o.dump();
}

void PlatformConfig::SetOptionU32(const std::string& key, uint32_t value)
{
    json o = ParseOptions(frameworkOptions);
    o[key] = value;
    frameworkOptions = o.dump();
}

void PlatformConfig::SetOptionBool(const std::string& key, bool value)
{
    json o = ParseOptions(frameworkOptions);
    o[key] = value;
    frameworkOptions = o.dump();
}

void PlatformConfig::SetOptionList(const std::string& key, const std::vector<std::string>& values)
{
    json o = ParseOptions(frameworkOptions);
    o[key] = values;
    frameworkOptions = o.dump();
}

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
    // comes from externalMemorySize, the same field a backend reads to switch
    // that memory on in its build - one source of truth, because when it was
    // two they drifted: a target created by copying a board without external
    // memory and then given some had the memory, booted with it detected, and
    // still failed to satisfy a package that asked for it.
    //
    // Regions beyond these two are a board's own business and it lists them in
    // `provides` ("memory:rtc"), because only that board knows it has them.
    caps.insert("memory:internal");
    if (externalMemorySize > 0)
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

static bool LoadPlatformFromJson(const json& j, PlatformConfig& config)
{
    config.id = j.value("id", "");
    if (config.id.empty())
        return false;
    // The id names the platform's folder (deleted when the platform is
    // restored) and its build directory: a project someone sent you must not
    // be able to point those anywhere with "../" or a drive letter.
    std::string why;
    if (!SafeNames::IsSafeName(config.id, why))
    {
        DEKI_LOG_ERROR("Platform '%s' is not loaded: its id %s", config.id.c_str(), why.c_str());
        return false;
    }

    // Shown in the UI and written into generated build files as a comment, so
    // a line break in it could add build commands: control characters go.
    config.displayName = j.value("displayName", config.id);
    std::erase_if(config.displayName, [](unsigned char c) { return c < 0x20 || c == 0x7F; });

    // Any spelling is accepted: the editor does not have a list of the world's
    // targets. (A pre-0.10 "type" key used to be translated here through a
    // table of three chip names; no platform file has carried one since.)
    config.family = j.value("family", kDefaultFamily);
    if (config.family.empty())
        config.family = kDefaultFamily;


    // No default, and no guess. The framework is what chooses a build backend;
    // a platform that does not say resolves to no backend, and the build says
    // so by name. It used to default to one framework, and then to be inferred
    // from that framework's settings - the editor recognising a target by its
    // keys is the same knowledge by another route.
    config.framework = j.value("framework", "");

    // Framework settings. The nested "frameworkOptions" object is the shape we
    // write. Platform files from before it existed keep those settings at the
    // top level, and they are read into the same place so nothing needs
    // migrating by hand. One-way: SavePlatformToFile only writes the nested
    // form.
    //
    // WHICH top-level keys are framework settings is decided without naming
    // any: every key this loader does not own is the framework's. It used to
    // be a list of nine of one framework's setting names, which meant the
    // editor knew them, and that any other framework's older files lost theirs.
    //
    // Their VALUES are not judged here either. The owning backend says so
    // through ITargetBuilder::ValidatePlatform and the build refuses. A bad
    // value used to be silently cleared, which turned a typo into a confusing
    // failure much further downstream.
    {
        static const std::set<std::string> kOwnKeys = {
            "id",          "displayName", "family",   "framework", "frameworkOptions",  "externalMemorySize",
            "psramSize",   "screenWidth", "screenHeight", "colorFormat", "defines",     "cFlags",
            "cxxFlags",    "linkFlags",   "provides", "requiresPackages", "bootScene", "assetStorage" };

        json options = json::object();
        if (j.contains("frameworkOptions") && j["frameworkOptions"].is_object())
            options = j["frameworkOptions"];

        for (auto it = j.begin(); it != j.end(); ++it)
            if (kOwnKeys.count(it.key()) == 0 && !options.contains(it.key()))
                options[it.key()] = it.value();

        if (!options.empty())
            config.frameworkOptions = options.dump();
    }

    // "psramSize" is what this key was called when the platform type
    // described a microcontroller. Read, never written.
    config.externalMemorySize = j.value("externalMemorySize", j.value("psramSize", 0u));

    config.screenWidth = j.value("screenWidth", 0);
    config.screenHeight = j.value("screenHeight", 0);
    config.colorFormat = j.value("colorFormat", "RGB565");
    config.assetStorage = j.value("assetStorage", "internal");

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

    config.bootScene = j.value("bootScene", "");
    return true;
}

bool PlatformFromJson(const std::string& text, PlatformConfig& config)
{
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object())
        return false;
    config = PlatformConfig();
    return LoadPlatformFromJson(j, config);
}

std::string PlatformToJson(const PlatformConfig& config)
{
    // Only non-default values, to keep files clean
    json j;
    j["id"] = config.id;
    j["displayName"] = config.displayName;

    j["family"] = config.family.empty() ? kDefaultFamily : config.family;

    // Written back as one nested object, never as the legacy top-level
    // keys: the editor does not know what any of them mean.
    if (!config.frameworkOptions.empty())
    {
        json options = json::parse(config.frameworkOptions, nullptr, false);
        if (options.is_object() && !options.empty())
            j["frameworkOptions"] = options;
    }

    j["framework"] = config.framework;

    if (config.externalMemorySize > 0) j["externalMemorySize"] = config.externalMemorySize;

    if (config.screenWidth > 0) j["screenWidth"] = config.screenWidth;
    if (config.screenHeight > 0) j["screenHeight"] = config.screenHeight;
    if (!config.colorFormat.empty()) j["colorFormat"] = config.colorFormat;

    if (!config.provides.empty()) j["provides"] = config.provides;
    if (!config.requiresPackages.empty()) j["requiresPackages"] = config.requiresPackages;
    if (!config.defines.empty()) j["defines"] = config.defines;
    if (!config.cFlags.empty()) j["cFlags"] = config.cFlags;
    if (!config.cxxFlags.empty()) j["cxxFlags"] = config.cxxFlags;
    if (!config.linkFlags.empty()) j["linkFlags"] = config.linkFlags;

    if (!config.bootScene.empty()) j["bootScene"] = config.bootScene;
    if (config.assetStorage != "internal") j["assetStorage"] = config.assetStorage;

    return j.dump(2);
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

        // Write to file: <dir>/<id>/<id>.json, in one step. A plain stream
        // left a cut-off platform file on a crash, and a failed write still
        // reported success.
        fs::path filePath = platformDir / (config.id + ".json");
        std::string err;
        if (!AtomicWriteFile(filePath.string(), PlatformToJson(config), err))
        {
            DEKI_LOG_ERROR("Could not save platform '%s': %s", config.id.c_str(), err.c_str());
            return false;
        }

        // Also register in memory
        RegisterPlatform(config);
        NoteProjectPlatformsChanged();

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
        NoteProjectPlatformsChanged();

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

}  // namespace DekiEditor
