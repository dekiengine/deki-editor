#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * @file DekiBuilderPlugin.h
 * @brief Plugin interface for Deki firmware builder extensions.
 *
 * Hardware makers or community developers create a DLL that exports
 * these functions to add support for new MCU families (e.g., Pico SDK,
 * Pico SDK). The editor loads builder plugins at startup from
 * ~/.deki-editor/builders/.
 *
 * Usage:
 *   1. Include this header in your builder project
 *   2. Implement ITargetBuilder for your platform
 *   3. Export the required C functions below
 *   4. Build as a shared library (DLL/so/dylib)
 *   5. Place in ~/.deki-editor/builders/
 *
 * Example implementation:
 * @code
 * #include <DekiBuilderPlugin.h>
 * #include "MyPicoSDKBuilder.h"
 *
 * DEKI_BUILDER_API const char* DekiBuilder_GetName() { return "Pico SDK Builder"; }
 * DEKI_BUILDER_API const char* DekiBuilder_GetVersion() { return "1.0.0"; }
 * DEKI_BUILDER_API int DekiBuilder_GetBuilderCount() { return 1; }
 * DEKI_BUILDER_API ITargetBuilder* DekiBuilder_CreateBuilder(int index) {
 *     return new MyPicoSDKBuilder();
 * }
 * DEKI_BUILDER_API void DekiBuilder_DestroyBuilder(ITargetBuilder* builder) {
 *     delete builder;
 * }
 * @endcode
 */

// Export/import macros following the same pattern as DekiPlugin.h
#ifdef _WIN32
#ifdef DEKI_BUILDER_IMPORTS
#define DEKI_BUILDER_API __declspec(dllimport)
#elif defined(DEKI_BUILDER_EXPORTS)
#define DEKI_BUILDER_API __declspec(dllexport)
#else
#define DEKI_BUILDER_API
#endif
#else
#define DEKI_BUILDER_API __attribute__((visibility("default")))
#endif

// Forward declaration
#ifdef __cplusplus
namespace DekiEditor
{
class ITargetBuilder;
}
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Get the plugin name for display in the editor
     * @return Human-readable name (e.g., "Pico SDK Builder")
     */
/**
 * @brief What the plugin was compiled against.
 *
 * A builder plugin shares C++ objects with the editor — PlatformConfig by
 * reference, std::string and std::vector across the boundary, a vtable it
 * inherits. That only works when both sides agree on the layout of those
 * things, and when they disagree the failure is a corrupted field or a crash
 * somewhere unrelated, not an error.
 *
 * So the editor refuses to load a plugin that cannot prove it matches. This
 * mirrors the reflection ABI guard the package loader already uses.
 *
 * Read `version` THROUGH the pointer before copying the struct: if the plugin
 * was built against a different revision, the struct it returns may be a
 * different size, and copying it first is the bug PluginLoader documents.
 */
struct DekiBuilderAbi
{
    uint32_t version;         ///< kDekiBuilderAbiVersion the plugin compiled with
    uint32_t structSize;      ///< sizeof(DekiBuilderAbi) as the plugin sees it
    uint32_t platformConfigSize;   ///< sizeof(DekiEditor::PlatformConfig)
    uint32_t packageEntrySize;     ///< sizeof(DekiEditor::CMakeGen::PackageEntry)
    uint32_t stdStringSize;        ///< sizeof(std::string)
    uint32_t stdVectorSize;        ///< sizeof(std::vector<int>)
    uint32_t compilerTag;     ///< compiler + STL identity; see DekiBuilder_ThisAbi
};

/// Bump when the builder interface changes shape: a new virtual, a reordered
/// one, a changed signature, or a change to the layout of a struct that
/// crosses the boundary.
///
/// 2: Flash/SetTarget/GetSupportedTargets/SupportsFlash replaced by the deploy
///    step; GetBootPayloadDirectory and DescribePlatform added;
///    CMakeGen::PackageEntry carries framework dependencies as written. The
///    handshake also gained packageEntrySize, because ScanPackageManifests
///    returns those by value: a backend built against another layout destroyed
///    them with the wrong destructor and took the editor down with it.
constexpr uint32_t kDekiBuilderAbiVersion = 2;

/// The compiler and standard library this translation unit is using. A MinGW
/// plugin and an MSVC editor lay out std::string differently, and nothing else
/// here would catch it.
constexpr uint32_t DekiBuilder_CompilerTag()
{
    uint32_t tag = 0;
#if defined(_MSC_VER)
    tag = 0x4d000000u | (uint32_t)(_MSC_VER);
#elif defined(__GNUC__)
    tag = 0x47000000u | ((uint32_t)__GNUC__ << 8) | (uint32_t)__GNUC_MINOR__;
#endif
#if defined(_GLIBCXX_USE_CXX11_ABI)
    tag ^= (uint32_t)(_GLIBCXX_USE_CXX11_ABI) << 16;
#endif
    return tag;
}

/// Fill in from the plugin's own point of view. A plugin implements
/// DekiBuilder_GetAbi by returning a static filled with this.
inline DekiBuilderAbi DekiBuilder_ThisAbi(uint32_t platformConfigSize, uint32_t packageEntrySize)
{
    DekiBuilderAbi abi{};
    abi.version = kDekiBuilderAbiVersion;
    abi.structSize = (uint32_t)sizeof(DekiBuilderAbi);
    abi.platformConfigSize = platformConfigSize;
    abi.packageEntrySize = packageEntrySize;
    abi.stdStringSize = (uint32_t)sizeof(std::string);
    abi.stdVectorSize = (uint32_t)sizeof(std::vector<int>);
    abi.compilerTag = DekiBuilder_CompilerTag();
    return abi;
}

/**
 * @brief REQUIRED. What this plugin was compiled against.
 *
 * A plugin without this export is refused: there is no legacy builder plugin
 * to be kind to, because LoadBuilderPlugins() has never had a caller.
 */
    DEKI_BUILDER_API const DekiBuilderAbi* DekiBuilder_GetAbi(void);

    DEKI_BUILDER_API const char* DekiBuilder_GetName(void);

    /**
     * @brief Get the plugin version
     * @return Version string (e.g., "1.0.0")
     */
    DEKI_BUILDER_API const char* DekiBuilder_GetVersion(void);

    /**
     * @brief Get the number of builders provided by this plugin
     * @return Number of builders (usually 1, but a plugin could provide multiple)
     */
    DEKI_BUILDER_API int DekiBuilder_GetBuilderCount(void);

/**
 * @brief Create a builder instance by index
 * @param index Builder index (0 to GetBuilderCount()-1)
 * @return New builder instance (caller takes ownership), or nullptr on error
 *
 * The editor will call DekiBuilder_DestroyBuilder() when done.
 */
#ifdef __cplusplus
    DEKI_BUILDER_API DekiEditor::ITargetBuilder* DekiBuilder_CreateBuilder(int index);
#else
DEKI_BUILDER_API void* DekiBuilder_CreateBuilder(int index);
#endif

/**
 * @brief Destroy a builder instance created by DekiBuilder_CreateBuilder
 * @param builder The builder to destroy (must have been created by this plugin)
 */
#ifdef __cplusplus
    DEKI_BUILDER_API void DekiBuilder_DestroyBuilder(DekiEditor::ITargetBuilder* builder);
#else
DEKI_BUILDER_API void DekiBuilder_DestroyBuilder(void* builder);
#endif

#ifdef __cplusplus
}
#endif
