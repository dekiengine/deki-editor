#include "FileDialogs.h"

#include "FileDialogFilter.h"

#include <deki/LogSystem.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#else
#include <SDL3/SDL.h>
#endif

namespace DekiEditor
{
namespace FileDialogs
{

namespace
{

SDL_Window* g_Window = nullptr;

// One dialog at a time. The flag is set before the dialog is raised and
// cleared only once its result has been handed to the call site, so a second
// request while one is up is dropped rather than queued.
std::atomic<bool> g_Pending{ false };

// A finished dialog parks its answer here. On Linux the SDL callback runs on
// whichever thread pumps SDL events -- the main thread today, but the mutex
// costs nothing and removes the assumption.
std::mutex g_ResultMutex;
ResultCallback g_Callback;
std::string g_ResultPath;
bool g_HaveResult = false;

void PostResult(std::string path)
{
    std::lock_guard<std::mutex> lock(g_ResultMutex);
    g_ResultPath = std::move(path);
    g_HaveResult = true;
}

// A save dialog that returns "level" when the filter said .scene should still
// write level.scene. The Win32 shell does this itself; the portable path does
// not, so both go through here.
std::string WithDefaultExtension(const std::string& path, const char* defaultExt)
{
    if (path.empty() || !defaultExt || defaultExt[0] == '\0')
        return path;
    if (std::filesystem::path(path).has_extension())
        return path;

    std::string ext = defaultExt;
    if (ext[0] == '.')
        ext.erase(0, 1);
    return path + "." + ext;
}

#ifdef _WIN32

// The wide (UTF-16) shell APIs are used so non-ASCII paths round-trip; the
// ANSI ones mangled anything outside the system code page.
std::wstring ToWide(const char* utf8, int len)
{
    if (!utf8 || len <= 0)
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, len, nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, utf8, len, &w[0], n);
    return w;
}

std::wstring ToWide(const char* utf8)
{
    return utf8 ? ToWide(utf8, static_cast<int>(strlen(utf8))) : std::wstring();
}

std::string ToUtf8(const wchar_t* wide)
{
    if (!wide || !wide[0])
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// A filter is a run of NUL-separated strings ended by a double NUL; the whole
// run, both terminators included, is converted so the embedded NULs survive.
std::wstring WideFilter(const char* filter)
{
    if (!filter || filter[0] == '\0')
        return {};
    int len = 0;
    while (!(filter[len] == '\0' && filter[len + 1] == '\0'))
        ++len;
    return ToWide(filter, len + 2);
}

#else

// The extension the pending save dialog should append. Held here because SDL
// carries only a void* through, and one dialog is live at a time. Null
// userdata means "no extension to add", which is every non-save dialog.
std::string g_PendingSaveExt;

// SDL hands back a null-terminated list of paths, or null on error. We only
// ever ask for one, so the first entry is the answer.
void SDLCALL DialogCallback(void* userdata, const char* const* filelist, int /*filter*/)
{
    std::string chosen;

    if (!filelist)
    {
        // SDL reports the reason; a missing desktop portal is the usual one
        // on Linux, and the user needs to be told rather than left looking at
        // a button that does nothing.
        const char* error = SDL_GetError();
        DEKI_LOG_ERROR("File dialog failed: %s", (error && error[0]) ? error : "unknown error");
    }
    else if (filelist[0])
    {
        chosen = filelist[0];
    }

    if (userdata)
        chosen = WithDefaultExtension(chosen, static_cast<const char*>(userdata));

    PostResult(std::move(chosen));
}

std::vector<SDL_DialogFileFilter> BuildSdlFilters(const std::vector<DialogFilter>& parsed)
{
    std::vector<SDL_DialogFileFilter> out;
    out.reserve(parsed.size());
    for (const DialogFilter& f : parsed)
        out.push_back(SDL_DialogFileFilter{ f.name.c_str(), f.pattern.c_str() });
    return out;
}

#endif

// Shared front half of every request: refuse a second dialog, remember the
// handler, and mark one in flight.
bool BeginRequest(ResultCallback onResult)
{
    if (!onResult)
        return false;

    bool expected = false;
    if (!g_Pending.compare_exchange_strong(expected, true))
    {
        DEKI_LOG_WARNING("A file dialog is already open; ignoring the second request");
        return false;
    }

    std::lock_guard<std::mutex> lock(g_ResultMutex);
    g_Callback = std::move(onResult);
    g_ResultPath.clear();
    g_HaveResult = false;
    return true;
}

}  // namespace

void SetWindow(SDL_Window* window)
{
    g_Window = window;
}

void OpenFolder(const char* title, ResultCallback onResult)
{
    if (!BeginRequest(std::move(onResult)))
        return;

#ifdef _WIN32
    const std::wstring wtitle = ToWide(title);
    wchar_t path[MAX_PATH] = { 0 };
    BROWSEINFOW bi = { 0 };
    bi.lpszTitle = wtitle.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    std::string chosen;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl != nullptr)
    {
        SHGetPathFromIDListW(pidl, path);
        CoTaskMemFree(pidl);
        chosen = ToUtf8(path);
    }
    PostResult(std::move(chosen));
#else
    (void)title;
    SDL_ShowOpenFolderDialog(DialogCallback, nullptr, g_Window, nullptr, false);
#endif
}

void OpenFile(const char* filter, const char* defaultPath, ResultCallback onResult)
{
    if (!BeginRequest(std::move(onResult)))
        return;

#ifdef _WIN32
    const std::wstring wfilter = WideFilter(filter);
    const std::wstring wdefault = ToWide(defaultPath);
    wchar_t filename[MAX_PATH] = { 0 };
    OPENFILENAMEW ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = wfilter.empty() ? nullptr : wfilter.c_str();
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!wdefault.empty())
        ofn.lpstrInitialDir = wdefault.c_str();

