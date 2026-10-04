// Exported functions over the EditorRegistry, which static initializers in
// each editor file (the REGISTER_EDITOR macro) fill when the DLL loads.
//
// Factory API (preferred):
//   - DekiEngineGetEditorFactoryCount() - number of factory-registered editors
//   - DekiEngineGetEditorPrototype(index) - the prototype, for metadata
//   - DekiEngineCreateEditor(index) - a new instance from the factory
//
// Older API, kept for existing callers:
//   - DekiEngineGetEditorCount() - the factory count
//   - DekiEngineGetEditor(index) - the prototype (not a new instance)

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

namespace DekiEditor
{
class EditorExtension;
}

extern "C"
{
    // ============================================================================
    // Factory API
    // ============================================================================

    /// The number of editor factories the engine registered.
    DEKI_ENGINE_EDITOR_EXPORT int DekiEngineGetEditorFactoryCount()
    {
        return static_cast<int>(DekiEditorInternal::EditorRegistry::Instance().GetAllFactories().size());
    }

    /// The shared prototype editor at `index` (not a new instance), for
    /// metadata queries.
    DEKI_ENGINE_EDITOR_EXPORT DekiEditor::EditorExtension* DekiEngineGetEditorPrototype(int index)
    {
        const auto& factories = DekiEditorInternal::EditorRegistry::Instance().GetAllFactories();
        if (index < 0 || index >= static_cast<int>(factories.size()))
        {
            return nullptr;
        }
        return factories[index].prototype;
    }

    /// Creates a new editor from the factory at `index`. The caller owns it.
    DEKI_ENGINE_EDITOR_EXPORT DekiEditor::EditorExtension* DekiEngineCreateEditor(int index)
    {
        const auto& factories = DekiEditorInternal::EditorRegistry::Instance().GetAllFactories();
        if (index < 0 || index >= static_cast<int>(factories.size()))
        {
            return nullptr;
        }
        auto instance = factories[index].factory();
        return instance.release();  // the caller owns it
    }

    // ============================================================================
    // Older API, kept for existing callers
    // ============================================================================

    /// The number of editor extensions the engine registered: the factory
    /// count.
    DEKI_ENGINE_EDITOR_EXPORT int DekiEngineGetEditorCount()
    {
        return static_cast<int>(DekiEditorInternal::EditorRegistry::Instance().GetAllFactories().size());
    }

    /// The editor extension at `index`: the shared prototype, not a new
    /// instance. Deprecated; use DekiEngineCreateEditor for a new instance.
    DEKI_ENGINE_EDITOR_EXPORT DekiEditor::EditorExtension* DekiEngineGetEditor(int index)
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
