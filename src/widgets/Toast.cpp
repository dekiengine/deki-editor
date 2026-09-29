#include "Toast.h"

#include <deki-editor/EditorTheme.h>
#include <deki-editor/Motion.h>

#include "imgui.h"
#include "imgui_internal.h"  // FindWindowByName - see BuildProgressBottom()

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <vector>

namespace DekiEditor
{

namespace
{

// Unscaled CSS-style px; every use multiplies by the viewport DPI scale, the
// same convention as Metrics in EditorTheme.h.
constexpr float kCardWidth = 380.0f;
constexpr float kMarginRight = 16.0f;
constexpr float kMarginBottom = 40.0f;  // clears the status bar, as ProgressModal does
constexpr float kGap = 8.0f;            // between stacked cards
constexpr float kAccentBarWidth = 3.0f;
constexpr float kSlideIn = 24.0f;  // how far a card travels in from the right

// A burst of toasts must not paper over the viewport. Oldest leave first: the
// newest message is the one the user is most likely acting on. Past the hard
// cap they go at once, so a flood cannot pile up cards that are still leaving.
constexpr std::size_t kMaxVisible = 5;
constexpr std::size_t kHardCap = 12;

struct Entry
{
    Toast::Level level = Toast::Level::Info;
    std::string title;
    std::string message;
    float remaining = 0.0f;  // at or below 0 the card is leaving
    // Measured after the first Begin/End. Positions are chosen BEFORE Begin, so
    // the first frame of a new toast uses the estimate and corrects on the next.
    float height = 64.0f;
    bool measured = false;
    unsigned id = 0;
    float shown = 0.0f;  // 0..1 arrival/departure progress, from Motion
};

// Motion keys for one card: its arrival/departure and its place in the stack.
ImGuiID ShowKey(unsigned id)
{
    return ImHashData(&id, sizeof(id), 0x7057A1u);
}
ImGuiID SlotKey(unsigned id)
{
    return ImHashData(&id, sizeof(id), 0x7057A2u);
}

std::mutex g_Mutex;
std::vector<Entry> g_Entries;
unsigned g_NextId = 1;

ImVec4 LevelColor(Toast::Level level)
{
    switch (level)
    {
        case Toast::Level::Success: return Palette::Green;
        case Toast::Level::Warning: return Palette::Amber;
        case Toast::Level::Error:   return Palette::Red;
        case Toast::Level::Info:
        default:                    return Palette::Accent;
    }
}

// ProgressModal::DrawInFrame owns the same bottom-right corner while a build
// runs. Rather than overlap it, start the stack above it. Its window is drawn
// earlier in the same frame, so Active is already set; WasActive covers the
// frame it first appears.
float BuildProgressHeight()
{
    ImGuiWindow* w = ImGui::FindWindowByName("##BuildProgress");
    if (!w || !(w->Active || w->WasActive))
        return 0.0f;
    return w->Size.y;
}

}  // namespace

void Toast::Push(Level level, const char* title, const std::string& message, float seconds)
{
    std::lock_guard<std::mutex> lock(g_Mutex);

    Entry e;
    e.level = level;
    e.title = title ? title : "";
    e.message = message;
    e.remaining = (std::max)(seconds, 0.5f);
    e.id = g_NextId++;
    g_Entries.push_back(std::move(e));

    // Over the limit: the oldest cards still showing start leaving.
    std::size_t staying = 0;
    for (auto it = g_Entries.rbegin(); it != g_Entries.rend(); ++it)
        if (it->remaining > 0.0f && ++staying > kMaxVisible)
            it->remaining = 0.0f;
    if (g_Entries.size() > kHardCap)
        g_Entries.erase(g_Entries.begin(), g_Entries.begin() + (g_Entries.size() - kHardCap));
}

void Toast::DismissAll()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    g_Entries.clear();
}

std::size_t Toast::Count()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    return g_Entries.size();
}

