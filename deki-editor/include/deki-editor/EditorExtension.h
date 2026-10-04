#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <memory>

#include <deki-editor/EditorExport.h>
// Forward declarations - engine types
namespace Deki
{
class Component;
}
namespace Deki
{
class Object;
}
class BitmapFont;

// Forward declaration for ImGui
struct ImDrawList;

namespace DekiEditor
{

// ============================================================================
// Editor Extension Types
// ============================================================================

enum class EditorType
{
    CustomEditor,  // Unified component editor
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

    // Identity - required for every subclass.
    virtual const char* GetTypeName() const = 0;
    virtual const char* GetDisplayName() const = 0;

    // Subclasses override either GetExtension() (single) or GetExtensions() (multiple).
    // Native types like Sprite own many extensions; custom JSON types usually own one.
    virtual const char* GetExtension() const { return nullptr; }
    virtual std::vector<std::string> GetExtensions() const
    {
        const char* single = GetExtension();
        return single ? std::vector<std::string>{ single } : std::vector<std::string>{};
    }

    // Default JSON content for asset creation. Empty for native types.
    virtual const char* GetDefaultContent() const { return ""; }

    // Compile target: 0=Texture, 1=Audio, 2=Data. Native pass-through types return Data.
    virtual int GetCompileTarget() const { return 2; }

    // NOTE: do not add new virtuals here. AssetTypeEditor is the cross-DLL
    // interface between deki-editor.dll and package DLLs, so adding a virtual
    // shifts the vtable and crashes every already-built package. Packages that
    // need to register asset categories should call
    // AssetTypeRegistry::RegisterCategory from a static initializer.

    // Compile to RGBA buffer (Texture target). Default no-op for native types.
    virtual bool Compile(const std::string& /*jsonData*/, std::vector<uint8_t>& /*rgba*/, int& /*width*/,
                         int& /*height*/)
    {
        return false;
    }

    // Compile to binary file (Data target). Default no-op.
    virtual bool CompileToFile(const std::string& /*jsonData*/, const std::string& /*cachePath*/) { return false; }

    // Custom inspector UI. Default no-op - native types let the generic inspector handle them.
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

    // Return list of supported extensions (lowercase, with dot)
    // e.g., {".ttf", ".otf"}
    virtual const char** GetExtensions() const = 0;
    virtual int GetExtensionCount() const = 0;

    // Draw the inspector UI for this file type
    virtual void OnInspectorGUI(const std::string& assetPath, const std::string& assetGuid) = 0;

    // Override the generic cache status bar for this file type.
    // Return true from OverridesCacheStatus() to use IsCached() instead of the default.
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

// Forward declaration (CustomEditor is defined in CustomEditor.h)
class CustomEditor;

class DEKI_EDITOR_API CustomEditorRegistry
{
public:
    static CustomEditorRegistry& Instance();

    // Non-copyable (unique_ptr members)
    CustomEditorRegistry(const CustomEditorRegistry&) = delete;
    CustomEditorRegistry& operator=(const CustomEditorRegistry&) = delete;

    // Factory function type
    using FactoryFunc = std::function<std::unique_ptr<CustomEditor>()>;

    // Register a factory function for creating editors (called from engine DLL)
    void RegisterFactory(const std::string& componentName, FactoryFunc factory);

    // Legacy: Register a singleton editor (for backward compatibility)
    void Register(CustomEditor* editor);

    // Get or create a cached editor instance for a specific component
    // Each component pointer gets its own editor instance with fresh state
    CustomEditor* GetEditorForComponent(const std::string& componentName, Deki::Component* comp)
    {
        return GetEditorForComponent(componentName.c_str(), comp);
    }
    // The per-frame form: every caller has meta->name as a C string, and the
    // cached-editor check needs no std::string at all.
    CustomEditor* GetEditorForComponent(const char* componentName, Deki::Component* comp);

    // Clear all cached editor instances (call when scene closes or DLL reloads)
    void ClearCache();

    // Clear cached instance for a specific component (call when component is deleted)
    void ClearCacheForComponent(Deki::Component* comp);

    // Clear everything (factories and cache) - for DLL hot-reload
    void Clear();

private:
    CustomEditorRegistry() = default;

    // Factory functions keyed by component name
    std::unordered_map<std::string, FactoryFunc> m_Factories;

    // Cached editor instances keyed by component pointer
    std::unordered_map<Deki::Component*, std::unique_ptr<CustomEditor>> m_Cache;

    // Legacy: singleton editors (for backward compatibility)
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

/**
 * @brief Entry for the hierarchy "Create Object" context menu.
 *
 * Packages register entries here to appear in the right-click menu.
 * Each entry creates a new Deki::Object with the specified component attached.
 */
struct CreateMenuEntry
{
    std::string category;       // Submenu category (e.g., "2D", "UI", "Core")
    std::string displayName;    // Menu item label (e.g., "Sprite", "Text")
    std::string objectName;     // Name for the created Deki::Object (e.g., "Sprite")
    std::string componentName;  // Component class name to add (e.g., "SpriteComponent")
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

/**
 * @brief Auto-register a component in the hierarchy "Create Object" menu.
 *
 * Usage in a package's editor .cpp file:
 *   REGISTER_CREATE_MENU_ITEM(SpriteComponent, "2D", "Sprite", "Sprite", "SpriteComponent")
 *
 * @param Id         Unique identifier for the registrar (no quotes)
 * @param Category   Submenu category string (e.g., "2D", "UI")
 * @param Display    Menu item display name string
 * @param ObjName    Name for the created Deki::Object string
 * @param CompName   Component class name string to add
 */
#define REGISTER_CREATE_MENU_ITEM(Id, Category, Display, ObjName, CompName)                                            \
    static struct CreateMenu##Id##Registrar                                                                            \
    {                                                                                                                  \
        CreateMenu##Id##Registrar()                                                                                    \
        {                                                                                                              \
            DekiEditor::CreateMenuRegistry::Instance().Register(Category, Display, ObjName, CompName);                 \
        }                                                                                                              \
    } s_CreateMenu##Id##Registrar;

}  // namespace DekiEditor
