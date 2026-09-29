#include <deki-editor/Motion.h>
#include <deki-editor/EditorSettings.h>

#include "imgui_internal.h"

#include <cmath>
#include <unordered_map>

namespace DekiEditor
{
namespace Motion
{
namespace
{
// ImGui rebuilds every widget from scratch each frame, so an animation needs a
// value that outlives the frame: one float per animated thing, keyed by an
// ImGui id (stable across frames, and across windows if the widget moves).
struct Entry
{
    float value = 0.0f;
    int frame = 0;  // last frame this id was seen, for pruning
};
std::unordered_map<ImGuiID, Entry> s_Values;

int s_MovingFrame = -2;  // last frame anything was still short of its target

int s_ReduceMotion = -1;  // -1 = not read from the settings yet

// Look up (or create at `initial`) the entry for `id`, pruning first: ids are
// cheap but not free, so anything not drawn for a few seconds (a closed
// panel, a destroyed window) goes whenever the table grows large. Pruned
// BEFORE the lookup, since erasing would invalidate a reference taken first.
Entry& Touch(ImGuiID id, float initial)
{
    const int frame = ImGui::GetFrameCount();
    if (s_Values.size() > 512 && (frame & 255) == 0)
    {
        for (auto i = s_Values.begin(); i != s_Values.end();)
            i = (frame - i->second.frame > 600) ? s_Values.erase(i) : ++i;
    }
    Entry& e = s_Values.try_emplace(id, Entry{ initial, frame }).first->second;
    e.frame = frame;
    return e;
}

float Step(Entry& e, float target, float seconds)
{
    if (seconds <= 0.0f || ReduceMotion())
    {
        e.value = target;
        return e.value;
    }
    const float step = ImGui::GetIO().DeltaTime / seconds;
    e.value = (e.value < target) ? ImMin(target, e.value + step) : ImMax(target, e.value - step);
    if (e.value != target)
        s_MovingFrame = ImGui::GetFrameCount();
    return e.value;
}
}  // namespace

bool ReduceMotion()
{
    if (s_ReduceMotion < 0)
        s_ReduceMotion = EditorSettings::GetBool("editor.reduceMotion", false) ? 1 : 0;
    return s_ReduceMotion == 1;
}

void SetReduceMotion(bool reduce)
{
    s_ReduceMotion = reduce ? 1 : 0;
    EditorSettings::SetBool("editor.reduceMotion", reduce);
}

float To(ImGuiID id, float target, float seconds)
{
    return Step(Touch(id, target), target, seconds);
}

float ToFrom(ImGuiID id, float from, float target, float seconds)
{
    return Step(Touch(id, ReduceMotion() ? target : from), target, seconds);
}

float Follow(ImGuiID id, float target, float seconds, float epsilon)
{
    Entry& e = Touch(id, target);
    if (seconds <= 0.0f || ReduceMotion())
    {
        e.value = target;
        return e.value;
    }
    // Exponential approach, frame-rate independent: after `seconds` about 95%
    // of the distance is covered (e^-3), whatever the frame rate.
    const float k = 1.0f - std::exp(-3.0f * ImGui::GetIO().DeltaTime / seconds);
    e.value += (target - e.value) * k;
    if (std::fabs(target - e.value) <= epsilon)
        e.value = target;
    else
        s_MovingFrame = ImGui::GetFrameCount();
    return e.value;
}

void Set(ImGuiID id, float value)
{
    Touch(id, value).value = value;
}

bool AnyMoving()
{
    return s_MovingFrame >= ImGui::GetFrameCount() - 1;
}
}  // namespace Motion
}  // namespace DekiEditor
