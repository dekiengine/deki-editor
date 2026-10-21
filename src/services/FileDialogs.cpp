#include "FileDialogs.h"

#include "FileDialogFilter.h"

#include <deki/LogSystem.h>
#include <deki-editor/Localization.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

#include <SDL3/SDL.h>

namespace DekiEditor
{
namespace FileDialogs
{

namespace
{

SDL_Window* g_Window = nullptr;

// One dialog at a time, tracked by id rather than a flag. The platform's
// dialog helper can die without answering (kill it on Linux and SDL never
// calls back), and a flag would then never clear. With an id, a newer request
// replaces the older one, so the next attempt always works, and a late answer
// to a replaced request is dropped instead of reaching a call site that has
// moved on.
//
// 0 means nothing is in flight.
std::atomic<uint64_t> g_ActiveId{ 0 };
uint64_t g_NextId = 1;

// A finished dialog leaves its answer here. On Linux the SDL callback runs on
// whichever thread pumps SDL events. That is the main thread, but the mutex
// costs nothing and does not depend on it.
std::mutex g_ResultMutex;
ResultCallback g_Callback;
std::string g_ResultPath;
bool g_HaveResult = false;

// The extension the in-flight save dialog should append, if it is a save.
std::string g_PendingSaveExt;

void PostResult(uint64_t id, std::string path)
{
    std::lock_guard<std::mutex> lock(g_ResultMutex);
    if (id != g_ActiveId.load())
    {
        return;  // superseded, or already delivered
    }
    g_ResultPath = std::move(path);
    g_HaveResult = true;
}

// A save dialog that returns "level" when the filter said .scene should still
// write level.scene. The Win32 shell does this itself; the portable path does
// not, so both go through here.
std::string WithDefaultExtension(const std::string& path, const char* defaultExt)
{
    if (path.empty() || !defaultExt || defaultExt[0] == '\0')
    {
        return path;
    }
    if (std::filesystem::path(path).has_extension())
    {
        return path;
    }

    std::string ext = defaultExt;
    if (ext[0] == '.')
    {
        ext.erase(0, 1);
    }
    return path + "." + ext;
}

// The file type names the editor's callers pass, marked so the translation
// table has them: a filter string cannot be marked where it is written.
[[maybe_unused]] constexpr const char* kFilterNames[] = { TrMark("Scene Files") };

// The filter with each file type's name in the editor's language. The
// description is translated and the patterns kept: "Scene Files (*.scene)"
// uses the entry for "Scene Files".
std::string TranslateFilter(const char* filter)
{
    std::string out;
    if (!filter)
    {
        return out;
    }
    const char* p = filter;
    while (*p)
    {
        const std::string name = p;
        p += name.size() + 1;
        const std::string spec = p;
        p += spec.empty() ? 0 : spec.size() + 1;

        const size_t paren = name.find(" (");
        out += Tr(name.substr(0, paren).c_str());
        if (paren != std::string::npos)
        {
            out += name.substr(paren);
        }
        out += '\0';
        out += spec;
        out += '\0';
    }
    if (!out.empty())
    {
        out += '\0';  // the list ends with a double NUL; c_str() adds the second
    }
    return out;
}

#ifdef _WIN32

// Uses the wide (UTF-16) shell APIs so non-ASCII paths survive; the ANSI ones
// mangle anything outside the system code page.
std::wstring ToWide(const char* utf8, int len)
{
    if (!utf8 || len <= 0)
    {
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, len, nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    if (n > 0)
    {
        MultiByteToWideChar(CP_UTF8, 0, utf8, len, &w[0], n);
    }
    return w;
}

std::wstring ToWide(const char* utf8)
{
    return utf8 ? ToWide(utf8, static_cast<int>(strlen(utf8))) : std::wstring();
}

std::string ToUtf8(const wchar_t* wide)
{
    if (!wide || !wide[0])
    {
        return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
    {
        return {};
    }
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// The dialog's owner, so it is modal to the editor and opens over it rather
// than wherever the shell last put one.
HWND OwnerWindow()
{
    if (!g_Window)
    {
        return nullptr;
    }
    return static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(g_Window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}

// The Explorer-style folder picker (IFileOpenDialog with FOS_PICKFOLDERS):
// address bar, search, Quick Access - rather than SHBrowseForFolder's tree.
// Returns "" on cancel.
std::string PickFolderWin32(const char* title, const char* defaultPath)
{
    // The UI thread may already be in an apartment (SDL, drag and drop);
    // only undo an initialisation this call made.
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninit = SUCCEEDED(coInit);

    std::string chosen;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                   reinterpret_cast<void**>(&dialog))))
    {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);

        const std::wstring wtitle = ToWide(title);
        if (!wtitle.empty())
        {
            dialog->SetTitle(wtitle.c_str());
        }

        const std::wstring wdefault = ToWide(defaultPath);
        if (!wdefault.empty())
        {
            IShellItem* folder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(wdefault.c_str(), nullptr, IID_IShellItem,
                                                      reinterpret_cast<void**>(&folder))))
            {
                dialog->SetFolder(folder);
                folder->Release();
            }
        }

        if (SUCCEEDED(dialog->Show(OwnerWindow())))
        {
            IShellItem* result = nullptr;
            if (SUCCEEDED(dialog->GetResult(&result)))
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                {
                    chosen = ToUtf8(path);
                    CoTaskMemFree(path);
                }
                result->Release();
            }
        }
        dialog->Release();
    }

    if (uninit)
    {
        CoUninitialize();
    }
    return chosen;
}

// A filter is a run of NUL-separated strings ended by a double NUL; the whole
// run, both terminators included, is converted so the embedded NULs survive.
std::wstring WideFilter(const char* filter)
{
    if (!filter || filter[0] == '\0')
    {
        return {};
    }
    int len = 0;
    while (!(filter[len] == '\0' && filter[len + 1] == '\0'))
    {
        ++len;
    }
    return ToWide(filter, len + 2);
}

