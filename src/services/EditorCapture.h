#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace DekiEditor
{

// Screenshot / frame-sequence capture of the editor's own window.
//
// Distinct from FrameCaptureService, which captures the *game* render buffer in
// play mode. This one reads the editor's OpenGL backbuffer, so it sees the real
// ImGui chrome - panels, tabs, gizmos - exactly as a user sees them.
//
// Requests are queued (usually from the MCP tool thread's main-thread pump,
// which runs mid-frame) and serviced by ProcessEndOfFrame(), called after
// ImGui_ImplOpenGL3_RenderDrawData() and before SDL_GL_SwapWindow(). Queuing is
// what makes the timing correct: a capture asked for during frame N lands at the
// end of frame N + framesToWait, so edits made in the same MCP batch are visible.
//
// Files are written to a .tmp sibling and renamed into place, so a client
// polling for the path never observes a half-written PNG.
class EditorCapture
{
public:
    static EditorCapture& Get();

    // Rect in ImGui/framebuffer pixels. An empty (w or h <= 0) rect means
    // "whole window". Resolved at capture time, not at request time.
    struct Region
    {
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    };

    // Queue a single PNG. `panel` (an ImGui window name) wins over `region` when
    // set; `padding` grows the resolved rect on every side, clamped to the window.
    void RequestScreenshot(const std::string& path,
                           const std::string& panel,
                           const Region& region,
                           float padding,
                           int framesToWait);

    // Queue a PNG sequence: `frames` files written every `stride` frames as
    // frame_0000.png … in `dir`, then a `done.txt` marker holding the count.
    // Intended to be assembled into a GIF/video outside the editor.
    void RequestSequence(const std::string& dir,
                         const std::string& panel,
                         const Region& region,
                         float padding,
                         int frames,
                         int stride,
                         int framesToWait);

    // Called once per frame, after the draw data is submitted and before swap.
    void ProcessEndOfFrame(int framebufferWidth, int framebufferHeight);

    // Bring a docked panel's tab to the front. MCP handlers run before
    // ImGui::NewFrame(), so the request is queued and applied by
    // ApplyPendingFocus() at the top of the next frame instead of poking
    // ImGui's focus state from outside a frame scope.
    void RequestFocus(const std::string& panel);
    void ApplyPendingFocus();

    // Names + rects of currently-live ImGui windows, for picking a `panel`.
    struct PanelInfo
    {
        std::string name;
        float x, y, w, h;
        bool visible;
        bool docked;
    };
    static std::vector<PanelInfo> ListPanels();

    bool HasPendingWork() const { return !m_Requests.empty(); }

    // A frame sequence is being recorded. The main loop then steps the UI at
    // a fixed 1/60 s per frame, so each captured frame is one sixtieth of a
    // second of animation however long writing it takes.
    bool IsRecordingSequence() const
    {
        for (const Request& r : m_Requests)
            if (r.sequence)
                return true;
        return false;
    }

private:
    EditorCapture() = default;

    struct Request
    {
        bool sequence = false;
        std::string path;  // file (single) or directory (sequence)
        std::string panel;
        Region region;
        float padding = 0.0f;
        int framesToWait = 0;
        // Sequence bookkeeping.
        int framesRemaining = 0;
        int stride = 1;
        int strideCounter = 0;
        int frameIndex = 0;
        // Consecutive frames where the target rect could not be resolved -
        // usually a docked panel whose tab is not currently selected. Transient,
        // so wait it out rather than abandoning the request.
        int misses = 0;
    };

    // How long to tolerate an unresolvable target before giving up. At 60 fps
    // this is a couple of seconds: long enough to ride out a tab switch, short
    // enough that a genuinely wrong panel name still fails rather than hangs.
    static constexpr int kMaxResolveMisses = 150;

    // Resolve panel/region into a pixel rect clamped to the framebuffer.
    // Returns false when the named panel does not exist or is off-screen.
    bool ResolveRect(const Request& req, int fbW, int fbH,
                     int& outX, int& outY, int& outW, int& outH) const;

    bool CaptureToFile(const std::string& path, int x, int y, int w, int h, int fbH);

    std::vector<Request> m_Requests;
    std::vector<std::string> m_PendingFocus;
    std::vector<uint8_t> m_Pixels;  // reused scratch for glReadPixels
};

}  // namespace DekiEditor
