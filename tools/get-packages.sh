#!/usr/bin/env bash
#
# Download every Deki package - plus the demo project and the registry - as
# working git clones next to the editor, to develop them alongside it, and keep
# them up to date. The result is one folder, ready to open in VS Code:
#
#     <root>/
#         deki-editor/            the editor, with deki-engine/ inside (submodule)
#         packages/deki-*/        every dekiengine package listed in the registry
#         deki-demo/              the demo project
#         deki-registry/          the package registry
#         hugo-dekiengine/        the website (only with --with-site)
#         .vscode/settings.json   Source Control shows every repo above
#
# Run it on a new machine to clone everything, and again whenever you switch
# machines: it fast-forwards each repo that has no local work, never touches
# one that has, and ends with a table that says which repos have commits that
# were not pushed or changes that were not committed - the things you would
# otherwise find missing on the other machine.
#
# New machine:
#     git clone https://github.com/dekiengine/deki-editor.git ~/deki/deki-editor
#     ~/deki/deki-editor/tools/get-packages.sh
#     code ~/deki
#
# tools/setup-linux.sh and tools/setup-windows.ps1 run this for you (menu
# choices 3 and 4, or --get-packages / -GetPackages).
#
# deki-editor, deki-engine and deki-demo are private, so git needs a GitHub login: an SSH
# key added to your account (then pass --ssh), or HTTPS with a personal access
# token when git asks for a password - saved with a credential helper (for
# example `git config --global credential.helper store`), or later runs cannot
# fetch those three.
#
# Usage: tools/get-packages.sh [--root DIR] [--ssh] [--no-pull] [--with-site]
#     --root DIR    workspace folder (default: the folder that holds deki-editor/)
#     --ssh         clone with git@github.com: URLs instead of https://
#     --no-pull     fetch and report, but do not update anything
#     --with-site   also clone hugo-dekiengine (a push to its main DEPLOYS the site)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
USE_SSH=0
PULL=1
WITH_SITE=0
REGISTRY_URL="https://dekiengine.github.io/registry/index.json"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --root) ROOT="$2"; shift 2 ;;
        --ssh) USE_SSH=1; shift ;;
        --no-pull) PULL=0; shift ;;
        --with-site) WITH_SITE=1; shift ;;
        -h|--help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
done

mkdir -p "$ROOT" || exit 1
ROOT="$(cd "$ROOT" && pwd)"

url_for() {  # owner/name -> clone URL
    if [[ $USE_SSH == 1 ]]; then echo "git@github.com:$1.git"; else echo "https://github.com/$1.git"; fi
}

# ---------------------------------------------------------------------------
# What belongs in the workspace: "<dir relative to root> <owner/repo>".

REPOS=(
    "deki-editor dekiengine/deki-editor"
    "deki-demo dekiengine/deki-demo"
    "deki-registry dekiengine/registry"
)
[[ $WITH_SITE == 1 ]] && REPOS+=("hugo-dekiengine dekiengine/hugo-dekiengine")

# Packages come from the registry, so a new package shows up without editing
# this script. The published index first; a local registry checkout if offline.
index="$(curl -fsSL --max-time 20 "$REGISTRY_URL" 2>/dev/null)"
if [[ -z "$index" && -f "$ROOT/deki-registry/index.json" ]]; then
    echo "note: registry unreachable, using the local deki-registry/index.json"
    index="$(cat "$ROOT/deki-registry/index.json")"
fi
if [[ -z "$index" ]]; then
    echo "error: could not read the package list from $REGISTRY_URL" >&2
    exit 1
