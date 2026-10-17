// EditorBoot: the editor's one startup path, shared by the GUI and the CLI.
//
// CLI verbs must not set up their own slice of startup (their own
// PackageManager, PackageLoader or cmake run): such a slice drifts from what
// EditorApp::OpenProject does, and the verb then works on a project in a
// state the editor never leaves it in, without failing.
//
// BootOptions::headless decides whether a window is created; it does not
// change what the editor does with the project.

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
#include <deki-editor/EditorSettings.h>
#include <deki-editor/Localization.h>
#include "services/FileDialogs.h"
#include "widgets/PopupMotion.h"
#include "widgets/PressMotion.h"
#include "widgets/ScrollMotion.h"
#include "widgets/WindowMotion.h"
#include "services/StartupProgress.h"
#include "services/TouchInput.h"
#include "services/HotReloadService.h"
#include <deki/LogSystem.h>

#include <stdio.h>
#include <string>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#include <windowsx.h>
#pragma comment(lib, "dwmapi.lib")
#endif

// Custom title bar state, updated each frame by EditorMenuBar.
float g_MenuBarHeight = 30.0f;
bool g_HoveringWindowControls = false;
float g_ControlsStartX = 0.0f;             // x where the window control buttons begin
float g_ControlButtonW = 0.0f;             // width of each button
float g_MenuItemsStartX = 0.0f;            // x where File/Edit/... begin; right of it is never a drag region
int g_NCHoverButton = 0;                   // 0=none, 1=min, 2=max, 3=close (set by WndProc)
static bool g_CloseButtonPressed = false;  // a close press, handled on mouse-up

#ifdef _WIN32
static WNDPROC g_OriginalWndProc = nullptr;

// The button region under the cursor: 0=none, 1=min, 2=max, 3=close.
static int GetTitleBarButton(HWND hwnd, int x, int y)
{
    if (y >= (int)g_MenuBarHeight || g_ControlButtonW <= 0.0f)
    {
        return 0;
    }
    float fx = (float)x;
    if (fx < g_ControlsStartX)
    {
        return 0;
    }
    float offset = fx - g_ControlsStartX;
    if (offset < g_ControlButtonW)
    {
        return 1;  // minimize
    }
    if (offset < g_ControlButtonW * 2.0f)
    {
        return 2;  // maximize
    }
    if (offset < g_ControlButtonW * 3.0f)
    {
        return 3;  // close
    }
    return 0;
}

