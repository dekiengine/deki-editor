// EditorBoot - the editor's one startup path, shared by the GUI and the CLI.
//
// Before this existed, every CLI verb bootstrapped whatever slice of startup it
// happened to need: its own PackageManager, its own PackageLoader, its own
// cmake invocation. Each slice drifted from what EditorApp::OpenProject
// actually does, and the drift was invisible — a verb did not fail, it just
// operated on a project in a state the editor never leaves it in.
// --verify-scenes ended up shipping a staleness warning purely to describe one
// of those gaps.
//
// So there is one boot. BootOptions::headless decides whether a window is
// created; it does not decide what the editor does with the project.

#include "EditorBoot.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"
#include "misc/freetype/imgui_freetype.h"

#include "EditorApp.h"
#include <deki-editor/EditorTheme.h>
#include <deki-editor/EditorDLL.h>  // Deki::SetImGuiContext
#include <deki-editor/IconsTabler.h>
#include <deki-editor/Paths.h>
#include <deki-profiling-hooks/ProfilingHooks.h>
#include "services/ProfilingService.h"
#include "services/EditorSettings.h"
#include "services/FileDialogs.h"
#include "services/StartupProgress.h"
#include "services/HotReloadService.h"
#include <deki/LogSystem.h>

#include <stdio.h>
#include <string>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#include <windowsx.h>
#pragma comment(lib, "dwmapi.lib")
#endif

// Custom title bar state (updated each frame by EditorMenuBar)
float g_MenuBarHeight = 30.0f;
bool g_HoveringWindowControls = false;
float g_ControlsStartX = 0.0f;             // X where window control buttons begin
float g_ControlButtonW = 0.0f;             // Width of each button
float g_MenuItemsStartX = 0.0f;            // X where File/Edit/... begin; right of this is NEVER drag region
int g_NCHoverButton = 0;                   // 0=none, 1=min, 2=max, 3=close (set by WndProc)
static bool g_CloseButtonPressed = false;  // Track close button press for mouse-up handling

#ifdef _WIN32
static WNDPROC g_OriginalWndProc = nullptr;

// Which button region is the cursor in? 0=none, 1=min, 2=max, 3=close
static int GetTitleBarButton(HWND hwnd, int x, int y)
{
    if (y >= (int)g_MenuBarHeight || g_ControlButtonW <= 0.0f)
        return 0;
    float fx = (float)x;
    if (fx < g_ControlsStartX)
        return 0;
    float offset = fx - g_ControlsStartX;
    if (offset < g_ControlButtonW) return 1;         // minimize
    if (offset < g_ControlButtonW * 2.0f) return 2;  // maximize
    if (offset < g_ControlButtonW * 3.0f) return 3;  // close
    return 0;
}

