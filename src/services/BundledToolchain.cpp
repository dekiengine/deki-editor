#include "BundledToolchain.h"

#include <deki-editor/EditorPaths.h>

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

/// <toolchain>/bin/<name><suffix> if it exists, else "".
std::string BundledPath(const std::string& name)
{
    const fs::path candidate =
        fs::path(BundledToolchainDir()) / "bin" / (name + kExeSuffix);
    std::error_code ec;
    if (fs::exists(candidate, ec) && !ec)
        return candidate.string();
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
    return bundled.empty() ? name : bundled;
}

std::string ProjectCxxCompiler()
{
    // Bundled first — the whole point is not needing a system compiler.
    for (const char* name : { "g++", "g++-16" })
    {
        const std::string bundled = BundledPath(name);
        if (!bundled.empty())
            return bundled;
    }
    // Otherwise the compiler this editor was built with. Packages are dlopen'd
    // into this process, so anything else risks two standard libraries meeting
    // across that boundary.
    return DEKI_EDITOR_CXX_COMPILER;
}

std::string ProjectCCompiler()
{
    for (const char* name : { "gcc", "gcc-16" })
    {
        const std::string bundled = BundledPath(name);
        if (!bundled.empty())
            return bundled;
    }
    return DEKI_EDITOR_C_COMPILER;
}

}  // namespace DekiEditor
