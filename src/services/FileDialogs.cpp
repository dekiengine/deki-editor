#include "FileDialogs.h"
#include <deki/LogSystem.h>

#include <string>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#endif

namespace DekiEditor
{
namespace FileDialogs
{

#ifdef _WIN32
namespace
{
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
    if (!filter)
        return {};
    int len = 0;
    while (!(filter[len] == '\0' && filter[len + 1] == '\0'))
        ++len;
    return ToWide(filter, len + 2);
}
}  // namespace
#endif

std::string OpenFolder(const char* title)
{
#ifdef _WIN32
    const std::wstring wtitle = ToWide(title);
    wchar_t path[MAX_PATH] = { 0 };
    BROWSEINFOW bi = { 0 };
    bi.lpszTitle = wtitle.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl != nullptr)
    {
        SHGetPathFromIDListW(pidl, path);
        CoTaskMemFree(pidl);
        return ToUtf8(path);
    }
    return "";
#else
    (void)title;
    DEKI_LOG_WARNING("FileDialogs::OpenFolder is not implemented on this platform");
    return "";
#endif
}

std::string OpenFile(const char* filter, const char* defaultPath)
{
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

    if (GetOpenFileNameW(&ofn))
    {
        return ToUtf8(filename);
    }
    return "";
#else
    (void)filter;
    (void)defaultPath;
    DEKI_LOG_WARNING("FileDialogs::OpenFile is not implemented on this platform");
    return "";
#endif
}

std::string SaveFile(const char* filter, const char* defaultExt, const char* defaultPath)
{
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

    if (GetSaveFileNameW(&ofn))
    {
        return ToUtf8(filename);
    }
    return "";
#else
    (void)filter;
    (void)defaultExt;
    (void)defaultPath;
    DEKI_LOG_WARNING("FileDialogs::SaveFile is not implemented on this platform");
    return "";
#endif
}

}  // namespace FileDialogs
}  // namespace DekiEditor