fi
while read -r repo; do
    [[ "$repo" == dekiengine/* ]] || continue  # third-party packages are not ours to work on
    REPOS+=("packages/${repo#dekiengine/} $repo")
done < <(grep -o '"repo": *"[^"]*"' <<<"$index" | sed 's/.*"\([^"]*\)"$/\1/')

# ---------------------------------------------------------------------------
# Clone what is missing, bring the rest up to date, and remember one line of
# state per repo for the table at the end.

declare -a REPORT=()
note() { REPORT+=("$(printf '%-42s %s' "$1" "$2")"); }

# Every existing repo is fetched at once, which is most of the waiting. Git
# may not ask for a password here: a dozen prompts at once would be garbled,
# so a fetch that needs one fails and is reported instead.
declare -A FETCH_FAILED=()
fetch_all() {
    local i dirs=("$@") pids=()
    for i in "${!dirs[@]}"; do
        GIT_TERMINAL_PROMPT=0 git -C "$ROOT/${dirs[$i]}" fetch -q origin 2>/dev/null &
        pids[$i]=$!
    done
    for i in "${!dirs[@]}"; do
        wait "${pids[$i]}" || FETCH_FAILED["${dirs[$i]}"]=1
    done
}

sync_repo() {  # dir
    local dir="$1" path="$ROOT/$1" branch ahead behind dirty state
    if [[ -n "${FETCH_FAILED[$dir]:-}" ]]; then
        note "$dir" "FETCH FAILED (offline, or git has no saved GitHub login) - not checked"
        return
    fi
    if ! branch="$(git -C "$path" symbolic-ref -q --short HEAD)"; then
        note "$dir" "detached HEAD - left alone"
        return
    fi
    if ! git -C "$path" rev-parse -q --verify "origin/$branch" >/dev/null; then
        note "$dir" "$branch has no origin/$branch - left alone"
        return
    fi
    ahead="$(git -C "$path" rev-list --count "origin/$branch..HEAD")"
    behind="$(git -C "$path" rev-list --count "HEAD..origin/$branch")"
    # A newer engine checkout shows as a change to the editor's submodule
    # pointer; that is not local work, so it does not count.
    dirty="$(git -C "$path" status --porcelain --ignore-submodules=all | wc -l | tr -d ' ')"

    state=""
    if [[ $behind -gt 0 ]]; then
        if [[ $ahead -gt 0 ]]; then
            state="DIVERGED: $ahead local and $behind remote commits - merge by hand"
        elif [[ $dirty -gt 0 ]]; then
            state="$behind behind, NOT updated: uncommitted changes"
        elif [[ $PULL == 0 ]]; then
            state="$behind behind (--no-pull)"
        elif git -C "$path" merge -q --ff-only "origin/$branch" >/dev/null 2>&1; then
            state="updated ($behind new)"
        else
            state="$behind behind, update FAILED"
        fi
    elif [[ $ahead -gt 0 ]]; then
        state="$ahead commit(s) NOT PUSHED"
    fi
    if [[ $dirty -gt 0 && "$state" != *uncommitted* ]]; then
        state="${state:+$state, }$dirty uncommitted change(s)"
    fi
    note "$dir" "${state:-up to date}"
}

clone_repo() {  # dir owner/repo
    local dir="$1" repo="$2"
    echo "==> cloning $repo into $dir"
    mkdir -p "$(dirname "$ROOT/$dir")"
    if git clone -q "$(url_for "$repo")" "$ROOT/$dir"; then
        note "$dir" "cloned"
    else
        note "$dir" "CLONE FAILED"
    fi
}

existing=()
for entry in "${REPOS[@]}"; do
    dir="${entry%% *}"
    [[ -e "$ROOT/$dir/.git" ]] && existing+=("$dir")
done
# The engine is a submodule of the editor but is worked on as its own repo.
[[ -e "$ROOT/deki-editor/deki-engine/.git" ]] && existing+=("deki-editor/deki-engine")
if [[ ${#existing[@]} -gt 0 ]]; then
    echo "==> fetching ${#existing[@]} repos"
    fetch_all "${existing[@]}"
fi

for entry in "${REPOS[@]}"; do
    dir="${entry%% *}" repo="${entry#* }"
    if [[ -e "$ROOT/$dir/.git" ]]; then
        sync_repo "$dir"
    elif [[ -e "$ROOT/$dir" && -n "$(ls -A "$ROOT/$dir" 2>/dev/null)" ]]; then
        note "$dir" "exists but is not a git repo - left alone"
    else
        clone_repo "$dir" "$repo"
    fi

    if [[ "$dir" == deki-editor && -e "$ROOT/deki-editor/.git" ]]; then
        engine="$ROOT/deki-editor/deki-engine"
        if [[ ! -e "$engine/.git" ]]; then
            echo "==> cloning dekiengine/deki-engine (submodule)"
            if [[ $USE_SSH == 1 ]]; then
                git -C "$ROOT/deki-editor" config submodule.deki-engine.url "$(url_for dekiengine/deki-engine)"
            fi
            # A submodule lands on a detached commit; put it on master, like
            # the editor, so work there can be committed and pushed.
            if git -C "$ROOT/deki-editor" submodule update -q --init deki-engine &&
               git -C "$engine" checkout -q master 2>/dev/null &&
               git -C "$engine" merge -q --ff-only origin/master >/dev/null 2>&1; then
                note "deki-editor/deki-engine" "cloned"
            else
                note "deki-editor/deki-engine" "CLONE FAILED"
            fi
        else
            sync_repo "deki-editor/deki-engine"
        fi
    fi
done

# ---------------------------------------------------------------------------
# VS Code: open <root> as the folder. Existing files are never overwritten.

mkdir -p "$ROOT/.vscode"
wrote=()
if [[ ! -f "$ROOT/.vscode/settings.json" ]]; then
    # The packages are two levels down, past VS Code's default scan depth of 1.
    # Build output is left out of search and file watching; the excludes are
    # anchored because deki-editor/include/deki-editor/build and
    # deki-engine/include/deki/generated are real source.
    cat > "$ROOT/.vscode/settings.json" <<'EOF'
{
  "git.autoRepositoryDetection": true,
  "git.repositoryScanMaxDepth": 2,
  "files.watcherExclude": {
    "deki-editor/build/**": true,
    "deki-editor/build-*/**": true,
    "deki-editor/dist/**": true,
    "**/generated/build/**": true,
    "**/generated/deps/**": true
  },
  "search.exclude": {
    "deki-editor/build": true,
    "deki-editor/build-*": true,
    "deki-editor/dist": true,
    "**/generated/build": true,
    "**/generated/deps": true
  }
}
EOF
    wrote+=("settings.json")
