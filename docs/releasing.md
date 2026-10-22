# Releasing

How a Deki release goes out: the editor and the engine, the packages, the
registry, the demo and the docs, in an order that never offers anyone
something their editor cannot use. Follow it top to bottom; every step names
the command.

The rules behind it are in [COMPATIBILITY.md](../COMPATIBILITY.md). This page
is how to apply them.

## The promise to current users

A release must not break a project that is not touched. That comes down to
five rules:

1. **A project changes only when its owner updates it.** Projects pin package
   versions in `deki-packages.json`; a board's platform is the project's own
   copy, which takes a package's new version only on **Update**. Nothing in
   a release edits someone's project.
2. **A package releases only when it changed.** An unchanged package keeps
   its version and its `minEngine`, and keeps working with the new editor.
3. **`DEKI_OLDEST_SUPPORTED_PACKAGE` moves only when a release breaks
   packages** (a `kDekiReflectionAbiVersion` bump, or a change to what
   packages compile against). Moving it makes every older package refuse to
   load, so every package must then release with it.
4. **A package that needs something new says so in `minEngine`.** `requires`
   names packages but no versions, so `minEngine` is what stops an older
   editor from mixing a new package with old ones and failing to compile
   with nothing naming the cause. An editor that is too old refuses the
   package with "needs engine X or newer" instead.
5. **The editor goes out before the packages that need it**, and the
   registry last, so that message always has a download to point at.

And one for whoever adds a package:

6. **A new package is optional for everyone who does not use it.** Another
   package that can work with it names it in a feature's `requires` and
   checks for it with `__has_include` (see [platforms.md](platforms.md),
   "Features that need other packages"); the board or project that uses it
   brings it. Only add it to a package's own `requires` when that package
   cannot build without it.

## 1. Choose the version

Releases share one number: the editor, the engine and every package that
releases, and the git tags, are all `X.Y.Z`. The editor's and engine's
`project()` versions must match the tag (`tools/ci/check_version.py`).

| The release... | Version |
|---|---|
| Only fixes things, nothing new to rely on | patch: `X.Y.Z+1` |
| Adds features, packages or public API; nothing breaks | minor: `X.Y+1.0` |
| Breaks packages or files (before 1.0) | minor, and `DEKI_OLDEST_SUPPORTED_PACKAGE` moves to it (rule 3) |

Then list what releases:

- **The editor and the engine:** always, together.
- **Each package with commits since its last tag:** check with
  `git -C packages/<id> log --oneline <last tag>..master`.
- **For each of those, its `minEngine`.** Raise it to the new version when the
  package calls engine API that is new in this release, or uses a new API of
  another package releasing with it. Otherwise leave it.

## 2. Get every branch ready

Work happens on feature branches named the same in every repo (CONTRIBUTING.md).
Before releasing:

1. **Bring master into each branch.** Fetch first; the local masters may be
   behind.

   ```bash
   git -C <repo> fetch origin
   git -C <repo> merge origin/master
   ```

   - **A CHANGELOG.md conflict:** keep master's file and put the branch's
     `## Unreleased` section above master's newest version.
   - **The `deki-engine` submodule conflict in deki-editor:** point it at the
     engine branch's own merge of master.
2. **Bring the local masters of packages that are not releasing up to date**
   (`git merge --ff-only origin/master`), so the builds below use what users
   have.
3. **The demo builds from its own copies in `deki-demo/packages/`.** Mirror
   every package into them before building it.
4. **Bring the registry checkout up to date:**
   `git -C deki-registry pull --ff-only`.

## 3. Check it

Every box, on the branches about to be released:

- [ ] **The editor builds**, and its tests pass:

  ```bash
  cmake --build build
  ctest --test-dir build --output-on-failure
  ```

- [ ] **Every releasing package's tests pass, in both byte orders:**
  `cmake --build build --target run-pkgtests-<id>`
- [ ] **Every releasing package compiles with its reflection:**
  `cmake --build build --target pkgcheck_<id>`
- [ ] **Versions and ABI agree:**

  ```bash
  python tools/ci/check_version.py
  python tools/ci/check_abi.py
  ```

- [ ] **Translations are complete for every releasing package.** Build the
  package first (the check reads its generated reflection), then for each of
  `ja`, `zh` and `ko`:

  ```bash
  python tools/ci/check_translations.py --strict --language <code> --package <path>
  ```

  Do the editor too (run with no `--package`). A new package ships
  `editor/lang/ja.json`, `zh.json` and `ko.json`.
- [ ] **The demo's firmware builds for every board it has**, and the
  [hardware checklist](hardware-checklist.md) passes on a T-Deck Plus, with
  this release's numbers added at its bottom. When a release changes frame
  timing, compare libraries and releases with the frame cap lifted:
  `targetFps` 200 in `deki-demo/settings/core.json`. At 60 the engine's
  16 ms budget caps every board at 62.5 fps. Put it back afterwards.
- [ ] **An existing project still works.** Open a project made with the
  previous release in the new editor without updating anything, and build
  it. It must load and build as before. This is the check for rule 1.

Nothing goes on master until every box is ticked.

## 4. Release, in this order