static LRESULT CALLBACK CustomWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_NCCALCSIZE:
        {
            // No title bar: the client area fills the window. When maximized,
            // clamp to the work area so the window does not cover the taskbar.
            if (wParam == TRUE)
            {
                NCCALCSIZE_PARAMS* params = (NCCALCSIZE_PARAMS*)lParam;
                HMONITOR monitor = MonitorFromRect(&params->rgrc[0], MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = { sizeof(mi) };
                GetMonitorInfo(monitor, &mi);

                // Clamp a rect that extends past the work area. Maximize does
                // this: Windows overshoots to hide the borders.
                if (params->rgrc[0].top < mi.rcWork.top)
                {
                    params->rgrc[0].top = mi.rcWork.top;
                }
                if (params->rgrc[0].bottom > mi.rcWork.bottom)
                {
                    params->rgrc[0].bottom = mi.rcWork.bottom;
                }
                if (params->rgrc[0].left < mi.rcWork.left)
                {
                    params->rgrc[0].left = mi.rcWork.left;
                }
                if (params->rgrc[0].right > mi.rcWork.right)
                {
                    params->rgrc[0].right = mi.rcWork.right;
                }

                return 0;
            }
            break;
        }
        case WM_GETMINMAXINFO:
        {
            // A maximized window fills the monitor's work area (without the taskbar).
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

            // Caption buttons first: they win over the resize borders, so the
            // close button reaches the top-right corner (as in Chromium).
            int btn = GetTitleBarButton(hwnd, x, y);
            if (btn == 1)
            {
                return HTMINBUTTON;
            }
            if (btn == 2)
            {
                return HTMAXBUTTON;
            }
            if (btn == 3)
            {
                return HTCLOSE;
            }

            const int border = 6;

            // Resize borders (not when maximized).
            if (!IsZoomed(hwnd))
            {
                if (y < border)
                {
                    if (x < border)
                    {
                        return HTTOPLEFT;
                    }
                    if (x > w - border)
                    {
                        return HTTOPRIGHT;
                    }
                    return HTTOP;
                }
                if (y > h - border)
                {
                    if (x < border)
                    {
                        return HTBOTTOMLEFT;
                    }
                    if (x > w - border)
                    {
                        return HTBOTTOMRIGHT;
                    }
                    return HTBOTTOM;
                }
                if (x < border)
                {
                    return HTLEFT;
                }
                if (x > w - border)
                {
                    return HTRIGHT;
                }
            }

            // Title bar drag: only the cat and DEKI/EDITOR block on the left of the
            // menu bar. The menu items (File/Edit/...) and everything right of
            // them report HTCLIENT, so ImGui gets mouse moves and clicks there.
            // Hover features like the item picker need that: HTCAPTION regions
            // get WM_NCMOUSEMOVE instead of WM_MOUSEMOVE.
            if (y < (int)g_MenuBarHeight && g_MenuItemsStartX > 0.0f && x < (int)g_MenuItemsStartX)
            {
                ImGuiContext* ctx = ImGui::GetCurrentContext();
                if (ctx && !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive())
                {
                    if (!ImGui::IsPopupOpen((ImGuiID)0, ImGuiPopupFlags_AnyPopup))
                    {
                        return HTCAPTION;
                    }
                }
            }
            return HTCLIENT;
        }
        case WM_NCLBUTTONDOWN:
        {
            // Caption buttons act on mouse-down. Returning 0 here stops
            // DefWindowProc from setting up non-client mouse tracking, so
            // NCLBUTTONUP never arrives.
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
            {
                g_NCHoverButton = 1;
            }
            else if (wParam == HTMAXBUTTON)
            {
                g_NCHoverButton = 2;
            }
            else if (wParam == HTCLOSE)
            {
                g_NCHoverButton = 3;
            }
            else
            {
                g_NCHoverButton = 0;
            }
            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE | TME_NONCLIENT;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_NCMOUSELEAVE: g_NCHoverButton = 0; return 0;
        case WM_NCACTIVATE:
            // Keep Windows from painting its activation border on this
            // borderless window. Returning TRUE says the activation was
            // handled; -1 as lParam skips the non-client repaint.
            return TRUE;
    }
    return CallWindowProc(g_OriginalWndProc, hwnd, msg, wParam, lParam);
}
#endif

namespace DekiEditor
{

namespace
{

/// GL_VENDOR / GL_RENDERER / GL_VERSION of the current context.
std::string CurrentGlStrings()
{
    auto str = [](GLenum name)
    {
        const GLubyte* s = glGetString(name);
        return std::string(s ? reinterpret_cast<const char*>(s) : "?");
    };
    return str(GL_VENDOR) + " / " + str(GL_RENDERER) + " / " + str(GL_VERSION);
}

/// OpenGL did not start: says why in an error box and the log. Without this
/// the first frame dereferences the GL backend's null data and the editor
/// crashes with nothing to go on. Lists the step and SDL's error, the adapter
/// behind each display, and what a plain (version-less) context gives here:
/// Microsoft's "GDI Generic" means Windows found no GPU OpenGL driver for the
/// display.
void ReportGlFailure(SDL_Window* window, const char* step)
{
    const char* sdlError = SDL_GetError();
    std::string text = std::string("Failed at: ") + step + "\n";
    text += std::string("SDL error: ") + (sdlError && *sdlError ? sdlError : "(none)") + "\n";

    // Windows' own OpenGL 1.1 software renderer: SDL accepts it as a context,
    // and the GL backend then refuses it.
    bool softwareGl = false;
    if (SDL_GL_GetCurrentContext())
    {
        const std::string strings = CurrentGlStrings();
        softwareGl = strings.find("GDI Generic") != std::string::npos;
        text += "Editor context: " + strings + "\n";
    }

    if (SDL_DisplayID display = SDL_GetDisplayForWindow(window))
    {
        const char* name = SDL_GetDisplayName(display);
        text += std::string("Window on display: ") + (name ? name : "?") + "\n";
    }

#ifdef _WIN32
    std::string windowDevice;
    if (HWND hwnd =
            (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL))
    {
        MONITORINFOEXA mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
        {
            windowDevice = mi.szDevice;
        }
    }
    text += "Displays:\n";
    DISPLAY_DEVICEA dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &dd, 0); ++i)
    {
        if (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)
        {
            text += std::string("  ") + dd.DeviceName + "  " + dd.DeviceString;
            if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)
            {
                text += "  [primary]";
            }
            if (windowDevice == dd.DeviceName)
            {
                text += "  [editor window]";
            }
            text += "\n";
        }
        dd = {};
        dd.cb = sizeof(dd);
    }
