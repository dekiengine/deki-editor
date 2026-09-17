# Security

Please report vulnerabilities privately to **devkirbyrawr@gmail.com**, or
through GitHub's private vulnerability reporting on the repository concerned,
rather than in a public issue. Include the repository (editor, engine or
package), a description, and steps to reproduce. You will get an
acknowledgement within a few days and a fix or a plan before any public
disclosure.

## What the editor trusts, and what it does not

A project you open and a package you install are C++ that the editor compiles
and loads into its own process: opening a project is running its code, by
design, the way opening a Makefile and typing `make` is. That is the line. On
the other side of it:

- The package registry, package indexes and board-pack indexes arrive over the
  network and name the directories the editor writes to. Every such name is
  allowlisted before it becomes a path, every path is checked to stay under
  the project's `packages/` before anything is deleted or handed to git, and
  git is spawned from an argument vector with a validated URL and ref.
- Everything the editor itself fetches or downloads goes over HTTPS from the
  first hop; nothing is spawned through a shell with a network-supplied string
  in it. One package is an exception and is called out here rather than left
  to be discovered: `deki-gps`'s desktop backend asks `ip-api.com` for an
  approximate location over plain HTTP, because that service offers no HTTPS
  on its free tier. It runs when the package initialises in a desktop build,
  so a simulator run with `deki-gps` active sends the machine's IP address to
  a third party unencrypted. Leave the package out of a desktop target if
  that is not wanted.
- Toolchain archives are pinned by SHA-256 in `resources/builders/*.json` and an unpinned
  download is refused; archives are listed and refused before extraction when
  an entry would escape the destination.
- The MCP server binds `127.0.0.1`, generates a 256-bit session token, requires
  it on every route, and rejects requests carrying an `Origin` header or a
  non-loopback `Host`.
- The update check reads GitHub's release API and only ever opens the
  editor's own releases page in the browser; nothing is downloaded.
- Crash dumps stay in a `crash/` folder next to the executable; the editor
  sends no telemetry.

In scope: the editor (including its MCP server), the package manager
(registry fetch, git clones), the firmware builders, and the engine's file and
network handling. Package repositories under github.com/dekiengine are covered
by the same address.
