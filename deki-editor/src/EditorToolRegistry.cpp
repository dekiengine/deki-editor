#include "imgui.h"

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

// ImGui context sharing across DLL boundary
// The editor must call this before using any windows that use ImGui
DEKI_EDITOR_API void SetImGuiContext(void* context)
{
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context));
}

// This DLL's own ImGui copy has its own allocator globals; point them at the
// host's so allocations made here can be freed there (and vice versa).
DEKI_EDITOR_API void SetImGuiAllocators(void* (*allocFunc)(size_t sz, void* userData),
                                        void (*freeFunc)(void* ptr, void* userData), void* userData)
{
    ImGui::SetAllocatorFunctions(allocFunc, freeFunc, userData);
}

}  // namespace Deki
