#include "TouchGestures.h"

#include <cmath>

namespace DekiEditor
{

TouchGestures::Finger* TouchGestures::Find(uint64_t id)
{
    for (auto& f : m_Fingers)
    {
        if (f.id == id)
        {
            return &f;
        }
    }
    return nullptr;
}

void TouchGestures::Emit(MouseEvent::Kind kind, int button, float x, float y)
{
    MouseEvent e;
    e.kind = kind;
    e.button = button;
    e.x = x;
    e.y = y;
    m_Pending.push_back(e);
    if (kind == MouseEvent::Kind::Move)
    {
        m_PointerX = x;
        m_PointerY = y;
    }
}

bool TouchGestures::GesturePoints(float& cx, float& cy, float& dist) const
{
    if (m_Fingers.size() < 2)
    {
        return false;
    }
    const Finger& a = m_Fingers[0];
    const Finger& b = m_Fingers[1];
    cx = (a.x + b.x) * 0.5f;
    cy = (a.y + b.y) * 0.5f;
    dist = std::hypot(b.x - a.x, b.y - a.y);
    return true;
}

void TouchGestures::FingerDown(uint64_t id, float x, float y, double time, bool direct)
{
    if (Finger* f = Find(id))
    {
        // A down for a finger already down: a missed up. Treat it as a move.
        f->x = x;
        f->y = y;
        return;
    }
    m_Fingers.push_back({ id, x, y });

    switch (m_State)
    {
        case State::Idle:
            m_OriginX = x;
            m_OriginY = y;
            m_DownTime = time;
            if (direct)
            {
                EmitMove(x, y);
                Emit(MouseEvent::Kind::Down, 0, x, y);
                m_State = State::Direct;
            }
            else
            {
                // Nothing pressed yet, but the pointer goes there so what is under
                // the finger shows it is about to be touched.
                EmitMove(x, y);
                m_State = State::Pending;
            }
            break;

        case State::Pending:
            // A second finger before the first decided anything: a gesture.
            GesturePoints(m_LastCenterX, m_LastCenterY, m_LastDist);
            EmitMove(m_LastCenterX, m_LastCenterY);
            m_State = State::Gesture;
            break;

        default:
            // Dragging and Direct keep the first finger's meaning; a gesture uses
            // the first two; Spent waits for everything to lift. Extra fingers
            // are only tracked so their lift is not mistaken for the last one.
            break;
    }
}

void TouchGestures::FingerMove(uint64_t id, float x, float y)
{
    Finger* f = Find(id);
    if (!f)
    {
        return;
    }
    f->x = x;
    f->y = y;
    const bool primary = (&m_Fingers.front() == f);

    switch (m_State)
    {
        case State::Pending:
            if (std::hypot(x - m_OriginX, y - m_OriginY) > m_Config.tapSlop)
            {
                // Press where it went down, so a drag starts on what was touched
                // rather than on whatever the finger has slid onto.
                EmitMove(m_OriginX, m_OriginY);
                Emit(MouseEvent::Kind::Down, 0, m_OriginX, m_OriginY);
                EmitMove(x, y);
                m_State = State::Dragging;
            }
            break;

        case State::Dragging:
        case State::Direct:
            if (primary)
            {
                EmitMove(x, y);
            }
            break;

        default:
            // Gesture movement is read in Update, once per frame, not per event.
            break;
    }
}

void TouchGestures::FingerUp(uint64_t id, float x, float y)
{
    Finger* f = Find(id);
    if (!f)
    {
        return;
    }
    f->x = x;
    f->y = y;
    const bool primary = (&m_Fingers.front() == f);

    switch (m_State)
    {
        case State::Pending:
            // Lifted before moving or holding: a tap, at the landing point, which
            // is where the user aimed; the lift can drift a few pixels.
            EmitMove(m_OriginX, m_OriginY);
            Emit(MouseEvent::Kind::Down, 0, m_OriginX, m_OriginY);
            Emit(MouseEvent::Kind::Up, 0, m_OriginX, m_OriginY);
            m_State = State::Spent;
            break;

        case State::Dragging:
        case State::Direct:
            if (primary)
            {
                EmitMove(x, y);
                Emit(MouseEvent::Kind::Up, 0, x, y);
                m_State = State::Spent;
            }
            break;

        case State::Gesture:
            // One of the two lifted. Going back to one-finger meaning halfway
            // would drag whatever is under the finger that stayed.
            m_State = State::Spent;
            break;

        default: break;
    }

    for (auto it = m_Fingers.begin(); it != m_Fingers.end(); ++it)
    {
        if (it->id == id)
        {
            m_Fingers.erase(it);
            break;
        }
    }
    if (m_Fingers.empty())
    {
        Release();
    }
}

void TouchGestures::Release()
{
    Emit(MouseEvent::Kind::Leave, 0, m_PointerX, m_PointerY);
    m_State = State::Idle;
}

void TouchGestures::CancelAll()
{
    if (m_State == State::Dragging || m_State == State::Direct)
    {
        Emit(MouseEvent::Kind::Up, 0, m_PointerX, m_PointerY);
    }
    m_Fingers.clear();
    if (m_State != State::Idle)
    {
        Release();
    }
}

TouchGestures::Output TouchGestures::Update(double time)
{
    if (m_State == State::Pending && time - m_DownTime >= m_Config.longPressSeconds)
    {
        EmitMove(m_OriginX, m_OriginY);
        Emit(MouseEvent::Kind::Down, 1, m_OriginX, m_OriginY);
        Emit(MouseEvent::Kind::Up, 1, m_OriginX, m_OriginY);
        m_State = State::Spent;
    }

    Output out;
    if (m_State == State::Gesture)
    {
        float cx, cy, dist;
        if (GesturePoints(cx, cy, dist))
        {
            out.gesture.active = true;
            out.gesture.centerX = cx;
            out.gesture.centerY = cy;
            out.gesture.panX = cx - m_LastCenterX;
            out.gesture.panY = cy - m_LastCenterY;
            // Two fingers on top of each other have no meaningful spread.
            out.gesture.scale = (m_LastDist > 1.0f && dist > 1.0f) ? dist / m_LastDist : 1.0f;
            if (cx != m_PointerX || cy != m_PointerY)
            {
                EmitMove(cx, cy);
            }
            m_LastCenterX = cx;
            m_LastCenterY = cy;
            m_LastDist = dist;
        }
    }
    out.mouse.swap(m_Pending);
    return out;
}

}  // namespace DekiEditor
