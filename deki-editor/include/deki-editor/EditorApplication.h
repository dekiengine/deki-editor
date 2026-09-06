#pragma once

#include <string>
#include <cstdint>

#include <deki-editor/EditorExport.h>
// Forward declarations - engine types
namespace Deki { class Object; }
namespace Deki { class Scene; }

namespace DekiEditor
{

/**
 * @brief Scene-view editing tool, requestable from package-DLL editors.
 *
 * Mirrors the internal SceneTool enum but lives in the public deki-editor
 * header so CustomEditors (which live in package DLLs) can ask the main editor
 * to switch tools without depending on editor-private headers.
 */
enum class SceneEditTool
{
    Select,
    Move,
    Rect,
};

/**
 * @brief Main editor application singleton (facade)
 *
 * Provides Unity-like static access for EditorWindow and CustomEditor implementations:
 *   auto& app = EditorApplication::Get();
 *   const char* projectPath = app.GetProjectPath();
 *   Deki::Object* selected = app.GetActiveObject();
 *
 * The main editor (EditorApp) populates this facade via SetProjectPaths() and SetActiveObject().
 * Engine editors only need to call the getters.
 */
class DEKI_EDITOR_API EditorApplication
{
public:
    /**
     * @brief Get singleton instance
     */
    static EditorApplication& Get();

    // =========================================================================
    // Project Paths (read-only for engine editors)
    // =========================================================================

    /**
     * @brief Get project root directory path
     * @return Path string, or empty if no project is open
     */
    const std::string& GetProjectPath() const { return m_ProjectPath; }

    /**
     * @brief Get assets directory path
     * @return Path string, or empty if no project is open
     */
    const std::string& GetAssetsPath() const { return m_AssetsPath; }

    /**
     * @brief Get cache directory path
     * @return Path string, or empty if no project is open
     */
    const std::string& GetCachePath() const { return m_CachePath; }

    /**
     * @brief Check if a project is currently open
     */
    bool HasProject() const { return !m_ProjectPath.empty(); }

    // =========================================================================
    // Time
    // =========================================================================

    /**
     * @brief Get delta time since last frame
     */
    float GetDeltaTime() const { return m_DeltaTime; }

    // =========================================================================
    // Selection (read-only for engine editors)
    // =========================================================================

    /**
     * @brief Get currently selected object
     * @return Selected object or nullptr
     */
    Deki::Object* GetActiveObject() const { return m_ActiveObject; }

    /**
     * @brief Get currently active scene
     * @return Active scene or nullptr
     */
    Deki::Scene* GetActiveScene() const { return m_ActiveScene; }

    // =========================================================================
    // Internal - Called by main editor (EditorApp)
    // =========================================================================

    /**
     * @brief Set project paths (called when project is opened)
     */
    void SetProjectPaths(const std::string& projectPath,
                         const std::string& assetsPath,
                         const std::string& cachePath);

    /**
     * @brief Clear project paths (called when project is closed)
     */
    void ClearProjectPaths();

    /**
     * @brief Set active object (called when selection changes)
     */
    void SetActiveObject(Deki::Object* obj) { m_ActiveObject = obj; }

    /**
     * @brief Set active scene (called when scene changes)
     */
    void SetActiveScene(Deki::Scene* scene) { m_ActiveScene = scene; }

    /**
     * @brief Set delta time (called each frame)
     */
    void SetDeltaTime(float dt) { m_DeltaTime = dt; }

    // =========================================================================
    // Asset Refresh (tools can request a refresh after writing new assets)
    // =========================================================================

    void RequestAssetRefresh() { m_AssetRefreshRequested = true; }
    bool PopAssetRefreshRequest()
    {
        bool r = m_AssetRefreshRequested;
        m_AssetRefreshRequested = false;
        return r;
    }

    // =========================================================================
    // Asset Invalidation (tools can request full cache invalidation after re-baking)
    // =========================================================================

    void RequestAssetInvalidation() { m_AssetInvalidationRequested = true; }
    bool PopAssetInvalidationRequest()
    {
        bool r = m_AssetInvalidationRequested;
        m_AssetInvalidationRequested = false;
        return r;
    }

    // =========================================================================
    // Open Tool Request (lets package-DLL editors ask the main editor's ToolHost
    // to dispatch an EditorWindow for a given file)
    // =========================================================================

    void RequestOpenTool(const std::string& filePath, const std::string& cachePath)
    {
        m_OpenToolFilePath = filePath;
        m_OpenToolCachePath = cachePath;
        m_OpenToolRequested = true;
    }

    bool PopOpenToolRequest(std::string& outFilePath, std::string& outCachePath)
    {
        if (!m_OpenToolRequested) return false;
        outFilePath = std::move(m_OpenToolFilePath);
        outCachePath = std::move(m_OpenToolCachePath);
        m_OpenToolFilePath.clear();
        m_OpenToolCachePath.clear();
        m_OpenToolRequested = false;
        return true;
    }

    // =========================================================================
    // Scene Tool Request (lets package-DLL editors switch the scene-view tool,
    // e.g. ClipComponent's "Edit Rect" button activating the Rect resize tool)
    // =========================================================================

    void RequestSceneTool(SceneEditTool tool)
    {
        m_RequestedSceneTool = tool;
        m_SceneToolRequested = true;
    }

    bool PopSceneToolRequest(SceneEditTool& outTool)
    {
        if (!m_SceneToolRequested) return false;
        outTool = m_RequestedSceneTool;
        m_SceneToolRequested = false;
        return true;
    }

private:
    EditorApplication() = default;
    ~EditorApplication() = default;

    // Prevent copying
    EditorApplication(const EditorApplication&) = delete;
    EditorApplication& operator=(const EditorApplication&) = delete;

    // Project state
    std::string m_ProjectPath;
    std::string m_AssetsPath;
    std::string m_CachePath;

    // Selection state
    Deki::Object* m_ActiveObject = nullptr;
    Deki::Scene* m_ActiveScene = nullptr;

    // Time
    float m_DeltaTime = 0.0f;

    // Asset refresh
    bool m_AssetRefreshRequested = false;

    // Asset invalidation (font re-bake, etc.)
    bool m_AssetInvalidationRequested = false;

    // Open-tool request (set by package-DLL editors, drained by EditorApp)
    bool m_OpenToolRequested = false;
    std::string m_OpenToolFilePath;
    std::string m_OpenToolCachePath;

    // Scene-tool request (set by package-DLL editors, drained by EditorApp)
    bool m_SceneToolRequested = false;
    SceneEditTool m_RequestedSceneTool = SceneEditTool::Select;
};

}  // namespace DekiEditor