fi

# Build and debug entries for Linux. (Windows has its own, with MSYS2 paths.)
if [[ "$(uname -s)" == Linux ]]; then
    if [[ ! -f "$ROOT/.vscode/tasks.json" ]]; then
        cat > "$ROOT/.vscode/tasks.json" <<'EOF'
{
  "version": "2.0.0",
  "options": { "cwd": "${workspaceFolder}/deki-editor" },
  "tasks": [
    {
      "label": "Configure editor",
      "type": "shell",
      "command": "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=gcc-16 -DCMAKE_CXX_COMPILER=g++-16",
      "problemMatcher": []
    },
    {
      "label": "Build editor",
      "type": "shell",
      "command": "cmake --build build --target DekiEditor",
      "group": { "kind": "build", "isDefault": true },
      "problemMatcher": "$gcc"
    },
    {
      "label": "Build tests",
      "type": "shell",
      "command": "cmake --build build --target DekiEditorTests DekiRenderingTests",
      "problemMatcher": "$gcc"
    },
    {
      "label": "Run tests",
      "type": "shell",
      "command": "build/bin/DekiEditorTests && build/bin/DekiRenderingTests",
      "dependsOn": "Build tests",
      "group": "test",
      "problemMatcher": []
    }
  ]
}
EOF
        wrote+=("tasks.json")
    fi
    if [[ ! -f "$ROOT/.vscode/launch.json" ]]; then
        cat > "$ROOT/.vscode/launch.json" <<'EOF'
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "Deki Editor",
      "type": "cppdbg",
      "request": "launch",
      "program": "${workspaceFolder}/deki-editor/build/bin/DekiEditor",
      "args": [],
      "cwd": "${workspaceFolder}/deki-editor/build/bin",
      "MIMode": "gdb",
      "preLaunchTask": "Build editor",
      "setupCommands": [{ "text": "-enable-pretty-printing", "ignoreFailures": true }]
    },
    {
      "name": "Deki Editor: deki-demo",
      "type": "cppdbg",
      "request": "launch",
      "program": "${workspaceFolder}/deki-editor/build/bin/DekiEditor",
      "args": ["${workspaceFolder}/deki-demo"],
      "cwd": "${workspaceFolder}/deki-editor/build/bin",
      "MIMode": "gdb",
      "preLaunchTask": "Build editor",
      "setupCommands": [{ "text": "-enable-pretty-printing", "ignoreFailures": true }]
    }
  ]
}
EOF
        wrote+=("launch.json")
    fi
fi

# ---------------------------------------------------------------------------

echo
echo "Workspace: $ROOT"
printf '%s\n' "${REPORT[@]}"
if [[ ${#wrote[@]} -gt 0 ]]; then
    echo
    echo "VS Code: wrote $(printf '.vscode/%s ' "${wrote[@]}")"
fi
echo
echo "Open it with:  code \"$ROOT\""
if printf '%s\n' "${REPORT[@]}" | grep -q 'FAILED\|DIVERGED'; then  # includes FETCH FAILED
    exit 1
fi
