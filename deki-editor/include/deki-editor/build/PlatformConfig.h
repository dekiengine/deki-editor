#pragma once

#include <deki-editor/EditorExport.h>

#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstdint>

namespace DekiEditor
{

// A platform's family ("espressif", "native", "rp2040", a console's name) is a
// free-form string, like `framework`, so a new target never needs an editor
// change; a vendor under NDA could not make one.
//
// The family is descriptive only: a capability source ("family:<x>") and a UI
// grouping. Builders are chosen by `framework`, never by family. See
// BuilderDispatch.h.

DEKI_EDITOR_API bool IsValidCapabilityName(const std::string& name);

/// What the editor knows about a platform: its identity, the framework whose
/// backend builds for it, that backend's settings (carried, not interpreted),
/// its screen, compiler flags, what it offers packages and what it needs from
/// them, and the scene it boots.
struct DEKI_EDITOR_API PlatformConfig
{
    std::string id;           // Unique identifier; also its directory under platforms/
    std::string displayName;  // Human-readable
    std::string family;       // Vendor family, descriptive only (see above)

    // The id of the build framework; whichever backend claims it builds this
    // platform. No default: a platform that does not say gets no backend.
    std::string framework;

    // Settings that belong to the build framework (an ESP-IDF target, an
    // sdkconfig, a partition table...), as a JSON object in text form. The
    // editor saves and loads it without reading it; only the builder claiming
    // `framework` knows what is in there.
    //
    // Text rather than a parsed object on purpose: this struct crosses a DLL
    // boundary to third-party builders, and std::string has a stable ABI there
    // that a JSON library's type does not.
    std::string frameworkOptions;  // JSON object, "{}" when empty

    /// Reads one value out of frameworkOptions. A missing or wrong-typed key
    /// gives the fallback, so a builder need not guard every lookup.
    std::string Option(const std::string& key, const std::string& fallback = "") const;
    uint32_t OptionU32(const std::string& key, uint32_t fallback = 0) const;
    bool OptionBool(const std::string& key, bool fallback = false) const;
    std::vector<std::string> OptionList(const std::string& key) const;

    /// Sets one value, creating the object if needed. For the platform editor
    /// and the MCP tool, which do not know what the keys mean.
    void SetOption(const std::string& key, const std::string& value);
    void SetOptionU32(const std::string& key, uint32_t value);
    void SetOptionBool(const std::string& key, bool value);
    void SetOptionList(const std::string& key, const std::vector<std::string>& values);

    // Memory the engine can allocate from beyond the internal heap, in bytes;
    // 0 if there is none. This is the engine's term (Deki::Memory::External),
    // not any chip's: whatever the target calls it (PSRAM on a microcontroller,
    // a second heap on a console), a package that allocates from that region
    // requires "memory:external", and a non-zero size grants it. See
    // Capabilities(). Chip details (flash, clock, pins) belong in
    // frameworkOptions.
    uint32_t externalMemorySize = 0;

    int screenWidth = 0;
    int screenHeight = 0;
    std::string colorFormat;  // "RGB565", "RGB888", "RGBA8888"; maps to Deki::ColorFormat

    std::vector<std::string> defines;    // Preprocessor defines
    std::vector<std::string> cFlags;     // C-specific flags
    std::vector<std::string> cxxFlags;   // C++-specific flags
    std::vector<std::string> linkFlags;  // Linker flags

    // What this platform can offer a package, as free-form capability names
    // ("desktop-os", "sdl3", "posix-process", "wifi", "bluetooth", "spi").
    //
    // A package says what it needs rather than which platforms it supports, so
    // a platform nobody has heard of yet still works: it declares what it can
    // supply, and the packages that need only those things build on it.
    //
    // Do not read this directly; call Capabilities(), which adds the ones
    // derived from other fields.
    std::vector<std::string> provides;

    /// `provides`, plus "framework:<framework>" and "family:<family>" derived
    /// from this config. A package truly tied to one toolchain can require
    /// those without anyone writing a new list.
    std::set<std::string> Capabilities() const;

    // Packages this platform needs whether or not any scene names one of their
    // components, typically its hardware abstraction layer. Package ids, e.g.
    // ["deki-esp32-integration"]. A board that declares none gets none.
    std::vector<std::string> requiresPackages;

    // Where a build keeps its assets: "internal" (part of the device: a
    // board's flash, an app bundle, the disk) or "external" (removable, an SD
    // card). External needs the "external-storage" capability, so only a
    // target that has a card can choose it.
    std::string assetStorage = "internal";

    // The scene a build of this platform boots: a PlatformSetupComponent and
    // the setup components that start the target's display, input and storage.
    // Relative to the project.
    std::string bootScene;
};

/// A platform as JSON text, in the one form SavePlatformToFile writes. Reading
/// accepts every older spelling; writing produces only this one, so two
/// platform files mean the same thing exactly when their round trips are equal.
DEKI_EDITOR_API bool PlatformFromJson(const std::string& text, PlatformConfig& config);
DEKI_EDITOR_API std::string PlatformToJson(const PlatformConfig& config);

/// The available platforms, all loaded from JSON: the project's own, those its
/// packages offer, and board packs.
class DEKI_EDITOR_API PlatformRegistry
{
public:
    static PlatformRegistry& Instance();

    void RegisterPlatform(const PlatformConfig& config);

    /// The platform with this id, or nullptr.
    const PlatformConfig* GetPlatform(const std::string& id) const;

    std::vector<std::string> GetAvailablePlatforms() const;

    /// (id, display name) pairs, for the UI.
    std::vector<std::pair<std::string, std::string>> GetPlatformDisplayList() const;

    /// Loads the platform JSON files in `path`. True if any were loaded.
    bool LoadPlatformsFromDirectory(const std::string& path);

    /// Loads platforms from installed board packs (~/.deki-editor/board-packs/).
    /// Skips ids already registered, so project platforms take priority. True
    /// if any were loaded.
    bool LoadPlatformsFromBoardPacks();

    void Clear();

    /// Saves `config` as a JSON file in `directory`. True on success.
    bool SavePlatformToFile(const PlatformConfig& config, const std::string& directory);

    /// Removes a platform from the registry and deletes its JSON file in
    /// `directory`. True on success.
    bool DeletePlatform(const std::string& id, const std::string& directory);

    /// Removes a platform from the registry; its file stays.
    void UnregisterPlatform(const std::string& id);

private:
    PlatformRegistry() = default;
    ~PlatformRegistry() = default;
    PlatformRegistry(const PlatformRegistry&) = delete;
    PlatformRegistry& operator=(const PlatformRegistry&) = delete;

    std::map<std::string, PlatformConfig> m_Platforms;
};

}  // namespace DekiEditor
