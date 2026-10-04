#pragma once

// PluginToolchain: picks the compiler that builds project plugin and package
// DLLs.
//
// The plugin links against deki-engine-core and passes C++ objects
// (std::string, Scene*, std::vector) across the DLL boundary. MSVC and GNU
// differ in name mangling, std:: layouts and exception handling, so a plugin
// built with another ABI than the editor fails to link or crashes at run time
// in ways that look nothing like a toolchain problem.
//
// So the ABI is fixed by how the editor itself was compiled, read off the
// preprocessor. Everything here finds a concrete compiler with that ABI.

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

    /// Finds a compiler matching HostAbi(), cached after the first call.
    /// Never throws; check .valid and report .error.
    static const PluginToolchainInfo& Get();

    /// Lowercase text a CMakeCache CMAKE_CXX_COMPILER entry must contain for
    /// the cache to count as built by this toolchain. Detects a cache left
    /// by a different compiler.
    static std::string CacheCompilerMarker();

    /// Human-readable install hint for when resolution fails.
    static std::string InstallHint();

    /// Finds an executable on PATH. `stem` has no platform suffix ("ninja",
    /// not "ninja.exe"). Returns "" when not found.
    static std::string FindOnPath(const std::string& stem);
};

}  // namespace DekiEditor