#endif

    // On a second, hidden window: a window's pixel format is set once, and the
    // editor window already has one.
    SDL_GL_ResetAttributes();
    if (SDL_Window* probe = SDL_CreateWindow("Deki GL probe", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN))
    {
        if (SDL_GLContext gl = SDL_GL_CreateContext(probe))
        {
            const std::string strings = CurrentGlStrings();
            softwareGl = softwareGl || strings.find("GDI Generic") != std::string::npos;
            text += "Plain OpenGL context: " + strings + "\n";
            SDL_GL_DestroyContext(gl);
        }
        else
        {
            text += std::string("Plain OpenGL context: none (") + SDL_GetError() + ")\n";
        }
        SDL_DestroyWindow(probe);
    }

    std::string advice = "Deki Editor could not start OpenGL 3.3, which it needs to draw.\n\n";
    if (softwareGl)
    {
        advice += "Windows found no OpenGL driver for the graphics card and fell back to its own OpenGL 1.1 "
                  "software renderer (\"GDI Generic\"). Other OpenGL programs, such as Blender, fail the same "
                  "way. Reinstall the graphics driver (AMD: the Adrenalin installer's Factory Reset option); "
                  "DirectX and Vulkan games can keep working while its OpenGL part is broken.\n\n";
    }
    else
    {
        advice += "Update the graphics driver. Over Remote Desktop, in a virtual machine or on a virtual "
                  "display, OpenGL 3.3 may not be available at all.\n\n";
    }
    text = advice + text;

    // Editor.log is not open yet this early in boot, so the report also goes
    // to a file next to the executable.
    const char* basePath = SDL_GetBasePath();
    const std::string reportPath = std::string(basePath ? basePath : "") + "deki_gl_report.txt";
    text += "\nThis report is also saved to " + reportPath;
    if (FILE* f = fopen(reportPath.c_str(), "w"))
    {
        fputs(text.c_str(), f);
        fclose(f);
    }

    DEKI_LOG_ERROR("[gl] %s", text.c_str());
    printf("Error: %s\n", text.c_str());
    fflush(stdout);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Deki Editor", text.c_str(), window);
}

/// Window, GL context, ImGui context, theme and fonts. GUI only: a headless
/// boot never calls this, so it needs no display.
bool BootDisplay(BootContext& ctx)
{
    // The editor reads touches itself (TouchInput); SDL's emulation would
    // press the left button the instant a finger lands.
    DekiEditor::TouchInput::DisableSdlTouchMouse();

    // SDL3 is always DPI-aware.
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        printf("Error: SDL_Init(): %s\n", SDL_GetError());
        return false;
    }

    // GL 3.3 + GLSL 330
    const char* glslVersion = "#version 330";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_WindowFlags windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS;

#ifdef _WIN32
    // Started minimized (`start /min`, a shortcut set to "Run: Minimized",
    // Start-Process -WindowStyle Minimized): come up minimized and inactive,
    // and restore to maximized when clicked, so the maximize below does not
    // take the foreground from whatever the user is doing.
    bool startInBackground = false;
    {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        GetStartupInfoW(&si);
        if (si.dwFlags & STARTF_USESHOWWINDOW)
        {
            const WORD show = si.wShowWindow;
            startInBackground = show == SW_SHOWMINNOACTIVE || show == SW_MINIMIZE || show == SW_SHOWMINIMIZED ||
                                show == SW_FORCEMINIMIZE;
        }
    }
    if (startInBackground)
    {
        // Hidden until GL and ImGui are set up (below), then shown minimized.
        // Do not create it minimized: it would have no size when its GL
        // context is made, and ImGui's GL backend would fail to load.
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        windowFlags |= SDL_WINDOW_HIDDEN;
    }
#endif

    ctx.window = SDL_CreateWindow("Deki Editor", 1600, 900, windowFlags);

    if (ctx.window == nullptr)
    {
        printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        return false;
    }
    DekiEditor::TouchInput::Get().SetWindow(ctx.window);