    PostResult(GetOpenFileNameW(&ofn) ? ToUtf8(filename) : std::string());
#else
    const std::vector<DialogFilter> parsed = ParseWin32Filter(filter);
    const std::vector<SDL_DialogFileFilter> sdlFilters = BuildSdlFilters(parsed);
    SDL_ShowOpenFileDialog(DialogCallback, nullptr, g_Window,
                           sdlFilters.empty() ? nullptr : sdlFilters.data(),
                           static_cast<int>(sdlFilters.size()), defaultPath, false);
#endif
}

void SaveFile(const char* filter, const char* defaultExt, const char* defaultPath,
              ResultCallback onResult)
{
    if (!BeginRequest(std::move(onResult)))
        return;

#ifdef _WIN32
    const std::wstring wfilter = WideFilter(filter);
    const std::wstring wext = ToWide(defaultExt);
    const std::wstring wdefault = ToWide(defaultPath);
    wchar_t filename[MAX_PATH] = { 0 };
    OPENFILENAMEW ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = wfilter.empty() ? nullptr : wfilter.c_str();
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = wext.empty() ? nullptr : wext.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (!wdefault.empty())
        ofn.lpstrInitialDir = wdefault.c_str();

    PostResult(GetSaveFileNameW(&ofn) ? ToUtf8(filename) : std::string());
#else
    const std::vector<DialogFilter> parsed = ParseWin32Filter(filter);
    const std::vector<SDL_DialogFileFilter> sdlFilters = BuildSdlFilters(parsed);
    g_PendingSaveExt = defaultExt ? defaultExt : "";
    SDL_ShowSaveFileDialog(DialogCallback,
                           g_PendingSaveExt.empty() ? nullptr : &g_PendingSaveExt[0],
                           g_Window,
                           sdlFilters.empty() ? nullptr : sdlFilters.data(),
                           static_cast<int>(sdlFilters.size()), defaultPath);
#endif
}

bool IsPending()
{
    return g_Pending.load();
}

void PumpResults()
{
    ResultCallback callback;
    std::string path;

    {
        std::lock_guard<std::mutex> lock(g_ResultMutex);
        if (!g_HaveResult)
            return;
        callback = std::move(g_Callback);
        path = std::move(g_ResultPath);
        g_Callback = nullptr;
        g_ResultPath.clear();
        g_HaveResult = false;
    }

    // Cleared before the handler runs, so a handler that opens another dialog
    // (Save As after a failed write) is not refused as a duplicate.
    g_Pending.store(false);

    if (callback)
        callback(path);
}

}  // namespace FileDialogs
}  // namespace DekiEditor
