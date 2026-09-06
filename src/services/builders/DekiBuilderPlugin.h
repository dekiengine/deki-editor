#pragma once

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
 *   2. Implement IFirmwareBuilder for your platform
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
 * DEKI_BUILDER_API IFirmwareBuilder* DekiBuilder_CreateBuilder(int index) {
 *     return new MyPicoSDKBuilder();
 * }
 * DEKI_BUILDER_API void DekiBuilder_DestroyBuilder(IFirmwareBuilder* builder) {
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
class IFirmwareBuilder;
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
    DEKI_BUILDER_API DekiEditor::IFirmwareBuilder* DekiBuilder_CreateBuilder(int index);
#else
DEKI_BUILDER_API void* DekiBuilder_CreateBuilder(int index);
#endif

/**
 * @brief Destroy a builder instance created by DekiBuilder_CreateBuilder
 * @param builder The builder to destroy (must have been created by this plugin)
 */
#ifdef __cplusplus
    DEKI_BUILDER_API void DekiBuilder_DestroyBuilder(DekiEditor::IFirmwareBuilder* builder);
#else
DEKI_BUILDER_API void DekiBuilder_DestroyBuilder(void* builder);
#endif

#ifdef __cplusplus
}
#endif
