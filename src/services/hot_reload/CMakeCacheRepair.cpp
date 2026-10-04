#include "CMakeCacheRepair.h"
#include "../../panels/ConsolePanel.h"
#include "../BundledToolchain.h"
#include "../PluginToolchain.h"
#include <deki/LogSystem.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>

namespace DekiEditor
{

void WipeBuildDirIfStaleToolchain(const std::filesystem::path& buildPath, const std::filesystem::path& cmakeCachePath,
                                  ConsolePanel* console)
{
    namespace fs = std::filesystem;

    // CMAKE_GENERATOR and CMAKE_CXX_COMPILER in CMakeCache.txt tell whether
    // the cache matches the toolchain we now use. If not, wipe the build dir;
    // otherwise Ninja runs old build files that still call g++/ld and fails
    // with a bare "collect2.exe: ld returned 1".
    auto readCacheVar = [&](const std::string& key) -> std::string
    {
        std::ifstream f(cmakeCachePath);
        std::string line;
        while (std::getline(f, line))
        {
            size_t eq = line.find('=');
            if (eq == std::string::npos)
            {
                continue;
            }
            std::string lhs = line.substr(0, eq);
            size_t colon = lhs.find(':');
            std::string name = (colon == std::string::npos) ? lhs : lhs.substr(0, colon);
            if (name != key)
            {
                continue;
            }

            // Strip trailing CR and whitespace. std::getline leaves the CR of
            // a CRLF line, which would fail the exact comparison below and
            // wipe the build dir on every reload.
            std::string val = line.substr(eq + 1);
            while (!val.empty() && (val.back() == '\r' || val.back() == ' ' || val.back() == '\t'))
            {
                val.pop_back();
            }
            return val;
        }
        return "";
    };

    if (fs::exists(cmakeCachePath))
    {
        std::string cacheGen = readCacheVar("CMAKE_GENERATOR");
        std::string cacheCxx = readCacheVar("CMAKE_CXX_COMPILER");
        std::string cxxLower = cacheCxx;
        std::transform(cxxLower.begin(), cxxLower.end(), cxxLower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        bool wrongGenerator = !cacheGen.empty() && cacheGen != "Ninja";
        // A cache written by a different compiler (a manual cmake run, or an
        // editor build with another toolchain) must be wiped, not reused:
        // mixed ABIs in one build dir give link errors that do not point to
        // the toolchain.
        //
        // Compare with the compiler this build configures, not a fixed name:
        // on Windows whatever PluginToolchain selected (clang-cl, cl, or MinGW
        // g++), on Linux/macOS the compiler baked into the editor. A fixed name
        // would wipe the build dir on every reload for anyone using another.
        auto compilerMarker = [](const std::string& path)
        {
            std::string stem = fs::path(path).stem().string();
            std::transform(stem.begin(), stem.end(), stem.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return stem;
        };
#ifdef _WIN32
        const std::string marker = PluginToolchain::CacheCompilerMarker();
#else
        const std::string marker = compilerMarker(ProjectCxxCompiler());
#endif
        (void)compilerMarker;
        bool wrongCompiler = !cacheCxx.empty() && !marker.empty() && cxxLower.find(marker) == std::string::npos;

        if (wrongGenerator || wrongCompiler)
        {
            // Log the raw values and their first 16 bytes in hex, so a hidden
            // control character or BOM shows.
            auto hexDump = [](const std::string& s)
            {
                std::string out;
                size_t n = (s.size() < 16) ? s.size() : 16;
                for (size_t i = 0; i < n; ++i)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "%02X ", static_cast<unsigned char>(s[i]));
                    out += buf;
                }
                return out;
            };
            DEKI_LOG_WARNING("HotReloadService: stale toolchain in CMakeCache "
                             "(generator='%s' [len=%zu hex=%s], compiler='%s' [len=%zu hex=%s]) "
                             "- wiping build dir",
                             cacheGen.c_str(), cacheGen.size(), hexDump(cacheGen).c_str(), cacheCxx.c_str(),
                             cacheCxx.size(), hexDump(cacheCxx).c_str());
            if (console)
            {
                console->LogEditor("Stale toolchain in CMake cache - wiping and reconfiguring with Ninja + MSVC...");
            }
            std::error_code ec;
            fs::remove_all(buildPath, ec);
            fs::create_directories(buildPath, ec);
        }
    }
}

}  // namespace DekiEditor
