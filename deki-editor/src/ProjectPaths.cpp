#include <deki-editor/build/ProjectPaths.h>

#include <deki-editor/Paths.h>
#include <deki-editor/FileIO.h>

#include <fstream>
#include <vector>
#include <utility>
#include <string>

namespace fs = std::filesystem;

namespace DekiEditor
{
namespace ProjectPaths
{

// Thin wrappers over the public, package-visible definitions in
// <deki-editor/Paths.h>. Kept as one definition on purpose: when the layout was
// stated in two places, the project root grew a second cache/ directory the
// moment one of them was updated and the other was not.

fs::path Generated(const std::string& projectRoot)
{
    return fs::path(GetGeneratedDirectory(projectRoot));
}

fs::path Runtime(const std::string& projectRoot)
{
    return fs::path(GetRuntimeDirectory(projectRoot));
}

fs::path Build(const std::string& projectRoot)
{
    return fs::path(GetBuildDirectory(projectRoot));
}

fs::path Cache(const std::string& projectRoot)
{
    return fs::path(GetCacheDirectory(projectRoot));
}

fs::path Shadow(const std::string& projectRoot)
{
    return fs::path(GetShadowDirectory(projectRoot));
}

fs::path CMakeListsFile(const std::string& projectRoot)
{
    return Generated(projectRoot) / "CMakeLists.txt";
}

fs::path Settings(const std::string& projectRoot)
{
    return fs::path(GetSettingsDirectory(projectRoot));
}

fs::path RuntimeBuildTemp(const std::string& projectRoot)
{
    return Runtime(projectRoot) / ".build_temp";
}

fs::path RuntimePdbArchive(const std::string& projectRoot)
{
    return Runtime(projectRoot) / ".pdb_archive";
}

bool MigrateModulesToPackages(const std::string& projectRoot, std::string& outSummary)
{
    const fs::path root(projectRoot);
    const std::pair<fs::path, fs::path> renames[] = {
        // Left-hand names are historical and must stay literal: they are what the
        // migration reads FROM. A blanket rename moved them too, which turned this
        // into packages -> packages and quietly stopped migrating anything.
        // "modules" and "deki-modules*.json" are the legacy (pre-0.9) names this migrates from.
        { root / "modules", fs::path(GetPackagesDirectory(projectRoot)) },
        { root / "deki-modules.json", fs::path(GetPackageManifestFile(projectRoot)) },
        { root / "deki-modules-lock.json", fs::path(GetPackageLockFile(projectRoot)) },
        { Settings(projectRoot) / "modules", Settings(projectRoot) / "packages" },
    };

    std::error_code ec;
    std::string moved;
    for (const auto& [from, to] : renames)
    {
        if (!fs::exists(from, ec) || fs::exists(to, ec))
        {
            continue;
        }
        std::error_code moveEc;
        fs::rename(from, to, moveEc);
        if (moveEc)
        {
            continue;
        }
        if (!moved.empty())
        {
            moved += ", ";
        }
        moved += from.filename().string();
    }

    if (!moved.empty())
    {
        outSummary = "renamed " + moved + " to packages";
    }
    return !moved.empty();
}

bool MigrateFlattenProjectFolder(const std::string& projectRoot, std::string& outSummary)
{
    const fs::path root(projectRoot);
    const fs::path legacy = root / "project";

    std::error_code ec;
    if (!fs::is_directory(legacy, ec))
    {
        return false;
    }

    const std::pair<fs::path, fs::path> moves[] = {
        { legacy / "src", fs::path(GetSourceDirectory(projectRoot)) },
        { legacy / "assets", fs::path(GetAssetsDirectory(projectRoot)) },
        { legacy / "modules", fs::path(GetPackagesDirectory(projectRoot)) },
        { legacy / "platforms", fs::path(GetPlatformsDirectory(projectRoot)) },
    };

    // All-or-nothing. This is authored content, not a cache: a half-applied move
    // leaves a project the editor cannot open, with the user's work split across
    // two layouts. Refuse unless every destination is free.
    for (const auto& [from, to] : moves)
    {
        if (fs::exists(from, ec) && fs::exists(to, ec))
        {
            outSummary = "cannot flatten project/: " + to.filename().string() + " already exists at the project root";
            return false;
        }
    }

    std::string moved;
    std::vector<std::pair<fs::path, fs::path>> done;
    for (const auto& [from, to] : moves)
    {
        if (!fs::exists(from, ec))
        {
            continue;
        }
        std::error_code moveEc;
        fs::rename(from, to, moveEc);
        if (moveEc)
        {
            // Put back what already moved. A rename here fails for reasons that
            // have nothing to do with the project - a watcher or an indexer
            // holding a directory is enough - and stopping halfway would leave
            // the user's source split across two layouts, with the version
            // already advanced so nothing would ever retry it.
            for (auto it = done.rbegin(); it != done.rend(); ++it)
            {
                std::error_code backEc;
                fs::rename(it->second, it->first, backEc);
            }
            outSummary = "could not move project/" + from.filename().string() + " (" + moveEc.message() +
                         "); left on the old layout, " + "will retry on next open";
            return false;
        }
        done.emplace_back(from, to);
        if (!moved.empty())
        {
            moved += ", ";
        }
        moved += from.filename().string();
    }

    // Rewrite the stored bootScene references, which carry the old segment.
    int rewritten = 0;
    const fs::path platforms(GetPlatformsDirectory(projectRoot));
    std::error_code iterEc;
    for (const auto& entry : fs::recursive_directory_iterator(platforms, iterEc))
    {
        if (iterEc)
        {
            break;
        }
        if (!entry.is_regular_file() || entry.path().extension() != ".json")
        {
            continue;
        }

        std::ifstream in(entry.path());
        if (!in.is_open())
        {
            continue;
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();

        const std::string needle = "project/" + GetPlatformsDirectoryRelative() + "/";
        if (text.find(needle) == std::string::npos)
        {
            continue;
        }

        for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at))
        {
            text.erase(at, std::char_traits<char>::length("project/"));
        }

        std::string err;
        if (AtomicWriteFile(entry.path().string(), text, err))
        {
            ++rewritten;
        }
    }

