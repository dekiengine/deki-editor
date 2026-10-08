#!/usr/bin/env bash
#
# Install everything a Linux machine needs to build Deki Editor from source.
#
# The Linux counterpart of tools/setup-windows.ps1. SETUP.md §2 lists the
# prerequisites; this script installs them. The one that matters is GCC 16:
# the reflection codegen needs C++26 static reflection (-freflection), which no
# current distribution ships by default. On Linux SDL3 is also built from source, so
# its X11/Wayland/audio development headers are needed too.
#
# It checks the machine first, prints what it is about to change and asks
# before changing anything (--yes skips the question). Only missing packages
# are installed, so it is safe to run again.
#
#   1. packages        apt (Ubuntu/Debian), dnf (Fedora) or pacman (Arch):
#                      GCC, cmake, ninja, git, git-lfs and SDL3's build deps.
#                      Ubuntu gets GCC 16 from ppa:ubuntu-toolchain-r/test.
#   2. GCC 16          g++-16, else g++; must compile a C++26 reflection test
#   3. git lfs install
#   4. tools/sync-repos.sh   only with --sync-repos (packages, demo, registry)
#   5. configure + build     only with --build, with the exact compiler from 2
#
# The apt list is the one CI builds with (.github/workflows/ci.yml); the dnf
# and pacman lists are the same libraries under those distributions' names.
#
# What it changes on the system, and nothing else:
#   - installs the packages above (on Arch, pacman -Syu upgrades the whole
#     system first: Arch does not support partial upgrades)
#   - on Ubuntu without g++-16, adds ppa:ubuntu-toolchain-r/test, which also
#     brings a newer, backward-compatible libstdc++
#   - git lfs install for the current user (~/.gitconfig)
# GCC 16 installs next to the system compiler on Ubuntu; plain gcc/g++ keep
# pointing at the old one.
#
# Usage: tools/setup-linux.sh [--yes] [--sync-repos [--ssh]] [--build [--build-type T]]
#     --yes             do not ask before installing (for CI and scripts)
#     --sync-repos      run tools/sync-repos.sh afterwards
#     --ssh             with --sync-repos: clone over SSH
#     --build           configure and build DekiEditor
#     --build-type T    Debug (default), Release, RelWithDebInfo or MinSizeRel
#     --allow-root      run as root (containers); the build will be owned by root
#
# Run as your normal user; it calls sudo for the package installs only and asks
# for the password once, at the start. A log is written to $LOG_DIR (printed).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MIN_GCC="16.0"      # first filter only; the reflection test decides
MIN_FREE_GB=10      # toolchain + FetchContent deps + a Debug build
YES=0
SYNC_REPOS=0
USE_SSH=0
BUILD=0
BUILD_TYPE="Debug"
ALLOW_ROOT=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --yes|-y) YES=1; shift ;;
        --sync-repos) SYNC_REPOS=1; shift ;;
        --ssh) USE_SSH=1; shift ;;
        --build) BUILD=1; shift ;;
        --build-type) [[ $# -ge 2 ]] || { echo "--build-type needs a value" >&2; exit 2; }
                      BUILD_TYPE="$2"; shift 2 ;;
        --allow-root) ALLOW_ROOT=1; shift ;;
        -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
done

# --- Logging ---------------------------------------------------------------
# Everything from here on goes to the terminal and to the log, so a failed run
# on someone else's machine can be diagnosed from one file.
LOG_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/deki"
mkdir -p "$LOG_DIR"
LOG="$LOG_DIR/setup-linux-$(date +%Y%m%d-%H%M%S).log"
# Colour only on a terminal - decided before stdout becomes the tee pipe.
if [[ -t 1 ]]; then C_STEP=$'\033[36m' C_OK=$'\033[32m' C_WARN=$'\033[33m' C_ERR=$'\033[31m' C_END=$'\033[0m'
else C_STEP="" C_OK="" C_WARN="" C_ERR="" C_END=""; fi
exec > >(tee -a "$LOG") 2>&1

CURRENT_STEP="start"
step() { CURRENT_STEP="$1"; printf '\n%s==> %s%s\n' "$C_STEP" "$1" "$C_END"; }
ok()   { printf '    %s%s%s\n' "$C_OK" "$1" "$C_END"; }
warn() { printf '    %swarning: %s%s\n' "$C_WARN" "$1" "$C_END"; }
die()  { printf '\n%serror: %s%s\n' "$C_ERR" "$1" "$C_END" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

on_exit() {
    local rc=$?
    if [[ $rc -ne 0 ]]; then
        printf '\n%sSetup failed during: %s (exit %d)%s\n' "$C_ERR" "$CURRENT_STEP" "$rc" "$C_END" >&2
        printf 'Full log: %s\n' "$LOG" >&2
    fi
}
trap on_exit EXIT

# version_ge A B: is version A >= B?
version_ge() { [[ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" == "$2" ]]; }

# --- Pre-flight ------------------------------------------------------------
step "Checking this machine"
echo "    log: $LOG"

case "$BUILD_TYPE" in
    Debug|Release|RelWithDebInfo|MinSizeRel) ;;
    *) die "--build-type must be Debug, Release, RelWithDebInfo or MinSizeRel, not '$BUILD_TYPE'" ;;
esac

# Run as root, everything the build writes - the repo's build/, the package
# clones, ~/.deki-editor - would belong to root and fight the real user later.
SUDO=""
if [[ $(id -u) -eq 0 ]]; then
    [[ $ALLOW_ROOT == 1 ]] || die "do not run this as root or with sudo: run it as your normal user, it calls sudo itself. (In a container, pass --allow-root.)"
else
    have sudo || die "sudo is not installed; install the packages as root, or install sudo."
    SUDO="sudo"
fi

# shellcheck disable=SC1091
[[ -f /etc/os-release ]] && . /etc/os-release
DISTRO="${ID:-unknown} ${ID_LIKE:-}"
ok "${PRETTY_NAME:-unknown distribution} ($(uname -m))"

if grep -qi microsoft /proc/version 2>/dev/null && [[ "$REPO_ROOT" == /mnt/* ]]; then
    warn "the repo is on the Windows drive ($REPO_ROOT). Builds there are many times slower under WSL; clone it inside Linux (e.g. ~/deki) instead."
fi

[[ -e "$REPO_ROOT/deki-engine/CMakeLists.txt" ]] ||
    die "deki-engine/ is empty - the submodule is missing. Run: git -C \"$REPO_ROOT\" submodule update --init --recursive"

free_gb=$(df -Pk "$REPO_ROOT" | awk 'NR==2 {print int($4 / 1048576)}')
if [[ ${free_gb:-0} -lt $MIN_FREE_GB ]]; then
    warn "only ${free_gb} GB free on the repo's disk; a first build needs about ${MIN_FREE_GB} GB."
fi

# --- What to install -------------------------------------------------------

APT_PACKAGES=(
    g++-16 gcc-16 cmake ninja-build git git-lfs curl pkg-config
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev
    libxss-dev libxkbcommon-dev libwayland-dev libdecor-0-dev
    libegl1-mesa-dev libgl1-mesa-dev libasound2-dev libpulse-dev
    libdbus-1-dev libudev-dev
)
DNF_PACKAGES=(
    gcc gcc-c++ cmake ninja-build git git-lfs curl pkgconf-pkg-config
    libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel
    libXScrnSaver-devel libxkbcommon-devel wayland-devel libdecor-devel
    mesa-libEGL-devel mesa-libGL-devel alsa-lib-devel pulseaudio-libs-devel
    dbus-devel systemd-devel
)
PACMAN_PACKAGES=(
    gcc cmake ninja git git-lfs curl pkgconf
    libx11 libxext libxrandr libxcursor libxi
    libxss libxkbcommon wayland libdecor
    mesa alsa-lib libpulse dbus systemd-libs
)

PM="" ADD_PPA=0
MISSING=()
if have apt-get; then
    PM=apt
    for p in "${APT_PACKAGES[@]}"; do
        dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q 'install ok installed' || MISSING+=("$p")
    done
    if [[ " ${MISSING[*]} " == *" g++-16 "* ]] &&
       ! apt-cache policy g++-16 2>/dev/null | grep -q 'Candidate: [0-9]'; then
        # Ubuntu's own archive has no GCC 16 yet; the toolchain PPA does, and it
        # is what CI uses. Debian has no PPAs, so there it has to be in the archive.
        [[ "$DISTRO" == *ubuntu* ]] ||
            die "this distribution's archive has no g++-16 and it is not Ubuntu, so the toolchain PPA cannot be used. Install GCC $MIN_GCC+ another way (see SETUP.md §2)."
        ADD_PPA=1
    fi
elif have dnf; then
    PM=dnf
    for p in "${DNF_PACKAGES[@]}"; do rpm -q "$p" >/dev/null 2>&1 || MISSING+=("$p"); done
elif have pacman; then
    PM=pacman
    for p in "${PACMAN_PACKAGES[@]}"; do pacman -Q "$p" >/dev/null 2>&1 || MISSING+=("$p"); done
else
    die "no apt-get, dnf or pacman found. Install GCC $MIN_GCC+, cmake, ninja, git, git-lfs and SDL3's build dependencies by hand (see SETUP.md §2)."
fi

# --- Plan + confirmation ---------------------------------------------------
step "Plan"
if [[ ${#MISSING[@]} -eq 0 ]]; then
    ok "all ${PM} packages already installed"
else
    echo "    install with $PM (${#MISSING[@]}): ${MISSING[*]}"
    [[ $PM == pacman ]] && echo "    pacman -Syu: upgrades the whole system first (Arch has no partial upgrades)"
fi
[[ $ADD_PPA == 1 ]] && echo "    add apt source: ppa:ubuntu-toolchain-r/test (GCC 16; also updates libstdc++)"
echo "    git lfs install for $(id -un)"
[[ $SYNC_REPOS == 1 ]] && echo "    tools/sync-repos.sh$([[ $USE_SSH == 1 ]] && echo ' --ssh') into $(dirname "$REPO_ROOT")"
[[ $BUILD == 1 ]] && echo "    build DekiEditor ($BUILD_TYPE) in $REPO_ROOT/build"

if [[ $YES == 0 ]]; then
    [[ -t 0 ]] || die "not running in a terminal, so cannot ask; pass --yes to proceed without asking."
    read -r -p $'\n    Continue? [y/N] ' answer
    [[ "$answer" == [yY]* ]] || { echo "    Nothing changed."; exit 0; }
fi

# --- 1. Packages -----------------------------------------------------------
if [[ ${#MISSING[@]} -gt 0 || $ADD_PPA == 1 ]]; then
    step "Installing packages ($PM)"
    # Ask for the password once now rather than in the middle of the output -
    # unless sudo already runs the package manager without one (a NOPASSWD
    # rule for it), where -v would still prompt.
    PM_BIN=$([[ $PM == apt ]] && echo apt-get || echo $PM)
    if [[ -n "$SUDO" ]] && ! $SUDO -n "$PM_BIN" --version >/dev/null 2>&1; then
        $SUDO -v
    fi
    case $PM in
        apt)
            # Wait for an unattended-upgrades run instead of failing on its lock.
            APT=($SUDO apt-get -o DPkg::Lock::Timeout=300)
            if [[ $ADD_PPA == 1 ]]; then
                "${APT[@]}" update
                "${APT[@]}" install -y --no-install-recommends software-properties-common
                $SUDO add-apt-repository -y ppa:ubuntu-toolchain-r/test
            fi
            "${APT[@]}" update
            "${APT[@]}" install -y --no-install-recommends "${MISSING[@]}"
            ;;
        dnf)    $SUDO dnf install -y "${MISSING[@]}" ;;
        pacman) $SUDO pacman -Syu --needed --noconfirm "${MISSING[@]}" ;;
    esac
fi

# --- 2. GCC with C++26 reflection ------------------------------------------
# Ubuntu installs it as g++-16 next to the system g++; Fedora and Arch replace
# g++ itself. CMake looks for g++-16 then g++ (SETUP.md §2) - so do we, and the
# build below is handed the absolute path so nothing earlier on PATH wins.
#
# The version number is only a first filter. What the codegen needs is
# -std=c++26 -freflection with <meta>, and a GCC 16 snapshot can have it before
# the 16.1 release does (the Ubuntu toolchain PPA ships 16.0.1, which CI uses),
# so each candidate compiles a small reflection program, the way the engine's
# configure step will.
reflection_ok() {  # g++ -> does it compile C++26 static reflection?
    local src rc
    src="$(mktemp --suffix=.cpp)"
    cat > "$src" <<'CPP'
#include <meta>
struct DekiProbe { int x; };
static_assert(std::meta::identifier_of(^^DekiProbe) == "DekiProbe");
int main() {}
CPP
    "$1" -std=c++26 -freflection -fsyntax-only "$src" >/dev/null 2>&1 && rc=0 || rc=1
    rm -f "$src"
    return $rc
}

step "Checking GCC (>= $MIN_GCC, C++26 reflection)"
CXX="" CC="" seen=()
for cand in g++-16 g++; do
    have "$cand" || continue
    ver="$("$cand" -dumpfullversion)"
    if ! version_ge "$ver" "$MIN_GCC"; then
        seen+=("$cand $ver (too old)"); continue
    fi
    if ! reflection_ok "$cand"; then
        seen+=("$cand $ver (no C++26 reflection)"); continue
    fi
    CXX="$(command -v "$cand")"; CC="$(command -v "${cand/g++/gcc}")"; break
done
if [[ -z "$CXX" ]]; then
    die "no GCC that can build Deki's reflection codegen (needs GCC $MIN_GCC+ with -freflection). Found: ${seen[*]:-no g++ at all}."
fi
[[ -n "$CC" ]] || die "found $CXX but no matching gcc next to it."
CMAKE="$(command -v cmake)" || die "cmake not found"
NINJA="$(command -v ninja)" || die "ninja not found"
ok "$CXX $("$CXX" -dumpfullversion)"
ok "$("$CMAKE" --version | head -n1) ($CMAKE)"
ok "ninja $("$NINJA" --version) ($NINJA)"

# --- 3. git-lfs ------------------------------------------------------------
# Package install shells out to git, and packages may keep assets in LFS.
step "Setting up git-lfs"
git lfs version >/dev/null 2>&1 || die "git-lfs not found"
git lfs install --skip-repo >/dev/null
ok "$(git lfs version)"

# --- 4. Sync repos (optional) ----------------------------------------------
if [[ $SYNC_REPOS == 1 ]]; then
    step "Syncing repos (tools/sync-repos.sh)"
    sync_args=()
    [[ $USE_SSH == 1 ]] && sync_args+=(--ssh)
    "$REPO_ROOT/tools/sync-repos.sh" ${sync_args[@]+"${sync_args[@]}"}
fi

# --- 5. Build (optional) ---------------------------------------------------
BUILD_CMD="\"$CMAKE\" -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=$BUILD_TYPE -DCMAKE_C_COMPILER=$CC -DCMAKE_CXX_COMPILER=$CXX -DCMAKE_MAKE_PROGRAM=$NINJA -DDEKI_GXX16=$CXX"
if [[ $BUILD == 1 ]]; then
    step "Building DekiEditor ($BUILD_TYPE)"
    cd "$REPO_ROOT"
    "$CMAKE" -B build -S . -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
             -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
             -DCMAKE_MAKE_PROGRAM="$NINJA" -DDEKI_GXX16="$CXX"
    "$CMAKE" --build build --target DekiEditor
    [[ -x build/bin/DekiEditor ]] || die "the build finished but build/bin/DekiEditor is missing."
    ok "$REPO_ROOT/build/bin/DekiEditor"
    ok "$(build/bin/DekiEditor -V 2>/dev/null | head -n1 || true)"
fi

step "Done"
if [[ $BUILD == 0 ]]; then
    cat <<EOF
    Build the editor (from $REPO_ROOT):
      $BUILD_CMD
      "$CMAKE" --build build --target DekiEditor
    or re-run with --build.
EOF
fi
echo "    log: $LOG"