#ifdef _WIN32
    // Custom title bar: subclass WndProc to keep the native window controls
    // (snap layouts, etc.).
    {
        HWND hwnd =
            (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(ctx.window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        if (hwnd)
        {
            g_OriginalWndProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)CustomWndProc);

            // Keep the thick frame for resizing and snapping; drop the caption.
            LONG style = GetWindowLong(hwnd, GWL_STYLE);
            style |= WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX;
            SetWindowLong(hwnd, GWL_STYLE, style);

            // DWM shadow on the borderless window.
            MARGINS margins = { 1, 1, 1, 1 };
            DwmExtendFrameIntoClientArea(hwnd, &margins);

            SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                         SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

            // Maximize now that the WndProc handles WM_NCCALCSIZE and
            // WM_GETMINMAXINFO. Started in the background, the window is shown
            // later, once GL is up: SW_MAXIMIZE would activate it.
            if (!startInBackground)
            {
                ShowWindow(hwnd, SW_MAXIMIZE);
            }
        }
    }
#else
    // Elsewhere: SDL3's hit test drags and resizes the borderless window.
    SDL_SetWindowHitTest(
        ctx.window,
        [](SDL_Window* win, const SDL_Point* area, void*) -> SDL_HitTestResult
        {
            int w, h;
            SDL_GetWindowSize(win, &w, &h);
            int x = area->x, y = area->y;
            const int border = 6;

            if (!(SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED))
            {
                if (y < border)
                {
                    if (x < border)
                    {
                        return SDL_HITTEST_RESIZE_TOPLEFT;
                    }
                    if (x > w - border)
                    {
                        return SDL_HITTEST_RESIZE_TOPRIGHT;
                    }
                    return SDL_HITTEST_RESIZE_TOP;
                }
                if (y > h - border)
                {
                    if (x < border)
                    {
                        return SDL_HITTEST_RESIZE_BOTTOMLEFT;
                    }
                    if (x > w - border)
                    {
                        return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
                    }
                    return SDL_HITTEST_RESIZE_BOTTOM;
                }
                if (x < border)
                {
                    return SDL_HITTEST_RESIZE_LEFT;
                }
                if (x > w - border)
                {
                    return SDL_HITTEST_RESIZE_RIGHT;
                }
            }

            if (y < (int)g_MenuBarHeight && !g_HoveringWindowControls)
            {
                ImGuiContext* ctx = ImGui::GetCurrentContext();
                if (ctx && !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive())
                {
                    if (!ImGui::IsPopupOpen((ImGuiID)0, ImGuiPopupFlags_AnyPopup))
                    {
                        return SDL_HITTEST_DRAGGABLE;
                    }
                }
            }
            return SDL_HITTEST_NORMAL;
        },
        nullptr);
