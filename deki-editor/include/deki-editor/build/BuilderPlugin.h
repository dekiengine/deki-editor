#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Plugin interface for Deki firmware builders.
//
// A hardware maker or community developer builds a DLL that exports these
// functions to add a build framework (the Pico SDK, say). The editor loads
// builder plugins from ~/.deki-editor/builders/.
//
// Steps:
//   1. Include this header in your builder project
//   2. Implement ITargetBuilder for your platform
//   3. Export the C functions below
//   4. Build as a shared library (DLL/so/dylib)
//   5. Place it in ~/.deki-editor/builders/
//
// Example:
//
//   #include <deki-editor/build/BuilderPlugin.h>
//   #include "MyPicoSDKBuilder.h"
//
//   DEKI_BUILDER_API const char* DekiBuilderGetName() { return "Pico SDK Builder"; }
//   DEKI_BUILDER_API const char* DekiBuilderGetVersion() { return "1.0.0"; }
//   DEKI_BUILDER_API int DekiBuilderGetBuilderCount() { return 1; }
//   DEKI_BUILDER_API ITargetBuilder* DekiBuilderCreateBuilder(int index) {
//       return new MyPicoSDKBuilder();
//   }
//   DEKI_BUILDER_API void DekiBuilderDestroyBuilder(ITargetBuilder* builder) {
//       delete builder;
//   }

// Export/import macros, the same pattern as DekiPlugin.h
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

    /// What the plugin was compiled against.
    ///
    /// A builder plugin shares C++ objects with the editor: PlatformConfig by
    /// reference, std::string and std::vector across the boundary, a vtable it
    /// inherits. That works only when both sides agree on their layout; when
    /// they do not, the result is a corrupted field or a crash somewhere
    /// unrelated, not an error. So the editor refuses a plugin that cannot show
    /// it matches, like the package loader's reflection ABI guard.
    ///
    /// Read `version` through the pointer before copying the struct: a plugin
    /// built against another revision may return a struct of another size.
    struct DekiBuilderAbi
    {
        uint32_t version;             ///< kDekiBuilderAbiVersion the plugin compiled with
        uint32_t structSize;          ///< sizeof(DekiBuilderAbi) as the plugin sees it
        uint32_t platformConfigSize;  ///< sizeof(DekiEditor::PlatformConfig)
        uint32_t packageEntrySize;    ///< sizeof(DekiEditor::CMakeGen::PackageEntry), returned by value
        uint32_t stdStringSize;       ///< sizeof(std::string)
        uint32_t stdVectorSize;       ///< sizeof(std::vector<int>)
        uint32_t compilerTag;         ///< compiler + STL identity; see DekiBuilderThisAbi
    };

    /// Bump when the builder interface changes shape: a new virtual, a reordered
    /// one, a changed signature, or a change to the layout of a struct that
    /// crosses the boundary.
    constexpr uint32_t kDekiBuilderAbiVersion = 3;

    /// The compiler and standard library this translation unit uses. A MinGW
    /// plugin and an MSVC editor lay out std::string differently, and nothing else
    /// here would catch it.
    constexpr uint32_t DekiBuilderCompilerTag()
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

    /// The ABI as the calling module sees it. A plugin implements
    /// DekiBuilderGetAbi by returning a static filled with this.
    inline DekiBuilderAbi DekiBuilderThisAbi(uint32_t platformConfigSize, uint32_t packageEntrySize)
    {
        DekiBuilderAbi abi{};
        abi.version = kDekiBuilderAbiVersion;
        abi.structSize = (uint32_t)sizeof(DekiBuilderAbi);
        abi.platformConfigSize = platformConfigSize;
        abi.packageEntrySize = packageEntrySize;
        abi.stdStringSize = (uint32_t)sizeof(std::string);
        abi.stdVectorSize = (uint32_t)sizeof(std::vector<int>);
        abi.compilerTag = DekiBuilderCompilerTag();
        return abi;
    }

    /// Required. What this plugin was compiled against; a plugin without this
    /// export is refused.
    DEKI_BUILDER_API const DekiBuilderAbi* DekiBuilderGetAbi(void);

    /// The plugin's name for display in the editor (e.g. "Pico SDK Builder").
    DEKI_BUILDER_API const char* DekiBuilderGetName(void);

    /// The plugin's version string (e.g. "1.0.0").
    DEKI_BUILDER_API const char* DekiBuilderGetVersion(void);

    /// How many builders the plugin provides, usually 1.
    DEKI_BUILDER_API int DekiBuilderGetBuilderCount(void);

/// Creates builder `index` (0 to count - 1), or returns nullptr on error. The
/// editor hands it back to DekiBuilderDestroyBuilder when done.
#ifdef __cplusplus
    DEKI_BUILDER_API DekiEditor::ITargetBuilder* DekiBuilderCreateBuilder(int index);
#else
DEKI_BUILDER_API void* DekiBuilderCreateBuilder(int index);
#endif

/// Destroys a builder this plugin's DekiBuilderCreateBuilder made.
#ifdef __cplusplus
    DEKI_BUILDER_API void DekiBuilderDestroyBuilder(DekiEditor::ITargetBuilder* builder);
#else
DEKI_BUILDER_API void DekiBuilderDestroyBuilder(void* builder);
#endif

#ifdef __cplusplus
}
#endif
