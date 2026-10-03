#pragma once

// Platform naming for the shared libraries the editor loads at runtime
// (package libraries and the project's component plugin).
//
// CMake decorates a SHARED/PACKAGE library differently per platform:
//
//   target "deki-2d"            Windows: deki-2d.dll
//                               Linux:   libdeki-2d.so
//                               macOS:   libdeki-2d.dylib
//
// Anything that turns a CMake target name into a filename, or a filename back
// into a target name, has to agree on those rules - so they live here rather
// than being open-coded at each call site. Before this existed the loaders
// tested `filename.rfind("deki-") == 0` and `extension() == ".dll"`, which
// rejects every package on ELF/Mach-O because of the "lib" prefix.

#include <algorithm>
#include <string>

namespace DekiEditor
{

// Extension CMake gives a SHARED/PACKAGE library on this platform.
#ifdef _WIN32
inline constexpr const char* kSharedLibExt = ".dll";
#elif defined(__APPLE__)
inline constexpr const char* kSharedLibExt = ".dylib";
#else
inline constexpr const char* kSharedLibExt = ".so";
#endif

// Prefix CMake puts in front of a SHARED/PACKAGE library.
//
// This follows the *toolchain*, not the OS: only the MSVC ABI leaves a shared
// library undecorated. GNU toolchains add "lib" everywhere they run, so a
// MinGW build on Windows produces libdeki-2d.dll, not deki-2d.dll. Keying this
// on _WIN32 alone made every package built by MinGW fail the "deki-*" name test
// and the editor silently loaded none of them.
//
// Compile-time detection is the right granularity because the editor only ever
// loads packages built with its own compiler - that ABI match is a hard
// requirement (see DEKI_EDITOR_CXX_COMPILER in CMakeLists.txt).
#if defined(_WIN32) && !defined(__MINGW32__)
inline constexpr const char* kSharedLibPrefix = "";
#else
inline constexpr const char* kSharedLibPrefix = "lib";
#endif

/// The CMake target of a project's own code, which is also its library's
/// name: "<folder>-components". CMake allows only letters, digits and _.+- in
/// a target name, so anything else in the folder name (a space, a non-ASCII
/// letter) becomes '_'. "My Game ü" used to stop every build with "target
/// name is reserved or not valid".
inline std::string ProjectPluginTarget(const std::string& projectFolderName)
{
    std::string name;
    name.reserve(projectFolderName.size() + 11);
    for (unsigned char c : projectFolderName)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '.' || c == '+' || c == '-';
        if (ok)
            name += static_cast<char>(c);
        else if ((c & 0xC0) != 0x80)  // one '_' per character, not per UTF-8 byte
            name += '_';
    }
    return name + "-components";
}

/// CMake target name -> the filename CMake produces for it.
/// "deki-demo-components" -> "libdeki-demo-components.so" (Linux) / "...dll" (Windows)
inline std::string SharedLibFileName(const std::string& targetName)
{
    return std::string(kSharedLibPrefix) + targetName + kSharedLibExt;
}

/// Filename -> CMake target name, lowercased.
/// "libdeki-2d.so" / "deki-2d.dll" -> "deki-2d"
inline std::string SharedLibStem(const std::string& filename)
{
    std::string stem = filename;
    std::transform(stem.begin(), stem.end(), stem.begin(), ::tolower);

    // Strip the prefix only on toolchains that add one. Under the MSVC ABI
    // nothing is prefixed, so a DLL genuinely named "libfoo.dll" keeps its name.
    if (*kSharedLibPrefix != '\0' && stem.rfind(kSharedLibPrefix, 0) == 0)
        stem = stem.substr(std::char_traits<char>::length(kSharedLibPrefix));

    // Cut at the FIRST dot, not the last: versioned sonames ("libdeki-2d.so.1")
    // and the loader's own ".stale-N" shadow suffixes both trail the extension.
    const auto dot = stem.find('.');
    if (dot != std::string::npos)
        stem = stem.substr(0, dot);

    return stem;
}

/// True when `filename` is a shared library for this platform.
inline bool IsSharedLibFile(const std::string& filename)
{
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.find(kSharedLibExt) != std::string::npos;
}

}  // namespace DekiEditor