static LRESULT CALLBACK CustomWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_NCCALCSIZE:
        {
            // Remove the title bar — client area fills entire window.
            // When maximized, clamp to the work area so we don't overlap the taskbar.
            if (wParam == TRUE)
            {
                NCCALCSIZE_PARAMS* params = (NCCALCSIZE_PARAMS*)lParam;
                HMONITOR monitor = MonitorFromRect(&params->rgrc[0], MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = { sizeof(mi) };
                GetMonitorInfo(monitor, &mi);

                // If the proposed rect extends beyond the work area, clamp it.
                // This handles maximize (where Windows overshoots to hide borders).
                if (params->rgrc[0].top < mi.rcWork.top)
                    params->rgrc[0].top = mi.rcWork.top;
                if (params->rgrc[0].bottom > mi.rcWork.bottom)
                    params->rgrc[0].bottom = mi.rcWork.bottom;
                if (params->rgrc[0].left < mi.rcWork.left)
                    params->rgrc[0].left = mi.rcWork.left;
                if (params->rgrc[0].right > mi.rcWork.right)
                    params->rgrc[0].right = mi.rcWork.right;

                return 0;
            }
            break;
        }
        case WM_GETMINMAXINFO:
        {
            // Constrain maximized window to the monitor's work area (excludes taskbar).
            HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) };
            GetMonitorInfo(monitor, &mi);
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
            mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
            mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
            mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
            return 0;
        }
        case WM_NCHITTEST:
        {
            RECT rc;
            GetWindowRect(hwnd, &rc);
            int x = GET_X_LPARAM(lParam) - rc.left;
            int y = GET_Y_LPARAM(lParam) - rc.top;
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;

            // When maximized, the window rect overshoots the visible area
            // (Windows hides borders behind screen edges). Remap coordinates
            // to the client area so they match ImGui's coordinate space.
            if (IsZoomed(hwnd))
            {
                POINT clientOrigin = { 0, 0 };
                ClientToScreen(hwnd, &clientOrigin);
                x -= (clientOrigin.x - rc.left);
                y -= (clientOrigin.y - rc.top);
                RECT clientRc;
                GetClientRect(hwnd, &clientRc);
                w = clientRc.right;
                h = clientRc.bottom;
            }

            // Caption buttons FIRST — they take priority over resize borders
            // so the close button reaches the top-right corner (Chromium behavior).
            int btn = GetTitleBarButton(hwnd, x, y);
            if (btn == 1) return HTMINBUTTON;
            if (btn == 2) return HTMAXBUTTON;
            if (btn == 3) return HTCLOSE;

            const int border = 6;

            // Resize borders (not when maximized)
            if (!IsZoomed(hwnd))
            {
                if (y < border)
                {
                    if (x < border) return HTTOPLEFT;
                    if (x > w - border) return HTTOPRIGHT;
                    return HTTOP;
                }
                if (y > h - border)
                {
                    if (x < border) return HTBOTTOMLEFT;
                    if (x > w - border) return HTBOTTOMRIGHT;
                    return HTBOTTOM;
                }
                if (x < border) return HTLEFT;
                if (x > w - border) return HTRIGHT;
            }

            // Title bar drag — only the "D DEKI/EDITOR" block on the left side of
            // the menu bar. The menu-items strip (File/Edit/...) and everything to
            // the right of it always reports HTCLIENT so ImGui receives mouse
            // moves and clicks — required for hover-based features like the item
            // picker, which would otherwise be starved of WM_MOUSEMOVE events
            // (HTCAPTION regions get WM_NCMOUSEMOVE instead).
            if (y < (int)g_MenuBarHeight && g_MenuItemsStartX > 0.0f && x < (int)g_MenuItemsStartX)
            {
                ImGuiContext* ctx = ImGui::GetCurrentContext();
                if (ctx && !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive())
                {
                    if (!ImGui::IsPopupOpen((ImGuiID)0, ImGuiPopupFlags_AnyPopup))
                        return HTCAPTION;
                }
            }
            return HTCLIENT;
        }
        case WM_NCLBUTTONDOWN:
        {
            // Handle caption button actions directly on mouse-down.
            // We can't defer to NCLBUTTONUP because returning 0 here prevents
            // DefWindowProc from setting up NC mouse tracking, so NCLBUTTONUP
            // never arrives.
            if (wParam == HTMINBUTTON)
            {
                ShowWindow(hwnd, SW_MINIMIZE);
                return 0;
            }
            if (wParam == HTMAXBUTTON)
            {
                ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
                return 0;
            }
            if (wParam == HTCLOSE)
            {
                g_CloseButtonPressed = true;
                SetCapture(hwnd);
                return 0;
            }
            break;
        }
        case WM_LBUTTONUP:
        {
            if (g_CloseButtonPressed && GetCapture() == hwnd)
            {
                ReleaseCapture();
                g_CloseButtonPressed = false;
                PostMessage(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            break;
        }
        case WM_NCMOUSEMOVE:
        {
            if (wParam == HTMINBUTTON)
                g_NCHoverButton = 1;
            else if (wParam == HTMAXBUTTON)
                g_NCHoverButton = 2;
            else if (wParam == HTCLOSE)
                g_NCHoverButton = 3;
            else
                g_NCHoverButton = 0;
            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE | TME_NONCLIENT;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_NCMOUSELEAVE:
            g_NCHoverButton = 0;
            return 0;
        case WM_NCACTIVATE:
            // Prevent Windows from painting the default activation border
            // on our custom-framed borderless window. Returning TRUE tells
            // Windows we handled the activation; passing -1 as lParam skips
            // the non-client repaint.
            return TRUE;
    }
    return CallWindowProc(g_OriginalWndProc, hwnd, msg, wParam, lParam);
}
#endif

namespace DekiEditor
{

namespace
{

/// Window, GL context, ImGui context, theme and fonts. GUI only — a headless
/// boot never calls this, which is the whole point: no display is required.
bool BootDisplay(BootContext& ctx)
{
    // Initialize SDL (SDL3 is always DPI-aware)
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        printf("Error: SDL_Init(): %s\n", SDL_GetError());
        return false;
    }

    // GL 3.3 + GLSL 330
    const char* glsl_version = "#version 330";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);

    // Create window with graphics context
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_WindowFlags window_flags =
        SDL_WINDOW_OPENGL |
        SDL_WINDOW_RESIZABLE |
        SDL_WINDOW_BORDERLESS;

    ctx.window = SDL_CreateWindow(
        "Deki Editor",
        1600, 900,
        window_flags);

    if (ctx.window == nullptr)
    {
        printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        return false;
    }

#ifdef _WIN32
    // Custom title bar: subclass WndProc for native window controls (snap layouts, etc.)
    {
        HWND hwnd = (HWND)SDL_GetPointerProperty(
            SDL_GetWindowProperties(ctx.window),
            SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        if (hwnd)
        {
            g_OriginalWndProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)CustomWndProc);

            // Keep thick frame for resize + snap, remove caption for title bar
            LONG style = GetWindowLong(hwnd, GWL_STYLE);
            style |= WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX;
            SetWindowLong(hwnd, GWL_STYLE, style);

            // DWM shadow on borderless window
            MARGINS margins = { 1, 1, 1, 1 };
            DwmExtendFrameIntoClientArea(hwnd, &margins);

            SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                         SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

            // Maximize now that our WndProc handles WM_NCCALCSIZE/WM_GETMINMAXINFO
            ShowWindow(hwnd, SW_MAXIMIZE);
        }
    }
#else
    // Non-Windows: use SDL3 hit test for borderless window drag/resize
    SDL_SetWindowHitTest(ctx.window, [](SDL_Window* win, const SDL_Point* area, void*) -> SDL_HitTestResult
                         {
        int w, h;
        SDL_GetWindowSize(win, &w, &h);
        int x = area->x, y = area->y;
        const int border = 6;

        if (!(SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED))
        {
            if (y < border) {
                if (x < border) return SDL_HITTEST_RESIZE_TOPLEFT;
                if (x > w - border) return SDL_HITTEST_RESIZE_TOPRIGHT;
                return SDL_HITTEST_RESIZE_TOP;
            }
            if (y > h - border) {
                if (x < border) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
                if (x > w - border) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
                return SDL_HITTEST_RESIZE_BOTTOM;
            }
            if (x < border) return SDL_HITTEST_RESIZE_LEFT;
            if (x > w - border) return SDL_HITTEST_RESIZE_RIGHT;
        }

        if (y < (int)g_MenuBarHeight && !g_HoveringWindowControls) {
            ImGuiContext* ctx = ImGui::GetCurrentContext();
            if (ctx && !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive())
                if (!ImGui::IsPopupOpen((ImGuiID)0, ImGuiPopupFlags_AnyPopup))
                    return SDL_HITTEST_DRAGGABLE;
        }
        return SDL_HITTEST_NORMAL; }, nullptr);
#endif

    ctx.glContext = SDL_GL_CreateContext(ctx.window);
    SDL_GL_MakeCurrent(ctx.window, ctx.glContext);
    SDL_GL_SetSwapInterval(1);  // Enable vsync for smooth frame pacing

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    // deki-editor.dll links its own copy of ImGui, so its GImGui stays null
    // until we hand it the exe's context. EditorTheme, EditorUI and every other
    // shared helper lives in that DLL, so this has to happen before the first
    // call into it (SetupEditorTheme below), not later at ToolHost init.
    // Allocators first: they must be in place before the DLL allocates anything.
    {
        ImGuiMemAllocFunc allocFn = nullptr;
        ImGuiMemFreeFunc freeFn = nullptr;
        void* allocUserData = nullptr;
        ImGui::GetAllocatorFunctions(&allocFn, &freeFn, &allocUserData);
        Deki::SetImGuiAllocators(allocFn, freeFn, allocUserData);
    }
    Deki::SetImGuiContext(ImGui::GetCurrentContext());

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = DekiEditor::EditorSettings::GetImGuiIniPath();

    // ImGui 1.92+ font auto-DPI. With this, fonts are loaded at base sizes
    // (no `px * dpiScale` baking) and ImGui scales them via style.FontScaleDpi
    // which the backend updates as monitor DPI changes. Sizes/padding still
    // need style.ScaleAllSizes() at setup — FAQ note: "scales fonts but NOT
    // sizes/padding for now." See FAQ "How should I handle DPI in my application?".
    io.ConfigDpiScaleFonts = true;

    // FreeType font loader is enabled at build time (IMGUI_ENABLE_FREETYPE in
    // CMakeLists.txt). Switch the hinting strategy to LightHinting — FreeType
    // snaps glyphs only on Y, leaving horizontal spacing unsnapped. This is
    // the same algorithm Microsoft's ClearType uses, which is why Inter looks
    // fuller in Chrome than under FreeType's default native hinter. Closes
    // most of the perceived "size" gap with the browser without changing the
    // logical font size.
    io.Fonts->FontLoaderFlags |= ImGuiFreeTypeLoaderFlags_LightHinting;

    // Without this, ImGui's ErrorRecoveryTryToRecoverState fires IM_ASSERT
    // when SafeDispatch unwinds a panel that faulted mid-BeginChild. With a
    // debugger that's a pause; without one (or in release) it aborts the
    // process. The recovery itself (the EndChild/End calls that follow the
    // assert) still runs — we just don't want the assert to be fatal. The
    // error is still surfaced via the callback wired below.
    io.ConfigErrorRecoveryEnableAssert = false;

    // Route ImGui's recovery messages ("Missing EndChild()" etc.) into the
    // editor log so the Console panel shows what was recovered, even when
    // no debugger is attached. Set via internal API because g.ErrorCallback
    // is on ImGuiContext, not ImGuiIO.
    {
        auto* ctx = ImGui::GetCurrentContext();
        ctx->ErrorCallback = [](ImGuiContext*, void*, const char* msg)
        {
            DEKI_LOG_ERROR("[imgui] recovered: %s", msg ? msg : "(null)");
        };
        ctx->ErrorCallbackUserData = nullptr;
    }
    // io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // Multi-viewport (optional)

    // Determine DPI scale using SDL_GetWindowDisplayScale
    ctx.dpiScale = SDL_GetWindowDisplayScale(ctx.window);
    if (ctx.dpiScale <= 0.0f) ctx.dpiScale = 1.0f;

    // Setup Dear ImGui style — Godot 4.6 Modern theme
    DekiEditor::SetupEditorTheme(ctx.dpiScale);

    // Setup Platform/Renderer backends
    ImGui_ImplSDL3_InitForOpenGL(ctx.window, ctx.glContext);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // We handle DPI manually (fonts at 16*dpiScale, ScaleAllSizes(dpiScale)).
    // Disable ImGui 1.92's per-viewport FramebufferScale font scaling to prevent double DPI.
    ImGui::GetPlatformIO().Platform_GetWindowFramebufferScale = nullptr;

    // Schematic theme fonts: Inter (UI body / tabs / labels) + JetBrains Mono
    // (values / code / wordmark) + Space Grotesk Medium (display headings + logo).
    // Tabler Icons merged into the body sizes. No fallback by design: if any TTF
    // is missing, abort with a clear error.
    {
        const char* basePath = SDL_GetBasePath();
        std::string fontDir = (std::filesystem::path(basePath ? basePath : ".") / "resources" / "fonts").string();

        std::string interReg = (std::filesystem::path(fontDir) / "Inter-Regular.ttf").string();
        std::string interMed = (std::filesystem::path(fontDir) / "Inter-Medium.ttf").string();
        std::string interSemi = (std::filesystem::path(fontDir) / "Inter-SemiBold.ttf").string();
        std::string monoReg = (std::filesystem::path(fontDir) / "JetBrainsMono-Regular.ttf").string();
        std::string monoMed = (std::filesystem::path(fontDir) / "JetBrainsMono-Medium.ttf").string();
        std::string displayPath = (std::filesystem::path(fontDir) / "SpaceGrotesk-Medium.ttf").string();
        std::string iconsPath = (std::filesystem::path(fontDir) / FONT_ICON_FILE_NAME_TI).string();

        for (const auto& p : { interReg, interMed, interSemi, monoReg, monoMed, displayPath, iconsPath })
        {
            if (!std::filesystem::exists(p))
            {
                DEKI_LOG_ERROR("[fonts] required font missing: %s", p.c_str());
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Deki Editor",
                                         ("Required font file not found:\n" + p +
                                          "\n\nRebuild the project so resources/fonts/ is copied next to the executable.")
                                             .c_str(),
                                         ctx.window);
                return false;
            }
        }

        ImFontConfig baseCfg;
        baseCfg.PixelSnapH = true;

        // Tabler codepoint range capped at 0xFFFF so we stay within ImWchar (uint16).
        // Icons in our target mockup (player-play, folder, search, etc.) are all
        // in the 0xEA02 - 0xF34F range.
        static const ImWchar iconRanges[] = { ICON_MIN_TI, 0xFFFF, 0 };

        // The text fonts (Inter, JetBrains Mono, …) carry a handful of stray
        // Private-Use-Area glyph mappings that overlap the Tabler icon range —
        // e.g. Inter maps U+EE94 to a Greek-accent glyph. In MergeMode the base
        // font wins, which silently shadows those icons (this is why hand-finger
        // rendered blank while hand-finger-off was fine). Exclude the icon range
        // from every text font so the merged Tabler font owns it.
        baseCfg.GlyphExcludeRanges = iconRanges;

        auto mergeIcons = [&](float size)
        {
            ImFontConfig iconCfg;
            iconCfg.MergeMode = true;
            iconCfg.PixelSnapH = true;
            iconCfg.GlyphMinAdvanceX = size;
            iconCfg.GlyphOffset.y = 1.0f;
            io.Fonts->AddFontFromFileTTF(iconsPath.c_str(), size, &iconCfg, iconRanges);
        };

        // Load fonts at BASE sizes (logical pixels). ImGui 1.92's
        // ConfigDpiScaleFonts scales them at draw time via style.FontScaleDpi.
        // loadAndRegister: adds the font via ImGui and registers its TTF with
        // Fonts::RegisterTtf so Fonts::CssPx can later convert CSS sizes for
        // it. Use this instead of the raw AddFontFromFileTTF path.
        auto loadAndRegister = [&](const std::string& path, float px) -> ImFont*
        {
            ImFont* f = io.Fonts->AddFontFromFileTTF(path.c_str(), px, &baseCfg);
            DekiEditor::Fonts::RegisterTtf(f, path.c_str());
            return f;
        };

        // Default body font is Inter Regular 13px with icons merged.
        // Loaded FIRST so io.FontDefault points to it.
        DekiEditor::g_FontUI = loadAndRegister(interReg, 13.0f);
        mergeIcons(13.0f);
        io.FontDefault = DekiEditor::g_FontUI;

        // Inter Regular 11.5px (with icons merged so chips/status keep icons inline).
        DekiEditor::g_FontUISmall = loadAndRegister(interReg, 11.5f);
        mergeIcons(11.5f);

        // Inter Medium 12.5px — inspector property labels, outlined action
        // buttons. Icons merged: a label like ICON_TI_PLUS "  Add Component"
        // rendered its glyph as nothing at all without them.
        DekiEditor::g_FontUIMed = loadAndRegister(interMed, 12.5f);
        mergeIcons(12.5f);

        // Inter SemiBold 11px — tab labels, section headers (rendered uppercased).
        DekiEditor::g_FontUIBold = loadAndRegister(interSemi, 11.0f);
        mergeIcons(11.0f);

        // JetBrains Mono Regular 12px — value cells, code.
        DekiEditor::g_FontMono = loadAndRegister(monoReg, 12.0f);

        // JetBrains Mono Regular 11.5px — log timestamps, commit hash, stats.
        DekiEditor::g_FontMonoSmall = loadAndRegister(monoReg, 11.5f);

        // JetBrains Mono Medium 12px — menubar DEKI/EDITOR wordmark.
        DekiEditor::g_FontMonoBold = loadAndRegister(monoMed, 12.0f);

        // Space Grotesk Medium 20px — inspector selection banner.
        DekiEditor::g_FontDisplay = loadAndRegister(displayPath, 20.0f);

        // Space Grotesk Medium 12px — section-header alt usage.
        DekiEditor::g_FontDisplaySmall = loadAndRegister(displayPath, 12.0f);

        // Space Grotesk Medium 13px — the "D" letter in the menubar logo box.
        DekiEditor::g_FontLogo = loadAndRegister(displayPath, 13.0f);
    }

    // Now that g_FontUI is registered with its ratio, set the CSS-equivalent
    // FontSizeBase. This is the size the default font renders at when no
    // PushFont is active; Fonts::CssPx applies Inter's REAL_DIM correction.
    ImGui::GetStyle().FontSizeBase = DekiEditor::Fonts::CssPx(DekiEditor::g_FontUI, 13.0f);

    return true;
}

}  // namespace

