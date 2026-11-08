#!/usr/bin/env bash
#
# Assemble a self-contained Deki Editor distribution for macOS on Apple Silicon.
#
# Same layout and reasons as package-linux.sh: a project is compiled C++, so the
# archive carries the compiler. The one thing it cannot carry is Apple's linker,
# assembler and SDK, which may not be redistributed: a user needs the Command
# Line Tools (`xcode-select --install`) and nothing else.
#
#     dist/DekiEditor-macos-arm64/
#         DekiEditor, libSDL3*.dylib, lib/, resources/
#         toolchain/bin/{g++,gcc,ninja,cmake,...}
#         sdk/{deki-engine,deki-editor/include,_deps,lib}
#
# Homebrew's GCC is not built to be moved. tools/macos_relocate.py rewrites it
# and its libraries to find each other relative to themselves.
#
# Usage: tools/package-macos.sh [build-dir]      (default: build)
#        DEKI_SKIP_PROJECT_CHECK=1 skips building a new project with the result.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/build}"
DIST="$REPO_ROOT/dist/DekiEditor-macos-arm64"

if [[ ! -f "$BUILD_DIR/bin/DekiEditor" ]]; then
    echo "error: $BUILD_DIR/bin/DekiEditor not found - build first" >&2
    exit 1
fi

echo "==> Staging editor"
rm -rf "$DIST"
mkdir -p "$DIST/lib"
cp -a "$BUILD_DIR/bin/." "$DIST/"
rm -f "$DIST"/*Tests
cp -a "$REPO_ROOT/LICENSES" "$DIST/LICENSES"
cp -a "$REPO_ROOT/LICENSE" "$DIST/LICENSE"
cp -a "$BUILD_DIR"/lib/*.dylib "$DIST/lib/"
chmod +x "$DIST/DekiEditor"

echo "==> Staging SDK"
SDK="$DIST/sdk"
mkdir -p "$SDK/deki-engine" "$SDK/deki-editor" "$SDK/_deps" "$SDK/lib"
git -C "$REPO_ROOT/deki-engine" archive HEAD | tar -x -C "$SDK/deki-engine"
cp -a "$REPO_ROOT/deki-editor/include" "$SDK/deki-editor/include"

DEPS_DIR="$BUILD_DIR/_deps"
mkdir -p "$SDK/_deps/imgui-src" "$SDK/_deps/freetype-src" "$SDK/_deps/stb-src" "$SDK/_deps/sdl3-src"
( cd "$DEPS_DIR/imgui-src" && find . -mindepth 1 -maxdepth 1 ! -name examples ! -name docs ! -name .git \
      -exec cp -a {} "$SDK/_deps/imgui-src/" \; )
cp -a "$DEPS_DIR/freetype-src/include" "$SDK/_deps/freetype-src/include"
cp -a "$DEPS_DIR"/stb-src/*.h "$DEPS_DIR/stb-src/LICENSE" "$SDK/_deps/stb-src/"
# SDL3 as its clang build installed it: the headers for package builds, and the
# whole install for a game built as a program (its CMake package).
cp -a "$DEPS_DIR/sdl3-clang/include" "$SDK/_deps/sdl3-src/include"
cp -a "$DEPS_DIR/sdl3-clang" "$SDK/_deps/sdl3-clang"

# Links, so the archive carries one copy of each library.
for lib in "$DIST"/lib/libdeki-*.dylib; do
    ln -sf "../../lib/$(basename "$lib")" "$SDK/lib/$(basename "$lib")"
done
cp -a "$DEPS_DIR"/sdl3-clang/lib/libSDL3*.dylib "$SDK/lib/"
cp -a "$BUILD_DIR"/lib/libimgui.a "$BUILD_DIR"/lib/libfreetype*.a "$SDK/lib/"
for needed in libdeki-editor.dylib libSDL3.dylib; do
    if [[ ! -e "$SDK/lib/$needed" ]]; then
        echo "error: $needed is missing from the SDK; build the DekiEditor target first. It has:" >&2
        ls -la "$SDK/lib" "$BUILD_DIR/lib" >&2
        exit 1
    fi
done

echo "==> Staging toolchain"
GCC_PREFIX="$(cd -P "$(brew --prefix gcc)" && pwd)"
GCC_MAJOR="$(basename "$GCC_PREFIX" | cut -d. -f1)"
echo "    GCC $(basename "$GCC_PREFIX") from $GCC_PREFIX"
if [[ "$GCC_MAJOR" -lt 16 ]]; then
    echo "error: GCC 16 or later is needed for the reflection codegen." >&2
    exit 1
fi

TC="$DIST/toolchain"
mkdir -p "$TC"
cp -R "$GCC_PREFIX/." "$TC/"
# Documentation and Fortran: most of the size and none of the function.
rm -rf "$TC/share" "$TC/.brew" "$TC/.bottle"
rm -f "$TC"/bin/*gfortran*
find "$TC/libexec" -name f951 -delete
find "$TC" -name '*.la' -delete

# Stable names for ProjectCxxCompiler. GCC finds cc1plus and its headers from
# its own location. The SDK and the linker it takes from the Command Line Tools,
# asked for by absolute path because PATH may be empty; the SDK named when
# Homebrew built this GCC need not be the one this Mac has. The run path is how
# a game built here finds this libstdc++.
for drv in g++:g++-$GCC_MAJOR gcc:gcc-$GCC_MAJOR; do
    name="${drv%%:*}"; real="${drv##*:}"
    cat > "$TC/bin/$name" <<EOF
#!/bin/sh
here=\${0%/*}
[ "\$here" = "\$0" ] && here=.
sdk=\$(/usr/bin/xcrun --show-sdk-path) || exit 1
lib=\$(cd "\$here/../lib/gcc/current" && pwd)
exec "\$here/$real" --sysroot="\$sdk" -Wl,-rpath,"\$lib" "\$@"
EOF
    chmod +x "$TC/bin/$name"
done

cp -L "$(command -v ninja)" "$TC/bin/ninja"

# cmake finds its modules from where it sits: ../share/cmake-<ver> for a plain
# install, and inside the bundle for Kitware's CMake.app, which the runners have.
CMAKE_BIN="$(python3 -c 'import os, shutil; print(os.path.realpath(shutil.which("cmake")))')"
probe_dir="$(mktemp -d)"
echo 'message("${CMAKE_ROOT}")' > "$probe_dir/root.cmake"
CMAKE_ROOT_DIR="$(cmake -P "$probe_dir/root.cmake" 2>&1)"
rm -rf "$probe_dir"
case "$CMAKE_BIN" in
    */CMake.app/Contents/bin/cmake)
        mkdir -p "$TC/CMake.app/Contents/bin" "$TC/CMake.app/Contents/share"
        cp -L "$CMAKE_BIN" "$TC/CMake.app/Contents/bin/cmake"
        cp -R "$CMAKE_ROOT_DIR" "$TC/CMake.app/Contents/share/"
        ln -sf ../CMake.app/Contents/bin/cmake "$TC/bin/cmake"
        ;;
    *)
        cp -L "$CMAKE_BIN" "$TC/bin/cmake"
        mkdir -p "$TC/share"
        cp -R "$CMAKE_ROOT_DIR" "$TC/share/"
        ;;
