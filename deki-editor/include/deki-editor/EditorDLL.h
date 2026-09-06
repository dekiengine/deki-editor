#pragma once

#include <stddef.h>

#include <deki-editor/EditorExport.h>

namespace Deki
{

/**
 * @brief Set ImGui context for DLL boundary crossing
 *
 * The editor must call this before using any windows that use ImGui.
 * This ensures that ImGui state is shared correctly across the DLL boundary.
 */
DEKI_EDITOR_API void SetImGuiContext(void* context);

/**
 * @brief Share the host's ImGui allocators with this DLL
 *
 * Sharing only the context is not enough: this DLL links its own copy of ImGui,
 * so it also has its own GImAllocatorAllocFunc/FreeFunc defaulting to its CRT
 * heap. Any ImGui object allocated on one side and freed on the other would
 * then cross heaps. Call this BEFORE SetImGuiContext and before any ImGui use.
 *
 * Signatures match ImGuiMemAllocFunc / ImGuiMemFreeFunc; spelled out here so
 * the header does not have to pull in imgui.h.
 */
DEKI_EDITOR_API void SetImGuiAllocators(void* (*allocFunc)(size_t sz, void* userData),
                                        void (*freeFunc)(void* ptr, void* userData),
                                        void* userData);

}  // namespace Deki
