#include "PluginToolchain.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace DekiEditor
{

namespace
{

std::string Normalize(std::string p)
{
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

#ifdef _WIN32
constexpr const char* kExeSuffix = ".exe";
#else
constexpr const char* kExeSuffix = "";
#endif

// Locate an executable on PATH. Returns an absolute, forward-slashed path.
std::string WhichExe(const std::string& stem)
{
    const std::string exe = stem + kExeSuffix;

#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = SearchPathA(nullptr, exe.c_str(), nullptr, MAX_PATH, buf, nullptr);
    if (n > 0 && n < MAX_PATH)
        return Normalize(std::string(buf, n));
    return {};
#else
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv)
        return {};

    std::string path(pathEnv);
    size_t start = 0;
    while (start <= path.size())
    {
        size_t sep = path.find(':', start);
        if (sep == std::string::npos) sep = path.size();

        if (sep > start)
        {
            fs::path candidate = fs::path(path.substr(start, sep - start)) / exe;
            std::error_code ec;
            if (fs::exists(candidate, ec) && !fs::is_directory(candidate, ec))
                return Normalize(candidate.string());
        }
        start = sep + 1;
    }
    return {};
#endif
}

// PATH first, then well-known install locations for the platform.
std::string FindCompiler(const char* const* stems, size_t stemCount,
                         const char* const* extraDirs, size_t extraDirCount)
{
    for (size_t i = 0; i < stemCount; ++i)
    {
        std::string found = WhichExe(stems[i]);
        if (!found.empty())
            return found;
    }

    for (size_t d = 0; d < extraDirCount; ++d)
    {
        for (size_t i = 0; i < stemCount; ++i)
        {
            fs::path candidate = fs::path(extraDirs[d]) / (std::string(stems[i]) + kExeSuffix);
            std::error_code ec;
            if (fs::exists(candidate, ec))
                return Normalize(candidate.string());
        }
    }

    return {};
}

PluginToolchainInfo ResolveMsvc()
{
    PluginToolchainInfo info;
    info.abi = PluginAbi::Msvc;
    info.needsMsvcEnv = true;

    // clang-cl first: MSVC-compatible driver, same ABI, typically 2-3x faster
    // than cl.exe. CMake keeps the MSVC variable TRUE for it, so every existing
    // if(MSVC) block in the generated CMakeLists still applies.
    static const char* kClangClStems[] = { "clang-cl" };
    static const char* kClangClDirs[] = {
        "C:/Program Files/LLVM/bin",
        "C:/Program Files (x86)/LLVM/bin",
    };

    std::string clangCl = FindCompiler(kClangClStems, 1, kClangClDirs, 2);
    if (!clangCl.empty())
    {
        info.cCompiler = clangCl;
        info.cxxCompiler = clangCl;
        info.displayName = "clang-cl (LLVM)";
        info.valid = true;
        return info;
    }

    // cl.exe is normally not on PATH unless the editor was launched from a
    // developer prompt, but the vcvars environment we inject puts it there.
    std::string cl = WhichExe("cl");
    if (!cl.empty())
    {
        info.cCompiler = cl;
        info.cxxCompiler = cl;
        info.displayName = "cl.exe (MSVC)";
        info.valid = true;
        return info;
    }

    info.error = "No MSVC-ABI compiler found (looked for clang-cl and cl.exe).";
    return info;
}

PluginToolchainInfo ResolveGnu()
{
    PluginToolchainInfo info;
    info.abi = PluginAbi::Gnu;
    info.needsMsvcEnv = false;

    // Prefer the same family the editor was compiled with, so the plugin links
    // against the identical libstdc++/libc++ the editor already loaded.
#if defined(__clang__)
    static const char* kCcStems[] = { "clang" };
    static const char* kCxxStems[] = { "clang++" };
    const char* kName = "clang++";
#else
    static const char* kCcStems[] = { "gcc" };
    static const char* kCxxStems[] = { "g++" };
    const char* kName = "g++";
#endif

#ifdef _WIN32
    // MinGW is rarely on PATH for a GUI app launched from Explorer.
    static const char* kDirs[] = {
        "C:/msys64/mingw64/bin",
        "C:/msys64/ucrt64/bin",
        "C:/mingw64/bin",
    };
    const size_t kDirCount = 3;
#else
    static const char* kDirs[] = { "/usr/bin", "/usr/local/bin", "/opt/homebrew/bin" };
    const size_t kDirCount = 3;
#endif

    std::string cc = FindCompiler(kCcStems, 1, kDirs, kDirCount);
    std::string cxx = FindCompiler(kCxxStems, 1, kDirs, kDirCount);

    if (cc.empty() || cxx.empty())
    {
        info.error = std::string("No GNU-ABI compiler found (looked for ") +
                     kCcStems[0] + " / " + kCxxStems[0] + ").";
        return info;
    }

    info.cCompiler = cc;
    info.cxxCompiler = cxx;
#ifdef _WIN32
    info.displayName = std::string(kName) + " (MinGW-w64)";
#else
    info.displayName = kName;
#endif
    info.valid = true;
    return info;
}

}  // namespace

const PluginToolchainInfo& PluginToolchain::Get()
{
    static const PluginToolchainInfo cached = []()
    {
        // An explicit override still has to satisfy the host ABI - it selects
        // *which* matching compiler to use, not whether to match.
        PluginToolchainInfo info = (HostAbi() == PluginAbi::Msvc) ? ResolveMsvc() : ResolveGnu();

        if (const char* cxxOverride = std::getenv("DEKI_PLUGIN_CXX"))
        {
            if (cxxOverride[0] != '\0')
            {
                info.cxxCompiler = Normalize(cxxOverride);
                if (const char* ccOverride = std::getenv("DEKI_PLUGIN_CC"))
                    info.cCompiler = Normalize(ccOverride);
                else
                    info.cCompiler = info.cxxCompiler;

                info.displayName += " (DEKI_PLUGIN_CXX override)";
                info.valid = true;
                info.error.clear();
            }
        }

        return info;
    }();

    return cached;
}

std::string PluginToolchain::FindOnPath(const std::string& stem)
{
    return WhichExe(stem);
}

std::string PluginToolchain::CacheCompilerMarker()
{
    const PluginToolchainInfo& info = Get();
    if (!info.valid)
        return {};

    // Compare on the compiler's filename rather than the full path: CMake writes
    // an absolute path that may differ in case or separators from ours.
    std::string stem = fs::path(info.cxxCompiler).stem().string();
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });
    return stem;
}

std::string PluginToolchain::InstallHint()
{
    if (HostAbi() == PluginAbi::Msvc)
    {
        return "Install LLVM (winget install LLVM.LLVM) or Visual Studio Build Tools "
               "with the C++ workload.";
    }

#ifdef _WIN32
    return "Install the MinGW-w64 toolchain (MSYS2: pacman -S mingw-w64-x86_64-gcc) "
           "and make sure it is on PATH.";
#else
    return "Install a C++ toolchain (g++ or clang++) and make sure it is on PATH.";
#endif
}

}  // namespace DekiEditor
