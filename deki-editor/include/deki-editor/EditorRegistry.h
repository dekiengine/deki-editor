#pragma once

#include <deki-editor/EditorExport.h>

#include <vector>
#include <memory>
#include <functional>
#include <deki-editor/EditorExtension.h>

namespace DekiEditorInternal
{

/// How to make one kind of editor extension.
struct EditorFactoryInfo
{
    std::function<std::unique_ptr<DekiEditor::EditorExtension>()> factory;

    // Static instance, asked for metadata (type, name).
    DekiEditor::EditorExtension* prototype;
};

/// Collects the editor factories that static initializers register when a
/// DLL loads. The engine exports functions to walk this list, and the editor
/// creates a fresh instance per component so no state goes stale.
class DEKI_EDITOR_API EditorRegistry
{
public:
    static EditorRegistry& Instance();

    /// Adds a factory, with a static `prototype` used for metadata.
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

    const std::vector<EditorFactoryInfo>& GetAllFactories() const { return m_Factories; }

    size_t GetTotalCount() const { return m_Factories.size(); }

    /// Removes every factory. Must be called before package DLLs unload.
    void Clear() { m_Factories.clear(); }

    /// Removes every factory after the first `count`.
    ///
    /// Entries are added in DLL load order, so a count taken before a DLL
    /// loaded marks where its registrations begin. Trimming to it removes that
    /// DLL's entries and keeps those of packages loaded earlier, which Clear()
    /// would lose for good (their static registrars do not run again without
    /// a DLL reload).
    ///
    /// MUST run while the owning DLL is still loaded. A std::function points
    /// at manager code inside the DLL that created it, and that code runs on
    /// COPY and on DESTRUCTION alike, so after FreeLibrary even discarding the
    /// entry faults. Always trim, then unload.
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

/// Registers editor class T at static init: a static prototype for metadata,
/// and a factory that makes new instances on demand.
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

/// Registers an editor extension (CustomEditor, AssetTypeEditor, ...). Put
/// it after the class definition:
///   REGISTER_EDITOR(MyComponentEditor)
///   REGISTER_EDITOR(MyAssetTypeEditor)
#define REGISTER_EDITOR(EditorClass) static DekiEditorInternal::EditorRegistrar<EditorClass> s_##EditorClass##Registrar;

}  // namespace DekiEditorInternal
