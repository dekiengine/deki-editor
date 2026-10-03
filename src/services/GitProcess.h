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
//
// A repository's own settings never run anything. A package folder can come
// from someone else (a zipped project with packages/*/.git), and the editor
// runs git in it on its own, on every focus. So every call passes
// core.fsmonitor=false and a hooks path with no hooks, and a repository whose
// .git/config would make git run a command (a filter other than git-lfs, an
// ssh/proxy/askpass command, a credential helper, an include) is refused:
// RunGit returns -1 and says why in `output`.

#include <string>
#include <vector>

namespace DekiEditor
{

int RunGit(const std::vector<std::string>& args, std::string& output);

/// Why git must not run in `repoDir` ("" when it may): the command-running
/// settings in its .git/config. Exposed for the tests.
std::string GitConfigRunsCommands(const std::string& repoDir);

}  // namespace DekiEditor
