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
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
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
#include "render/EditorRenderer.h"
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

// The CJK fallback fonts, behind every editor font: Noto Sans JP, SC and KR.
// Each file is read once and shared by every font that merges it.
struct CjkFont
{
    const char* language;  // the editor language whose shapes it draws: "ja", "zh", "ko"
    const char* regular;   // file names in resources/fonts
    const char* medium;
    std::vector<char> regularData, mediumData;
};
CjkFont s_CjkFonts[] = {
    { "ja", "NotoSansJP-Regular.otf", "NotoSansJP-Medium.otf", {}, {} },
    { "zh", "NotoSansSC-Regular.otf", "NotoSansSC-Medium.otf", {}, {} },
    { "ko", "NotoSansKR-Regular.otf", "NotoSansKR-Medium.otf", {}, {} },
};

// What a CJK source leaves to the fonts after it. All skip the icons, which
// are in the Private Use Area only, so full-width punctuation (U+FF00 on)
// still draws. The slots after the icon pair are the characters several of
// these fonts draw, each in its own language's shapes: switched on for every
// font but the current language's, so that one draws them. An "off" slot is an
// empty range. ImGui copies the list when the font is added, and
// SetCjkPreference writes the copies.
constexpr ImWchar kRangeOff[2] = { 0xFFFF, 0xFFFE };
constexpr ImWchar kSharedCjk[][2] = {
    { 0x3000, 0x303F },  // CJK symbols and punctuation
    { 0x3400, 0x4DBF },  // CJK unified ideographs extension A
    { 0x4E00, 0x9FFF },  // CJK unified ideographs
    { 0xF900, 0xFAFF },  // CJK compatibility ideographs
    { 0xFF00, 0xFFEF },  // half-width and full-width forms
};
// The icon pairs (see kIconRanges) first, then one slot per kSharedCjk range.
constexpr int kCjkSlotStart = 4;
const ImWchar kCjkExclude[] = { ICON_MIN_TI, 0xF8FF, 0xFA17, 0xFA17, 0xFFFF, 0xFFFE, 0xFFFF, 0xFFFE,
                                0xFFFF,      0xFFFE, 0xFFFF, 0xFFFE, 0xFFFF, 0xFFFE, 0 };

}  // namespace

void SetCjkPreference(const std::string& language)
{
    if (!ImGui::GetCurrentContext())
    {
        return;
    }
    // English and other languages keep the first font's shapes (Japanese).
    const bool known = language == "ja" || language == "zh" || language == "ko";
    const std::string preferred = known ? language : "ja";
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    for (ImFontConfig& src : atlas->Sources)
    {
        for (const CjkFont& cjk : s_CjkFonts)
        {
            if (src.FontData != cjk.regularData.data() && src.FontData != cjk.mediumData.data())
            {
                continue;
            }
            // ImGui's own copy of kCjkExclude: the shared slots follow the icon pairs.
            const bool stepAside = preferred != cjk.language;
            auto* ranges = const_cast<ImWchar*>(src.GlyphExcludeRanges);
            for (size_t i = 0; i < std::size(kSharedCjk); ++i)
            {
                ranges[kCjkSlotStart + i * 2] = stepAside ? kSharedCjk[i][0] : kRangeOff[0];
                ranges[kCjkSlotStart + 1 + i * 2] = stepAside ? kSharedCjk[i][1] : kRangeOff[1];
            }
        }
    }
    // Glyphs already drawn came from another font.
    for (ImFont* font : atlas->Fonts)
    {
        ImFontAtlasFontDiscardBakes(atlas, font, 0);
    }
}

