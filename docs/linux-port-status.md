# Linux port & toolchain — status and open items

Written at the end of a working session so it can be picked up on another
machine. Everything described here is committed and pushed.

## Where things stand

The editor builds and runs on Linux, and a project set up with Deki works by
opening it — no CLI, no manual cloning.

Verified end to end on a Steam Deck (Ubuntu 24.04 container, X11 + Wayland):
clone the sample project (`deki-demo`) into an empty directory, open it in the editor, touch nothing →
22 packages restored, 23 libraries built, every component resolves.

| Repo | Commit | What landed |
|---|---|---|
| `dekiengine/deki-editor` | `3659bdf` | Linux port, CLI verbs, auto-restore on open, single compiler, bundled toolchain, registry fallback |
| `dekiengine/deki-engine` | `c26408c` | GCC portability, relative codegen paths |
| `dekiengine/deki-demo` | (sample project) | manifest fixes, deki-gps removed |
| `dekiengine/deki-http` | `3c3282e` (tag `1.0.1`) | POSIX curl backend, `<cstdint>` |
| `dekiengine/deki-gps` | `6d2f971` (tag `1.0.1`) | unity-build static collision |
| `dekiengine/registry` | `18fb27e` | GitHub Pages enabled, 1.0.1 published |

`linux-support` branches were merged into `master` in both editor and engine;
nothing lives on them any more.

## Setting up on another machine

```bash
git clone --recurse-submodules https://github.com/dekiengine/deki-editor
cd deki-editor
cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Requires **GCC 16+** (C++26 static reflection). No distribution ships it yet:

```bash
sudo add-apt-repository ppa:ubuntu-toolchain-r/test && sudo apt install g++-16
```

Plus `cmake ninja-build git curl` and SDL3's build deps (X11/Wayland dev
packages, `libgl1-mesa-dev`). The configure step picks `g++-16` automatically
and prints which compiler it chose. If a prerequisite is missing, opening a
project now reports exactly what and how to install it, rather than failing
partway through a build.

To produce a distribution that needs none of the above on the target machine:

```bash
tools/package-linux.sh build        # -> dist/DekiEditor-linux-x86_64 (414MB)
```

## Open items

### ~~1. `registry.dekiengine.com`~~ — DONE, nothing outstanding

`registry.dekiengine.com` is retired. The registry is served from
`dekiengine.github.io/registry`, straight off GitHub IPs, with
`raw.githubusercontent.com/dekiengine/registry/master` as the backup. Nothing
here ever needed a custom domain — this is JSON a program fetches, not a site
anyone visits — and routing it through Cloudflare is what made it breakable in
the first place.

All four steps are complete:

- `CNAME` deleted from `dekiengine/registry` (`24b2fde`)
- `DekiRegistrySource` endpoints reordered, `github.io` primary
- Pages custom domain cleared (`PUT /repos/dekiengine/registry/pages`,
  `{"cname": null}`) — `https_enforced` flipped to true on its own once the
  custom domain was gone
- Cloudflare DNS record for `registry.dekiengine.com` deleted

Verified after: `github.io/registry/index.json` and `.../packages/deki-2d.json`
both 200 `application/json` with no redirect, the old hostname no longer
resolves, and the editor fetches the registry without logging a fallback.

Two things worth remembering, because both cost time to work out:

- **Deleting the `CNAME` file does not clear the custom domain.** It also lives
  in the repo's Pages settings, and while it is set there `dekiengine.github.io`
  **301-redirects** to the custom domain — so the GitHub URL is useless as a
  fallback, which is the very thing the fallback existed for.
- **The redirect was to `http://`, not `https://`.** Windows fetches through
  WinHTTP, which refuses HTTPS→HTTP redirects by default, so on Windows the
  primary endpoint failed and fell through to `raw` on every single request.
  Do not "fix" that by relaxing the redirect policy — silently downgrading to
  plaintext is worse than the extra request.

`$DEKI_REGISTRY_URL` still overrides both endpoints without a rebuild.

### 2. Windows packaging — decided: MinGW GCC 16. Implemented: `tools/package-windows.ps1`

Implemented since 0.9: `tools/package-windows.ps1` stages the editor with the
bundled MinGW toolchain, ninja and sccache, proves the bundle compiles C++26
reflection with an emptied `PATH`, and writes a versioned zip plus its SHA-256
(the Linux script does the same with a tarball). The paragraphs below record
the decision that led there.

**The decision is made; nobody needs to re-open it.** Windows is supported on
**MinGW GCC 16**, the same compiler family as Linux. clang-cl and the MSVC ABI
are not the supported path.

What settled it: the reflection codegen is GCC-only and always will be for now.
`DekiReflectionCodegen.cmake` builds the generator with `-std=c++26 -freflection`
from an explicit `GXX` path, deliberately independent of the project's own
compiler. GCC 16+ is therefore required on every machine on every platform no
matter what compiles the editor — so targeting the MSVC ABI means a Windows
machine needs LLVM *and* GCC 16, and bundling means shipping both. MinGW makes
one toolchain cover the editor, project builds and the codegen, exactly as on
Linux. The MSVC ABI bought nothing in return: every dependency (SDL3, ImGui,
freetype, json, httplib, gtest) is built from source via FetchContent, so there
are no prebuilt MSVC-ABI binaries to link against.

This is not theoretical — the editor is built and used this way on Windows
today: MinGW g++ 16 from MSYS2, full test suite green, the sample project building all 22
packages plus the plugin.

