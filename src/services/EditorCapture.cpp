#include "EditorCapture.h"
#include "EditorInput.h"

#include <SDL3/SDL_opengl.h>
#include "imgui.h"
#include "imgui_internal.h"  // FindWindowByName

// STB_IMAGE_WRITE_IMPLEMENTATION is in StbImageWrite.cpp; this file only uses
// the declarations.
#include "stb_image_write.h"

#include <deki/LogSystem.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cstdio>

namespace fs = std::filesystem;

namespace DekiEditor
{

EditorCapture& EditorCapture::Get()
{
    static EditorCapture s_Instance;
    return s_Instance;
}

uint64_t EditorCapture::RequestScreenshot(const std::string& path, const std::string& panel, const Region& region,
                                          float padding, int framesToWait)
{
    Request req;
    req.sequence = false;
    req.ticket = ++m_LastTicket;
    req.path = path;
    req.panel = panel;
    req.region = region;
    req.padding = padding;
    req.framesToWait = std::max(0, framesToWait);
    m_Requests.push_back(std::move(req));
    return m_LastTicket;
}

bool EditorCapture::TakeOutcome(uint64_t ticket, std::string& error)
{
    auto it = m_Outcomes.find(ticket);
    if (it == m_Outcomes.end())
    {
        return false;
    }
    error = std::move(it->second);
    m_Outcomes.erase(it);
    return true;
}

void EditorCapture::Finish(const Request& req, std::string error)
{
    // Kept only for a caller that asked for it (McpCompletion polls by ticket).
    if (req.ticket != 0)
    {
        m_Outcomes[req.ticket] = std::move(error);
    }
}

void EditorCapture::RequestSequence(const std::string& dir, const std::string& panel, const Region& region,
                                    float padding, int frames, int stride, int framesToWait, bool agentPointer)
{
    Request req;
    req.sequence = true;
    req.agentPointer = agentPointer;
    req.path = dir;
    req.panel = panel;
    req.region = region;
    req.padding = padding;
    req.framesToWait = std::max(0, framesToWait);
    req.framesRemaining = std::max(1, frames);
    req.stride = std::max(1, stride);
    req.strideCounter = 0;
    req.frameIndex = 0;
    m_Requests.push_back(std::move(req));
}

void EditorCapture::RequestFocus(const std::string& panel)
{
    if (!panel.empty())
    {
        m_PendingFocus.push_back(panel);
    }
}

void EditorCapture::ApplyPendingFocus()
{
    for (const auto& name : m_PendingFocus)
    {
        ImGui::SetWindowFocus(name.c_str());
    }
    m_PendingFocus.clear();
}

std::vector<EditorCapture::PanelInfo> EditorCapture::ListPanels()
{
    std::vector<PanelInfo> out;
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx)
    {
        return out;
    }

    for (ImGuiWindow* w : ctx->Windows)
    {
        if (!w)
        {
            continue;
        }

        // A docked panel is moved under its dock node's host window, so it has
        // ChildWindow set, and that flag alone would filter out the windows
        // worth capturing. Anything docked counts as a top-level panel; only
        // real child windows are dropped.
        const bool isDockedPanel = (w->DockId != 0) || (w->DockNode != nullptr);
        if (!isDockedPanel && (w->Flags & ImGuiWindowFlags_ChildWindow))
        {
            continue;
        }
        if (w->Flags & ImGuiWindowFlags_Tooltip)
        {
            continue;
        }
        if (w->Hidden && !w->WasActive)
        {
            continue;
        }
        // Dock node host windows are internal to ImGui, not panels.
        if (w->Name && strncmp(w->Name, "##", 2) == 0)
        {
            continue;
        }

        PanelInfo info;
        info.name = w->Name ? w->Name : "";
        info.x = w->Pos.x;
        info.y = w->Pos.y;
        info.w = w->Size.x;
        info.h = w->Size.y;
        // A docked panel whose tab is not selected is alive but not drawn, so
        // it must be focused before it can be captured.
        info.visible = w->WasActive && !w->Hidden;
        info.docked = w->DockIsActive;
        out.push_back(std::move(info));
    }
    return out;
}

bool EditorCapture::ResolveRect(const Request& req, int fbW, int fbH, int& outX, int& outY, int& outW, int& outH) const
{
    float x = 0.0f, y = 0.0f, w = (float)fbW, h = (float)fbH;

    if (!req.panel.empty())
    {
        ImGuiWindow* win = ImGui::FindWindowByName(req.panel.c_str());
        if (!win || !win->WasActive)
        {
            return false;
        }
        x = win->Pos.x;
        y = win->Pos.y;
        w = win->Size.x;
        h = win->Size.y;
    }
    else if (req.region.w > 0.0f && req.region.h > 0.0f)
    {
        x = req.region.x;
        y = req.region.y;
        w = req.region.w;
        h = req.region.h;
    }

    x -= req.padding;
    y -= req.padding;
    w += req.padding * 2.0f;
    h += req.padding * 2.0f;

    // Clamp into the framebuffer. Do it on the rect edges so padding that runs
    // off-screen shrinks the capture instead of shifting it.
    float x0 = std::max(0.0f, x);
    float y0 = std::max(0.0f, y);
    float x1 = std::min((float)fbW, x + w);
    float y1 = std::min((float)fbH, y + h);

    outX = (int)x0;
    outY = (int)y0;
    outW = (int)(x1 - x0);
    outH = (int)(y1 - y0);

    return outW > 0 && outH > 0;
}