namespace
{

/// The graphics device did not start: says why in an error box and the log.
/// Lists the step and SDL's error, and the adapter behind each display.
void ReportGpuFailure(SDL_Window* window, const char* step)
{
    const char* sdlError = SDL_GetError();
    std::string text = std::string("Failed at: ") + step + "\n";
    text += std::string("SDL error: ") + (sdlError && *sdlError ? sdlError : "(none)") + "\n";

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

    text = "Deki Editor could not start the graphics device it draws with: Direct3D 12 on Windows, Vulkan on "
           "Linux, Metal on macOS.\n\nUpdate the graphics driver. On Linux, install your GPU's Vulkan driver "
           "(Mesa's, or the vendor's). Over Remote Desktop or in a virtual machine there may be none.\n\n" +
           text;

    // Editor.log is not open yet this early in boot, so the report also goes
    // to a file next to the executable.
    const char* basePath = SDL_GetBasePath();
    const std::string reportPath = std::string(basePath ? basePath : "") + "deki_gpu_report.txt";
    text += "\nThis report is also saved to " + reportPath;
    if (FILE* f = fopen(reportPath.c_str(), "w"))
    {
        fputs(text.c_str(), f);
        fclose(f);
    }

    DEKI_LOG_ERROR("[gpu] %s", text.c_str());
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

    SDL_WindowFlags windowFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS;

    // Started minimized (`--background`, or on Windows `start /min`, a
    // shortcut set to "Run: Minimized", Start-Process -WindowStyle
    // Minimized): come up minimized and inactive, and restore to maximized
    // when clicked, so the maximize below does not take the foreground from
    // whatever the user is doing.
    bool startInBackground = ctx.background;
#ifdef _WIN32
    {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        GetStartupInfoW(&si);
        if (si.dwFlags & STARTF_USESHOWWINDOW)
        {
            const WORD show = si.wShowWindow;
            startInBackground = startInBackground || show == SW_SHOWMINNOACTIVE || show == SW_MINIMIZE ||
                                show == SW_SHOWMINIMIZED || show == SW_FORCEMINIMIZE;
        }
    }
#endif
    if (startInBackground)
    {
        // Hidden until the GPU device and ImGui are set up (below), then shown
        // minimized. Not created minimized: its swapchain would be made for a
        // window with no size.
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        windowFlags |= SDL_WINDOW_HIDDEN;
    }

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
            // later, once the GPU device is up: SW_MAXIMIZE would activate it.
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

    std::string failedStep;
    if (!EditorRenderer::Attach(ctx.window, failedStep))
    {
        ReportGpuFailure(ctx.window, failedStep.c_str());
        return false;
    }

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

    // Greyscale text: subpixel glyphs need a blend the GPU API's ImGui backend
    // does not have yet.
    ImGuiFreeType::DekiSetLcdMode(0);

    if (!EditorRenderer::InitImGui(ctx.window))
    {
        ReportGpuFailure(ctx.window, "ImGui_ImplSDLGPU3_Init");
        return false;
    }

    // Started in the background: show the hidden window now, minimized and
    // without activating it (the hint above). Through SDL, so SDL knows the
    // window is shown: shown behind its back, SDL restored and activated it.
    if (startInBackground)
    {
        SDL_MinimizeWindow(ctx.window);
        SDL_ShowWindow(ctx.window);
#ifdef _WIN32
        // Restore to maximized when the user clicks it. The window is already
        // minimized, so this changes nothing on screen.
        HWND hwnd =
            (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(ctx.window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        if (hwnd && IsIconic(hwnd))
        {
            WINDOWPLACEMENT wp{};
            wp.length = sizeof(wp);
            GetWindowPlacement(hwnd, &wp);
            wp.flags |= WPF_RESTORETOMAXIMIZED;
            wp.showCmd = SW_SHOWMINNOACTIVE;
            SetWindowPlacement(hwnd, &wp);
        }
#endif
    }

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
        std::vector<std::string> required = { interReg, interMed, interSemi, monoReg, monoMed, displayPath, iconsPath };
        for (const CjkFont& cjk : s_CjkFonts)
        {
            required.push_back((std::filesystem::path(fontDir) / cjk.regular).string());
            required.push_back((std::filesystem::path(fontDir) / cjk.medium).string());
        }

        for (const auto& p : required)
        {
            if (!std::filesystem::exists(p))
            {
                DEKI_LOG_ERROR("[fonts] required font missing: %s", p.c_str());
                SDL_ShowSimpleMessageBox(
                    SDL_MESSAGEBOX_ERROR, "Deki Editor",
                    (DekiEditor::Tr("Required font file not found:") + ("\n" + p + "\n\n") +
                     DekiEditor::Tr("Rebuild the project so resources/fonts/ is copied next to the executable."))
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

        // The icons the editor uses: the Private Use Area from Tabler's first
        // icon, and database-plus (U+FA17). Tabler also puts icons over the
        // CJK compatibility ideographs and full-width forms (up to U+10095),
        // which Chinese and Japanese text needs as characters, so the rest of
        // its range is left out. kCjkExclude keeps the same pairs first.
        static const ImWchar kIconRanges[] = { ICON_MIN_TI, 0xF8FF, 0xFA17, 0xFA17, 0 };

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

        // ImGui 1.92 loads a glyph from the first source that has it, and
        // the ranges passed to AddFont only steer the old preload. So the
        // icon font is kept to the icons by excluding everything else; it
        // would otherwise answer for full-width punctuation with icons.
        static const ImWchar kNotIcons[] = { 0x0001, ICON_MIN_TI - 1, 0xF900, 0xFA16, 0xFA18, 0xFFFF, 0 };

        auto mergeIcons = [&](float size)
        {
            ImFontConfig iconCfg;
            iconCfg.GlyphExcludeRanges = kNotIcons;
            iconCfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;
            iconCfg.MergeMode = true;
            iconCfg.PixelSnapH = true;
            iconCfg.GlyphMinAdvanceX = size;
            iconCfg.GlyphOffset.y = 1.0f;
            io.Fonts->AddFontFromFileTTF(iconsPath.c_str(), size, &iconCfg, kIconRanges);
        };

        // Japanese, Chinese and Korean text: whatever glyph the font in front
        // lacks comes from Noto Sans JP, SC or KR, Medium behind the heavier
        // fonts. Glyphs are only drawn when first shown. The three draw some
        // of the same characters differently; SetCjkPreference picks whose
        // shapes win.
        auto readFile = [](const std::string& path, std::vector<char>& out)
        {
            std::ifstream file(path, std::ios::binary);
            out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        };
        for (CjkFont& cjk : s_CjkFonts)
        {
            readFile((std::filesystem::path(fontDir) / cjk.regular).string(), cjk.regularData);
            readFile((std::filesystem::path(fontDir) / cjk.medium).string(), cjk.mediumData);
        }
        auto mergeCjk = [&](float size, bool medium)
        {
            ImFontConfig cfg;
            cfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;
            cfg.MergeMode = true;
            cfg.PixelSnapH = true;
            cfg.FontDataOwnedByAtlas = false;
            cfg.GlyphExcludeRanges = kCjkExclude;
            for (CjkFont& cjk : s_CjkFonts)
            {
                std::vector<char>& data = medium ? cjk.mediumData : cjk.regularData;
                io.Fonts->AddFontFromMemoryTTF(data.data(), static_cast<int>(data.size()), size, &cfg);
            }
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
        mergeCjk(13.0f, false);
        io.FontDefault = DekiEditor::g_FontUI;

        // Inter Regular 11.5px (with icons merged so chips/status keep icons inline).
        DekiEditor::g_FontUISmall = loadAndRegister(interReg, 11.5f);
        mergeIcons(11.5f);
        mergeCjk(11.5f, false);

        // Inter Medium 12.5px - inspector property labels, outlined action
        // buttons. Icons merged, for labels like ICON_TI_PLUS "  Add Component".
        DekiEditor::g_FontUIMed = loadAndRegister(interMed, 12.5f);
        mergeIcons(12.5f);
        mergeCjk(12.5f, true);

        // Inter SemiBold 11px - tab labels, section headers (rendered uppercased).
        DekiEditor::g_FontUIBold = loadAndRegister(interSemi, 11.0f);
        mergeIcons(11.0f);
        mergeCjk(11.0f, true);

        // JetBrains Mono Regular 12px - value cells, code.
        DekiEditor::g_FontMono = loadAndRegister(monoReg, 12.0f);
        mergeCjk(12.0f, false);

        // JetBrains Mono Regular 11.5px - log timestamps, commit hash, stats.
        DekiEditor::g_FontMonoSmall = loadAndRegister(monoReg, 11.5f);
        mergeCjk(11.5f, false);

        // JetBrains Mono Medium 12px - menubar DEKI/EDITOR wordmark.
        DekiEditor::g_FontMonoBold = loadAndRegister(monoMed, 12.0f);
        mergeCjk(12.0f, true);

        // Space Grotesk Medium 20px - inspector selection banner.
        DekiEditor::g_FontDisplay = loadAndRegister(displayPath, 20.0f);
        mergeCjk(20.0f, true);

        // Space Grotesk Medium 12px - section-header alt usage.
        DekiEditor::g_FontDisplaySmall = loadAndRegister(displayPath, 12.0f);
        mergeCjk(12.0f, true);

        // Space Grotesk Medium 13px - the "D" letter in the menubar logo box.
        DekiEditor::g_FontLogo = loadAndRegister(displayPath, 13.0f);
    }

    SetCjkPreference(Localization::Current());

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
    ctx.background = opts.background;

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
    StartupProgress::Update(DekiEditor::TrMark("Initializing..."));

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
        StartupProgress::Update(DekiEditor::TrMark("Loading project..."));
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
        EditorRenderer::Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyWindow(ctx.window);
    }

    SDL_Quit();
}

}  // namespace DekiEditor
