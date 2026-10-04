#ifdef DEKI_EDITOR

#include <deki-editor/EditorExtension.h>
#include <cstring>
#include <deki-editor/EditorRegistry.h>
#include <deki-editor/CustomEditor.h>
#include <cctype>
#include <memory>

// ============================================================================
// Editor Registry Implementation (shared singleton across all DLLs)
// ============================================================================

static DekiEditorInternal::EditorRegistry* s_EditorRegistry = nullptr;

DekiEditorInternal::EditorRegistry& DekiEditorInternal::EditorRegistry::Instance()
{
    if (!s_EditorRegistry)
    {
        s_EditorRegistry = new DekiEditorInternal::EditorRegistry();
    }
    return *s_EditorRegistry;
}

namespace DekiEditor
{

// ============================================================================
// Custom Editor Registry Implementation
// ============================================================================

// One instance shared across DLL boundaries.
static CustomEditorRegistry* s_CustomEditorRegistry = nullptr;

CustomEditorRegistry& CustomEditorRegistry::Instance()
{
    if (!s_CustomEditorRegistry)
    {
        s_CustomEditorRegistry = new CustomEditorRegistry();
    }
    return *s_CustomEditorRegistry;
}

void CustomEditorRegistry::RegisterFactory(const std::string& componentName, FactoryFunc factory)
{
    if (factory)
    {
        m_Factories[componentName] = std::move(factory);
    }
}

void CustomEditorRegistry::Register(CustomEditor* editor)
{
    // A single editor instance shared by every component of this type. Used
    // only when no factory is registered.
    if (editor)
    {
        m_Editors[editor->GetComponentName()] = editor;
    }
}

CustomEditor* CustomEditorRegistry::GetEditorForComponent(const char* componentName, Deki::Component* comp)
{
    if (!comp || !componentName)
    {
        return nullptr;
    }

    // A cached editor for this component, if its name still matches: the key
    // is a raw pointer, and a freed component's address can be reused by a
    // component of another type.
    auto cacheIt = m_Cache.find(comp);
    if (cacheIt != m_Cache.end())
    {
        const char* cachedName = cacheIt->second->GetComponentName();
        if (cachedName && std::strcmp(componentName, cachedName) == 0)
        {
            return cacheIt->second.get();
        }
        m_Cache.erase(cacheIt);
    }

    // Create one from the factory. The std::string is built only on this
    // rare path; the cache hit above runs per component per frame.
    const std::string nameKey(componentName);
    auto factoryIt = m_Factories.find(nameKey);
    if (factoryIt != m_Factories.end())
    {
        auto newEditor = factoryIt->second();
        if (newEditor)
        {
            CustomEditor* rawPtr = newEditor.get();
            m_Cache[comp] = std::move(newEditor);
            return rawPtr;
        }
    }

    // Fall back to the shared single instance.
    auto editorIt = m_Editors.find(nameKey);
    return (editorIt != m_Editors.end()) ? editorIt->second : nullptr;
}

void CustomEditorRegistry::ClearCache()
{
    m_Cache.clear();
}

void CustomEditorRegistry::ClearCacheForComponent(Deki::Component* comp)
{
    if (comp)
    {
        m_Cache.erase(comp);
    }
}

void CustomEditorRegistry::Clear()
{
    m_Cache.clear();
    m_Factories.clear();
    m_Editors.clear();
}

// ============================================================================
// Asset Type Editor Registry Implementation
// ============================================================================

// One instance shared across DLL boundaries.
static AssetTypeEditorRegistry* s_AssetTypeEditorRegistry = nullptr;

AssetTypeEditorRegistry& AssetTypeEditorRegistry::Instance()
{
    if (!s_AssetTypeEditorRegistry)
    {
        s_AssetTypeEditorRegistry = new AssetTypeEditorRegistry();
    }
    return *s_AssetTypeEditorRegistry;
}

void AssetTypeEditorRegistry::Register(AssetTypeEditor* editor)
{
    if (editor)
    {
        m_Editors[editor->GetTypeName()] = editor;
        m_EditorList.push_back(editor);
    }
}

AssetTypeEditor* AssetTypeEditorRegistry::GetEditor(const std::string& typeName)
{
    auto it = m_Editors.find(typeName);
    return (it != m_Editors.end()) ? it->second : nullptr;
}

const std::vector<AssetTypeEditor*>& AssetTypeEditorRegistry::GetAllEditors() const
{
    return m_EditorList;
}

void AssetTypeEditorRegistry::Clear()
{
    m_Editors.clear();
    m_EditorList.clear();
}

// ============================================================================
// File Inspector Registry Implementation
// ============================================================================

// One instance shared across DLL boundaries.
static FileInspectorRegistry* s_FileInspectorRegistry = nullptr;

FileInspectorRegistry& FileInspectorRegistry::Instance()
{
    if (!s_FileInspectorRegistry)
    {
        s_FileInspectorRegistry = new FileInspectorRegistry();
    }
    return *s_FileInspectorRegistry;
}

void FileInspectorRegistry::Register(FileInspector* inspector)
{
    if (!inspector)
    {
        return;
    }

    const char** extensions = inspector->GetExtensions();
    int count = inspector->GetExtensionCount();

    for (int i = 0; i < count; ++i)
    {
        if (extensions[i])
        {
            std::string ext = extensions[i];
            // Normalize to lowercase
            for (char& c : ext)
            {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            m_Inspectors[ext] = inspector;
        }
    }
}

FileInspector* FileInspectorRegistry::GetInspector(const std::string& extension)
{
    // Normalize to lowercase
    std::string ext = extension;
    for (char& c : ext)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    auto it = m_Inspectors.find(ext);
    return (it != m_Inspectors.end()) ? it->second : nullptr;
}

void FileInspectorRegistry::Clear()
{
    m_Inspectors.clear();
}

// ============================================================================
// Create Menu Registry Implementation
// ============================================================================

static CreateMenuRegistry* s_CreateMenuRegistry = nullptr;

CreateMenuRegistry& CreateMenuRegistry::Instance()
{
    if (!s_CreateMenuRegistry)
    {
        s_CreateMenuRegistry = new CreateMenuRegistry();
    }
    return *s_CreateMenuRegistry;
}

void CreateMenuRegistry::Register(const std::string& category, const std::string& displayName,
                                  const std::string& objectName, const std::string& componentName)
{
    m_Entries.push_back({ category, displayName, objectName, componentName });
}

void CreateMenuRegistry::Clear()
{
    m_Entries.clear();
}

}  // namespace DekiEditor

#endif  // DEKI_EDITOR