bool BootPlatform(const BootOptions& opts, BootContext& ctx)
{
    ctx.headless = opts.headless;

    if (!opts.headless)
    {
        if (!BootDisplay(ctx))
            return false;
    }
    else
    {
        // SDL still comes up headless: the editor installs an SDL-backed time
        // provider (Deki::Time) while opening a project, and ProgressModal pumps
        // SDL events. Video is the part a CLI run must not require — a build
        // machine has no display and SDL_INIT_VIDEO fails outright there.
        if (!SDL_Init(SDL_INIT_EVENTS))
        {
            printf("Error: SDL_Init(): %s\n", SDL_GetError());
            return false;
        }

        // No ImGui context is created, and nothing below needs one. The two
        // places the open path hands one over take null: PackageLoader skips
        // its cross-DLL context hand-off, and ProgressModal returns before
        // touching ImGui because no window is set. Anything that actually draws
        // only runs from the interactive loop, which a headless boot never
        // enters.
    }

    // Same call either way. With a null window ProgressModal::Show returns
    // immediately, so every StartupProgress::Update on the open path is a
    // no-op headless rather than a check at each of its ~10 call sites.
    StartupProgress::SetWindow(ctx.window);
    StartupProgress::Update("Initializing...");

    // Parents the file dialogs. Null headless, which is what the portable
    // backend expects when there is no window to sit in front of.
    FileDialogs::SetWindow(ctx.window);

    ProfilingService::Get().Initialize();

    // Deep profiling callbacks (for plugin /Gh /GH instrumentation)
    Profiling_SetCallbacks(
        &ProfilingService::OnProfileEnter,
        &ProfilingService::OnProfileExit);

    return true;
}

