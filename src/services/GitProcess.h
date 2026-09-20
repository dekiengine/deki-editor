#pragma once

// RunGit - spawn `git` with an argv array and capture its combined output.
//
// Extracted from DekiPackageInstaller so the package installer and the
// package integrity check share one implementation rather than two copies of
// ~120 lines of platform process spawning. No shell is ever involved: Windows
// passes an explicitly quoted command line with the application name given
// separately, POSIX uses fork + execvp. That is what keeps a hostile ref or
// URL (both arrive over the network from a registry) from becoming command
// execution - see DekiPackageInstaller::IsSafeGitRef / IsSafeGitUrl, which
// validate before anything is spawned.
//
// Returns the child's exit code, or -1 if it could not be started.

#include <string>
#include <vector>

namespace DekiEditor
{

int RunGit(const std::vector<std::string>& args, std::string& output);

}  // namespace DekiEditor
