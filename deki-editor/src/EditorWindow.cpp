#ifdef DEKI_EDITOR

#include <deki-editor/EditorWindow.h>
#include <vector>
#include <cstring>

namespace DekiEditor
{

// ============================================================================
// Window Registration
// ============================================================================

static std::vector<EditorWindowMeta>& GetWindowRegistry()
{
    static std::vector<EditorWindowMeta> s_Registry;
    return s_Registry;
}

int GetRegisteredWindowCount()
{
    return static_cast<int>(GetWindowRegistry().size());
}

const EditorWindowMeta* GetRegisteredWindowMeta(int index)
{
    auto& registry = GetWindowRegistry();
    if (index < 0 || index >= static_cast<int>(registry.size()))
    {
        return nullptr;
    }
    return &registry[index];
}

void RegisterEditorWindow(const EditorWindowMeta& meta)
{
    auto& registry = GetWindowRegistry();

    // Check for duplicate by title (prevent accumulation on DLL reload)
    for (const auto& existing : registry)
    {
        if (existing.title && meta.title && std::strcmp(existing.title, meta.title) == 0)
        {
            return;  // Already registered
        }
    }

    registry.push_back(meta);
}

void ClearRegisteredWindows()
{
    GetWindowRegistry().clear();
}

void TrimRegisteredWindows(int keepCount)
{
    auto& registry = GetWindowRegistry();
    if (keepCount >= 0 && keepCount < static_cast<int>(registry.size()))
        registry.resize(keepCount);
}

}  // namespace DekiEditor

#endif  // DEKI_EDITOR