#endif

    ctx.glContext = SDL_GL_CreateContext(ctx.window);
    if (!ctx.glContext)
    {
        ReportGlFailure(ctx.window, "SDL_GL_CreateContext");
        return false;
    }
    if (!SDL_GL_MakeCurrent(ctx.window, ctx.glContext))
    {
        ReportGlFailure(ctx.window, "SDL_GL_MakeCurrent");
        return false;
    }
    DEKI_LOG_EDITOR("OpenGL: %s", CurrentGlStrings().c_str());
    SDL_GL_SetSwapInterval(1);  // vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    // deki-editor.dll links its own copy of ImGui, so its GImGui stays null
    // until it gets the exe's context. EditorTheme, EditorUI and the other
    // shared helpers live in that DLL, so this must happen before the first
    // call into it (SetupEditorTheme below). Allocators first: they must be in
    // place before the DLL allocates anything.
    {
        ImGuiMemAllocFunc allocFn = nullptr;
        ImGuiMemFreeFunc freeFn = nullptr;
        void* allocUserData = nullptr;
        ImGui::GetAllocatorFunctions(&allocFn, &freeFn, &allocUserData);
        Deki::SetImGuiAllocators(allocFn, freeFn, allocUserData);
    }
    Deki::SetImGuiContext(ImGui::GetCurrentContext());

    // Popups, menus and dialogs animate in and out (widgets/PopupMotion.h),
    // and wheel scrolling glides (widgets/ScrollMotion.h).
    DekiEditor::PopupMotion::Install(ImGui::GetCurrentContext());
    DekiEditor::ScrollMotion::Install(ImGui::GetCurrentContext());
    DekiEditor::WindowMotion::Install(ImGui::GetCurrentContext());
    DekiEditor::PressMotion::Install(ImGui::GetCurrentContext());

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = DekiEditor::EditorSettings::GetImGuiIniPath();

    // ImGui 1.92+ font auto-DPI: fonts are loaded at base sizes and ImGui
    // scales them by style.FontScaleDpi, which the backend updates when the
    // monitor DPI changes. Sizes and padding still need style.ScaleAllSizes()
    // at setup (ImGui FAQ, "How should I handle DPI in my application?").
    io.ConfigDpiScaleFonts = true;

    // FreeType font loader is enabled at build time (IMGUI_ENABLE_FREETYPE in
    // CMakeLists.txt). Hinting is chosen per font where the fonts are loaded:
    // LightHinting for the proportional fonts, full autohinting for the mono.

    // Otherwise ImGui's ErrorRecoveryTryToRecoverState asserts when
    // SafeDispatch unwinds a panel that faulted inside BeginChild, which
    // aborts the process without a debugger. The recovery (the EndChild/End
    // calls after the assert) still runs, and the error is reported through
    // the callback set below.
    io.ConfigErrorRecoveryEnableAssert = false;

    // ImGui's recovery messages ("Missing EndChild()" etc.) go to the editor
    // log, so the Console panel shows what was recovered. Set through the
    // internal API because g.ErrorCallback is on ImGuiContext, not ImGuiIO.
    {
        auto* ctx = ImGui::GetCurrentContext();
        ctx->ErrorCallback = [](ImGuiContext*, void*, const char* msg)
        { DEKI_LOG_ERROR("[imgui] recovered: %s", msg ? msg : "(null)"); };
        ctx->ErrorCallbackUserData = nullptr;
    }
    // io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // Multi-viewport (optional)

    ctx.dpiScale = SDL_GetWindowDisplayScale(ctx.window);
    if (ctx.dpiScale <= 0.0f)
    {
        ctx.dpiScale = 1.0f;
    }

    DekiEditor::SetupEditorTheme(ctx.dpiScale);

    // The language text is shown in, and the editor's translations.
    DekiEditor::Localization::Load();

    // Text antialiasing follows the system, like every other Windows app:
    // subpixel (ClearType) when the desktop uses it, in the panel's stripe
    // order, greyscale otherwise. DEKI_TEXT_AA=gray|rgb|bgr overrides it
    // (Linux has no system setting to read, so it defaults to greyscale).
    int lcdMode = 0;
#ifdef _WIN32
    {
        BOOL smoothing = FALSE;
        UINT type = 0, orientation = FE_FONTSMOOTHINGORIENTATIONRGB;
        if (SystemParametersInfoW(SPI_GETFONTSMOOTHING, 0, &smoothing, 0) && smoothing &&
            SystemParametersInfoW(SPI_GETFONTSMOOTHINGTYPE, 0, &type, 0) && type == FE_FONTSMOOTHINGCLEARTYPE)
        {
            SystemParametersInfoW(SPI_GETFONTSMOOTHINGORIENTATION, 0, &orientation, 0);
            lcdMode = (orientation == FE_FONTSMOOTHINGORIENTATIONBGR) ? 2 : 1;
        }
    }
#endif
    // Edit > Editor Settings > Text can pin it; the variable still wins.
    auto modeFor = [](const std::string& v) { return (v == "rgb") ? 1 : (v == "bgr") ? 2 : 0; };
    if (const std::string pinned = EditorSettings::GetString("editor.textSmoothing", "auto"); pinned != "auto")
    {
        lcdMode = modeFor(pinned);
    }
    if (const char* aa = getenv("DEKI_TEXT_AA"))
    {
        lcdMode = modeFor(aa);
    }
    ImGuiFreeType::DekiSetLcdMode(lcdMode);
    // Coverage exponents for black and for white text: light text on the dark
    // theme is thickened a little, as DirectWrite and Chromium do.
    ImGui_ImplOpenGL3_DekiSetTextGamma(1.0f, 0.8f);
    DEKI_LOG_EDITOR("Text antialiasing: %s", lcdMode == 1   ? "subpixel RGB"
                                             : lcdMode == 2 ? "subpixel BGR"
                                                            : "greyscale");

    ImGui_ImplSDL3_InitForOpenGL(ctx.window, ctx.glContext);
    if (!ImGui_ImplOpenGL3_Init(glslVersion))
    {
        ReportGlFailure(ctx.window, "ImGui_ImplOpenGL3_Init");
        return false;
    }