#else

// SDL hands back a null-terminated list of paths, or null on error. We only
// ever ask for one, so the first entry is the answer.
//
// userdata is the request id, not a pointer: a pointer into g_PendingSaveExt
// would leave SDL holding a buffer that a later request rewrites.
void SDLCALL DialogCallback(void* userdata, const char* const* filelist, int /*filter*/)
{
    const uint64_t id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(userdata));
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

    {
        std::lock_guard<std::mutex> lock(g_ResultMutex);
        if (id == g_ActiveId.load() && !g_PendingSaveExt.empty())
        {
            chosen = WithDefaultExtension(chosen, g_PendingSaveExt.c_str());
        }
    }

    PostResult(id, std::move(chosen));
}

std::vector<SDL_DialogFileFilter> BuildSdlFilters(const std::vector<DialogFilter>& parsed)
{
    std::vector<SDL_DialogFileFilter> out;
    out.reserve(parsed.size());
    for (const DialogFilter& f : parsed)
    {
        out.push_back(SDL_DialogFileFilter{ f.name.c_str(), f.pattern.c_str() });
    }
    return out;
}

#endif

// The start of every request: takes an id, keeps the handler, and marks it in
// flight. Returns 0 only for a missing handler. A request made while another
// is outstanding replaces it rather than being refused, so a dialog whose
// helper died does not block later ones.
uint64_t BeginRequest(ResultCallback onResult, const char* defaultExt)
{
    if (!onResult)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(g_ResultMutex);
    if (g_ActiveId.load() != 0)
    {
        DEKI_LOG_WARNING("A file dialog was already open; the new request supersedes it");
    }

    const uint64_t id = g_NextId++;
    g_ActiveId.store(id);
    g_Callback = std::move(onResult);
    g_ResultPath.clear();
    g_HaveResult = false;
    g_PendingSaveExt = defaultExt ? defaultExt : "";
    return id;
}

}  // namespace

void SetWindow(SDL_Window* window)
{
    g_Window = window;
}

void OpenFolder(const char* title, ResultCallback onResult, const char* defaultPath)
{
    const uint64_t id = BeginRequest(std::move(onResult), nullptr);
    if (id == 0)
    {
        return;
    }
    title = Tr(title);

#ifdef _WIN32
    PostResult(id, PickFolderWin32(title, defaultPath));
#else
    // SDL asks the desktop: the XDG portal or zenity on Linux, NSOpenPanel on
    // macOS. Neither takes a title here.
    (void)title;
    SDL_ShowOpenFolderDialog(DialogCallback, reinterpret_cast<void*>(static_cast<uintptr_t>(id)), g_Window,
                             (defaultPath && defaultPath[0]) ? defaultPath : nullptr, false);
#endif
}

void OpenFile(const char* filter, const char* defaultPath, ResultCallback onResult)
{
    const uint64_t id = BeginRequest(std::move(onResult), nullptr);
    if (id == 0)
    {
        return;
    }
    const std::string shownFilter = TranslateFilter(filter);
    filter = shownFilter.c_str();

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
    {
        ofn.lpstrInitialDir = wdefault.c_str();
    }

    PostResult(id, GetOpenFileNameW(&ofn) ? ToUtf8(filename) : std::string());
#else
    const std::vector<DialogFilter> parsed = ParseWin32Filter(filter);
    const std::vector<SDL_DialogFileFilter> sdlFilters = BuildSdlFilters(parsed);
    SDL_ShowOpenFileDialog(DialogCallback, reinterpret_cast<void*>(static_cast<uintptr_t>(id)), g_Window,
                           sdlFilters.empty() ? nullptr : sdlFilters.data(), static_cast<int>(sdlFilters.size()),
                           defaultPath, false);
#endif
}

void SaveFile(const char* filter, const char* defaultExt, const char* defaultPath, ResultCallback onResult)
{
    const uint64_t id = BeginRequest(std::move(onResult), defaultExt);
    if (id == 0)
    {
        return;
    }
    const std::string shownFilter = TranslateFilter(filter);
    filter = shownFilter.c_str();

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
    {
        ofn.lpstrInitialDir = wdefault.c_str();
    }

    PostResult(id, GetSaveFileNameW(&ofn) ? ToUtf8(filename) : std::string());
#else
    const std::vector<DialogFilter> parsed = ParseWin32Filter(filter);
    const std::vector<SDL_DialogFileFilter> sdlFilters = BuildSdlFilters(parsed);
    SDL_ShowSaveFileDialog(DialogCallback, reinterpret_cast<void*>(static_cast<uintptr_t>(id)), g_Window,
                           sdlFilters.empty() ? nullptr : sdlFilters.data(), static_cast<int>(sdlFilters.size()),
                           defaultPath);
#endif
}

bool IsPending()
{
    return g_ActiveId.load() != 0;
}

void PumpResults()
{
    ResultCallback callback;
    std::string path;

    {
        std::lock_guard<std::mutex> lock(g_ResultMutex);
        if (!g_HaveResult)
        {
            return;
        }
        callback = std::move(g_Callback);
        path = std::move(g_ResultPath);
        g_Callback = nullptr;
        g_ResultPath.clear();
        g_HaveResult = false;

        // Cleared under the same lock that took the answer, so a late callback
        // for this id cannot post a second result into the emptied slot. And
        // before the handler runs, so a handler that opens another dialog
        // (Save As after a failed write) starts clean.
        g_ActiveId.store(0);
    }

    if (callback)
    {
        callback(path);
    }
}

}  // namespace FileDialogs
}  // namespace DekiEditor