`X.Y.Z` is the new version. Commits follow the house style: the subject is
the bare version, the body a few bullets, then the Co-Authored-By line if one
applies. Tags are annotated, with the version as the message:
`git tag -a X.Y.Z -m X.Y.Z`.

### 4.1 Each repo's branch onto master

For every repo releasing (and the demo):

```bash
git -C <repo> switch master
git -C <repo> merge --ff-only <branch>
```

The branch already has master in it (step 2), so this fast-forwards.
Nothing is pushed yet.

### 4.2 The engine

1. **`deki-editor/deki-engine/CMakeLists.txt`:**
   - `project(deki-engine VERSION X.Y.Z ...)`
   - `DEKI_OLDEST_SUPPORTED_PACKAGE`, only if rule 3 applies.
2. **`CHANGELOG.md`:** rename `## Unreleased` to `## X.Y.Z - YYYY-MM-DD`. With
   no engine changes the section says "No engine changes; released with the
   editor's X.Y.Z."
3. Commit `X.Y.Z`, tag `X.Y.Z`, then push master and the tag.

### 4.3 The editor

1. **`deki-editor/CMakeLists.txt`:** `project(DekiEditor VERSION X.Y.Z ...)`.
2. **`CHANGELOG.md`:** rename `## Unreleased` to `## X.Y.Z - YYYY-MM-DD`. This
   section becomes the GitHub release notes, so it must not be empty.
3. **The `deki-engine` submodule pointer:** the engine's release commit.
4. `python tools/ci/check_version.py --tag X.Y.Z` must pass.
5. Commit `X.Y.Z`, tag `X.Y.Z`, then push master and then the tag.

The tag starts `.github/workflows/release.yml`. It builds the Windows and
Linux archives and publishes the GitHub release **Deki Editor X.Y.Z**.

- [ ] **Wait for it to go green** before going on: `gh run watch` or the
  Actions tab.
- [ ] **Check the release** has both archives and their `.sha256` files.

If it fails, fix on master and run the workflow again for the same tag
(`workflow_dispatch`, input `tag`). Never move a published tag; release a
patch instead.

### 4.4 The packages

For each releasing package:

1. **`CHANGELOG.md`:** rename `## Unreleased` to `## X.Y.Z` (packages put no
   date on the heading).
2. **If `minEngine` moves:** set it in `package.json`, and add under
   `### Changed`:

   > `minEngine` X.Y.Z: built against engine X.Y.Z. A build of this package
   > from before does not load and is rebuilt.

3. Commit `X.Y.Z`, tag `X.Y.Z`, then push master and the tag.

**A new package** first needs its repository at `dekiengine/<id>`. Create it
public with a description, add it as `origin`, and push master. Its first
release is the current `X.Y.Z`.

### 4.5 The registry

The registry is what users see, and a push to its master publishes at once,
so it comes last.

1. **For each releasing package, `deki-registry/packages/<id>.json`:**
   - append `X.Y.Z` to `versions`
   - set `latest` to it
   - copy `minEngine` and `requires` from the package's own `package.json`
2. **For a new package:** a new file with `id`, `displayName`,
   `description`, `repo`, `minEngine`, `versions`, `latest`, `requires` and
   `docs`. The `id` must match the file name.
3. Regenerate and publish the index:

   ```bash
   python build-index.py
   ```

   Commit `packages/` and `index.json` together, then push.

### 4.6 The demo

`deki-demo/deki-packages.json` and `deki-packages-lock.json`: set each
releasing package to `X.Y.Z`. Commit "Moved the demo onto X.Y.Z" and push.

### 4.7 The docs and the site

- **Reference docs:** `python tools/docs/build_docs.py --languages --publish`
  (needs `gh`). It pushes the main docs and each package's `gh-pages`.
- **hugo-dekiengine:**
  - the version in `layouts/partials/nav.html`
  - `data/registry.json`, copied from the registry's `index.json`

  Then commit and push `main`, which deploys the site.

### 4.8 Clean up

Delete the released feature branches, local and remote:

```bash
git branch -d <branch>
git push origin --delete <branch>
```

`-d` refuses a branch that is not merged, which is the check that nothing is
lost.

## 5. Right after

- [ ] **Install the release** from the GitHub release page on a clean
  machine, or with the old one moved aside. Create a project, add the
  released packages, and build for the desktop and one board.
- [ ] **Run the update path:** open a project from the previous release.
  Package Manager offers the updates; take them, build, and run.
- [ ] **Check from an editor one version back** that a releasing package with
  a raised `minEngine` is refused with "needs engine X.Y.Z or newer", not a
  compile error.

## If something is wrong

- **A package version is broken:** remove it from that package's registry
  entry (`versions`, and point `latest` back), run `build-index.py`, then
  commit and push. New installs stop at once. Fix it on master and release
  a patch. The bad tag stays; tags are never moved or deleted once pushed.
- **The editor release is broken:** release `X.Y.Z+1` with the fix. If it is
  bad enough, mark the GitHub release as a pre-release, so the update notice
  does not offer it, until the fix is out.
- **Something reached users that should not have:** say so in the next
  release's changelog, under `### Fixed`, with what to do about it.
