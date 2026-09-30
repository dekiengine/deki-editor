#pragma once

#include <vector>

#include "TouchGestures.h"
#include "imgui.h"

union SDL_Event;
struct SDL_Window;

namespace DekiEditor
{

// Touchscreen input for the editor: taps click, a drag drags, holding still
// opens the context menu, and two fingers scroll a panel or, in the scene
// view, pan and pinch-zoom. TouchGestures decides what the fingers mean; this
// reads them from SDL and plays the result into ImGui.
//
// SDL's own touch-to-mouse emulation is switched off (DisableSdlTouchMouse,
// before SDL_Init), because it presses the left button the moment a finger
// lands, which makes a scroll select whatever it started on and leaves no way
// to right click.
//
// Wiring, per frame:
//   ProcessEvent(e)   next to ImGui_ImplSDL3_ProcessEvent, for every event
//   InjectPending()   after ImGui_ImplSDL3_NewFrame, before ImGui::NewFrame,
//                     like EditorInput: after, so a touch position wins over
//                     the backend's report of the real cursor; before, so
//                     ImGui sees it this frame
//
// Panels, during the frame:
//   ClaimGesture(...)     take this frame's two-finger pan/pinch over a rect;
//                         an unclaimed one scrolls the panel under the fingers
//   AddDirectRegion(...)  fingers landing here press at once (the Play view)
class TouchInput
{
public:
    static TouchInput& Get();

    // Before SDL_Init. Only the editor's own windows are affected; the crash
    // dialog turns emulation back on for itself (RestoreSdlTouchMouse).
    static void DisableSdlTouchMouse();
    static void RestoreSdlTouchMouse();

    void SetWindow(SDL_Window* window) { m_Window = window; }

    void ProcessEvent(const SDL_Event& event);
    void InjectPending();

    struct Gesture
    {
        ImVec2 center;           // midpoint of the two fingers, ImGui pixels
        ImVec2 pan;              // how far it moved this frame
        float scale = 1.0f;      // pinch: >1 fingers spread apart
    };

    // This frame's two-finger gesture, when its midpoint is inside [min, max].
    // Claiming it stops it scrolling the panel underneath.
    bool ClaimGesture(const ImVec2& min, const ImVec2& max, Gesture& out);

    void AddDirectRegion(const ImVec2& min, const ImVec2& max);

private:
    TouchInput() = default;

    struct Rect
    {
        ImVec2 min, max;
        bool Contains(float x, float y) const
        {
            return x >= min.x && y >= min.y && x < max.x && y < max.y;
        }
    };

    bool ToWindow(const SDL_Event& event, float& x, float& y) const;
    bool InDirectRegion(float x, float y) const;
    void ScrollUnclaimed();

    SDL_Window* m_Window = nullptr;
    TouchGestures m_Gestures;

    TouchGestures::Gesture m_Frame;  // this frame's gesture
    bool m_FrameClaimed = false;

    // Direct regions from the last complete frame: a finger lands between
    // frames, before this frame's panels have said where they are.
    std::vector<Rect> m_Direct, m_DirectNext;
};

}  // namespace DekiEditor
