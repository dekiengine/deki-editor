#pragma once

// Locating the build tools the editor drives.
//
// Deki compiles C++ to open a project — packages and the game plugin are native
// code — so unlike an engine with an interpreted scripting language it cannot
// ship as a single binary with no external dependencies. The next best thing is
// to carry the toolchain in the distribution, so a user downloads one archive
// and never installs a compiler.
//
// That matters most for the compiler itself: the reflection codegen needs
// GCC 16+ for C++26 static reflection, which no current distribution ships, so
// "install the prerequisites" means adding a toolchain PPA before the editor
// will open anything.
//
// Layout, relative to the executable:
//
//     DekiEditor
//     toolchain/
//         bin/{g++,gcc,ninja,cmake,...}
//
// Everything falls back to PATH when there is no bundled copy, so a build from
// source keeps working exactly as before.

#include <string>

namespace DekiEditor
{

/// Absolute path to <executable dir>/toolchain, whether or not it exists.
std::string BundledToolchainDir();

/// True if a bundled toolchain is present (its bin/ directory exists).
bool HasBundledToolchain();

/// Resolve a tool to the bundled copy when there is one, otherwise return
/// `name` unchanged for PATH lookup. `name` is the plain tool name ("ninja",
/// "cmake"); the platform's executable suffix is added as needed.
std::string ResolveTool(const std::string& name);

/// The C++ / C compiler to build projects with.
///
/// Prefers the bundled toolchain; otherwise falls back to the compiler this
/// editor was built with, which is what packages must stay ABI-compatible with
/// (they are dlopen'd into this process).
std::string ProjectCxxCompiler();
std::string ProjectCCompiler();

}  // namespace DekiEditor
