#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace DekiEditor
{

/// The libraries a shared library asks the loader for, by file name: the
/// DT_NEEDED entries of a 64-bit little-endian ELF file. Empty for anything
/// else, such as a Windows DLL or a file that cannot be read.
std::vector<std::string> NeededLibraries(const std::filesystem::path& library);

/// `libraries` in an order where each comes after the ones it needs, as
/// `neededOf` names them by file name; alphabetical where nothing decides.
///
/// Package libraries are loaded from shadow copies. On Linux a library the
/// loader has not seen yet is found through its RUNPATH, which points at the
/// originals, so loading deki-2d before deki-rendering maps a second
/// deki-rendering and splits its state in two. Loaded first, a dependency is
/// found by its soname instead.
std::vector<std::filesystem::path>
DependencyOrder(const std::vector<std::filesystem::path>& libraries,
                const std::function<std::vector<std::string>(const std::filesystem::path&)>& neededOf);

}  // namespace DekiEditor