#ifdef _WIN32
    // Started in the background: show the hidden window now, minimized and
    // inactive, restoring to maximized when the user clicks it.
    if (startInBackground)
    {
        HWND hwnd =
            (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(ctx.window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        if (hwnd)
        {
            WINDOWPLACEMENT wp{};
            wp.length = sizeof(wp);
            GetWindowPlacement(hwnd, &wp);
            wp.flags |= WPF_RESTORETOMAXIMIZED;
            wp.showCmd = SW_SHOWMINNOACTIVE;
            SetWindowPlacement(hwnd, &wp);
        }
    }
#endif

    // DPI is handled by style.FontScaleDpi and ScaleAllSizes(dpiScale). Turn
    // off ImGui 1.92's per-viewport FramebufferScale font scaling, or DPI
    // would apply twice.
    ImGui::GetPlatformIO().Platform_GetWindowFramebufferScale = nullptr;

    // Theme fonts: Inter (UI body, tabs, labels), JetBrains Mono (values,
    // code, wordmark) and Space Grotesk Medium (display headings, logo), with
    // Tabler Icons merged into the body sizes and Noto Sans JP behind every
    // one, for Japanese text. There is no fallback: if a font file is
    // missing, startup stops with a clear error.
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
        std::string japaneseReg = (std::filesystem::path(fontDir) / "NotoSansJP-Regular.otf").string();
        std::string japaneseMed = (std::filesystem::path(fontDir) / "NotoSansJP-Medium.otf").string();

        for (const auto& p :
             { interReg, interMed, interSemi, monoReg, monoMed, displayPath, iconsPath, japaneseReg, japaneseMed })
        {
            if (!std::filesystem::exists(p))
            {
                DEKI_LOG_ERROR("[fonts] required font missing: %s", p.c_str());
                SDL_ShowSimpleMessageBox(
                    SDL_MESSAGEBOX_ERROR, "Deki Editor",
                    ("Required font file not found:\n" + p +
                     "\n\nRebuild the project so resources/fonts/ is copied next to the executable.")
                        .c_str(),
                    ctx.window);
                return false;
            }
        }

        ImFontConfig baseCfg;
        baseCfg.PixelSnapH = true;
        // LightHinting: FreeType snaps glyphs only on Y and leaves horizontal
        // spacing alone, as ClearType does. Inter then looks as full as in
        // Chrome, without changing the logical font size.
        baseCfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;

        // The Tabler range is capped at 0xFFFF to fit ImWchar (uint16). The
        // icons the editor uses (player-play, folder, search...) are all in
        // 0xEA02 - 0xF34F.
        static const ImWchar kIconRanges[] = { ICON_MIN_TI, 0xFFFF, 0 };

        // The text fonts (Inter, JetBrains Mono, ...) map a few Private Use
        // Area codepoints that overlap the Tabler range (Inter maps U+EE94 to
        // a Greek accent). In MergeMode the base font wins and hides those
        // icons, so every text font excludes the icon range.
        baseCfg.GlyphExcludeRanges = kIconRanges;

        // With greyscale text, JetBrains Mono gets the autohinter in normal
        // mode, which snaps stems on X too; under LightHinting its stems
        // straddle two pixels and look soft. Subpixel text has three times the
        // horizontal resolution, so it keeps LightHinting, like DirectWrite.
        ImFontConfig monoCfg = baseCfg;
        if (ImGuiFreeType::DekiGetLcdMode() == 0)
        {
            monoCfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_ForceAutoHint;
        }

        auto mergeIcons = [&](float size)
        {
            ImFontConfig iconCfg;
            iconCfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;
            iconCfg.MergeMode = true;
            iconCfg.PixelSnapH = true;
            iconCfg.GlyphMinAdvanceX = size;
            iconCfg.GlyphOffset.y = 1.0f;
            io.Fonts->AddFontFromFileTTF(iconsPath.c_str(), size, &iconCfg, kIconRanges);
        };

        // Japanese text: whatever glyph the font in front lacks comes from Noto
        // Sans JP, Medium behind the heavier fonts. Glyphs are only drawn when
        // first shown, and each file is read once and shared by every font.
        static std::vector<char> s_JapaneseReg, s_JapaneseMed;
        auto readFile = [](const std::string& path, std::vector<char>& out)
        {
            std::ifstream file(path, std::ios::binary);
            out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        };
        readFile(japaneseReg, s_JapaneseReg);
        readFile(japaneseMed, s_JapaneseMed);
        auto mergeJapanese = [&](float size, bool medium)
        {
            std::vector<char>& data = medium ? s_JapaneseMed : s_JapaneseReg;
            ImFontConfig japaneseCfg;
            japaneseCfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;
            japaneseCfg.MergeMode = true;
            japaneseCfg.PixelSnapH = true;
            japaneseCfg.FontDataOwnedByAtlas = false;
            japaneseCfg.GlyphExcludeRanges = kIconRanges;
            io.Fonts->AddFontFromMemoryTTF(data.data(), static_cast<int>(data.size()), size, &japaneseCfg);
        };

        // Fonts load at base sizes (logical pixels); ImGui 1.92's
        // ConfigDpiScaleFonts scales them at draw time by style.FontScaleDpi.
        // loadAndRegister adds the font to ImGui and registers its TTF with
        // Fonts::RegisterTtf, so Fonts::CssPx can convert CSS sizes for it.
        // Use it instead of AddFontFromFileTTF.
        auto loadAndRegister = [&](const std::string& path, float px) -> ImFont*
        {
            const bool mono = (path == monoReg || path == monoMed);
            ImFont* f = io.Fonts->AddFontFromFileTTF(path.c_str(), px, mono ? &monoCfg : &baseCfg);
            DekiEditor::Fonts::RegisterTtf(f, path.c_str());
            return f;
        };

        // The default body font, Inter Regular 13px with icons merged. Loaded
        // first, so io.FontDefault points to it.
        DekiEditor::g_FontUI = loadAndRegister(interReg, 13.0f);
        mergeIcons(13.0f);
        mergeJapanese(13.0f, false);
        io.FontDefault = DekiEditor::g_FontUI;

        // Inter Regular 11.5px (with icons merged so chips/status keep icons inline).
        DekiEditor::g_FontUISmall = loadAndRegister(interReg, 11.5f);
        mergeIcons(11.5f);
        mergeJapanese(11.5f, false);

        // Inter Medium 12.5px - inspector property labels, outlined action
        // buttons. Icons merged, for labels like ICON_TI_PLUS "  Add Component".
        DekiEditor::g_FontUIMed = loadAndRegister(interMed, 12.5f);
        mergeIcons(12.5f);
        mergeJapanese(12.5f, true);

        // Inter SemiBold 11px - tab labels, section headers (rendered uppercased).
        DekiEditor::g_FontUIBold = loadAndRegister(interSemi, 11.0f);
        mergeIcons(11.0f);
        mergeJapanese(11.0f, true);

        // JetBrains Mono Regular 12px - value cells, code.
        DekiEditor::g_FontMono = loadAndRegister(monoReg, 12.0f);
        mergeJapanese(12.0f, false);

        // JetBrains Mono Regular 11.5px - log timestamps, commit hash, stats.
        DekiEditor::g_FontMonoSmall = loadAndRegister(monoReg, 11.5f);
        mergeJapanese(11.5f, false);

        // JetBrains Mono Medium 12px - menubar DEKI/EDITOR wordmark.
        DekiEditor::g_FontMonoBold = loadAndRegister(monoMed, 12.0f);
        mergeJapanese(12.0f, true);

        // Space Grotesk Medium 20px - inspector selection banner.
        DekiEditor::g_FontDisplay = loadAndRegister(displayPath, 20.0f);
        mergeJapanese(20.0f, true);

        // Space Grotesk Medium 12px - section-header alt usage.
        DekiEditor::g_FontDisplaySmall = loadAndRegister(displayPath, 12.0f);
        mergeJapanese(12.0f, true);

        // Space Grotesk Medium 13px - the "D" letter in the menubar logo box.
        DekiEditor::g_FontLogo = loadAndRegister(displayPath, 13.0f);
    }

    // Now that g_FontUI is registered with its ratio, set the CSS-equivalent
    // FontSizeBase: the size of the default font when no PushFont is active.
    // Fonts::CssPx applies Inter's REAL_DIM correction.
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
        {
            return false;
        }
    }
    else
    {
        // SDL still starts headless: the editor installs an SDL-backed time
        // provider (Deki::Time) while opening a project, and ProgressModal
        // pumps SDL events. Video is left out: a build machine has no display,
        // and SDL_INIT_VIDEO fails there.
        if (!SDL_Init(SDL_INIT_EVENTS))
        {
            printf("Error: SDL_Init(): %s\n", SDL_GetError());
            return false;
        }

        // No ImGui context is created, and nothing below needs one. The two
        // places the open path passes one on accept null: PackageLoader skips
        // its cross-DLL context hand-off, and ProgressModal returns before
        // touching ImGui because no window is set. Drawing happens only in the
        // interactive loop, which a headless boot never enters.
    }

    // The same call either way. With a null window ProgressModal::Show
    // returns at once, so each StartupProgress::Update on the open path does
    // nothing headless, with no check at its ~10 call sites.
    StartupProgress::SetWindow(ctx.window);
    StartupProgress::Update("Initializing...");

    // Parents the file dialogs. Null headless, which is what the portable
    // backend expects when there is no window to sit in front of.
    FileDialogs::SetWindow(ctx.window);

    ProfilingService::Get().Initialize();

    // Deep profiling callbacks (for plugin /Gh /GH instrumentation).
    ProfilingSetCallbacks(&ProfilingService::OnProfileEnter, &ProfilingService::OnProfileExit);

    return true;
}

