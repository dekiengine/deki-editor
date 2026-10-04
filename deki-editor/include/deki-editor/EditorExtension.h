#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <memory>

#include <deki-editor/EditorExport.h>
namespace Deki
{
class Component;
}
namespace Deki
{
class Object;
}
class BitmapFont;

struct ImDrawList;

namespace DekiEditor
{

// ============================================================================
// Editor Extension Types
// ============================================================================

enum class EditorType
{
    CustomEditor,  // component editor (CustomEditor.h)
    AssetTypeEditor,
    CliTool  // A command-line / MCP tool a package adds (CliTool.h)
};

// ============================================================================
// Base Class for All Editor Extensions
// ============================================================================

class DEKI_EDITOR_API EditorExtension
{
public:
    virtual ~EditorExtension() = default;

    virtual EditorType GetEditorType() const = 0;
    virtual const char* GetName() const = 0;
};

// ============================================================================
// Asset Type Editor
// ============================================================================

class DEKI_EDITOR_API AssetTypeEditor : public EditorExtension
{
public:
    EditorType GetEditorType() const override { return EditorType::AssetTypeEditor; }
    const char* GetName() const override { return GetTypeName(); }

    // Every subclass must give these.
    virtual const char* GetTypeName() const = 0;
    virtual const char* GetDisplayName() const = 0;

    // Override GetExtension() for one extension or GetExtensions() for several.
    // Native types like Sprite own many extensions; custom JSON types usually own one.
    virtual const char* GetExtension() const { return nullptr; }
    virtual std::vector<std::string> GetExtensions() const
    {
        const char* single = GetExtension();
        return single ? std::vector<std::string>{ single } : std::vector<std::string>{};
    }

    // JSON content of a new asset. Empty for native types.
    virtual const char* GetDefaultContent() const { return ""; }

    // 0=Texture, 1=Audio, 2=Data. Native pass-through types return Data.
    virtual int GetCompileTarget() const { return 2; }

    // Do not add virtuals here. AssetTypeEditor is the cross-DLL interface
    // between deki-editor.dll and package DLLs, so a new virtual shifts the
    // vtable and crashes every already-built package. To register asset
    // categories, packages call AssetTypeRegistry::RegisterCategory from a
    // static initializer.

    // Compiles to an RGBA buffer (Texture target). Does nothing by default.
    virtual bool Compile(const std::string& /*jsonData*/, std::vector<uint8_t>& /*rgba*/, int& /*width*/,
                         int& /*height*/)
    {
        return false;
    }

    // Compiles to a binary file (Data target). Does nothing by default.
    virtual bool CompileToFile(const std::string& /*jsonData*/, const std::string& /*cachePath*/) { return false; }

    // Custom Inspector UI. Does nothing by default; native types use the generic Inspector.
    virtual bool OnInspectorGUI(std::string& /*jsonData*/, const std::string& /*assetPath*/,
                                const std::string& /*assetGuid*/)
    {
        return false;
    }
};

// ============================================================================
// File Inspector (for native file types like .ttf, .otf, .png)
// ============================================================================

class DEKI_EDITOR_API FileInspector : public EditorExtension
{
public:
    virtual ~FileInspector() = default;

    EditorType GetEditorType() const override { return EditorType::AssetTypeEditor; }
    const char* GetName() const override { return "FileInspector"; }

    // The supported extensions, lowercase with dot, e.g. {".ttf", ".otf"}.
    virtual const char** GetExtensions() const = 0;
    virtual int GetExtensionCount() const = 0;

    virtual void OnInspectorGUI(const std::string& assetPath, const std::string& assetGuid) = 0;

    // Return true from OverridesCacheStatus() to have the cache status bar
    // use IsCached() instead of the generic check.
    virtual bool OverridesCacheStatus() const { return false; }
    virtual bool IsCached(const std::string& assetGuid) const
    {
        (void)assetGuid;
        return false;
    }
};

// ============================================================================
// Custom Editor Registry (editor-side)
// ============================================================================

class CustomEditor;

class DEKI_EDITOR_API CustomEditorRegistry
{
public:
    static CustomEditorRegistry& Instance();

    // Holds unique_ptrs.
    CustomEditorRegistry(const CustomEditorRegistry&) = delete;
    CustomEditorRegistry& operator=(const CustomEditorRegistry&) = delete;

    using FactoryFunc = std::function<std::unique_ptr<CustomEditor>()>;

