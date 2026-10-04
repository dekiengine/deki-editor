#pragma once

#include <cstdint>
#include <vector>

namespace DekiEditor
{

// What the fingers on a touchscreen mean to the editor, decided without SDL or
// ImGui so it can be tested on its own. TouchInput feeds it finger events and
// plays what it decides into ImGui.
//
// One finger is a mouse with a left button, decided lazily: until the finger
// moves past the tap slop, lifts, or is joined by a second finger, nothing is
// pressed, because each of those means something different.
// - lifts in place:          a left click where it went down
// - moves past the slop:     a left drag from where it went down
// - held still long enough:  a right click (the context menu)
// - joined by a second one:  a two-finger gesture, and no button at all
//
// Two fingers pan (their midpoint's movement) and pinch (the change in the
// distance between them). Whoever reads the gesture decides what that does:
// the scene view pans and zooms, every other panel scrolls.
//
// A finger that lands in a direct region (the Play view while a game runs)
// presses at once and never becomes a right click or a gesture: a game wants
// touch down when the finger touches, not when the editor has made up its mind.
//
// Positions are in ImGui pixels; times are seconds on any steady clock.
class TouchGestures
{
public:
    struct Config
    {
        float tapSlop = 10.0f;          // pixels a finger may drift and still tap
        double longPressSeconds = 0.5;  // held this long in place: right click
    };

    struct MouseEvent
    {
        enum class Kind
        {
            Move,  // pointer to (x, y)
            Down,  // `button` pressed at the current position
            Up,    // `button` released at the current position
            Leave  // no pointer: a touchscreen has nothing hovering once lifted
        } kind = Kind::Move;
        int button = 0;  // 0 left, 1 right
        float x = 0.0f, y = 0.0f;
    };

    struct Gesture
    {
        bool active = false;  // two fingers down this update
        float centerX = 0.0f, centerY = 0.0f;
        float panX = 0.0f, panY = 0.0f;  // midpoint movement since the last update
        float scale = 1.0f;              // distance ratio since the last update
    };

    struct Output
    {
        std::vector<MouseEvent> mouse;  // in order
        Gesture gesture;
    };

    TouchGestures() = default;
    explicit TouchGestures(const Config& config)
        : m_Config(config)
    {
    }

    void SetConfig(const Config& config) { m_Config = config; }
    const Config& GetConfig() const { return m_Config; }

    void FingerDown(uint64_t id, float x, float y, double time, bool direct);
    void FingerMove(uint64_t id, float x, float y);
    void FingerUp(uint64_t id, float x, float y);
    // The system took the touches away (SDL_EVENT_FINGER_CANCELED): release
    // whatever is pressed and forget every finger.
    void CancelAll();

    // What happened since the last call. `time` drives the long press, which
    // fires while the finger is still, so it cannot wait for an event.
    Output Update(double time);

    // Whether a touch currently owns the pointer (TouchInput repeats its
    // position every frame while it does, so a real mouse cursor cannot move it).
    bool OwnsPointer() const { return m_State != State::Idle; }
    float PointerX() const { return m_PointerX; }
    float PointerY() const { return m_PointerY; }

private:
    enum class State
    {
        Idle,      // no fingers
        Pending,   // one finger, undecided
        Dragging,  // one finger, left button held
        Direct,    // one finger in a direct region, left button held
        Gesture,   // two fingers
        Spent      // the touch has done its thing; wait for every finger to lift
    };

    struct Finger
    {
        uint64_t id = 0;
        float x = 0.0f, y = 0.0f;
    };

    Finger* Find(uint64_t id);
    void Emit(MouseEvent::Kind kind, int button, float x, float y);
    void EmitMove(float x, float y) { Emit(MouseEvent::Kind::Move, 0, x, y); }
    void Release();  // every finger is gone
    bool GesturePoints(float& cx, float& cy, float& dist) const;

    Config m_Config;
    State m_State = State::Idle;
    std::vector<Finger> m_Fingers;  // in the order they went down

    float m_OriginX = 0.0f, m_OriginY = 0.0f;  // where the first finger landed
    double m_DownTime = 0.0;
    float m_PointerX = 0.0f, m_PointerY = 0.0f;

    // Gesture baseline: midpoint and spread at the last Update.
    float m_LastCenterX = 0.0f, m_LastCenterY = 0.0f, m_LastDist = 0.0f;

    std::vector<MouseEvent> m_Pending;
};

}  // namespace DekiEditor
