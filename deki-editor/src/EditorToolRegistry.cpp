#include "imgui.h"
#include "imgui_internal.h"  // ImTextInitClassifiers

#ifdef _WIN32
#ifdef DEKI_EDITOR_EXPORTS
#define DEKI_EDITOR_API __declspec(dllexport)
#else
#define DEKI_EDITOR_API __declspec(dllimport)
#endif
#else
#define DEKI_EDITOR_API
#endif

namespace Deki
{

// Shares the editor's ImGui context with this DLL. The editor calls it before
// any window here uses ImGui.
DEKI_EDITOR_API void SetImGuiContext(void* context)
{
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context));
    // This DLL's ImGui copy also has its own table of which characters are
    // spaces and punctuation, which ImGui fills in only when it builds a font
    // atlas, and the editor builds the atlas in its own copy. Left empty, every
    // character counts as a space, and word-wrapped text (EditorUI's
    // TextWrapped, so every dialog and package window) breaks mid-word.
    if (context != nullptr)
    {
        ImTextInitClassifiers();
    }
}

// This DLL's own ImGui copy has its own allocator globals; point them at the
// host's so allocations made here can be freed there (and vice versa).
DEKI_EDITOR_API void SetImGuiAllocators(void* (*allocFunc)(size_t sz, void* userData),
                                        void (*freeFunc)(void* ptr, void* userData), void* userData)
{
    ImGui::SetAllocatorFunctions(allocFunc, freeFunc, userData);
}

}  // namespace Deki