void Toast::DrawInFrame()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    if (g_Entries.empty())
        return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float dpi = (vp->DpiScale > 0.0f) ? vp->DpiScale : 1.0f;
    const float cardW = kCardWidth * dpi;
    const float dt = ImGui::GetIO().DeltaTime;

    // Bottom edge of the stack, then upward one card at a time.
    float cursorY = vp->WorkPos.y + vp->WorkSize.y - kMarginBottom * dpi;
    const float buildH = BuildProgressHeight();
    if (buildH > 0.0f)
        cursorY -= buildH + kGap * dpi;

    const float rightX = vp->WorkPos.x + vp->WorkSize.x - kMarginRight * dpi;

    // Newest at the bottom, older pushed up.
    for (auto it = g_Entries.rbegin(); it != g_Entries.rend(); ++it)
    {
        Entry& e = *it;

        // Arrives by fading in and sliding from the right; leaves the same way
        // once its time is up or it is clicked.
        e.shown = Motion::ToFrom(ShowKey(e.id), 0.0f, e.remaining > 0.0f ? 1.0f : 0.0f, Motion::Slow);
        const float eased = Motion::EaseOut(e.shown);
        const float alpha = eased;
        const float slideX = (1.0f - eased) * kSlideIn * dpi;

        // The card glides to its slot when the stack changes under it, instead
        // of jumping when a neighbour arrives or leaves.
        const float y = Motion::Follow(SlotKey(e.id), cursorY, Motion::Normal, 0.5f);

        char windowName[32];
        std::snprintf(windowName, sizeof(windowName), "##Toast_%u", e.id);

        ImGui::SetNextWindowPos(ImVec2(rightX + slideX, y), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(cardW, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.97f * alpha);

        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoTitleBar |
                                       ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse |
                                       ImGuiWindowFlags_NoDocking |
                                       ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav;

        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * dpi, 12.0f * dpi));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * dpi, 6.0f * dpi));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * dpi);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Palette::Bg2);
        ImGui::PushStyleColor(ImGuiCol_Border, Palette::Line3);

        bool hovered = false;
        bool clicked = false;

        if (ImGui::Begin(windowName, nullptr, flags))
        {
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

            const ImVec4 accent = LevelColor(e.level);

            // Level is carried by a bar down the left edge rather than an icon:
            // the icon font is merged only at the UI sizes and a missing glyph
            // would render as a box.
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 wpos = ImGui::GetWindowPos();
                const ImVec2 wsize = ImGui::GetWindowSize();
                const float inset = 6.0f * dpi;
                const float barW = kAccentBarWidth * dpi;
                ImVec4 barCol = accent;
                barCol.w *= alpha;
                dl->AddRectFilled(ImVec2(wpos.x + inset, wpos.y + inset),
                                  ImVec2(wpos.x + inset + barW, wpos.y + wsize.y - inset),
                                  ImGui::ColorConvertFloat4ToU32(barCol),
                                  barW * 0.5f);
            }

            if (!e.title.empty())
                ImGui::TextColored(accent, "%s", e.title.c_str());

            if (!e.message.empty())
            {
                ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
                ImGui::TextColored(Palette::Fg, "%s", e.message.c_str());
                ImGui::PopTextWrapPos();
            }

            e.height = ImGui::GetWindowHeight();
            e.measured = true;

            hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem |
                                             ImGuiHoveredFlags_ChildWindows);
            clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        }
        ImGui::End();

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(5);

        // Hover holds the toast open: a message worth reading should not expire
        // while it is being read. Click anywhere to dismiss immediately.
        if (clicked)
            e.remaining = 0.0f;
        else if (!hovered && e.remaining > 0.0f)
            e.remaining -= dt;

        // A leaving card gives up its space as it fades, so the ones above
        // settle down smoothly rather than all at once.
        cursorY -= (e.height + kGap * dpi) * (e.remaining > 0.0f ? 1.0f : eased);
    }

    g_Entries.erase(std::remove_if(g_Entries.begin(), g_Entries.end(),
                                   [](const Entry& e) { return e.remaining <= 0.0f && e.shown <= 0.0f; }),
                    g_Entries.end());
}

}  // namespace DekiEditor
