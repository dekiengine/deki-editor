#pragma once

#include "imgui.h"

#include <cstdint>

// NOLINTNEXTLINE(readability-identifier-naming): SDL's name
struct SDL_Window;

namespace DekiEditor::Brand
{

/// Draws the Deki icon (the blue block with ears, its screen and its two
/// eyes, over its shadow) as a `size` x `size` square at `min`. Drawn from the
/// brand's geometry, so it is sharp at any size; at 32 px or less the eyes are
/// the larger ones the brand uses there.
void DrawIcon(ImDrawList* drawList, const ImVec2& min, float size);

/// Gives `window` the Deki app icon, resources/brand/deki-app-icon-256.png.
/// For Linux and macOS: on Windows the executable's icon resource
/// (resources/windows/deki.ico) is the window's icon at every size.
void SetWindowIcon(SDL_Window* window);

/// The editor logo (DEKI, EDITOR between two rules) for the About window,
/// resources/brand/deki-editor-logo.png, loaded on the first call. Returns its
/// texture, or 0 when it is missing or there is no GPU, and its size in pixels.
uint32_t EditorLogo(int& outWidth, int& outHeight);

}  // namespace DekiEditor::Brand