bool BootEditor(const BootOptions& opts, BootContext& ctx, EditorApp& editor)
{
    editor.SetHeadless(ctx.headless);
    editor.SetDpiScale(ctx.dpiScale);
    editor.SetWindow(ctx.window);

    // With no path the GUI opens on the project manager screen instead of
    // reopening the last project. A headless boot always has a path: the CLI
    // rejects a verb without one before this point.
    bool opened = true;
    if (opts.projectPath)
    {
        StartupProgress::Update("Loading project...");
        opened = editor.OpenProject(opts.projectPath);
        if (!opened)
        {
            DEKI_LOG_ERROR("[boot] could not open project '%s'", opts.projectPath);
            printf("Error: could not open project '%s'. %s\n", opts.projectPath, editor.GetOpenProjectError().c_str());
            fflush(stdout);
        }
    }

    // Start the MCP server at startup, without the Activity panel toggle, so
    // external tools can attach to a freshly launched editor (SSE on
    // http://localhost:3000/sse; SETUP.md section 8).
    if (opts.startMcp)
    {
        // The URL carries a per-session token; the server binds loopback only
        // and rejects requests without it, so this line is what a client needs.
        if (editor.StartMcpServer(opts.mcpPort))
        {
            printf("MCP server listening on %s\n", editor.GetMcpSseUrl().c_str());
        }
        else
        {
            printf("Error: MCP server could not listen on port %d (in use?)\n", opts.mcpPort);
        }
        fflush(stdout);
    }
    else if (!opts.headless)
    {
        // Every windowed editor can be driven from the command line: any free
        // port, published per user for `--tool/--script <project>` to find
        // (services/LiveSession.h), never printed.
        if (!editor.StartMcpServer(0))
        {
            DEKI_LOG_WARNING("[boot] live session server did not start; the command line "
                             "cannot reach this editor");
        }
    }

    // OpenProject defers a package check and a build check to
    // EditorApp::ProcessDeferredTasks, which only the interactive loop calls.
    // Headless there is no loop, so run the package check here, in the order
    // the loop would: the same work the GUI does on its first frames. The
    // build check runs below for both modes.
    if (ctx.headless)
    {
        if (auto* hr = editor.GetHotReloadService(); hr && hr->HasPendingPackageCheck())
        {
            hr->ProcessPendingPackageCheck();
        }
    }

    // Finish the deferred plugin build while the startup modal is still up;
    // otherwise the editor renders ~3 frames between asset import and the
    // build modal, which looks like a flash.
    //
    // Headless this is required: it produces and promotes the plugin DLL a
    // CLI verb is about to read.
    //
    // Anything else still building (a package rebuild the package check
    // queued) finishes here too: headless there is no frame loop to drive it,
    // and the verbs read the built libraries next.
    editor.FinishPendingBuilds();

    // Startup is done: hide the progress screen.
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
