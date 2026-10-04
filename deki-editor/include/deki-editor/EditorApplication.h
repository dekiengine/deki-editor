#pragma once

#include <string>
#include <cstdint>

#include <deki-editor/EditorExport.h>
namespace Deki
{
class Object;
}
namespace Deki
{
class Scene;
}

namespace DekiEditor
{

/// Scene-view editing tool that package-DLL editors can ask for.
///
/// Mirrors the internal SceneTool enum, but lives in this public header so
/// CustomEditors in package DLLs can switch tools without editor-private
/// headers.
enum class SceneEditTool
{
    Select,
    Move,
    Rect,
};

/// The editor's state as seen by EditorWindow and CustomEditor code, like
/// Unity's static editor access:
///   auto& app = EditorApplication::Get();
///   const char* projectPath = app.GetProjectPath();
///   Deki::Object* selected = app.GetActiveObject();
///
/// The main editor (EditorApp) fills it through SetProjectPaths() and
/// SetActiveObject(); other editors only read it.
class DEKI_EDITOR_API EditorApplication
{
public:
    static EditorApplication& Get();

    // =========================================================================
    // Project Paths (read-only for engine editors)
    // =========================================================================

    /// The project root, or "" when no project is open.
    const std::string& GetProjectPath() const { return m_ProjectPath; }

    /// The assets folder, or "" when no project is open.
    const std::string& GetAssetsPath() const { return m_AssetsPath; }

    /// The cache folder, or "" when no project is open.
    const std::string& GetCachePath() const { return m_CachePath; }

    bool HasProject() const { return !m_ProjectPath.empty(); }

    // =========================================================================
    // Time
    // =========================================================================

    /// Time since the last frame.
    float GetDeltaTime() const { return m_DeltaTime; }

    // =========================================================================
    // Selection (read-only for engine editors)
    // =========================================================================

    /// The selected object, or nullptr.
    Deki::Object* GetActiveObject() const { return m_ActiveObject; }

    /// The active scene, or nullptr.
    Deki::Scene* GetActiveScene() const { return m_ActiveScene; }

    // =========================================================================
    // Internal - Called by main editor (EditorApp)
    // =========================================================================

    /// Called when a project opens.
    void SetProjectPaths(const std::string& projectPath, const std::string& assetsPath, const std::string& cachePath);

    /// Called when a project closes.
    void ClearProjectPaths();

    /// Called when the selection changes.
    void SetActiveObject(Deki::Object* obj) { m_ActiveObject = obj; }

    /// Called when the scene changes.
    void SetActiveScene(Deki::Scene* scene) { m_ActiveScene = scene; }

    /// Called each frame.
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
    // to open an EditorWindow for a file)
    // =========================================================================

    // `toolTitle` names the window to open (its GetTitle()); empty lets the
    // file's type pick, which for a .png is whichever tool claims it first.
    void RequestOpenTool(const std::string& filePath, const std::string& cachePath, const std::string& toolTitle = {})
    {
        m_OpenToolFilePath = filePath;
        m_OpenToolCachePath = cachePath;
        m_OpenToolTitle = toolTitle;
        m_OpenToolRequested = true;
    }

    bool PopOpenToolRequest(std::string& outFilePath, std::string& outCachePath, std::string* outToolTitle = nullptr)
    {
        if (!m_OpenToolRequested)
        {
            return false;
        }
        outFilePath = std::move(m_OpenToolFilePath);
        outCachePath = std::move(m_OpenToolCachePath);
        if (outToolTitle)
        {
            *outToolTitle = std::move(m_OpenToolTitle);
        }
        m_OpenToolFilePath.clear();
        m_OpenToolCachePath.clear();
        m_OpenToolTitle.clear();
        m_OpenToolRequested = false;
        return true;
    }

    // =========================================================================
    // Scene Tool Request (lets package-DLL editors switch the scene-view tool,
    // e.g. ClipComponent's "Edit Rect" button selecting the Rect resize tool)
    // =========================================================================

    void RequestSceneTool(SceneEditTool tool)
    {
        m_RequestedSceneTool = tool;
        m_SceneToolRequested = true;
    }

    bool PopSceneToolRequest(SceneEditTool& outTool)
    {
        if (!m_SceneToolRequested)
        {
            return false;
        }
        outTool = m_RequestedSceneTool;
        m_SceneToolRequested = false;
        return true;
    }

private:
    EditorApplication() = default;
    ~EditorApplication() = default;

    EditorApplication(const EditorApplication&) = delete;
    EditorApplication& operator=(const EditorApplication&) = delete;

    std::string m_ProjectPath;
    std::string m_AssetsPath;
    std::string m_CachePath;

    Deki::Object* m_ActiveObject = nullptr;
    Deki::Scene* m_ActiveScene = nullptr;

    float m_DeltaTime = 0.0f;

    bool m_AssetRefreshRequested = false;

    // Font re-bake, etc.
    bool m_AssetInvalidationRequested = false;

    // Set by package-DLL editors, taken by EditorApp.
    bool m_OpenToolRequested = false;
    std::string m_OpenToolFilePath;
    std::string m_OpenToolCachePath;

    // Set by package-DLL editors, taken by EditorApp.
    bool m_SceneToolRequested = false;
    SceneEditTool m_RequestedSceneTool = SceneEditTool::Select;

    // The window asked for by title, if any (see RequestOpenTool).
    std::string m_OpenToolTitle;

    // See GetSelectedAssetPath.
    std::string m_SelectedAssetPath;

public:
    // The asset selected in the Asset Browser, as a project-relative path; ""
    // when none. For tool windows that work on "the selected asset".
    const std::string& GetSelectedAssetPath() const { return m_SelectedAssetPath; }
    void SetSelectedAssetPath(const std::string& path) { m_SelectedAssetPath = path; }
};

}  // namespace DekiEditor
