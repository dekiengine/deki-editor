#include "SourceChangeDetector.h"

#include "../PackageLoader.h"
#include <deki-editor/Paths.h>
#include <deki/LogSystem.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <system_error>
#include <vector>

namespace DekiEditor
{
namespace SourceChangeDetector
{

std::string PackageNameFromEventPath(const std::string& relativePath)
{
    const size_t first = relativePath.find_first_of("/\\");
    if (first == std::string::npos) return {};
    if (relativePath.compare(0, first, "packages") != 0) return {};

    const size_t second = relativePath.find_first_of("/\\", first + 1);
    const std::string name = (second == std::string::npos)
                                 ? relativePath.substr(first + 1)
                                 : relativePath.substr(first + 1, second - first - 1);
    return name;
}

bool IsBuildArtifact(const std::string& relativePath)
{
    // Filter watcher events that come from our own build output. CMake emits
    // metadata into <package>/generated/, MSVC drops intermediates with these
    // extensions, and the editor's build/ tree is full of churn. None of these
    // should mark sources as dirty.
    //
    // Match on path SEGMENTS, not on "/generated/" substrings: the junction
    // watchers in StartWatching are rooted at the package repo itself, so their
    // relative paths start at the repo root ("generated/Foo.gen.cpp") with no
    // leading slash. A substring check misses those, and every build's codegen
    // then re-marks sources dirty — the editor rebuilds on each alt-tab forever.
    auto hasSegment = [&relativePath](const char* seg)
    {
        const std::string s(seg);
        if (relativePath.rfind(s + "/", 0) == 0) return true;          // leading segment
        return relativePath.find("/" + s + "/") != std::string::npos;  // interior segment
    };
    if (hasSegment("generated") || hasSegment("build"))
        return true;

    // Written into the package repo through the junction by the build itself.
    {
        static const std::string kPkg = ".deki-package.json";
        if (relativePath == kPkg ||
            (relativePath.size() > kPkg.size() &&
             relativePath.compare(relativePath.size() - kPkg.size() - 1,
                                  kPkg.size() + 1, "/" + kPkg) == 0))
        {
            return true;
        }
    }

    auto dotPos = relativePath.find_last_of('.');
    if (dotPos == std::string::npos)
        return false;
    std::string ext = relativePath.substr(dotPos);
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    return ext == ".obj" || ext == ".pdb" || ext == ".lib" || ext == ".exp" ||
           ext == ".ilk" || ext == ".tlog";
}

std::filesystem::file_time_type GetLatestSourceTime(const std::string& srcDir)
{
    namespace fs = std::filesystem;

    fs::file_time_type latestTime = (fs::file_time_type::min)();

    try
    {
        for (const auto& entry : fs::recursive_directory_iterator(
                 srcDir, fs::directory_options::follow_directory_symlink))
        {
            if (entry.is_regular_file())
            {
                auto ext = entry.path().extension().string();
                if (ext == ".cpp" || ext == ".h" || ext == ".hpp")
                {
                    auto fileTime = fs::last_write_time(entry.path());
                    if (fileTime > latestTime)
                    {
                        latestTime = fileTime;
                    }
                }
            }
        }
    }
    catch (const fs::filesystem_error&)
    {
        // Ignore errors
    }

    return latestTime;
}

void WalkSourceTree(const std::filesystem::path& root,
                    const std::function<void(const std::filesystem::directory_entry&)>& cb)
{
    namespace fs = std::filesystem;
    if (!fs::exists(root)) return;

    // Step 1: Enumerate immediate children non-recursively. Any directory
    // child — whether a plain directory or a Windows junction — is treated as
    // a subtree root in Step 2.
    std::vector<fs::path> subtreeRoots;
    std::error_code ec;
    for (const auto& child : fs::directory_iterator(root, ec))
    {
        // is_directory() follows reparse points on Windows, so junctions
        // report true here — that's what we want.
        if (child.is_directory(ec))
            subtreeRoots.push_back(child.path());
        else if (child.is_regular_file(ec))
            cb(child);  // files directly under root
    }

    // Step 2: For each subtree root, start a fresh recursive_directory_iterator.
    // Starting iteration AT a junction path works because the junction is
    // resolved by CreateFileW at open time; the iterator then walks the
    // target's contents but reports paths prefixed by the junction-side root,
    // keeping map keys stable across calls.
    for (const auto& subRoot : subtreeRoots)
    {
        try
        {
            for (const auto& entry : fs::recursive_directory_iterator(
                     subRoot,
                     fs::directory_options::follow_directory_symlink |
                         fs::directory_options::skip_permission_denied))
            {
                if (entry.is_regular_file(ec))
                    cb(entry);
            }
        }
        catch (const fs::filesystem_error&)
        {
            // Partial walks are fine — next tick retries.
        }
    }
}

uint64_t ComputeConfigureInputsHash(const std::string& projectPath,
                                    const std::string& cmakeListsPath)
{
    namespace fs = std::filesystem;

    constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
    constexpr uint64_t kFnvPrime = 0x100000001b3ULL;

    uint64_t h = kFnvOffset;
    auto mix = [&](const void* data, size_t n)
    {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < n; ++i)
        {
            h ^= p[i];
            h *= kFnvPrime;
        }
    };
    auto mixStr = [&](const std::string& s)
    { mix(s.data(), s.size()); mix("\0", 1); };
    auto mixFileContent = [&](const fs::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        if (!f) return;
        char buf[65536];
        while (f.read(buf, sizeof(buf)) || f.gcount() > 0)
            mix(buf, static_cast<size_t>(f.gcount()));
        mix("\0", 1);
    };
    // Size + mtime stand in for the content: an edit changes at least one of
    // them, and reading every header on every build check (on the main
    // thread, on every window focus) was the cost this hash is meant to save.
    auto mixFileStat = [&](const fs::path& p)
    {
        std::error_code ec;
        const std::uintmax_t size = fs::file_size(p, ec);
        if (ec) return;
        const auto mtime = fs::last_write_time(p, ec).time_since_epoch().count();
        if (ec) return;
        mix(&size, sizeof(size));
        mix(&mtime, sizeof(mtime));
    };