    // Registers the factory that makes editors for a component type. Called
    // from the engine DLL.
    void RegisterFactory(const std::string& componentName, FactoryFunc factory);

    // Registers one editor instance shared by every component of its type.
    void Register(CustomEditor* editor);

    // The editor for one component, created on first use. Each component
    // gets its own instance, so editor state is per component.
    CustomEditor* GetEditorForComponent(const std::string& componentName, Deki::Component* comp)
    {
        return GetEditorForComponent(componentName.c_str(), comp);
    }
    // The per-frame form: every caller has meta->name as a C string, and the
    // cached-editor check needs no std::string at all.
    CustomEditor* GetEditorForComponent(const char* componentName, Deki::Component* comp);

    // Drops every cached editor instance. Call when a scene closes or a DLL reloads.
    void ClearCache();

    // Drops the cached editor of one component. Call when the component is deleted.
    void ClearCacheForComponent(Deki::Component* comp);

    // Drops factories and cached editors, for DLL hot reload.
    void Clear();

private:
    CustomEditorRegistry() = default;

    // Keyed by component name.
    std::unordered_map<std::string, FactoryFunc> m_Factories;

    // Keyed by component pointer.
    std::unordered_map<Deki::Component*, std::unique_ptr<CustomEditor>> m_Cache;

    // Shared editors from Register(), keyed by component name.
    std::unordered_map<std::string, CustomEditor*> m_Editors;
};

// ============================================================================
// Asset Type Editor Registry (editor-side)
// ============================================================================

class DEKI_EDITOR_API AssetTypeEditorRegistry
{
public:
    static AssetTypeEditorRegistry& Instance();

    void Register(AssetTypeEditor* editor);
    AssetTypeEditor* GetEditor(const std::string& typeName);
    const std::vector<AssetTypeEditor*>& GetAllEditors() const;
    void Clear();

private:
    AssetTypeEditorRegistry() = default;
    std::unordered_map<std::string, AssetTypeEditor*> m_Editors;
    std::vector<AssetTypeEditor*> m_EditorList;
};

// ============================================================================
// File Inspector Registry (editor-side)
// ============================================================================

class DEKI_EDITOR_API FileInspectorRegistry
{
public:
    static FileInspectorRegistry& Instance();

    void Register(FileInspector* inspector);
    FileInspector* GetInspector(const std::string& extension);
    void Clear();

private:
    FileInspectorRegistry() = default;
    std::unordered_map<std::string, FileInspector*> m_Inspectors;
};

// ============================================================================
// Create Menu Registry (hierarchy right-click "Create Object" menu)
// ============================================================================

/// An item in the hierarchy's right-click "Create Object" menu, registered by
/// a package. It creates a new Deki::Object with the named component.
struct CreateMenuEntry
{
    std::string category;       // submenu, e.g. "2D", "UI", "Core"
    std::string displayName;    // menu label, e.g. "Sprite", "Text"
    std::string objectName;     // name of the new Deki::Object, e.g. "Sprite"
    std::string componentName;  // component class to add, e.g. "SpriteComponent"
};

class DEKI_EDITOR_API CreateMenuRegistry
{
public:
    static CreateMenuRegistry& Instance();

    void Register(const std::string& category, const std::string& displayName, const std::string& objectName,
                  const std::string& componentName);

    const std::vector<CreateMenuEntry>& GetEntries() const { return m_Entries; }
    void Clear();

private:
    CreateMenuRegistry() = default;
    std::vector<CreateMenuEntry> m_Entries;
};

/// Adds a component to the hierarchy's "Create Object" menu at static init.
/// `Id` is a unique identifier (no quotes); the other arguments are the
/// strings of CreateMenuEntry, in order. In a package's editor .cpp file:
///   REGISTER_CREATE_MENU_ITEM(SpriteComponent, "2D", "Sprite", "Sprite", "SpriteComponent")
#define REGISTER_CREATE_MENU_ITEM(Id, Category, Display, ObjName, CompName)                                            \
    static struct CreateMenu##Id##Registrar                                                                            \
    {                                                                                                                  \
        CreateMenu##Id##Registrar()                                                                                    \
        {                                                                                                              \
            DekiEditor::CreateMenuRegistry::Instance().Register(Category, Display, ObjName, CompName);                 \
        }                                                                                                              \
    } s_CreateMenu##Id##Registrar;

}  // namespace DekiEditor
