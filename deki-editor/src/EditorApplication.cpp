#ifdef DEKI_EDITOR

#include <deki-editor/EditorApplication.h>

namespace DekiEditor
{

// ============================================================================
// Singleton Implementation
// ============================================================================

EditorApplication& EditorApplication::Get()
{
    static EditorApplication s_Instance;
    return s_Instance;
}

// ============================================================================
// Project Paths
// ============================================================================

void EditorApplication::SetProjectPaths(const std::string& projectPath, const std::string& assetsPath,
                                        const std::string& cachePath)
{
    m_ProjectPath = projectPath;
    m_AssetsPath = assetsPath;
    m_CachePath = cachePath;
}

void EditorApplication::ClearProjectPaths()
{
    m_ProjectPath.clear();
    m_AssetsPath.clear();
    m_CachePath.clear();
    m_ActiveObject = nullptr;
    m_ActiveScene = nullptr;
}

}  // namespace DekiEditor

#endif  // DEKI_EDITOR
