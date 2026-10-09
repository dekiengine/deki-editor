#include "BundledToolchain.h"

#include <deki-editor/EditorPaths.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace DekiEditor
{

namespace
{

#ifdef _WIN32
constexpr const char* kExeSuffix = ".exe";
#else
constexpr const char* kExeSuffix = "";
#endif

#ifdef _WIN32
/// Whether `name` (plus the executable suffix) is in a folder on PATH.
bool OnPath(const std::string& name)
{
    const char* path = std::getenv("PATH");
    if (!path)
    {
        return false;
    }
    const std::string list = path;
    size_t start = 0;
    while (start <= list.size())
    {
        const size_t end = list.find(';', start);
        const std::string dir = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
        std::error_code ec;
        if (!dir.empty() && fs::exists(fs::path(dir) / (name + kExeSuffix), ec) && !ec)
        {
            return true;
        }
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1;
    }
    return false;
}
#endif

/// <toolchain>/bin/<name><suffix> if it exists, else "".
std::string BundledPath(const std::string& name)
{
    const fs::path candidate = fs::path(BundledToolchainDir()) / "bin" / (name + kExeSuffix);
    std::error_code ec;
    if (fs::exists(candidate, ec) && !ec)
    {
        return candidate.string();
    }
    return "";
}

}  // namespace

std::string BundledToolchainDir()
{
    return (fs::path(EditorPaths::GetExecutableDir()) / "toolchain").string();
}

bool HasBundledToolchain()
{
    std::error_code ec;
    return fs::is_directory(fs::path(BundledToolchainDir()) / "bin", ec) && !ec;
}

std::string ResolveTool(const std::string& name)
{
    const std::string bundled = BundledPath(name);
    if (!bundled.empty())
    {
        return bundled;
    }
#ifdef _WIN32
    // Not on PATH (a GUI app started from Explorer seldom has MinGW there):
    // the toolchain's own copy, by full path.
    if (!OnPath(name))
    {
        const std::string dir = ToolchainBinDir();
        std::error_code ec;
        const fs::path candidate = fs::path(dir) / (name + kExeSuffix);
        if (!dir.empty() && fs::exists(candidate, ec) && !ec)
        {
            return candidate.string();
        }
    }
#endif
    return name;
}

std::string ToolchainBinDir()
{
#ifdef _WIN32
    std::error_code ec;
    if (HasBundledToolchain())
    {
        return (fs::path(BundledToolchainDir()) / "bin").string();
    }
    // The compiler this editor was built with: projects are built with it too
    // (ProjectCxxCompiler), so its folder is the one that matches.
    fs::path own = fs::path(DEKI_EDITOR_CXX_COMPILER).parent_path();
    if (own.is_absolute() && fs::exists(own / "g++.exe", ec) && !ec)
    {
        return own.make_preferred().string();
    }
    for (const char* dir : { "C:\\msys64\\mingw64\\bin", "C:\\msys64\\ucrt64\\bin", "C:\\mingw64\\bin" })
    {
        if (fs::exists(fs::path(dir) / "g++.exe", ec) && !ec)
        {
            return dir;
        }
    }
#endif
    return "";
}

bool PathListHasDir(const std::string& pathList, const std::string& dir)
{
    auto norm = [](std::string s)
    {
        for (char& c : s)
        {
            c = (c == '/') ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        while (s.size() > 1 && s.back() == '\\')
        {
            s.pop_back();
        }
        return s;
    };
    const std::string want = norm(dir);
    if (want.empty())
    {
        return false;
    }
    size_t start = 0;
    while (start <= pathList.size())
    {
        const size_t end = pathList.find(';', start);
        const std::string entry = pathList.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!entry.empty() && norm(entry) == want)
        {
            return true;
        }
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1;
    }
    return false;
}

void PrepareToolchainPath()
{
#ifdef _WIN32
    const std::string dir = ToolchainBinDir();
    if (dir.empty())
    {
        return;
    }
    const char* current = std::getenv("PATH");
    const std::string path = current ? current : "";
    if (PathListHasDir(path, dir))
    {
        return;
    }
    // In front: the compiler's helpers must load the toolchain's own DLLs, not
    // a same-named one from an older MinGW further along PATH. Only this
    // process and what it starts see it; the user's PATH is not touched.
    const std::string updated = path.empty() ? dir : dir + ";" + path;
    _putenv_s("PATH", updated.c_str());
#endif
}

std::string ProjectCxxCompiler()
{
    for (const char* name : { "g++", "g++-16" })
    {
        const std::string bundled = BundledPath(name);
        if (!bundled.empty())
        {
            return bundled;
        }
    }
    // Otherwise the compiler this editor was built with. Packages are loaded
    // into this process, and another compiler could bring a second standard
    // library across that boundary.
    return DEKI_EDITOR_CXX_COMPILER;
}

std::string ProjectCCompiler()
{
    for (const char* name : { "gcc", "gcc-16" })
    {
        const std::string bundled = BundledPath(name);
        if (!bundled.empty())
        {
            return bundled;
        }
    }
    return DEKI_EDITOR_C_COMPILER;
}

}  // namespace DekiEditor
