#include "Prerequisites.h"
#include "BundledToolchain.h"
#include "PluginToolchain.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace DekiEditor
{

namespace
{

/// Run a command and return its first line of output, or "" if it could not run.
std::string RunCapture(const std::string& cmd)
{
#ifdef _WIN32
    FILE* pipe = _popen((cmd + " 2>NUL").c_str(), "r");
#else
    FILE* pipe = popen((cmd + " 2>/dev/null").c_str(), "r");
#endif
    if (!pipe)
        return "";

    std::string out;
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe))
        out += buffer;

#ifdef _WIN32
    const int rc = _pclose(pipe);
#else
    const int rc = pclose(pipe);
#endif

    if (rc != 0)
        return "";

    // First line, trimmed.
    const size_t nl = out.find_first_of("\r\n");
    if (nl != std::string::npos)
        out = out.substr(0, nl);
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t'))
        out.pop_back();
    return out;
}

bool ToolExists(const std::string& tool)
{
#ifdef _WIN32
    return !RunCapture("where " + tool).empty();
#else
    return !RunCapture("command -v " + tool).empty();
#endif
}

/// Leading integer of a version string ("16.0.1" -> 16). -1 if unparseable.
int MajorVersion(const std::string& v)
{
    size_t i = 0;
    while (i < v.size() && !isdigit(static_cast<unsigned char>(v[i])))
        ++i;
    if (i >= v.size()) return -1;
    return std::atoi(v.c_str() + i);
}

/// Find a g++ new enough for the C++26 reflection codegen. Mirrors the
/// find_program order the generated CMakeLists uses, so what we report and what
/// the build actually picks cannot disagree.
Prerequisite CheckReflectionCompiler()
{
    Prerequisite p;
    p.name = "GCC 16+ (compiles projects and runs the reflection codegen)";
    p.why = "builds project packages and the plugin, and generates component metadata";
#ifdef _WIN32
    p.install = "Install MSYS2 and `pacman -S mingw-w64-x86_64-gcc`, or set DEKI_GXX16 "
                "to a g++ 16+ binary";
#elif defined(__APPLE__)
    p.install = "brew install gcc@16  (Apple clang cannot do C++26 static reflection)";
#else
    p.install = "sudo add-apt-repository ppa:ubuntu-toolchain-r/test && "
                "sudo apt install g++-16   (no current distro ships GCC 16 by default)";
#endif

    // The compiler project builds will actually use: the bundled one if this is
    // a packaged install, otherwise the one the editor was built with (packages
    // are dlopen'd into this process, so it has to match). Probing anything else
    // would report a tool the build never invokes.
    const std::string projectCxx = ProjectCxxCompiler();
    for (const std::string& candidate : { projectCxx, std::string("g++-16"), std::string("g++") })
    {
        if (!ToolExists(candidate))
            continue;
        const std::string version = RunCapture(candidate + " -dumpversion");
        if (MajorVersion(version) >= 16)
        {
            p.found = candidate + " " + version;
            p.ok = true;
            return p;
        }
    }
    return p;
}

/// The compiler PluginToolchain resolves for the editor's ABI - the one the
/// package and plugin builds actually invoke.
[[maybe_unused]] Prerequisite CheckPluginToolchain()
{
    const PluginToolchainInfo& tc = PluginToolchain::Get();
    Prerequisite p;
    p.name = "C++ compiler for packages and the plugin";
    p.why = "compiles project packages and the plugin with the editor's ABI";
    p.install = PluginToolchain::InstallHint();
    p.required = true;
    p.ok = tc.valid;
    if (tc.valid)
        p.found = tc.displayName + " (" + tc.cxxCompiler + ")";
    return p;
}

Prerequisite CheckTool(const std::string& tool, const std::string& why,
                       const std::string& install, bool required,
                       const std::string& versionArg = "--version")
{
    Prerequisite p;
    p.name = tool;
    p.why = why;
    p.install = install;
    p.required = required;
    if (ToolExists(tool))
    {
        p.found = RunCapture(tool + " " + versionArg);
        if (p.found.empty()) p.found = "present";
        p.ok = true;
    }
    return p;
}

}  // namespace

std::vector<Prerequisite> CheckPrerequisites()
{
    std::vector<Prerequisite> results;

#ifdef _WIN32
    const char* cmakeInstall = "winget install Kitware.CMake";
    const char* gitInstall = "winget install Git.Git";
#elif defined(__APPLE__)
    const char* cmakeInstall = "brew install cmake";
    const char* gitInstall = "xcode-select --install";
#else
    const char* cmakeInstall = "sudo apt install cmake";
    const char* gitInstall = "sudo apt install git";
#endif

    results.push_back(CheckReflectionCompiler());
    results.push_back(CheckTool(ResolveTool("cmake"), "configures and drives every project build",
                                cmakeInstall, true));
    results.push_back(CheckTool("git", "installs packages declared by the project",
                                gitInstall, true));

#ifdef _WIN32
    // ninja and sccache ship next to the executable on Windows. The compiler
    // that builds packages and the plugin must match the editor's own ABI: an
    // MSVC-built editor needs clang-cl or cl, while a MinGW-built one uses the
    // GCC checked above. Demanding clang-cl unconditionally refused machines
    // that build fine and skipped the first build, leaving every component
    // unresolved.
    if constexpr (PluginToolchain::HostAbi() == PluginAbi::Msvc)
        results.push_back(CheckPluginToolchain());
#else
    results.push_back(CheckTool(ResolveTool("ninja"), "the build generator the editor drives",
#if defined(__APPLE__)
                                "brew install ninja", true));
#else
                                "sudo apt install ninja-build", true));
#endif
    // No separate host compiler entry: project builds are pinned to the
    // editor's own compiler (checked above), so requiring a system c++ as well
    // would report a tool the build never invokes.
    // Not required: without curl the registry is unreachable, so only packages
    // that already carry an explicit URL can be installed. Everything else still
    // works, so this is a warning rather than a hard stop.
    results.push_back(CheckTool("curl", "fetches the Deki package registry",
#if defined(__APPLE__)
                                "brew install curl", false));
#else
                                "sudo apt install curl", false));
#endif
#endif

    return results;
}

bool PrerequisitesSatisfied(const std::vector<Prerequisite>& results)
{
    for (const auto& p : results)
        if (p.required && !p.ok)
            return false;
    return true;
}

std::vector<std::string> DescribeMissing(const std::vector<Prerequisite>& results)
{
    std::vector<std::string> lines;
    for (const auto& p : results)
    {
        if (p.ok) continue;
        lines.push_back((p.required ? "MISSING: " : "optional, missing: ") + p.name);
        lines.push_back("    needed for: " + p.why);
        lines.push_back("    install:    " + p.install);
    }
    return lines;
}

}  // namespace DekiEditor
