#!/usr/bin/env bash
#
# Assemble a self-contained Deki Editor distribution for Linux.
#
# Deki compiles C++ to open a project - packages and the game plugin are native
# code - so it cannot ship as a single binary the way an engine with an
# interpreted scripting language can. What it can do is carry the toolchain, so
# a user downloads one archive and never installs a compiler.
#
# That matters most for GCC: the reflection codegen needs C++26 static
# reflection, which means GCC 16+, which no current distribution ships. Without
# bundling, "install Deki" starts with adding a toolchain PPA.
#
# Produces:
#
#     dist/DekiEditor-linux-x86_64/
#         DekiEditor
#         lib*.so, resources/
#         toolchain/bin/{g++,gcc,ninja,...}     <- found automatically at runtime
#
# The editor prefers toolchain/ and falls back to PATH, so a build from source
# with no bundle keeps working (see services/BundledToolchain.h).
#
# Usage: tools/package-linux.sh [build-dir]      (default: build)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/build}"
DIST="$REPO_ROOT/dist/DekiEditor-linux-x86_64"

if [[ ! -x "$BUILD_DIR/bin/DekiEditor" ]]; then
    echo "error: $BUILD_DIR/bin/DekiEditor not found - build first:" >&2
    echo "  cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build" >&2
    exit 1
fi

