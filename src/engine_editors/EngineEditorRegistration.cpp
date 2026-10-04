/**
 * @file EngineEditorRegistration.cpp
 * @brief Generic export functions for all auto-registered editor extensions
 *
 * This file exports generic functions that iterate over the EditorRegistry.
 * The registry is populated at DLL load time via static initializers in
 * individual editor files (using REGISTER_EDITOR macro).
 *
 * Factory-based API (preferred):
 *   - DekiEngine_GetEditorFactoryCount() - number of factory-registered editors
 *   - DekiEngine_GetEditorPrototype(index) - get prototype for metadata
 *   - DekiEngine_CreateEditor(index) - create new instance from factory
 *
 * Legacy API (for backward compatibility):
 *   - DekiEngine_GetEditorCount() - returns factory count
 *   - DekiEngine_GetEditor(index) - returns prototype (not a fresh instance)
 */

#ifdef DEKI_EDITOR

#include <deki-editor/EditorRegistry.h>
#include <memory>

// Export marker for the DekiEngine_* entry points below. These are compiled into
// the editor binary and resolved by direct linkage, but they are also part of the
// documented engine<->editor ABI, so keep them exported on both toolchains.
#ifdef _WIN32
#define DEKI_ENGINE_EDITOR_EXPORT __declspec(dllexport)
#else
#define DEKI_ENGINE_EDITOR_EXPORT __attribute__((visibility("default")))
#endif

// Forward declaration of EditorExtension base class
namespace DekiEditor
{
class EditorExtension;
}

extern "C"
{
    // ============================================================================
    // Factory-based API (new)
    // ============================================================================

    /**
     * @brief Get the number of editor factories registered by the engine
     */
    DEKI_ENGINE_EDITOR_EXPORT int DekiEngine_GetEditorFactoryCount()
    {
        return static_cast<int>(DekiEditorInternal::EditorRegistry::Instance().GetAllFactories().size());
    }

    /**
     * @brief Get a prototype editor by index (for metadata queries)
     * @note This returns a shared prototype, not a fresh instance
     */
    DEKI_ENGINE_EDITOR_EXPORT DekiEditor::EditorExtension* DekiEngine_GetEditorPrototype(int index)
    {
        const auto& factories = DekiEditorInternal::EditorRegistry::Instance().GetAllFactories();
        if (index < 0 || index >= static_cast<int>(factories.size()))
        {
            return nullptr;
        }
        return factories[index].prototype;
    }

    /**
     * @brief Create a new editor instance from factory
     * @param index Factory index
     * @return Raw pointer to new instance (caller takes ownership)
     */
    DEKI_ENGINE_EDITOR_EXPORT DekiEditor::EditorExtension* DekiEngine_CreateEditor(int index)
    {
        const auto& factories = DekiEditorInternal::EditorRegistry::Instance().GetAllFactories();
        if (index < 0 || index >= static_cast<int>(factories.size()))
        {
            return nullptr;
        }
        auto instance = factories[index].factory();
        return instance.release();  // Transfer ownership to caller
    }

    // ============================================================================
    // Legacy API (backward compatibility)
    // ============================================================================

    /**
     * @brief Get the number of editor extensions registered by the engine
     * @note Now returns factory count (new editors use factories)
     */
    DEKI_ENGINE_EDITOR_EXPORT int DekiEngine_GetEditorCount()
    {
        return static_cast<int>(DekiEditorInternal::EditorRegistry::Instance().GetAllFactories().size());
    }

    /**
     * @brief Get an editor extension by index
     * @note Returns prototype (not a fresh instance)
     * @deprecated Use DekiEngine_CreateEditor for fresh instances
     */
    DEKI_ENGINE_EDITOR_EXPORT DekiEditor::EditorExtension* DekiEngine_GetEditor(int index)
    {
        const auto& factories = DekiEditorInternal::EditorRegistry::Instance().GetAllFactories();
        if (index < 0 || index >= static_cast<int>(factories.size()))
        {
            return nullptr;
        }
        return factories[index].prototype;
    }

}  // extern "C"

#endif  // DEKI_EDITOR