    // The folder only ever held those four; anything else is the user's and stays,
    // which also leaves evidence that the move was not clean.
    fs::remove(legacy, ec);

    if (!moved.empty())
    {
        outSummary = "lifted " + moved + " out of project/";
        if (rewritten > 0)
        {
            outSummary += "; repointed " + std::to_string(rewritten) + " platform config(s)";
        }
    }
    return !moved.empty();
}

bool MigrateToGeneratedLayout(const std::string& projectRoot, std::string& outSummary)
{
    const fs::path root(projectRoot);

    // Old name -> new home. Only generated content appears here; a project's
    // tracked files are never candidates.
    const std::pair<fs::path, fs::path> moves[] = {
        { root / "bin", Runtime(projectRoot) },
        { root / "build", Build(projectRoot) },
        { root / "cache", Cache(projectRoot) },
        { root / "CMakeLists.txt", CMakeListsFile(projectRoot) },
    };

    bool movedAny = false;
    std::string moved;
    std::string failed;

    std::error_code ec;
    for (const auto& [from, to] : moves)
    {
        if (!fs::exists(from, ec) || fs::exists(to, ec))
        {
            continue;
        }

        fs::create_directories(to.parent_path(), ec);

        std::error_code moveEc;
        fs::rename(from, to, moveEc);
        if (moveEc)
        {
            if (!failed.empty())
            {
                failed += ", ";
            }
            failed += from.filename().string();
            continue;
        }

        movedAny = true;
        if (!moved.empty())
        {
            moved += ", ";
        }
        moved += from.filename().string();
    }

    // The shadow dir used to live inside the cache; it is a sibling now so that
    // the copy destination is never nested inside a copy source.
    const fs::path shadowInCache = Cache(projectRoot) / "packages";
    if (fs::exists(shadowInCache, ec) && !fs::exists(Shadow(projectRoot), ec))
    {
        std::error_code moveEc;
        fs::rename(shadowInCache, Shadow(projectRoot), moveEc);
        if (!moveEc)
        {
            movedAny = true;
            if (!moved.empty())
            {
                moved += ", ";
            }
            moved += "cache/packages";
        }
    }

    if (!moved.empty())
    {
        outSummary = "moved " + moved + " into generated/";
    }
    if (!failed.empty())
    {
        if (!outSummary.empty())
        {
            outSummary += "; ";
        }
        outSummary += "could not move " + failed + " (in use; will retry on next open)";
    }
    return movedAny;
}

}  // namespace ProjectPaths
}  // namespace DekiEditor
