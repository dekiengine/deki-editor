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
// Each tool falls back to PATH when there is no bundled copy, so a build from
// source works too.

#include <string>

namespace DekiEditor
{

/// Absolute path to <executable dir>/toolchain, whether or not it exists.
std::string BundledToolchainDir();

/// True if a bundled toolchain is present (its bin/ directory exists).
bool HasBundledToolchain();

/// Returns the bundled copy of a tool when there is one, otherwise `name`
/// unchanged for PATH lookup. `name` is the plain tool name ("ninja",
/// "cmake"); the platform's executable suffix is added as needed.
std::string ResolveTool(const std::string& name);

/// The C++ and C compilers to build projects with: the bundled toolchain's,
/// otherwise the compiler this editor was built with. Packages are loaded
/// into this process, so they must stay ABI-compatible with it.
std::string ProjectCxxCompiler();
std::string ProjectCCompiler();

}  // namespace DekiEditor