echo "==> Staging editor"
rm -rf "$DIST"
mkdir -p "$DIST"
cp -a "$BUILD_DIR/bin/." "$DIST/"
# The test binaries land in bin/ when they were built; not part of the product.
rm -f "$DIST"/*Tests
# Third-party license texts (fonts, ImGui, SDL3, FreeType, ...) travel with the binaries.
cp -a "$REPO_ROOT/LICENSES" "$DIST/LICENSES"
cp -a "$REPO_ROOT/LICENSE" "$DIST/LICENSE"
# The editor's own shared libraries live in build/lib; ship them beside the
# binary and let the rpath below find them there.
mkdir -p "$DIST/lib"
cp -a "$BUILD_DIR"/lib/*.so "$DIST/lib/" 2>/dev/null || true
patchelf --set-rpath '$ORIGIN/lib:$ORIGIN' "$DIST/DekiEditor" 2>/dev/null \
    || echo "    (patchelf not installed - set LD_LIBRARY_PATH, or install patchelf for a relocatable rpath)"

echo "==> Staging toolchain"
# The compiler the editor was built with, so project builds match its ABI
# exactly (packages are dlopen'd into the editor process).
CXX_PATH="$(grep -m1 '^CMAKE_CXX_COMPILER:' "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2)"
CXX_MAJOR="$("$CXX_PATH" -dumpversion 2>/dev/null | cut -d. -f1)"
echo "    editor was built with: $CXX_PATH (${CXX_MAJOR:-unknown})"

# The bundled compiler builds the project's packages, which are then dlopen'd
# into this editor binary - std::string, std::vector and the ImGui context all
# cross that boundary. If the editor was built by a different major GCC than the
# one being bundled, that is two standard libraries in one process, which
# corrupts rather than failing cleanly. Refuse to ship it.
if [[ "${CXX_MAJOR:-0}" -lt 16 ]]; then
    echo >&2
    echo "error: the editor was built with GCC ${CXX_MAJOR:-unknown}, but this bundles GCC 16." >&2
    echo "       Packages built by the bundled compiler are loaded into this binary, so the two" >&2
    echo "       must match. Rebuild the editor first:" >&2
    echo >&2
    echo "         rm -rf $BUILD_DIR && cmake -B $(basename "$BUILD_DIR") -S . -G Ninja -DCMAKE_BUILD_TYPE=Release" >&2
    echo "         cmake --build $(basename "$BUILD_DIR")" >&2
    exit 1
fi

TC="$DIST/toolchain"
mkdir -p "$TC/bin"

# Everything a working g++ needs: the driver, its backend (cc1plus lives in
# libexec), the target-prefixed binaries, and the C++/GCC headers.
GCC_PKGS=(gcc-16 gcc-16-base g++-16 cpp-16
          gcc-16-x86-64-linux-gnu g++-16-x86-64-linux-gnu cpp-16-x86-64-linux-gnu
          libstdc++-16-dev libgcc-16-dev
          binutils binutils-x86-64-linux-gnu binutils-common)

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

for pkg in "${GCC_PKGS[@]}"; do
    dpkg -L "$pkg" 2>/dev/null || true
done | sort -u | while read -r f; do
    # -f OR -L: the g++ driver is a symlink to the target-prefixed binary.
    if [[ -f "$f" || -L "$f" ]]; then
        d="$STAGE$(dirname "$f")"
        mkdir -p "$d"
        cp -a "$f" "$d/" 2>/dev/null || true
    fi
done

# Documentation and locales are most of the raw size and none of the function.
rm -rf "$STAGE/usr/share"
find "$STAGE" -name '*.la' -delete 2>/dev/null || true

# GCC locates cc1plus, its headers and libs relative to argv[0], so preserving
# the usr/ layout is what makes the copy relocatable.
cp -a "$STAGE/usr/." "$TC/"

# An unstripped GCC trunk build is ~800MB, almost all debug info in cc1plus.
echo "    stripping"
find "$TC" -type f -executable -exec strip --strip-unneeded {} \; 2>/dev/null || true

# Stable names for BundledToolchain::ProjectCxxCompiler / ResolveTool, as
# wrappers rather than symlinks.
#
# GCC relocates its *own* internals by argv[0] - that is how it finds cc1plus in
# libexec - but it looks up the assembler and linker under the prefix it was
# configured with (/usr) and then PATH. Bundling binutils is therefore not
# enough on a machine with an empty PATH:
#
#   gcc: fatal error: cannot execute 'as': posix_spawnp: No such file or directory
#
# -B adds a directory to that search, so the wrapper points GCC at its own bin/.
# Doing it here rather than in the editor covers every caller, including the
# reflection codegen, which invokes the compiler directly rather than through
# CMake.
#
# No external commands: ${0%/*} instead of dirname, because PATH may be empty.
for drv in g++:g++-16 gcc:gcc-16; do
    name="${drv%%:*}"; real="${drv##*:}"
    cat > "$TC/bin/$name" <<EOF
#!/bin/sh
here=\${0%/*}
[ "\$here" = "\$0" ] && here=.
exec "\$here/$real" -B"\$here/" "\$@"
EOF
    chmod +x "$TC/bin/$name"
done

if command -v ninja >/dev/null 2>&1; then
    cp -a "$(command -v ninja)" "$TC/bin/" && echo "    + ninja"
else
    echo "    ! ninja not found; the editor will fall back to PATH"
fi

# cmake is not just its binary: it looks up CMAKE_ROOT to find the Modules
# directory, and without it every invocation dies with
#   CMake Error: Could not find CMAKE_ROOT !!!
# It resolves that relative to the executable, so share/cmake-<ver> has to come
# along at the matching path.
if command -v cmake >/dev/null 2>&1; then
    cp -a "$(command -v cmake)" "$TC/bin/"
    CMAKE_ROOT_DIR="$(cmake --system-information 2>/dev/null \
                      | sed -n 's/^CMAKE_ROOT "\(.*\)"$/\1/p' | head -1)"
    if [[ -n "$CMAKE_ROOT_DIR" && -d "$CMAKE_ROOT_DIR" ]]; then
        mkdir -p "$TC/share"
        cp -a "$CMAKE_ROOT_DIR" "$TC/share/"
        echo "    + cmake (with $(basename "$CMAKE_ROOT_DIR"))"
    else
        echo "    ! could not locate CMAKE_ROOT; dropping bundled cmake so PATH is used"
        rm -f "$TC/bin/cmake"
    fi
else
    echo "    ! cmake not found; the editor will fall back to PATH"
fi

echo
echo "Done: $DIST"
du -sh "$DIST"

# --- archive ----------------------------------------------------------------
# One versioned tarball plus its SHA-256, the pair a GitHub release carries.
# The version is the editor's project() version, so the archive name and the
# `-V` output of the binary inside agree.
VERSION="$(sed -nE 's/^project\(DekiEditor VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$REPO_ROOT/CMakeLists.txt" | head -n1)"
if [[ -z "$VERSION" ]]; then
    echo "error: could not read project(DekiEditor VERSION ...) from CMakeLists.txt" >&2
    exit 1
fi
ARCHIVE_NAME="DekiEditor-$VERSION-linux-x86_64.tar.gz"
ARCHIVE="$(dirname "$DIST")/$ARCHIVE_NAME"
echo "==> Writing $ARCHIVE_NAME"
rm -f "$ARCHIVE"
tar -C "$(dirname "$DIST")" -czf "$ARCHIVE" "$(basename "$DIST")"
( cd "$(dirname "$ARCHIVE")" && sha256sum "$ARCHIVE_NAME" > "$ARCHIVE_NAME.sha256" )
echo "    $ARCHIVE ($(du -h "$ARCHIVE" | cut -f1))"
echo "    $(cat "$ARCHIVE.sha256")"
echo
echo "Sanity check:"
echo "  \"$TC/bin/g++\" -std=c++26 -freflection -x c++ - -o /tmp/deki-tc-check <<< 'int main(){}'"
