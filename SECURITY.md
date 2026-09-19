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
- Everything fetched or downloaded goes over HTTPS from the first hop, with no
  exceptions; nothing is spawned through a shell with a network-supplied string
  in it. One request is worth naming because it is the only one a *running
  game* makes: `deki-gps`'s desktop backend asks `ipwho.is` for an approximate
  location, over HTTPS, when the package initialises in a desktop build, and
  caches the answer for an hour. Leaving the package out of a desktop target
  stops it. Device builds never make it.
- Toolchain archives are pinned by SHA-256 in the toolchain definition each build backend
  carries (the ESP-IDF one is `editor/ESPIDFToolchainDefinition.h` in deki-esp32-integration) and an unpinned
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
