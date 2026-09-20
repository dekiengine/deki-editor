#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace DekiEditor
{

/// Deciding whether a project's sources have changed since the last build.
///
/// Split out of HotReloadService, which had grown to cover file watching,
/// change detection, CMake input hashing, process spawning, DLL promotion,
/// plugin/package swapping and its own ImGui progress modal. This is the part
/// that owns the "why didn't it rebuild?" and "why did it rebuild again?"
/// questions, and - being pure filesystem and hashing, with no service state,
/// no ImGui and no process spawning - the part that can actually be tested.
namespace SourceChangeDetector
{

/// Should a watcher event for @p relativePath be ignored?
///
/// True for our own build output: CMake writes metadata into
/// <package>/generated/, MSVC drops intermediates, and the editor's build/ tree
/// churns constantly. Without this filter a build's own output retriggers the
/// watcher and the editor rebuilds forever.
bool IsBuildArtifact(const std::string& relativePath);

/// Newest modification time across every source under @p srcDir, or
/// file_time_type::min() when the directory is missing or empty.
std::filesystem::file_time_type GetLatestSourceTime(const std::string& srcDir);

/// The package directory name named by a watcher event path, or "" when the
/// path does not name one.
///
/// The packages watcher is started as Start(<project>/packages, <project>), and
/// the second argument is the base its relative paths are computed against - so
/// an event reads "packages/deki-2d/Foo.cpp", NOT "deki-2d/Foo.cpp". Taking the
/// first segment yields the literal "packages" for every package on earth,
/// which is why this is a named, tested function rather than a find_first_of.
std::string PackageNameFromEventPath(const std::string& relativePath);

/// Visit every regular file under @p root, descending through Windows
/// directory junctions.
///
/// Works around recursive_directory_iterator skipping junctions even with
/// follow_directory_symlink: a junction carries IO_REPARSE_TAG_MOUNT_POINT
/// rather than IO_REPARSE_TAG_SYMLINK, so is_symlink() returns false and the
/// iterator declines to descend. Starting a fresh iterator rooted at each
/// junction sidesteps it - CreateFileW follows reparse points at open time, so
/// enumeration inside works transparently.
void WalkSourceTree(const std::filesystem::path& root,
                    const std::function<void(const std::filesystem::directory_entry&)>& cb);

/// Hash of everything that affects a CMake configure: the CMakeLists content,
/// the set of package directories, and the path/size/mtime of every header
/// (metadata generation runs at configure time and keys off headers).
///
/// Non-zero 64-bit FNV-1a, so zero is usable as "not computed".
uint64_t ComputeConfigureInputsHash(const std::string& projectPath,
                                    const std::string& cmakeListsPath);

}  // namespace SourceChangeDetector
}  // namespace DekiEditor
