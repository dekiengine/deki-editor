#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>

namespace DekiEditor
{

/// Screenshots and frame sequences of the editor's own window.
///
/// FrameCaptureService captures the game's render buffer in play mode; this
/// reads the frame EditorRenderer drew, so it shows the ImGui panels, tabs and
/// gizmos exactly as a user sees them, minimized or not.
///
/// Requests are queued (usually from the MCP main-thread pump, which runs
/// mid-frame) and handled by ProcessEndOfFrame(), called after
/// EditorRenderer::Render(). Queuing
/// gets the timing right: a capture asked for during frame N happens at the end
/// of frame N + framesToWait, so edits made in the same MCP batch are visible.
///
/// Files are written to a .tmp file beside the target and renamed into place,
/// so a client polling for the path never sees a half-written PNG.
class EditorCapture
{
public:
    static EditorCapture& Get();

    /// Rect in ImGui (framebuffer) pixels. An empty rect (w or h <= 0) means
    /// the whole window. Resolved at capture time, not at request time.
    struct Region
    {
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    };

    /// Queues one PNG. `panel` (an ImGui window name), when set, wins over
    /// `region`; `padding` grows the rect on every side, clamped to the window.
    /// Returns a ticket for TakeOutcome.
    uint64_t RequestScreenshot(const std::string& path, const std::string& panel, const Region& region, float padding,
                               int framesToWait);

    /// True once the screenshot with this ticket is done: written (`error`
    /// empty) or given up on (`error` says why). Each outcome is taken once.
    bool TakeOutcome(uint64_t ticket, std::string& error);

    /// Queues a PNG sequence: `frames` files, one every `stride` frames, as
    /// frame_0000.png … in `dir`, then a `done.txt` marker holding the count.
    /// Meant to be made into a GIF or video outside the editor. With
    /// `agentPointer` the frames show the agent's pointer (AgentCursor), for a
    /// recording of an agent at work; screenshots never do.
    void RequestSequence(const std::string& dir, const std::string& panel, const Region& region, float padding,
                         int frames, int stride, int framesToWait, bool agentPointer = false);

    /// Called once per frame, after EditorRenderer::Render.
    void ProcessEndOfFrame(int framebufferWidth, int framebufferHeight);

    /// Brings a docked panel's tab to the front. MCP handlers run before
    /// ImGui::NewFrame(), so the request is queued and ApplyPendingFocus()
    /// applies it at the top of the next frame, inside a frame scope.
    void RequestFocus(const std::string& panel);
    void ApplyPendingFocus();

    /// Names and rects of the open ImGui windows, for picking a `panel`.
    struct PanelInfo
    {
        std::string name;
        float x, y, w, h;
        bool visible;
        bool docked;
    };
    static std::vector<PanelInfo> ListPanels();

    bool HasPendingWork() const { return !m_Requests.empty(); }

    /// True while a pending capture must not show the agent's pointer: any
    /// screenshot, and a sequence not asked to show it.
    bool HidesAgentPointer() const
    {
        for (const Request& r : m_Requests)
        {
            if (!r.agentPointer)
            {
                return true;
            }
        }
        return false;
    }

    /// True while a frame sequence is being recorded. The main loop then steps
    /// the UI by a fixed 1/60 s per frame, so each captured frame is 1/60 s of
    /// animation however long writing it takes.
    bool IsRecordingSequence() const
    {
        for (const Request& r : m_Requests)
        {
            if (r.sequence)
            {
                return true;
            }
        }
        return false;
    }

private:
    EditorCapture() = default;

    struct Request
    {
        bool sequence = false;
        bool agentPointer = false;  // a sequence that shows AgentCursor
        uint64_t ticket = 0;        // a screenshot's, for TakeOutcome
        std::string path;           // file (single) or directory (sequence)
        std::string panel;
        Region region;
        float padding = 0.0f;
        int framesToWait = 0;
        // Sequence state.
        int framesRemaining = 0;
        int stride = 1;
        int strideCounter = 0;
        int frameIndex = 0;
        // Frames in a row where the target rect could not be found, usually a
        // docked panel whose tab is not selected. That passes, so the request
        // waits instead of failing.
        int misses = 0;
    };

    // How many frames to wait for a missing target before giving up. At 60 fps
    // a couple of seconds: long enough for a tab switch, short enough that a
    // wrong panel name fails rather than hangs.
    static constexpr int kMaxResolveMisses = 150;

    // Turns the panel or region into a pixel rect clamped to the framebuffer.
    // Returns false when the named panel does not exist or is off-screen.
    bool ResolveRect(const Request& req, int fbW, int fbH, int& outX, int& outY, int& outW, int& outH) const;

    bool CaptureToFile(const std::string& path, int x, int y, int w, int h);

    // Records how a screenshot ended, for TakeOutcome.
    void Finish(const Request& req, std::string error);

    // Fast PNG settings (see CaptureToFile): deflate level 1, filter 0 (none).
    static constexpr int kPngCompressionLevel = 1;
    static constexpr int kPngFilter = 0;

    std::vector<Request> m_Requests;
    uint64_t m_LastTicket = 0;
    std::unordered_map<uint64_t, std::string> m_Outcomes;  // ticket -> error, "" when written
    std::vector<std::string> m_PendingFocus;
    std::vector<uint8_t> m_Pixels;  // reused scratch for glReadPixels
};

}  // namespace DekiEditor