Note that the code supports both regardless: `PluginToolchain::HostAbi()` is
`constexpr` on `_MSC_VER`, so the editor builds projects with whatever ABI it
was itself compiled with, and a clang-cl build still works for anyone who wants
it. It is simply not the path that gets packaged or tested.

Still to do:

- `tools/package-windows.ps1`, mirroring `package-linux.sh`, bundling MinGW
  GCC 16 + cmake + ninja so a packaged editor needs nothing installed
- Drop the `NOT WIN32` gate on the single-compiler default in `CMakeLists.txt`,
  which was only there pending this decision

### 3. Prebuilt packages — with source override

First open is ~3.5 minutes because 22 packages compile from source. Shipping
prebuilt binaries per platform per version would cut it to under a minute.

The rule that keeps this usable: **sources win when present**. A package you are
editing is already a git checkout in `packages/`, so if its sources are
newer than the prebuilt binary, build from source and ignore the download.
`PackageLoader::HasNewerSources` already does most of this. Editing a package in
place and pushing it must keep working exactly as it does today.

This is the requirement, not a nice-to-have, and it is the normal workflow
rather than an edge case: you are using `deki-2d`, something is missing, you add
it to the package right there in `project/packages/deki-2d`, and the next build
must pick up **your** version. Concretely that means:

- A prebuilt is only ever used when the checkout is untouched. Any local edit,
  any commit, any branch switch, and the prebuilt is ignored for that package —
  not merged with, not "preferred if newer", ignored.
- The comparison cannot rest on mtime alone. A fresh `git clone` writes current
  timestamps, so a just-cloned package can look "newer" than a prebuilt it
  exactly matches, and a `git checkout` of an older tag can look older than
  edits it actually replaced. Pair it with the checked-out commit, plus dirty
  state from `git status --porcelain`.
- Downloading a prebuilt must never touch the working tree of a package that has
  local changes — no overwrite, no stash, no clean.
- The fallback direction is always "build from source": a missing, corrupt or
  wrong-platform prebuilt is a cache miss, never an error.

If any of that is in doubt for a given package, build it from source. A slower
first open is a cost; silently compiling something other than what the developer
just wrote is a bug that would be extremely hard to diagnose from the symptom.

### 4. Building every package at once

Two of ~25 packages did not compile on Linux at all (`deki-gps`, `deki-http`),
and each broke the entire project build. Neither was exotic — they had simply
never been compiled outside MSVC. Nothing catches that class of bug except
actually building every package.

There is no CI. `tools/docs/make_docs_project.py` generates a throwaway
project that declares every package the registry lists; `--packages-restore` +
`--build-code` on it is the same install and build path a developer takes, run
by hand before touching anything package-related. The docs build (`tools/docs/`)
uses the same project to load every package and dump its reflection metadata.

### 5. Smaller things

- **`--packages-update`** — there is no way to move a project off a pinned
  version. Getting a project onto `deki-http` 1.0.1 needed an explicit
  `--packages-add --version`. Note that `SavePackages` writes `installedVersion`
  back into the manifest, so `"version": "*"` becomes a hard pin on first
  install.
- **sccache on Linux** — bundled and used on Windows, not wired up here. Would
  cut repeat cold builds.
- **Registry fallback pins branch `master`** — the previous fallback pointed at
  `main` and had silently 404'd forever. A CI check that both endpoints serve
  identical `index.json` would stop that recurring.
- **`deki-http` `PostJson` and custom headers are untested** — code-reviewed
  only; nothing in the tree calls them yet. `Get` is verified (DesktopGPS gets a
  live fix).

## Gotchas worth remembering

- **Generated files are committed on purpose.** Embedded builds glob
  `project/src/generated/*.gen.cpp` and never run the codegen, so they cannot be
  gitignored. Their churn was fixed at the source instead (relative `// Source:`
  paths, sorted component order). Only the generated root `CMakeLists.txt` is
  ignored.
- **The project compiler must match the editor's.** Packages are `dlopen`'d into
  the editor process and pass `std::string` / `std::vector` / the ImGui context
  across that boundary; two standard libraries there corrupt rather than fail.
  The editor bakes in its own compiler and pins project builds to it, and
  `package-linux.sh` refuses to bundle a mismatched pair.
- **Counting `DekiPlugin_RegisterComponents` in the log undercounts packages.**
  Only the newer peripheral packages emit those lines. To check what is really
  loaded: `grep -oE 'libdeki-[a-z0-9-]*\.so' /proc/<pid>/maps | sort -u`.
- **"0 unregistered components" only means something if a real scene loaded.**
  The editor opens an empty *Untitled Scene* by default, where there is nothing
  to resolve.

## Quick verification

```bash
# Whole flow from an empty clone
git clone https://github.com/dekiengine/deki-demo /tmp/gh
build/bin/DekiEditor --packages-restore /tmp/gh     # expect 0 failed, 0 unresolvable
build/bin/DekiEditor --build-code    /tmp/gh       # expect "Build complete"
build/bin/DekiEditor                 /tmp/gh       # expect 0 "not registered" in Editor.log

# Registry reachability (both should serve identical JSON)
curl -sL https://dekiengine.github.io/registry/index.json | head -3
curl -s  https://raw.githubusercontent.com/dekiengine/registry/master/index.json | head -3

# Once the Pages custom domain is cleared, this should be 200 + application/json
# with no redirect. While it still 301s, the setting has not been cleared yet.
curl -s -o /dev/null -w '%{http_code} %{content_type} %{redirect_url}\n' \
  https://dekiengine.github.io/registry/index.json
```