esac

echo "==> Relocating"
python3 "$REPO_ROOT/tools/macos_relocate.py" "$DIST" "$TC/lib/bundled"

"$TC/bin/g++" --version | head -n1
"$TC/bin/cmake" --version | head -n1

# What a user does first, with a plain system PATH. CI's archive job does the
# same on a machine that never had this GCC.
if [[ "${DEKI_SKIP_PROJECT_CHECK:-0}" != "1" ]]; then
    echo "==> Building a new project with the packaged editor"
    PROJ_PARENT="$(mktemp -d)"
    trap 'rm -rf "$PROJ_PARENT"' EXIT
    env PATH=/usr/bin:/bin "$DIST/DekiEditor" --new-project deki-check "$PROJ_PARENT"
    env PATH=/usr/bin:/bin "$DIST/DekiEditor" --build-code "$PROJ_PARENT/deki-check"
    echo "    --new-project, --build-code: ok"
fi

echo
echo "Done: $DIST"
du -sh "$DIST"

VERSION="$(sed -nE 's/^project\(DekiEditor VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$REPO_ROOT/CMakeLists.txt" | head -n1)"
if [[ -z "$VERSION" ]]; then
    echo "error: could not read project(DekiEditor VERSION ...) from CMakeLists.txt" >&2
    exit 1
fi
ARCHIVE_NAME="DekiEditor-$VERSION-macos-arm64.tar.gz"
ARCHIVE="$(dirname "$DIST")/$ARCHIVE_NAME"
echo "==> Writing $ARCHIVE_NAME"
rm -f "$ARCHIVE"
tar -C "$(dirname "$DIST")" -czf "$ARCHIVE" "$(basename "$DIST")"
( cd "$(dirname "$ARCHIVE")" && shasum -a 256 "$ARCHIVE_NAME" > "$ARCHIVE_NAME.sha256" )
echo "    $ARCHIVE ($(du -h "$ARCHIVE" | cut -f1))"
