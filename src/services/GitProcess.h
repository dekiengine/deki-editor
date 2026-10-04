#pragma once

// RunGit runs `git` with an argument list and captures its combined output.
// The package installer and the package integrity check both use it.
//
// No shell is involved: Windows gets an explicitly quoted command line with
// the application name given separately, POSIX uses fork + execvp. This keeps
// a hostile ref or URL (both come over the network from a registry) from
// running commands. DekiPackageInstaller::IsSafeGitRef / IsSafeGitUrl check
// them before anything starts.
//
// A repository's own settings never run anything. A package folder can come
// from someone else (a zipped project with packages/*/.git), and the editor
// runs git in it by itself on every focus. So every call passes
// core.fsmonitor=false and a hooks path with no hooks, and a repository whose
// .git/config would make git run a command (a filter other than git-lfs, an
// ssh/proxy/askpass command, a credential helper, an include) is refused.

#include <string>
#include <vector>

namespace DekiEditor
{

/// Runs git with `args` and puts its output in `output`. Returns git's exit
/// code, or -1 when it could not start or the repository was refused (then
/// `output` says why).
int RunGit(const std::vector<std::string>& args, std::string& output);

/// Why git must not run in `repoDir` ("" when it may): the command-running
/// settings in its .git/config. Exposed for the tests.
std::string GitConfigRunsCommands(const std::string& repoDir);

}  // namespace DekiEditor
