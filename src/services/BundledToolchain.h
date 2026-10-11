#pragma once

// Finds the build tools the editor drives.
//
// Opening a project compiles C++ (packages and the game plugin are native
// code), so the distribution carries its own toolchain: a user downloads one
// archive and never installs a compiler. This matters most for the compiler:
// the reflection codegen needs GCC 16+ for C++26 static reflection, which
// Linux distributions do not ship yet.
//
// Layout, relative to the executable:
//
//     DekiEditor
//     toolchain/
//         bin/{g++,gcc,ninja,cmake,...}
//
// On Windows it is toolchain/mingw64/: MSYS2's GCC finds the C headers through
// its sysroot, /mingw64, which it relocates as bin/../../mingw64, so the
// toolchain works only from a directory of that name.
//
// Each tool falls back to PATH when there is no bundled copy, so a build from
// source works too.

#include <string>

namespace DekiEditor
{

/// Absolute path to <executable dir>/toolchain (toolchain/mingw64 on Windows),
/// whether or not it exists.
std::string BundledToolchainDir();

/// True if a bundled toolchain is present (its bin/ directory exists).
bool HasBundledToolchain();

/// Returns the bundled copy of a tool when there is one; else `name` unchanged
/// for PATH lookup when it is on PATH; else its full path in the toolchain
/// folder (ToolchainBinDir) when it is there; else `name`. `name` is the plain
/// tool name ("ninja", "cmake"); the platform's executable suffix is added as
/// needed.
std::string ResolveTool(const std::string& name);

/// Windows: the folder of the GNU toolchain projects are built with - the
/// bundled toolchain's bin/, else the folder of the compiler this editor was
/// built with, else the first standard MSYS2 / MinGW folder that has g++.
/// Empty when there is none, and always empty elsewhere (the toolchain is on
/// the system PATH there).
std::string ToolchainBinDir();

/// Windows: puts ToolchainBinDir() at the front of this process's PATH unless
/// it is on PATH already. Every build the editor starts inherits it, so cmake,
/// ninja and the compiler - whose own helpers (cc1plus, as, ld) load DLLs from
/// that folder - are found however the editor was started: from the Start
/// menu, from a terminal opened before MSYS2 was installed, or with MSYS2 not
/// on the user PATH at all. Call once, first thing in main().
void PrepareToolchainPath();

/// Whether `dir` is one of the ;-separated entries of `pathList`, ignoring
/// case, slash direction and a trailing slash, as Windows does.
bool PathListHasDir(const std::string& pathList, const std::string& dir);

/// The C++ and C compilers to build projects with: the bundled toolchain's,
/// otherwise the compiler this editor was built with. Packages are loaded
/// into this process, so they must stay ABI-compatible with it.
std::string ProjectCxxCompiler();
std::string ProjectCCompiler();

}  // namespace DekiEditor
