#pragma once

#include <SDL3/SDL.h>

#include "imgui.h"

#include <cstdint>
#include <string>

namespace DekiEditor
{

/// Draws the editor window with SDL's GPU API: Direct3D 12 on Windows, Vulkan
/// on Linux, Metal on macOS. It also makes the textures behind EditorTexture.
///
/// Each frame is drawn into a texture of its own and then copied to the
/// window, so a minimized editor can still draw, and screenshots read that
/// texture rather than the window.
namespace EditorRenderer
{

/// Whether `window` is minimized. SDL's own flag misses an editor started in
/// the background (minimized while still hidden), so Windows is asked too.
bool IsMinimized(SDL_Window* window);

/// A GPU device ImGui's SDL GPU backend can draw with, or null when the machine
/// has none. The editor's own is made by Attach; this is for another window's
/// (the crash dialog's).
SDL_GPUDevice* CreateDevice();

/// Makes the GPU device and draws into `window` from now on. False when the
/// machine has no device for it; `failedStep` names the call that failed, and
/// SDL_GetError() says why.
bool Attach(SDL_Window* window, std::string& failedStep);

/// ImGui's platform and renderer backends for the window.
bool InitImGui(SDL_Window* window);

/// Undoes InitImGui and Attach.
void Shutdown();

/// "direct3d12", "vulkan" or "metal".
const char* DriverName();

/// Before ImGui_ImplSDL3_NewFrame, every frame.
void NewFrame();

/// Draws ImGui's frame over `clear` and shows it. `waitForVsync` false shows
/// it without waiting for the display. Returns false when it was drawn but not
/// shown, as when the window is minimized.
bool Render(ImDrawData* drawData, const ImVec4& clear, bool waitForVsync = true);

/// Copies a rectangle of the last frame drawn into `rgba`: `width` x `height`
/// RGBA8 pixels, rows top to bottom. False when there is no such frame.
bool ReadPixels(int x, int y, int width, int height, uint8_t* rgba);

/// Whether frames wait for the display's refresh. On by default; play mode
/// turns it off to run uncapped.
void SetVsync(bool on);

}  // namespace EditorRenderer

}  // namespace DekiEditor