    // 1. CMakeLists.txt content
    mixStr("cmake:");
    if (fs::exists(cmakeListsPath))
        mixFileContent(cmakeListsPath);

    // 2. Sorted list of package directory names (install/uninstall must reconfigure)
    mixStr("packages:");
    fs::path packagesPath = fs::path(GetPackagesDirectory(projectPath));
    std::vector<std::string> packageDirs;
    if (fs::exists(packagesPath))
    {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(packagesPath, ec))
        {
            if (entry.is_directory(ec))
                packageDirs.push_back(entry.path().filename().string());
        }
    }
    std::sort(packageDirs.begin(), packageDirs.end());
    for (const auto& name : packageDirs)
        mixStr(name);

    // 3. Every header under project/src and project/packages: path, size and
    // mtime. The reflection codegen parses headers for DEKI_EXPORT — header
    // edits must trigger reconfigure; .cpp edits (compile-only) must not.
    // Uses WalkSourceTree so junctioned package trees are included.
    auto mixHeaders = [&](const fs::path& root)
    {
        std::vector<fs::path> headers;
        WalkSourceTree(root, [&](const fs::directory_entry& entry)
                       {
            auto ext = entry.path().extension().string();
            if (ext != ".h" && ext != ".hpp") return;
            if (entry.path().string().find("generated") != std::string::npos) return;
            headers.push_back(entry.path()); });
        std::sort(headers.begin(), headers.end());
        for (const auto& p : headers)
        {
            mixStr(p.string());
            mixFileStat(p);
        }
    };
    mixStr("src-headers:");
    mixHeaders(fs::path(GetSourceDirectory(projectPath)));
    mixStr("package-headers:");
    mixHeaders(packagesPath);

    // Never return 0 — 0 is our "no stamp on disk" sentinel.
    return h == 0 ? 1 : h;
}

}  // namespace SourceChangeDetector
}  // namespace DekiEditor
