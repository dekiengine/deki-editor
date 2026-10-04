#pragma once

#include <deki-editor/EditorExport.h>

#include <vector>
#include <memory>
#include <functional>
#include <deki-editor/EditorExtension.h>

namespace DekiEditorInternal
{

/**
 * @brief Editor factory info for runtime creation
 */
struct EditorFactoryInfo
{
    // Factory function that creates a new instance
    std::function<std::unique_ptr<DekiEditor::EditorExtension>()> factory;

    // Static instance for metadata queries (type, name)
    DekiEditor::EditorExtension* prototype;
};

/**
 * @brief Internal registry for auto-registered editor extensions.
 *
 * This registry collects editor factories at DLL load time via static initializers.
 * The engine exports functions to iterate this list, and the editor can create
 * fresh instances per-component to avoid stale state issues.
 */
class DEKI_EDITOR_API EditorRegistry
{
public:
    static EditorRegistry& Instance();

    /**
     * @brief Add an editor factory (preferred method)
     * @param prototype Static instance for metadata queries
     * @param factory Function that creates new instances
     */
    void AddFactory(DekiEditor::EditorExtension* prototype,
                    std::function<std::unique_ptr<DekiEditor::EditorExtension>()> factory)
    {
        if (prototype && factory)
        {
            EditorFactoryInfo info;
            info.prototype = prototype;
            info.factory = std::move(factory);
            m_Factories.push_back(std::move(info));
        }
    }

    /**
     * @brief Get all factory infos
     */
    const std::vector<EditorFactoryInfo>& GetAllFactories() const { return m_Factories; }

    /**
     * @brief Get total count of registered factories
     */
    size_t GetTotalCount() const { return m_Factories.size(); }

    /**
     * @brief Clear all factories (must be called before package DLLs are unloaded)
     */
    void Clear() { m_Factories.clear(); }

    /**
     * @brief Drop every factory registered after the first `count`, keeping the
     *        earlier ones untouched.
     *
     * Entries are appended in DLL load order, so a count taken before a DLL was
     * loaded marks exactly where that DLL's registrations begin. Trimming back
     * to it removes one DLL's entries without disturbing the packages loaded
     * before it, which a blunt Clear() would also destroy (their static
     * registrars never rerun without a DLL reload).
     *
     * MUST run while the owning DLL is still loaded. A std::function carries a
     * pointer to manager code inside the DLL that created it, and that code
     * runs on COPY and on DESTRUCTION alike - so after FreeLibrary even
     * discarding the entry faults, and there is no way to recover. This is the
     * only safe order: trim, then unload.
     */
    void TrimTo(size_t count)
    {
        if (count < m_Factories.size())
        {
            m_Factories.resize(count);
        }
    }

private:
    EditorRegistry() = default;
    std::vector<EditorFactoryInfo> m_Factories;
};

/**
 * @brief Auto-registration helper template (factory-based).
 *
 * Creates a static prototype instance for metadata and registers a factory
 * function that creates new instances on demand.
 */
template <typename T>
struct EditorRegistrar
{
    EditorRegistrar()
    {
        static T s_Prototype;
        EditorRegistry::Instance().AddFactory(&s_Prototype, []() -> std::unique_ptr<DekiEditor::EditorExtension>
                                              { return std::make_unique<T>(); });
    }
};

/**
 * @brief Macro for auto-registering editor extensions.
 *
 * Use this at the end of your editor class definition:
 *   REGISTER_EDITOR(MyComponentEditor)
 *   REGISTER_EDITOR(MyAssetTypeEditor)
 *
 * Works for both CustomEditor and AssetTypeEditor subclasses.
 * Now uses factory pattern to create fresh instances per-component.
 */
#define REGISTER_EDITOR(EditorClass) static DekiEditorInternal::EditorRegistrar<EditorClass> s_##EditorClass##Registrar;

}  // namespace DekiEditorInternal
