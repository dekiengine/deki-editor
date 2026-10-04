#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace DekiEditor
{

/// Decides whether a project's sources have changed since the last build.
///
/// Answers "why didn't it rebuild?" and "why did it rebuild again?". It is
/// pure filesystem and hashing, with no service state, ImGui or processes, so
/// it can be tested on its own.
namespace SourceChangeDetector
{

/// True when a watcher event for `relativePath` should be ignored because it
/// is our own build output: CMake metadata in <package>/generated/, MSVC
/// intermediates, the editor's build/ tree. Without this filter a build's own
/// output triggers the watcher and the editor rebuilds forever.
bool IsBuildArtifact(const std::string& relativePath);

/// Newest modification time of any source under `srcDir`, or
/// file_time_type::min() when the directory is missing or empty.
std::filesystem::file_time_type GetLatestSourceTime(const std::string& srcDir);

/// The package directory name in a watcher event path, or "" when the path
/// names none.
///
/// The packages watcher is started as Start(<project>/packages, <project>), and
/// the second argument is the base for its relative paths, so an event reads
/// "packages/deki-2d/Foo.cpp", not "deki-2d/Foo.cpp". The first segment is
/// always "packages"; the name is the second.
std::string PackageNameFromEventPath(const std::string& relativePath);

/// Visits every regular file under `root`, descending through Windows
/// directory junctions.
///
/// recursive_directory_iterator skips junctions even with
/// follow_directory_symlink: a junction is IO_REPARSE_TAG_MOUNT_POINT, not
/// IO_REPARSE_TAG_SYMLINK, so is_symlink() is false and it does not descend.
/// A new iterator rooted at each junction works, because CreateFileW follows
/// reparse points when it opens the path.
void WalkSourceTree(const std::filesystem::path& root,
                    const std::function<void(const std::filesystem::directory_entry&)>& cb);

/// Hash of everything that affects a CMake configure: the CMakeLists content,
/// the set of package directories, and the path, size and mtime of every
/// header (metadata generation runs at configure time and reads headers).
///
/// A 64-bit FNV-1a that is never zero, so zero can mean "not computed".
uint64_t ComputeConfigureInputsHash(const std::string& projectPath, const std::string& cmakeListsPath);

}  // namespace SourceChangeDetector
}  // namespace DekiEditor
