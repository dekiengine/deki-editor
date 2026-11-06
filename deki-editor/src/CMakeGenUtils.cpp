#include <deki-editor/build/CMakeGenUtils.h>
#include <deki/LogSystem.h>
#include <deki-editor/Paths.h>
#include <deki-editor/build/PlatformConfig.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace DekiEditor
{
namespace CMakeGen
{

// ============================================================================
// Package manifest scanning
// ============================================================================

// "tags" from a manifest or descriptor: an array of strings. Anything else,
// or no "tags" at all, means no tags. Unknown tags are kept, so new tags
// need no change in every reader.
static std::vector<std::string> TagsFrom(const nlohmann::json& j)
{
    std::vector<std::string> tags;
    if (j.contains("tags") && j["tags"].is_array())
    {
        for (const auto& t : j["tags"])
        {
            if (t.is_string())
            {
                tags.push_back(t.get<std::string>());
            }
        }
    }
    return tags;
}

// Scans one packages directory for package.json manifests.
static void ScanPackagesDir(const fs::path& packagesDir, std::vector<PackageEntry>& allPackages)
{
    if (!fs::exists(packagesDir))
    {
        return;
    }

    for (const auto& entry : fs::directory_iterator(packagesDir))
    {
        if (!entry.is_directory())
        {
            continue;
        }
        fs::path manifestPath = entry.path() / "package.json";
        if (!fs::exists(manifestPath))
        {
            continue;
        }

        try
        {
            std::ifstream mf(manifestPath);
            nlohmann::json j;
            mf >> j;

            PackageEntry info;
            info.dirName = entry.path().filename().string();
            info.buildDefine = j.value("buildDefine", "");
            info.id = j.value("id", "");
            info.tags = TagsFrom(j);

            // "platformRequires": what the package needs from a platform.
            // Absent means it builds anywhere.
            if (j.contains("platformRequires") && j["platformRequires"].is_array())
            {
                for (const auto& p : j["platformRequires"])
                {
                    if (!p.is_string())
                    {
                        continue;
                    }
                    std::string need = p.get<std::string>();
                    const std::string current = CurrentCapabilityName(need);
                    if (current != need)
                    {
                        DEKI_LOG_WARNING("Package '%s': platformRequires '%s' is called '%s' now, and is "
                                         "read as that. Rename it in %s; the old name is read for one release",
                                         info.id.empty() ? info.dirName.c_str() : info.id.c_str(), need.c_str(),
                                         current.c_str(), manifestPath.string().c_str());
                        need = current;
                    }
                    // Checked the same way as a platform's "provides" (which
                    // is checked when it loads). A requirement no valid name
                    // can match would leave the package out of every target
                    // with no message saying why.
                    if (!IsValidCapabilityName(need))
                    {
                        DEKI_LOG_WARNING("Package '%s': platformRequires '%s' is not a valid "
                                         "capability name (lowercase letters, digits and "
                                         ". - : _ only), so no platform can provide it",
                                         info.id.empty() ? info.dirName.c_str() : info.id.c_str(), need.c_str());
                    }
                    info.platformRequires.push_back(std::move(need));
                }
            }
            // Capability names, checked as platformRequires is.
            for (const auto& [key, into] : { std::pair<const char*, std::vector<std::string>*>{
                                                 "capabilityDefines", &info.capabilityDefines },
                                             std::pair<const char*, std::vector<std::string>*>{
                                                 "filtersTexturesWith", &info.filtersTexturesWith } })
            {
                if (!j.contains(key) || !j[key].is_array())
                {
                    continue;
                }
                for (const auto& p : j[key])
                {
                    if (!p.is_string())
                    {
                        continue;
                    }
                    const std::string name = CurrentCapabilityName(p.get<std::string>());
                    if (!IsValidCapabilityName(name))
                    {
                        DEKI_LOG_WARNING("Package '%s': %s '%s' is not a valid capability name (lowercase "
                                         "letters, digits and . - : _ only), so no platform can provide it",
                                         info.id.empty() ? info.dirName.c_str() : info.id.c_str(), key,
                                         name.c_str());
                    }
                    into->push_back(name);
                }
            }
            if (j.contains("features") && j["features"].is_array())
            {
                for (const auto& f : j["features"])
                {
                    if (!f.is_object())
                    {
                        continue;
                    }
                    FeatureEntry fe;
                    fe.id = f.value("id", "");
                    fe.displayName = f.value("displayName", "");
                    fe.buildDefine = f.value("buildDefine", "");
                    fe.required = f.value("required", false);
                    if (f.contains("componentTypes") && f["componentTypes"].is_array())
                    {
                        for (const auto& t : f["componentTypes"])
                        {
                            if (t.is_string())
                            {
                                fe.componentTypes.push_back(t.get<std::string>());
                            }
                        }
                    }
                    if (f.contains("requires") && f["requires"].is_array())
                    {
                        for (const auto& r : f["requires"])
                        {
                            if (r.is_string() && r.get<std::string>().rfind("deki-", 0) == 0)
                            {
                                fe.requiredPackages.push_back(r.get<std::string>());
                            }
                        }
                    }
                    if (!fe.id.empty())
                    {
                        info.features.push_back(fe);
                    }
                }
            }

            // "requires": the packages this one cannot build without, whatever
            // the platform. The platform dependency lists below may repeat them.
            if (j.contains("requires") && j["requires"].is_array())
            {
                for (const auto& r : j["requires"])
                {
                    if (r.is_string() && r.get<std::string>().rfind("deki-", 0) == 0)
                    {
                        info.dekiDeps.push_back(r.get<std::string>());
                    }
                }
            }

            // Dependencies from every platform's dependency list.
            if (j.contains("dependencies"))
            {
                for (auto& [platform, deps] : j["dependencies"].items())
                {
                    if (!deps.is_array())
                    {
                        continue;
                    }
                    for (auto& dep : deps)
                    {
                        std::string name;
                        if (dep.is_string())
                        {
                            name = dep.get<std::string>();
                        }
                        else if (dep.is_object())
                        {
                            name = dep.value("name", "");
                        }

                        if (name.empty())
                        {
                            continue;
                        }

                        if (name.rfind("deki-", 0) == 0)
                        {
                            if (std::find(info.dekiDeps.begin(), info.dekiDeps.end(), name) == info.dekiDeps.end())
                            {
                                info.dekiDeps.push_back(name);
                            }
                        }
                        else
                        {
                            // Any framework. Kept as written; the backend that
                            // claims `platform` interprets it.
                            FrameworkDependency fd;
                            fd.name = name;
                            if (dep.is_object())
                            {
                                fd.json = dep.dump();
                            }
                            info.frameworkDeps[platform].push_back(std::move(fd));
                        }
                    }
                }
            }

            // Settings from package.cmake.
            fs::path cmakePath = entry.path() / "package.cmake";
            if (fs::exists(cmakePath))
            {
                std::ifstream cmf(cmakePath);
                std::string cmakeContent((std::istreambuf_iterator<char>(cmf)), std::istreambuf_iterator<char>());

                // Extract PACKAGE_FILE_PREFIX
                {
                    std::string marker = "set(PACKAGE_FILE_PREFIX";
                    auto pos = cmakeContent.find(marker);
                    if (pos != std::string::npos)
                    {
                        pos += marker.length();
                        auto end = cmakeContent.find(")", pos);
                        if (end != std::string::npos)
                        {
                            std::string val = cmakeContent.substr(pos, end - pos);
                            val.erase(0, val.find_first_not_of(" \t\n\r\""));
                            val.erase(val.find_last_not_of(" \t\n\r\"") + 1);
                            info.filePrefix = val;
                        }
                    }
                }

                // Extract PACKAGE_PREFIX (e.g., "Deki2D")
                {
                    std::string marker = "set(PACKAGE_PREFIX";
                    auto pos = cmakeContent.find(marker);
                    if (pos != std::string::npos)
                    {
                        pos += marker.length();
                        auto end = cmakeContent.find(")", pos);
                        if (end != std::string::npos)
                        {
                            std::string val = cmakeContent.substr(pos, end - pos);
                            val.erase(0, val.find_first_not_of(" \t\n\r\""));
                            val.erase(val.find_last_not_of(" \t\n\r\"") + 1);
                            info.packagePrefix = val;
                        }
                    }
                }

                // Extract PACKAGE_HAS_SYSTEM_INIT
                {
                    if (cmakeContent.find("set(PACKAGE_HAS_SYSTEM_INIT ON)") != std::string::npos)
                    {
                        info.hasSystemInit = true;
                    }
                }

                // PACKAGE_ENTRY: the entry file, which is left out of the sources.
                std::string entryFile;
                {
                    std::string marker = "set(PACKAGE_ENTRY";
                    auto pos = cmakeContent.find(marker);
                    if (pos != std::string::npos)
                    {
                        pos += marker.length();
                        auto end = cmakeContent.find(")", pos);
                        if (end != std::string::npos)
                        {
                            entryFile = cmakeContent.substr(pos, end - pos);
                            entryFile.erase(0, entryFile.find_first_not_of(" \t\n\r\""));
                            entryFile.erase(entryFile.find_last_not_of(" \t\n\r\"") + 1);
                        }
                    }
                }

                // Source stems, from the *.cpp files in the package directory.
                {
                    // Skip whole folders, not file by file: every project open
                    // scans each package, and a clone's .git alone holds
                    // thousands of files.
                    fs::recursive_directory_iterator it(entry.path());
                    for (; it != fs::recursive_directory_iterator(); ++it)
                    {
                        const fs::directory_entry& cppEntry = *it;
                        if (cppEntry.is_directory())
                        {
                            const std::string name = cppEntry.path().filename().string();
                            // Top-level editor/, generated/ and tests/ are not
                            // runtime sources; dot folders (.git) never are.
                            const bool topLevel = it.depth() == 0;
                            if ((!name.empty() && name[0] == '.') ||
                                (topLevel && (name == "editor" || name == "generated" || name == "tests")))
                            {
                                it.disable_recursion_pending();
                            }
                            continue;
                        }
                        if (!cppEntry.is_regular_file())
                        {
                            continue;
                        }
                        if (cppEntry.path().extension() != ".cpp")
                        {
                            continue;
                        }
                        if (!entryFile.empty() && cppEntry.path().filename().string() == entryFile)
                        {
                            continue;
                        }
                        info.sourceStems.push_back(cppEntry.path().stem().string());
                    }
                }
            }

            allPackages.push_back(info);
        }
        catch (const std::exception& e)
        {
            DEKI_LOG_WARNING(
                "CMakeGenUtils: package descriptor unreadable (%s); this package is left out of the build files",
                e.what());
        }
    }
}

std::string CapabilityDefineName(const std::string& capability)
{
    std::string define = "DEKI_CAPABILITY_";
    for (unsigned char ch : capability)
    {
        define += std::isalnum(ch) ? static_cast<char>(std::toupper(ch)) : '_';
    }
    return define;
}

std::vector<std::string> CapabilityDefinesFor(const std::set<std::string>& platformCapabilities,
                                              const std::vector<PackageEntry>& packages)
{
    std::set<std::string> defines;
    for (const auto& pkg : packages)
    {
        for (const auto& capability : pkg.capabilityDefines)
        {
            if (platformCapabilities.count(capability) != 0)
            {
                defines.insert(CapabilityDefineName(capability));
            }
        }
    }
    return { defines.begin(), defines.end() };
}

namespace
{
// A package's game code: what a game build compiles of it. Its editor code is
// built with the editor, which has every capability, and its tests apart.
std::vector<fs::path> GameSourcesOf(const fs::path& packageDir)
{
    std::vector<fs::path> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(packageDir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
    {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec))
        {
            if (it.depth() == 0 && (name == "editor" || name == "tests" || name == "generated" || name == "build"))
            {
                it.disable_recursion_pending();
            }
            else if (!name.empty() && name[0] == '.')
            {
                it.disable_recursion_pending();
            }
            continue;
        }
        const std::string ext = it->path().extension().string();
        if (ext == ".c" || ext == ".cpp" || ext == ".h" || ext == ".hpp")
        {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string ReadWhole(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}
}  // namespace

std::set<std::string> CapabilityDefinesTestedBy(const fs::path& packageDir)
{
    // A plain search, not std::regex: it runs over every active package's
    // code on every build.
    static const std::string kPrefix = "DEKI_CAPABILITY_";
    std::set<std::string> tested;
    for (const fs::path& file : GameSourcesOf(packageDir))
    {
        const std::string text = ReadWhole(file);
        for (size_t at = text.find(kPrefix); at != std::string::npos; at = text.find(kPrefix, at + 1))
        {
            size_t end = at + kPrefix.size();
            while (end < text.size() && (std::isupper(static_cast<unsigned char>(text[end])) ||
                                         std::isdigit(static_cast<unsigned char>(text[end])) || text[end] == '_'))
            {
                ++end;
            }
            if (end > at + kPrefix.size())
            {
                tested.insert(text.substr(at, end - at));
            }
        }
    }
    return tested;
}

std::vector<std::string> UndeclaredCapabilities(const std::set<std::string>& platformCapabilities,
                                                const PackageEntry& pkg, const std::set<std::string>& tested)
{
    std::vector<std::string> undeclared;
    for (const std::string& capability : platformCapabilities)
    {
        if (tested.count(CapabilityDefineName(capability)) != 0 &&
            std::find(pkg.capabilityDefines.begin(), pkg.capabilityDefines.end(), capability) ==
                pkg.capabilityDefines.end())
        {
            undeclared.push_back(capability);
        }
    }
    return undeclared;
}

namespace
{
// Whether `line` (no line break) starts a definition of main: "int main(",
// after spaces, maybe after extern "C". So a comment that mentions one does
// not count.
bool DefinesMain(const std::string& line)
{
    size_t at = line.find_first_not_of(" \t");
    auto word = [&](const char* w)
    {
        const size_t n = std::strlen(w);
        if (at == std::string::npos || line.compare(at, n, w) != 0)
        {
            return false;
        }
        at = line.find_first_not_of(" \t", at + n);
        return true;
    };
    if (word("extern"))
    {
        if (!word("\"C\""))
        {
            return false;
        }
    }
    return word("int") && word("main") && at != std::string::npos && line[at] == '(';
}

// A conditional preprocessor line, as far as the main() check needs it:
// whether it opens a block (#if, #ifdef, #ifndef), switches one (#elif,
// #else) or closes it (#endif), and whether its block compiles only with
// SIMULATOR defined, which a build from this release never defines.
enum class Directive
{
    None,
    Open,
    Else,
    Elif,
    Close
};

Directive ReadDirective(const std::string& line, bool& onlyWithSimulator)
{
    onlyWithSimulator = false;
    size_t at = line.find_first_not_of(" \t");
    if (at == std::string::npos || line[at] != '#')
    {
        return Directive::None;
    }
    at = line.find_first_not_of(" \t", at + 1);
    if (at == std::string::npos)
    {
        return Directive::None;
    }
    size_t end = at;
    while (end < line.size() && std::isalpha(static_cast<unsigned char>(line[end])))
    {
        ++end;
    }
    const std::string name = line.substr(at, end - at);
    std::string expression = line.substr(end);
    const size_t comment = expression.find("//");
    if (comment != std::string::npos)
    {
        expression.erase(comment);
    }
    std::string squeezed;
    for (char c : expression)
    {
        if (c != ' ' && c != '\t' && c != '\r')
        {
            squeezed += c;
        }
    }
    if (name == "ifdef")
    {
        onlyWithSimulator = squeezed == "SIMULATOR";
        return Directive::Open;
    }
    if (name == "if")
    {
        // defined(SIMULATOR) on its own or joined by && only: false whenever
        // SIMULATOR is not defined. Anything with || or a negation is left
        // as possibly compiled.
        const bool tests = squeezed.find("defined(SIMULATOR)") != std::string::npos ||
                           squeezed.find("definedSIMULATOR") != std::string::npos || squeezed == "SIMULATOR";
        onlyWithSimulator = tests && squeezed.find("||") == std::string::npos && squeezed.find('!') == std::string::npos;
        return Directive::Open;
    }
    if (name == "ifndef")
    {
        return Directive::Open;
    }
    if (name == "elif" || name == "elifdef" || name == "elifndef")
    {
        return Directive::Elif;
    }
    if (name == "else")
    {
        return Directive::Else;
    }
    if (name == "endif")
    {
        return Directive::Close;
    }
    return Directive::None;
}
}  // namespace

std::vector<std::string> SourcesDefiningMain(const fs::path& packageDir)
{
    std::vector<std::string> files;
    for (const fs::path& file : GameSourcesOf(packageDir))
    {
        const std::string text = ReadWhole(file);
        if (text.find("main") == std::string::npos)
        {
            continue;
        }
        // Each open conditional block, and whether it compiles only with
        // SIMULATOR defined: a main() in one is left out of a build from this
        // release, which defines DEKI_HOSTED instead, so it does not count.
        struct Block
        {
            bool onlySimulator;  // the block's test is SIMULATOR alone
            bool skipped;        // this part of it compiles only with SIMULATOR
        };
        std::vector<Block> blocks;
        std::istringstream lines(text);
        for (std::string line; std::getline(lines, line);)
        {
            bool onlySimulator = false;
            switch (ReadDirective(line, onlySimulator))
            {
                case Directive::Open: blocks.push_back({ onlySimulator, onlySimulator }); continue;
                case Directive::Else:
                    if (!blocks.empty())
                    {
                        blocks.back().skipped = false;  // the #else of a SIMULATOR test compiles
                    }
                    continue;
                case Directive::Elif:
                    if (!blocks.empty())
                    {
                        blocks.back().skipped = false;
                    }
                    continue;
                case Directive::Close:
                    if (!blocks.empty())
                    {
                        blocks.pop_back();
                    }
                    continue;
                case Directive::None: break;
            }
            const bool compiled =
                std::none_of(blocks.begin(), blocks.end(), [](const Block& b) { return b.skipped; });
            if (compiled && DefinesMain(line))
            {
                std::error_code ec;
                files.push_back(fs::relative(file, packageDir, ec).generic_string());
                break;
            }
        }
    }
    return files;
}

std::set<std::string> TextureFilteringCapabilities(const std::vector<PackageEntry>& packages)
{
    std::set<std::string> capabilities;
    for (const auto& pkg : packages)
    {
        capabilities.insert(pkg.filtersTexturesWith.begin(), pkg.filtersTexturesWith.end());
    }
    return capabilities;
}

std::vector<PackageEntry> ScanPackageManifests(const std::string& projectPath)
{
    std::vector<PackageEntry> allPackages;
    if (!projectPath.empty())
    {
        ScanPackagesDir(fs::path(GetPackagesDirectory(projectPath)), allPackages);
    }
    return allPackages;
}

// ============================================================================
// Active package resolution
// ============================================================================

std::vector<std::string> MissingCapabilities(const PackageEntry& pkg, const std::set<std::string>& platformCapabilities)
{
    // No requirements means it builds anywhere. No capability set means there
    // is no target (the editor's own build, which loads everything), so every
    // package passes.
    if (pkg.platformRequires.empty() || platformCapabilities.empty())
    {
        return {};
    }

    std::vector<std::string> missing;
    for (const auto& need : pkg.platformRequires)
    {
        if (platformCapabilities.count(need) == 0)
        {
            missing.push_back(need);
        }
    }
    return missing;
}

bool PackageSupportsPlatform(const PackageEntry& pkg, const std::set<std::string>& platformCapabilities)
{
    return MissingCapabilities(pkg, platformCapabilities).empty();
}

bool FeatureInBuild(const FeatureEntry& feature, const std::vector<std::string>& packageDefines)
{
    // Without a feature plan every feature is compiled in (the package headers
    // define them all), so every feature's packages are needed.
    const bool configured = std::find(packageDefines.begin(), packageDefines.end(),
                                      "DEKI_PACKAGE_FEATURES_CONFIGURED") != packageDefines.end();
    if (!configured || feature.required || feature.buildDefine.empty())
    {
        return true;
    }
    return std::find(packageDefines.begin(), packageDefines.end(), feature.buildDefine) != packageDefines.end();
}

std::vector<std::string> RequiredPackagesInBuild(const PackageEntry& pkg,
                                                 const std::vector<std::string>& packageDefines)
{
    std::vector<std::string> out = pkg.dekiDeps;
    for (const auto& feature : pkg.features)
    {
        if (!FeatureInBuild(feature, packageDefines))
        {
            continue;
        }
        for (const auto& dep : feature.requiredPackages)
        {
            if (std::find(out.begin(), out.end(), dep) == out.end())
            {
                out.push_back(dep);
            }
        }
    }
    return out;
}

std::set<std::string> ResolveActivePackages(const std::vector<PackageEntry>& allPackages,
                                            const std::vector<std::string>& packageDefines,
                                            const std::set<std::string>& platformCapabilities)
{
    // Seed with packages whose buildDefine appears in packageDefines, skipping
    // any this target cannot satisfy.
    std::set<std::string> activeIds;
    for (const auto& pkg : allPackages)
    {
        if (!PackageSupportsPlatform(pkg, platformCapabilities))
        {
            continue;
        }
        if (std::find(packageDefines.begin(), packageDefines.end(), pkg.buildDefine) != packageDefines.end())
        {
            activeIds.insert(pkg.id);
        }
    }

    // Resolve transitive deki:* dependencies (loop until nothing changes).
    // The platform filter applies to dependencies too: dekiDeps holds names
    // from every platform's "dependencies" list, so without the filter a
    // desktop-only dependency would be pulled into a firmware build.
    std::map<std::string, const PackageEntry*> byId;
    for (const auto& pkg : allPackages)
    {
        byId[pkg.id] = &pkg;
    }

    bool changed = true;
    while (changed)
    {
        changed = false;
        for (const auto& pkg : allPackages)
        {
            if (activeIds.count(pkg.id) == 0)
            {
                continue;
            }
            for (const auto& dep : RequiredPackagesInBuild(pkg, packageDefines))
            {
                auto it = byId.find(dep);
                if (it != byId.end() && !PackageSupportsPlatform(*it->second, platformCapabilities))
                {
                    continue;
                }
                if (activeIds.insert(dep).second)
                {
                    changed = true;
                }
            }
        }
    }

    return activeIds;
}

// ============================================================================
// CMake snippet emission
// ============================================================================

void EmitEngineCoreSourceCollection(std::ostream& out, const std::vector<std::string>& extraExclusions)
{
    // CONFIGURE_DEPENDS on every glob here: an engine or package update that
    // adds a file would otherwise stay out of a build configured before it.
    // src/hosted/ is the entry of a game run as a program (main() and its
    // file system): a board has its own.
    out << "# Engine core sources (excluding packages, generated, editor, hosted)\n";
    out << "file(GLOB_RECURSE DEKI_ENGINE_SOURCES CONFIGURE_DEPENDS \"${DEKI_ENGINE_PATH}/src/*.cpp\")\n";
    out << "list(FILTER DEKI_ENGINE_SOURCES EXCLUDE REGEX \".*/packages/.*\")\n";
    out << "list(FILTER DEKI_ENGINE_SOURCES EXCLUDE REGEX \".*/generated/.*\")\n";
    out << "list(FILTER DEKI_ENGINE_SOURCES EXCLUDE REGEX \".*/editor/.*\")\n";
    out << "list(FILTER DEKI_ENGINE_SOURCES EXCLUDE REGEX \".*/src/hosted/.*\")\n";

    for (const auto& exclusion : extraExclusions)
    {
        out << "list(FILTER DEKI_ENGINE_SOURCES EXCLUDE REGEX \"" << exclusion << "\")\n";
    }

    out << "\n";
}

std::string CMakeIdentifier(const std::string& name)
{
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name)
    {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        out.push_back(ok ? static_cast<char>(c) : '_');
    }
    if (out.empty() || (out[0] >= '0' && out[0] <= '9'))
    {
        out.insert(out.begin(), '_');
    }
    return out;
}

std::string EscapeCppString(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string EscapeCMakeString(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '$': out += "\\$"; break;
            case ';': out += "\\;"; break;
            case '\n': out += "\\n"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

void EmitActivePackageSources(std::ostream& out, const std::vector<PackageEntry>& allPackages,
                              const std::set<std::string>& activePackageIds,
                              const std::vector<std::string>& sourceExcludeRegexes, bool excludeGenerated)
{
    out << "# Active package sources (auto-detected from project configuration)\n";
    for (const auto& pkg : allPackages)
    {
        if (activePackageIds.count(pkg.id) == 0)
        {
            continue;
        }
        // The directory name was checked by SafeNames at install, but as a
        // CMake variable suffix it is reduced to identifier characters, as
        // string(MAKE_C_IDENTIFIER) would.
        const std::string var = "DEKI_PKG_SOURCES_" + CMakeIdentifier(pkg.dirName);
        out << "file(GLOB_RECURSE " << var << " CONFIGURE_DEPENDS \"${DEKI_PROJECT_PATH}/packages/" << pkg.dirName
            << "/*.cpp\""
            << " \"${DEKI_PROJECT_PATH}/packages/" << pkg.dirName << "/*.c\")\n";
        out << "list(FILTER " << var << " EXCLUDE REGEX \".*/editor/.*\")\n";
        out << "list(FILTER " << var << " EXCLUDE REGEX \".*/tests/.*\")\n";
        if (excludeGenerated)
        {
            out << "list(FILTER " << var << " EXCLUDE REGEX \".*/generated/.*\")\n";
        }
        // The stripped components. The regexes name their package directory,
        // so applying every one to every package is harmless.
        for (const auto& rx : sourceExcludeRegexes)
        {
            out << "list(FILTER " << var << " EXCLUDE REGEX \"" << EscapeCMakeString(rx) << "\")\n";
        }
        out << "list(APPEND DEKI_ENGINE_SOURCES ${" << var << "})\n";
    }

    // Per-package include dirs (so generated/*.gen.cpp can find package headers)
    out << "\n# Per-package include directories\n";
    out << "set(DEKI_PACKAGE_INCLUDE_DIRS\n";
    for (const auto& pkg : allPackages)
    {
        if (activePackageIds.count(pkg.id) == 0)
        {
            continue;
        }
        out << "    \"${DEKI_PROJECT_PATH}/packages/" << pkg.dirName << "\"\n";
    }
    out << ")\n";
}

void EmitGeneratedFileSources(std::ostream& out, const std::string& enginePath,
                              const std::vector<PackageEntry>& allPackages,
                              const std::set<std::string>& activePackageIds)
{
    // Generated file stem -> package id.
    std::map<std::string, std::string> genStemToPackageId;
    for (const auto& pkg : allPackages)
    {
        if (!pkg.filePrefix.empty())
        {
            genStemToPackageId[pkg.filePrefix + "Registration"] = pkg.id;
        }
        for (const auto& stem : pkg.sourceStems)
        {
            genStemToPackageId[stem] = pkg.id;
        }
    }

    // Add the generated files of the core and of active packages.
    out << "\n# Generated files (core + active packages only)\n";
    fs::path genDir = fs::path(enginePath) / "include" / "deki" / "generated";
    if (fs::exists(genDir))
    {
        for (const auto& genEntry : fs::directory_iterator(genDir))
        {
            std::string filename = genEntry.path().filename().string();
            auto genPos = filename.find(".gen.cpp");
            if (genPos == std::string::npos)
            {
                continue;
            }

            // ComponentFactory.gen.cpp holds static initializers, not needed on firmware.
            if (filename == "ComponentFactory.gen.cpp")
            {
                continue;
            }
            // CoreRegistration.gen.cpp must stay in: it defines
            // DekiCoreRegisterComponents() and the core components' factory
            // entries, which AllComponentRegistration only declares and calls.
            // Its static registrar runs once (a `done` flag), so it costs
            // nothing next to the explicit call.
            //
            // AllPackageInit.gen.cpp is left out: deki_package_init.gen.cpp
            // provides init and shutdown.
            if (filename == "AllPackageInit.gen.cpp")
            {
                continue;
            }

            // Always in: it provides DekiRegisterAllComponents.
            if (filename == "AllComponentRegistration.gen.cpp")
            {
                out << "list(APPEND DEKI_ENGINE_SOURCES \"${DEKI_ENGINE_PATH}/include/deki/generated/" << filename
                    << "\")\n";
                continue;
            }

            std::string stem = filename.substr(0, genPos);

            auto it = genStemToPackageId.find(stem);
            if (it != genStemToPackageId.end())
            {
                // A package's file: only when the package is active.
                if (activePackageIds.count(it->second) > 0)
                {
                    // Per-package Registration files: Deki::Engine does not call them.
                    if (stem.size() >= 12 && stem.compare(stem.size() - 12, 12, "Registration") == 0)
                    {
                        continue;
                    }
                    out << "list(APPEND DEKI_ENGINE_SOURCES \"${DEKI_ENGINE_PATH}/include/deki/generated/" << filename
                        << "\")\n";
                }
            }
            else
            {
                // A core file, not tied to a package: always in.
                out << "list(APPEND DEKI_ENGINE_SOURCES \"${DEKI_ENGINE_PATH}/include/deki/generated/" << filename
                    << "\")\n";
            }
        }
    }
}

void EmitProjectSourceCollection(std::ostream& out, const std::string& relPath, bool excludePluginExports)
{
    out << "\n";
    out << "# Project sources\n";
    out << "file(GLOB_RECURSE PROJECT_SOURCES CONFIGURE_DEPENDS\n";
    out << "    \"" << relPath << "/*.cpp\"\n";
    out << "    \"" << relPath << "/*.c\"\n";
    out << ")\n";

    if (excludePluginExports)
    {
        out << "list(FILTER PROJECT_SOURCES EXCLUDE REGEX \".*PluginExports\\\\.cpp$\")\n";
    }
}

void EmitPlatformDefines(std::ostream& out, const std::string& targetName, const PlatformConfig& config,
                         const std::vector<std::string>& packageDefines, const std::vector<std::string>& skipDefines)
{
    out << "\n";
    out << "# Platform defines (" << config.displayName << ")\n";
    out << "target_compile_definitions(" << targetName << " PRIVATE\n";

    for (const auto& define : config.defines)
    {
        out << "    " << define << "\n";
    }

    // There is no screen size or colour format define: a device renders at
    // whatever its display reports.

    // Package feature defines, minus skipDefines.
    std::set<std::string> skipSet(skipDefines.begin(), skipDefines.end());
    out << "    DEKI_PACKAGE_FEATURES_CONFIGURED\n";
    for (const auto& define : packageDefines)
    {
        if (skipSet.count(define) > 0)
        {
            continue;
        }
        out << "    " << define << "\n";
    }

    out << ")\n";
}

void EmitIncludePaths(std::ostream& out, const std::string& targetName, const std::string& projectSrcRelPath,
                      const std::vector<std::string>& extraPaths)
{
    out << "\n";
    out << "target_include_directories(" << targetName << " PRIVATE\n";
    // include/ is the public API; src/ is needed here because this target compiles
    // the engine's own sources, which reach its internal headers.
    out << "    \"${DEKI_ENGINE_PATH}/include\"\n";
    out << "    \"${DEKI_ENGINE_PATH}/src\"\n";
    out << "    \"${DEKI_ENGINE_PATH}/third_party\"\n";
    out << "    \"${DEKI_PROJECT_PATH}/packages\"\n";
    out << "    ${DEKI_PACKAGE_INCLUDE_DIRS}\n";
    out << "    \"" << projectSrcRelPath << "\"\n";

    for (const auto& path : extraPaths)
    {
        out << "    " << path << "\n";
    }

    out << ")\n";
}

// ============================================================================
// File utility
// ============================================================================

bool WriteIfChanged(const fs::path& filePath, const std::string& content)
{
    if (fs::exists(filePath))
    {
        std::ifstream existing(filePath, std::ios::binary);
        std::string existingContent((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
        if (existingContent == content)
        {
            return true;
        }
    }
    std::ofstream file(filePath, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }
    file << content;
    return true;
}

std::string GeneratePackageInitFile(const fs::path& outputDir, const std::vector<PackageEntry>& allPackages,
                                    const std::set<std::string>& activePackageIds, const fs::path& projectSrcPath,
                                    bool engineDefinesSystemInit, bool gameShutsPackagesDown)
{
    std::ostringstream cpp;
    cpp << "// AUTO-GENERATED by Deki Editor - calls package registration for embedded builds\n";
    cpp << "#include <deki/reflection/ComponentFactory.h>\n";

    // Project components register through the codegen's PluginRegistration.gen.cpp,
    // exactly like a package does through its own registration unit.
    const bool hasProjectSources = !projectSrcPath.empty() && fs::exists(projectSrcPath);
    if (hasProjectSources)
    {
        cpp << "extern void PluginRegisterComponents();\n";
    }
    cpp << "\n";

    // Registration functions, named by PACKAGE_PREFIX from package.cmake.
    for (const auto& pkg : allPackages)
    {
        if (activePackageIds.count(pkg.id) == 0)
        {
            continue;
        }
        if (pkg.packagePrefix.empty())
        {
            continue;
        }
        cpp << "extern void " << pkg.packagePrefix << "RegisterComponents();\n";
    }

    // Packages with a system init. Collected up front so
    // DekiRegisterProjectPackages() can call them itself when the engine core
    // already provides DekiInitPackageSystems().
    std::vector<const PackageEntry*> initPackages;
    for (const auto& pkg : allPackages)
    {
        if (activePackageIds.count(pkg.id) == 0)
        {
            continue;
        }
        if (!pkg.hasSystemInit || pkg.packagePrefix.empty())
        {
            continue;
        }
        initPackages.push_back(&pkg);
    }

    for (const auto* pkg : initPackages)
    {
        cpp << "extern void " << pkg->packagePrefix << "InitSystem();\n";
        cpp << "extern void " << pkg->packagePrefix << "ShutdownSystem();\n";
    }

    cpp << "\nextern \"C\" void DekiRegisterProjectPackages(void)\n";
    cpp << "{\n";
    for (const auto& pkg : allPackages)
    {
        if (activePackageIds.count(pkg.id) == 0)
        {
            continue;
        }
        if (pkg.packagePrefix.empty())
        {
            continue;
        }
        cpp << "    " << pkg.packagePrefix << "RegisterComponents();\n";
    }

    // Static desktop build: the engine core defines an empty
    // DekiInitPackageSystems(), so this file must not define it again (link
    // error). Start the package systems here instead, after component
    // registration, in the same order as DekiInitPackageSystems() below.
    if (engineDefinesSystemInit && !initPackages.empty())
    {
        cpp << "\n    // Package systems (engine core owns the DekiInitPackageSystems() symbol)\n";
        for (const auto* pkg : initPackages)
        {
            cpp << "    " << pkg->packagePrefix << "InitSystem();\n";
        }
    }

    // Project components (the generated PluginRegistration.gen.cpp).
    if (hasProjectSources)
    {
        cpp << "\n    PluginRegisterComponents();\n";
    }

    cpp << "}\n";

    // Firmware builds: the engine leaves out the empty stub, so define the
    // system init and shutdown entry points here (Deki::Engine::Initialize
    // and Shutdown call them).
    if (!engineDefinesSystemInit)
    {
        cpp << "\nextern \"C\" void DekiInitPackageSystems(void)\n";
        cpp << "{\n";
        for (const auto* pkg : initPackages)
        {
            cpp << "    " << pkg->packagePrefix << "InitSystem();\n";
        }
        cpp << "}\n";

        // Shutdown runs in reverse order.
        cpp << "\nextern \"C\" void DekiShutdownPackageSystems(void)\n";
        cpp << "{\n";
        for (auto it = initPackages.rbegin(); it != initPackages.rend(); ++it)
        {
            cpp << "    " << (*it)->packagePrefix << "ShutdownSystem();\n";
        }
        cpp << "}\n";
    }
    else if (gameShutsPackagesDown)
    {
        // The systems started in DekiRegisterProjectPackages() above, so the
        // engine's call to start them has nothing left to do. Its call to shut
        // them down, from Engine::Shutdown, is what frees their memory.
        cpp << "\nextern \"C\" void DekiInitPackageSystems(void)\n";
        cpp << "{\n";
        cpp << "}\n";
        cpp << "\nextern \"C\" void DekiShutdownPackageSystems(void)\n";
        cpp << "{\n";
        for (auto it = initPackages.rbegin(); it != initPackages.rend(); ++it)
        {
            cpp << "    " << (*it)->packagePrefix << "ShutdownSystem();\n";
        }
        cpp << "}\n";
    }

    fs::path outPath = outputDir / "deki_package_init.gen.cpp";
    WriteIfChanged(outPath, cpp.str());
    return outPath.string();
}

std::vector<std::string> ReadProjectTags(const std::string& projectPath)
{
    std::vector<std::string> tags;
    if (projectPath.empty())
    {
        return tags;
    }
    std::ifstream in(fs::path(projectPath) / "deki.json");
    if (!in)
    {
        return tags;
    }
    try
    {
        nlohmann::json j;
        in >> j;
        tags = TagsFrom(j);
    }
    catch (const std::exception& e)
    {
        DEKI_LOG_WARNING("deki.json: could not read tags (%s)", e.what());
    }
    return tags;
}

TransformWidth ResolveTransformWidth(const std::vector<PackageEntry>& allPackages,
                                     const std::set<std::string>& activeIds,
                                     const std::vector<std::string>& projectTags, std::string* why)
{
    TransformWidth width = TransformWidth::None;
    std::vector<std::string> declared2d;
    std::vector<std::string> declared3d;

    auto consider = [&](const std::string& who, const std::vector<std::string>& tags)
    {
        for (const auto& tag : tags)
        {
            if (tag == "transform_3d")
            {
                width = TransformWidth::ThreeD;
                declared3d.push_back(who);
            }
            else if (tag == "transform_2d")
            {
                if (width == TransformWidth::None)
                {
                    width = TransformWidth::TwoD;
                }
                declared2d.push_back(who);
            }
        }
    };

    for (const auto& pkg : allPackages)
    {
        if (activeIds.count(pkg.id) == 0)
        {
            continue;
        }
        std::vector<std::string> declared = pkg.tags;
        // A required feature whose define is the switch itself is the same
        // declaration, in the descriptor's own vocabulary.
        for (const auto& feat : pkg.features)
        {
            if (!feat.required)
            {
                continue;
            }
            if (feat.buildDefine == "DEKI_TRANSFORM_2D")
            {
                declared.push_back("transform_2d");
            }
            else if (feat.buildDefine == "DEKI_TRANSFORM_3D")
            {
                declared.push_back("transform_3d");
            }
        }
        consider(pkg.id, declared);
    }
    consider("the project", projectTags);

    if (why)
    {
        auto join = [](const std::vector<std::string>& v)
        {
            std::string s;
            for (size_t i = 0; i < v.size(); ++i)
            {
                s += (i ? ", " : "") + v[i];
            }
            return s;
        };
        switch (width)
        {
            case TransformWidth::None:
                *why = "no transform: nothing active declares transform_2d or transform_3d";
                break;
            case TransformWidth::TwoD: *why = "2D transform, declared by " + join(declared2d); break;
            case TransformWidth::ThreeD:
                *why = "3D transform, declared by " + join(declared3d);
                if (!declared2d.empty())
                {
                    *why += " (2D by " + join(declared2d) + ")";
                }
                break;
        }
    }
    return width;
}

TransformWidth ResolveProjectTransformWidth(const std::vector<PackageEntry>& allPackages,
                                            const std::vector<std::string>& projectTags, std::string* why)
{
    // Every installed package counts, not the ones a particular target keeps.
    // See the header for why the layout must not vary by target.
    std::set<std::string> everything;
    for (const auto& pkg : allPackages)
    {
        everything.insert(pkg.id);
    }
    return ResolveTransformWidth(allPackages, everything, projectTags, why);
}

std::vector<std::string> TransformDefines(TransformWidth width)
{
    std::vector<std::string> defines;
    if (width == TransformWidth::TwoD || width == TransformWidth::ThreeD)
    {
        defines.push_back("DEKI_TRANSFORM_2D");
    }
    if (width == TransformWidth::ThreeD)
    {
        defines.push_back("DEKI_TRANSFORM_3D");
    }
    return defines;
}

}  // namespace CMakeGen
}  // namespace DekiEditor
