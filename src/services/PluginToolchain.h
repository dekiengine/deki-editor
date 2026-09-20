#pragma once

// PluginToolchain - picks the compiler used to build project plugin/package DLLs.
//
// The plugin is not a standalone program: it links against deki-engine-core and
// passes C++ objects (std::string, Scene*, std::vector) across the DLL boundary.
// MSVC and GNU disagree on name mangling, std:: layouts and exception handling,
// so a plugin built with a different ABI than the editor will fail to link or
// crash at runtime in ways that look nothing like a toolchain problem.
//
// The required ABI is therefore not a preference - it is fixed by how the editor
// itself was compiled, which we can read off the preprocessor. Everything here
// resolves a concrete compiler that satisfies that ABI.

#include <string>

namespace DekiEditor
{

enum class PluginAbi
{
    Msvc,  // MSVC ABI - cl.exe or clang-cl. Needs the vcvars environment.
    Gnu    // Itanium ABI - g++ / clang++ (MinGW on Windows, native elsewhere).
};

struct PluginToolchainInfo
{
    PluginAbi abi = PluginAbi::Gnu;
    std::string cCompiler;      // absolute path, forward slashes
    std::string cxxCompiler;    // absolute path, forward slashes
    std::string displayName;    // e.g. "clang-cl (LLVM)", "g++ (MinGW-w64)"
    bool needsMsvcEnv = false;  // true only for the MSVC ABI
    bool valid = false;
    std::string error;  // populated when valid == false
};

class PluginToolchain
{
public:
    /// The ABI the editor binary itself was compiled with. Plugins must match.
    static constexpr PluginAbi HostAbi()
    {
        // clang-cl defines _MSC_VER too, and that is correct here: it targets
        // the MSVC ABI, so it belongs in the Msvc bucket alongside cl.exe.
#if defined(_MSC_VER)
        return PluginAbi::Msvc;
#else
        return PluginAbi::Gnu;
#endif
    }

    /// Resolve a compiler matching HostAbi(). Cached after the first call.
    /// Never throws; check .valid and report .error.
    static const PluginToolchainInfo& Get();

    /// Substring that must appear in a CMakeCache CMAKE_CXX_COMPILER entry for
    /// the cache to be considered built by this toolchain (lowercase).
    /// Used to detect a cache left behind by a different compiler.
    static std::string CacheCompilerMarker();

    /// Human-readable install hint for when resolution fails.
    static std::string InstallHint();

    /// Locate an executable on PATH. `stem` is given without the platform
    /// suffix ("ninja", not "ninja.exe"). Returns "" when not found.
    static std::string FindOnPath(const std::string& stem);
};

}  // namespace DekiEditor