bool BootEditor(const BootOptions& opts, BootContext& ctx, EditorApp& editor)
{
    editor.SetHeadless(ctx.headless);
    editor.SetDpiScale(ctx.dpiScale);
    editor.SetWindow(ctx.window);

    // With no path the GUI comes up on the project manager screen rather than
    // reopening whatever was last open. A headless boot always has one — the
    // CLI rejects a verb without a project path before it gets here.
    bool opened = true;
    if (opts.projectPath)
    {
        StartupProgress::Update("Loading project...");
        opened = editor.OpenProject(opts.projectPath);
        if (!opened)
        {
            DEKI_LOG_ERROR("[boot] could not open project '%s'", opts.projectPath);
            printf("Error: could not open project '%s'\n", opts.projectPath);
            fflush(stdout);
        }
    }

    // Bring the MCP server up at startup instead of requiring the Activity
    // panel toggle, so external tooling can attach to a freshly launched
    // editor (see .mcp.json — SSE on http://localhost:3000/sse).
    if (opts.startMcp)
    {
        editor.StartMcpServer(opts.mcpPort);
        // The URL carries a per-session token; the server binds loopback only
        // and rejects requests without it, so this line is what a client needs.
        printf("MCP server listening on %s\n", editor.GetMcpSseUrl().c_str());
        fflush(stdout);
    }

    // OpenProject defers a package check and a build check to
    // EditorApp::ProcessDeferredTasks, which only the interactive loop calls.
    // Headless there is no loop, so run the package half here in the order the
    // loop would — this is the same work the GUI does on its first frames, not
    // an extra step. The build half is driven below for both modes.
    if (ctx.headless)
    {
        if (auto* hr = editor.GetHotReloadService(); hr && hr->HasPendingPackageCheck())
            hr->ProcessPendingPackageCheck();
    }

    // Drive the deferred plugin build to completion while the startup modal is
    // still visible. Otherwise the editor renders for ~3 frames between asset
    // import and the build modal, which the user perceives as a flash.
    //
    // Headless this is not cosmetic but required: it is the step that produces
    // and promotes the plugin DLL a CLI verb is about to read.
    if (auto* hr = editor.GetHotReloadService(); hr && hr->HasPendingBuild())
    {
        StartupProgress::Update("Building plugin...");
        hr->RunPendingBuildNow();
    }

    // Anything still building (a package rebuild the package check queued)
    // finishes under the startup modal too. Headless there is no frame loop
    // to pump it, and the verbs read the built libraries next.
    if (auto* hr = editor.GetHotReloadService())
        hr->WaitForBuild();

    // Startup complete - stop showing progress screen
    StartupProgress::Complete();

    return opened;
}

void ShutdownPlatform(BootContext& ctx)
{
    ProfilingService::Get().Shutdown();

    if (!ctx.headless)
    {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();

        SDL_GL_DestroyContext(ctx.glContext);
        SDL_DestroyWindow(ctx.window);
    }

    SDL_Quit();
}

}  // namespace DekiEditor