bool EditorCapture::CaptureToFile(const std::string& path, int x, int y, int w, int h, int fbH)
{
    const size_t byteCount = (size_t)w * (size_t)h * 4u;
    m_Pixels.resize(byteCount);

    // GL origin is bottom-left; our rect is top-left based.
    const int glY = fbH - (y + h);

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(x, glY, w, h, GL_RGBA, GL_UNSIGNED_BYTE, m_Pixels.data());

    // glReadPixels returns rows bottom-up; PNG wants top-down.
    const size_t stride = (size_t)w * 4u;
    std::vector<uint8_t> row(stride);
    for (int r = 0; r < h / 2; ++r)
    {
        uint8_t* a = m_Pixels.data() + (size_t)r * stride;
        uint8_t* b = m_Pixels.data() + (size_t)(h - 1 - r) * stride;
        std::copy(a, a + stride, row.data());
        std::copy(b, b + stride, a);
        std::copy(row.begin(), row.end(), b);
    }

    // The backbuffer's alpha means nothing here (nothing is composited over
    // it), and alpha 0 would make a fully transparent PNG, so force it opaque.
    for (size_t i = 3; i < byteCount; i += 4)
    {
        m_Pixels[i] = 255;
    }

    std::error_code ec;
    fs::path target(path);
    if (target.has_parent_path())
    {
        fs::create_directories(target.parent_path(), ec);
    }

    // The write blocks the editor's frame and an MCP screenshot call waits for
    // it. By default stb tries all five PNG row filters on every row and keeps
    // the best, which is most of its time; no filter and the lightest deflate
    // encode the whole window about 2.5 times faster for a file some 4% larger.
    const std::string tmp = path + ".tmp";
    const int savedLevel = stbi_write_png_compression_level;
    const int savedFilter = stbi_write_force_png_filter;
    stbi_write_png_compression_level = kPngCompressionLevel;
    stbi_write_force_png_filter = kPngFilter;
    const bool encoded = stbi_write_png(tmp.c_str(), w, h, 4, m_Pixels.data(), (int)stride) != 0;
    stbi_write_png_compression_level = savedLevel;
    stbi_write_force_png_filter = savedFilter;
    if (!encoded)
    {
        Deki::LogSystem::LogWithLevel(Deki::LogLevel::Error, __FILE__, __LINE__, "EditorCapture: failed to write %s",
                                      tmp.c_str());
        return false;
    }

    fs::remove(target, ec);
    fs::rename(tmp, target, ec);
    if (ec)
    {
        Deki::LogSystem::LogWithLevel(Deki::LogLevel::Error, __FILE__, __LINE__,
                                      "EditorCapture: failed to move capture into place: %s", ec.message().c_str());
        return false;
    }
    return true;
}

void EditorCapture::ProcessEndOfFrame(int fbW, int fbH)
{
    if (m_Requests.empty() || fbW <= 0 || fbH <= 0)
    {
        return;
    }

    // A screenshot asked for after a click shows what the click did: the
    // frames to wait start counting once the queued input has played out. A
    // sequence does not wait: it records whatever happens, the input included
    // (a press as it is held, a menu as it opens).
    const bool inputPending = EditorInput::Get().HasPendingWork();

    for (size_t i = 0; i < m_Requests.size();)
    {
        Request& req = m_Requests[i];
        if (inputPending && !req.sequence)
        {
            ++i;
            continue;
        }

        if (req.framesToWait > 0)
        {
            --req.framesToWait;
            ++i;
            continue;
        }

        int x, y, w, h;
        if (!ResolveRect(req, fbW, fbH, x, y, w, h))
        {
            // A docked panel whose tab is not selected is alive but not drawn.
            // Wait for it rather than drop the request on the first miss: a tab
            // switch part-way through a sequence would otherwise leave half an
            // animation and no done marker.
            if (++req.misses < kMaxResolveMisses)
            {
                ++i;
                continue;
            }
            Deki::LogSystem::LogWithLevel(Deki::LogLevel::Warning, __FILE__, __LINE__,
                                          "EditorCapture: panel '%s' was not drawn for %d frames, dropping request "
                                          "(is its tab selected? see focus_panel)",
                                          req.panel.c_str(), req.misses);
            Finish(req, req.panel.empty() ? std::string("the region is outside the window")
                                          : "panel '" + req.panel + "' was not drawn for " +
                                                std::to_string(req.misses) +
                                                " frames (is its tab selected? see focus_panel)");
            m_Requests.erase(m_Requests.begin() + i);
            continue;
        }
        req.misses = 0;

        if (!req.sequence)
        {
            const bool written = CaptureToFile(req.path, x, y, w, h, fbH);
            Finish(req, written ? std::string() : "could not write " + req.path);
            m_Requests.erase(m_Requests.begin() + i);
            continue;
        }

        // Sequence: only every `stride`-th frame contributes.
        if (req.strideCounter++ % req.stride != 0)
        {
            ++i;
            continue;
        }

        char name[64];
        std::snprintf(name, sizeof(name), "frame_%04d.png", req.frameIndex);
        CaptureToFile((fs::path(req.path) / name).string(), x, y, w, h, fbH);
        ++req.frameIndex;

        if (--req.framesRemaining <= 0)
        {
            std::error_code ec;
            fs::create_directories(req.path, ec);
            std::ofstream done((fs::path(req.path) / "done.txt").string());
            done << req.frameIndex << "\n";
            m_Requests.erase(m_Requests.begin() + i);
            continue;
        }

        ++i;
    }
}

}  // namespace DekiEditor
