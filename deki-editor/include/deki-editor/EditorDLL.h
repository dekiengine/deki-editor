#pragma once

#include <stddef.h>

#include <deki-editor/EditorExport.h>

namespace Deki
{

/// Gives this DLL the host's ImGui context, so ImGui state is shared across
/// the DLL boundary. Call it before using any window that uses ImGui.
DEKI_EDITOR_API void SetImGuiContext(void* context);

/// Gives this DLL the host's ImGui allocators.
///
/// The context alone is not enough: this DLL links its own copy of ImGui,
/// whose GImAllocatorAllocFunc/FreeFunc default to its own CRT heap, so an
/// ImGui object allocated on one side and freed on the other would cross
/// heaps. Call this BEFORE SetImGuiContext and before any ImGui use.
///
/// The signatures match ImGuiMemAllocFunc / ImGuiMemFreeFunc, spelled out so
/// this header does not need imgui.h.
DEKI_EDITOR_API void SetImGuiAllocators(void* (*allocFunc)(size_t sz, void* userData),
                                        void (*freeFunc)(void* ptr, void* userData), void* userData);

}  // namespace Deki
