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

// Static instance pointer - ensures single instance across DLL boundary
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
    // Legacy: store singleton editor for backward compatibility
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

    // Check if we have a cached editor for this component.
    // Validate the cached editor still matches the requested component name:
    // raw-pointer keys can collide when a component is freed and the heap
    // hands the same address to a component of a different type.
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

    // Try to create a new instance from factory (a std::string only on this
    // cold path; the hit above, which runs per component per frame, has none)
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

    // Fall back to legacy singleton
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

// Static instance pointer - ensures single instance across DLL boundary
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

// Static instance pointer - ensures single instance across DLL boundary
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
