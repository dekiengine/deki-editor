# Assemble a self-contained Deki Editor distribution for Windows.
#
# The Windows counterpart of tools/package-linux.sh, and it exists for the same
# reason: Deki compiles C++ to open a project - packages and the game plugin are
# native code - so it cannot ship as a single binary. What it can do is carry the
# toolchain, so a user downloads one archive and never installs a compiler.
#
# That matters most for GCC. The reflection codegen needs C++26 static
# reflection, so GCC 16+, which nothing on Windows ships by default; without
# bundling, "install Deki" starts with installing MSYS2.
#
# Produces:
#
#     dist\DekiEditor-windows-x86_64\
#         DekiEditor.exe
#         libdeki-*.dll, SDL3.dll, MinGW runtime DLLs, resources\, ninja.exe, sccache.exe
#         toolchain\mingw64\bin\{g++.exe,gcc.exe,as.exe,ld.exe,cmake.exe,ninja.exe,...}
#         toolchain\mingw64\lib\gcc\...            <- cc1plus, found via argv[0]
#         toolchain\mingw64\include\{c++\...,wchar.h,...}
#         sdk\deki-engine\                         <- engine source
#         sdk\deki-editor\include\                 <- editor DLL headers
#         sdk\_deps\{imgui-src,freetype-src,stb-src,sdl3-src}\
#         sdk\lib\{libdeki-*.dll.a,libimgui.a,libfreetype.a}
#
# The editor prefers toolchain\mingw64\ and falls back to PATH, so a build from
# source with no bundle keeps working (see services/BundledToolchain.h). The
# same for sdk\ (EditorSettings::GetSdkDir) and the source checkout.
#
# Usage: tools\package-windows.ps1 [-BuildDir build] [-Msys2 C:\msys64] [-SkipProjectCheck]

[CmdletBinding()]
param(
    [string]$BuildDir = "",
    [string]$Msys2    = "C:\msys64",
    [switch]$SkipProjectCheck
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $RepoRoot 'build' }
$Dist   = Join-Path $RepoRoot 'dist\DekiEditor-windows-x86_64'
$Mingw  = Join-Path $Msys2 'mingw64'

$editorExe = Join-Path $BuildDir 'bin\DekiEditor.exe'
if (-not (Test-Path $editorExe)) {
    Write-Error @"
$editorExe not found - build first:
  cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build build --target DekiEditor
"@
}
if (-not (Test-Path "$Mingw\bin\g++.exe")) {
    Write-Error "No MinGW toolchain at $Mingw. Pass -Msys2 <path>, or install MSYS2 (see SETUP.md §2)."
}

# --- ABI guard -------------------------------------------------------------
# The bundled compiler builds the project's packages, which are then loaded into
# this editor binary - std::string, std::vector and the ImGui context all cross
# that boundary. Two different standard libraries there corrupt rather than fail
# cleanly, so the compiler being shipped must be the one the editor was built
# with. Same check as the Linux script.
$cache = Join-Path $BuildDir 'CMakeCache.txt'
$editorCxx = (Select-String -Path $cache -Pattern '^CMAKE_CXX_COMPILER:' |
              Select-Object -First 1).Line -replace '^[^=]*=',''
$bundledVer = (& "$Mingw\bin\g++.exe" -dumpversion).Trim()
$editorVer  = if (Test-Path $editorCxx) { (& $editorCxx -dumpversion).Trim() } else { '' }

Write-Host "==> Toolchain"
Write-Host "    editor built with: $editorCxx ($editorVer)"
Write-Host "    bundling:          $Mingw\bin\g++.exe ($bundledVer)"

if ([int]($bundledVer -split '\.')[0] -lt 16) {
    Write-Error "Bundled GCC is $bundledVer; the reflection codegen needs 16+. Update MSYS2: pacman -S mingw-w64-x86_64-gcc"
}
if (-not $editorVer) {
    Write-Error "Could not run the editor's compiler ($editorCxx) to check its version. Reconfigure the build."
}
if (($editorVer -split '\.')[0] -ne ($bundledVer -split '\.')[0]) {
    Write-Error @"
The editor was built with GCC $editorVer but this would bundle GCC $bundledVer.
Packages built by the bundled compiler are loaded into this binary, so the two must
match. Rebuild the editor with the compiler being bundled first.
"@
}

# --- editor ----------------------------------------------------------------
Write-Host "==> Staging editor"
if (Test-Path $Dist) { Remove-Item -Recurse -Force $Dist }
New-Item -ItemType Directory -Force -Path $Dist | Out-Null
# bin\ already holds DekiEditor.exe, the editor's own DLLs, resources\ and the
# ninja/sccache CMake copies next to it. Windows resolves DLLs from the
# executable's directory, so no rpath equivalent is needed here.
Copy-Item "$BuildDir\bin\*" $Dist -Recurse -Force
# Third-party license texts (fonts, ImGui, SDL3, FreeType, ...) travel with the binaries.
Copy-Item "$RepoRoot\LICENSES" "$Dist\LICENSES" -Recurse -Force
Copy-Item "$RepoRoot\LICENSE" "$Dist\LICENSE" -Force

# bin\ accumulates runtime artefacts from local use that have no business in a
# distribution: the copies hot reload shunts aside while the editor holds the
# original open, and crash\ is local crash dumps, which may contain paths and
# memory from whatever was being worked on.
#
# The filter was '*.locked.exe', which matched exactly one of the names hot
# reload actually produces. It also writes a timestamp in
# (DekiEditor.locked-191236.exe) and does the same for DLLs
# (libdeki-editor.locked-191415.dll, libdeki-engine-core.locked-191415.dll).
# A local bin\ here held 637 MB of them, all of which would have shipped.
Get-ChildItem $Dist -Recurse -File |
    Where-Object { $_.Name -match '\.locked(-\d+)?\.(exe|dll)$' } |
    Remove-Item -Force -ErrorAction SilentlyContinue
Remove-Item "$Dist\crash" -Recurse -Force -ErrorAction SilentlyContinue
# Debug info doubles the size and is not useful without matching sources.
Get-ChildItem $Dist -Filter '*.pdb' -Recurse | Remove-Item -Force -ErrorAction SilentlyContinue
# The test binaries land in bin\ when they were built; they are not part of the
# product.
Get-ChildItem $Dist -Filter '*Tests.exe' | Remove-Item -Force -ErrorAction SilentlyContinue

$objdump = Join-Path $Mingw 'bin\objdump.exe'

# The MinGW DLLs a set of binaries needs, recursively: the names that exist in
# mingw64\bin. Anything else is a system DLL every Windows machine has.
function Resolve-MingwDlls([string[]]$Binaries) {
    $found = New-Object System.Collections.Generic.HashSet[string]
    $queue = New-Object System.Collections.Generic.Queue[string]
    foreach ($b in $Binaries) { $queue.Enqueue($b) }
    while ($queue.Count -gt 0) {
        $bin = $queue.Dequeue()
        $deps = & $objdump -p $bin 2>$null | Select-String -Pattern '^\s*DLL Name:\s*(.+)$' |
                ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() }
        foreach ($d in $deps) {
            $cand = Join-Path $Mingw "bin\$d"
            if ((Test-Path $cand) -and $found.Add($d)) { $queue.Enqueue($cand) }
        }
    }
    return $found
}

# --- editor runtime DLLs ---------------------------------------------------
# The build copies only libstdc++, libgcc and libwinpthread next to the
# executable. The rest it links (OpenSSL, zlib and brotli for the HTTP client)
# were found on the build machine's PATH, so an archive without them started
# only where MSYS2 happened to be on PATH, and there loaded whichever zlib1.dll
# came first.
Write-Host "==> Staging editor runtime DLLs"
$editorBins = Get-ChildItem $Dist -File | Where-Object { $_.Extension -in '.exe','.dll' } |
              ForEach-Object { $_.FullName }
$editorDlls = Resolve-MingwDlls $editorBins
foreach ($d in $editorDlls) {
    if (-not (Test-Path (Join-Path $Dist $d))) {
        Copy-Item (Join-Path $Mingw "bin\$d") $Dist -Force
        Write-Host "    + $d"
    }
}

# --- SDK -------------------------------------------------------------------
# What a project build compiles and links against, which a source checkout has
# around the build folder and a download does not: the engine source, the
# editor DLL's headers, the third-party headers the editor was built with and
# the import libraries. The editor finds them in sdk\ (EditorSettings::
# GetSdkDir); without them every project build stopped at "DEKI_ENGINE_PATH and
# DEKI_EDITOR_DIR must be supplied".
Write-Host "==> Staging SDK"
$Sdk = Join-Path $Dist 'sdk'

function Copy-Tree([string]$From, [string]$To, [string[]]$Exclude = @()) {
    New-Item -ItemType Directory -Force -Path $To | Out-Null
    Get-ChildItem $From -Force | Where-Object { $_.Name -notin (@('.git') + $Exclude) } |
        ForEach-Object { Copy-Item $_.FullName (Join-Path $To $_.Name) -Recurse -Force }
}

# The engine: its tracked files, so no build output or files a local firmware
# build generated into it come along.
$engine = Join-Path $RepoRoot 'deki-engine'
$engineFiles = & git -C $engine ls-files 2>$null
if ($LASTEXITCODE -eq 0 -and $engineFiles) {
    foreach ($f in $engineFiles) {
        $dst = Join-Path "$Sdk\deki-engine" ($f -replace '/','\')
        $dstDir = Split-Path $dst -Parent
        if (-not (Test-Path $dstDir)) { New-Item -ItemType Directory -Force -Path $dstDir | Out-Null }
        Copy-Item (Join-Path $engine ($f -replace '/','\')) $dst -Force
    }
} else {
    Copy-Tree $engine "$Sdk\deki-engine"
}
Write-Host "    + deki-engine"

Copy-Tree "$RepoRoot\deki-editor\include" "$Sdk\deki-editor\include"
Write-Host "    + deki-editor\include"

$depsDir = (Select-String -Path $cache -Pattern '^FETCHCONTENT_BASE_DIR:' |
            Select-Object -First 1).Line -replace '^[^=]*=',''
if (-not $depsDir -or -not (Test-Path $depsDir)) { $depsDir = Join-Path $BuildDir '_deps' }
$sdkDeps = Join-Path $Sdk '_deps'
# Headers only, plus licenses; the examples and docs are most of ImGui's size.
Copy-Tree "$depsDir\imgui-src" "$sdkDeps\imgui-src" -Exclude @('examples','docs')
Copy-Tree "$depsDir\freetype-src\include" "$sdkDeps\freetype-src\include"
Copy-Item "$depsDir\freetype-src\LICENSE.TXT" "$sdkDeps\freetype-src\" -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path "$sdkDeps\stb-src" | Out-Null
Get-ChildItem "$depsDir\stb-src" -File | Where-Object { $_.Extension -eq '.h' -or $_.Name -eq 'LICENSE' } |
    ForEach-Object { Copy-Item $_.FullName "$sdkDeps\stb-src\" -Force }
# SDL3's prebuilt archive prefix, at the path the generated project CMake globs
# for (sdl3-src\x86_64-w64-mingw32).
$sdl3Include = (Select-String -Path $cache -Pattern '^DEKI_SDL3_INCLUDE_DIR:' |
                Select-Object -First 1).Line -replace '^[^=]*=',''
$sdl3Prefix = if ($sdl3Include) { Split-Path ($sdl3Include -replace '/','\') -Parent } else { "$depsDir\sdl3-src\x86_64-w64-mingw32" }
if (-not (Test-Path "$sdl3Prefix\include\SDL3\SDL.h")) {
    Write-Error "SDL3's prebuilt archive is not at $sdl3Prefix; reconfigure the editor build."
}
Copy-Tree "$sdl3Prefix\include" "$sdkDeps\sdl3-src\x86_64-w64-mingw32\include"
Copy-Tree "$sdl3Prefix\lib" "$sdkDeps\sdl3-src\x86_64-w64-mingw32\lib"
Write-Host "    + third-party headers (imgui, freetype, stb, SDL3)"

# Import libraries for the editor DLLs, and the static ImGui and FreeType the
# packages link like the editor does.
New-Item -ItemType Directory -Force -Path "$Sdk\lib" | Out-Null
$libs = @(Get-ChildItem "$BuildDir\lib" -File | Where-Object {
    $_.Name -like 'libdeki-*.dll.a' -or $_.Name -eq 'libimgui.a' -or $_.Name -like 'libfreetype*.a' })
if (-not ($libs | Where-Object Name -eq 'libdeki-editor.dll.a')) {
    Write-Error "No libdeki-editor.dll.a in $BuildDir\lib; build the DekiEditor target first."
}
$libs | ForEach-Object { Copy-Item $_.FullName "$Sdk\lib\" -Force }
Write-Host "    + $($libs.Count) libraries"

# In a directory named mingw64: MSYS2's GCC has /mingw64 as its sysroot, which
# it relocates as bin\..\..\mingw64, and finds the C headers (wchar.h, ...) in
# its include\. Anywhere else the C++ headers are found and the C ones are not.
$TC = Join-Path $Dist 'toolchain\mingw64'
New-Item -ItemType Directory -Force -Path "$TC\bin" | Out-Null

# --- toolchain -------------------------------------------------------------
# GCC locates cc1plus, its headers and its libs relative to argv[0]
# (`g++ -print-prog-name=cc1plus` reports bin\..\lib\gcc\...), so preserving the
# mingw64 layout under toolchain\mingw64\ is what makes the copy relocatable.
Write-Host "==> Staging GCC"

$gccTargets = @('gcc.exe','g++.exe','cpp.exe','as.exe','ld.exe','ar.exe','ranlib.exe','strip.exe','objdump.exe')
foreach ($t in $gccTargets) {
    $src = Join-Path $Mingw "bin\$t"
    if (Test-Path $src) { Copy-Item $src "$TC\bin\" -Force }
    else { Write-Host "    ! missing $t" }
}

# Resolve the DLLs those binaries need, recursively, instead of hardcoding a
# list that silently rots when MSYS2 repackages GCC. Only DLLs that live in
# mingw64\bin are bundled; anything else is a system DLL that the target machine
# already has.
Write-Host "    resolving DLL dependencies"
$tcBins = @(Get-ChildItem "$TC\bin" -Filter *.exe | ForEach-Object { $_.FullName })
# cc1plus and friends pull in their own set (libisl, libmpc, ...).
$tcBins += @(Get-ChildItem "$Mingw\lib\gcc\x86_64-w64-mingw32" -Recurse -Filter 'cc1*.exe' -ErrorAction SilentlyContinue |
             ForEach-Object { $_.FullName })
$needed = Resolve-MingwDlls $tcBins
foreach ($d in $needed) { Copy-Item (Join-Path $Mingw "bin\$d") "$TC\bin\" -Force }
Write-Host "    + $($needed.Count) runtime DLLs"

# cc1plus, the C and C++ headers, and the target libraries.
#
# Taken from pacman's own file lists rather than by copying directories, for the
# same reason package-linux.sh reads `dpkg -L`: the useful files are interleaved
# with everything else MSYS2 has installed. mingw64\include alone is 124 MB
# because every library's headers land there, of which GCC needs only the CRT's.
#
# The packages are resolved by asking which one owns a known file, so this keeps
# working across the plain/-git naming MSYS2 uses for these.
Write-Host "    resolving GCC packages"
$bash = Join-Path $Msys2 'usr\bin\bash.exe'
$probeFiles = @(
    '/mingw64/bin/g++.exe',            # gcc: driver + cc1plus
    '/mingw64/bin/as.exe',             # binutils: assembler + linker
    '/mingw64/include/wchar.h',        # headers: the CRT headers GCC includes
    '/mingw64/lib/libmsvcrt.a',        # crt: startup objects and import libs
    '/mingw64/bin/libstdc++-6.dll',    # gcc-libs
    '/mingw64/bin/libwinpthread-1.dll',# libwinpthread: the runtime DLL
    # winpthreads: libpthread.a. A separate package from the DLL above, and
    # needed because GCC links -lpthread implicitly - without it every link
    # fails with "cannot find -lpthread" even though nothing mentions threads.
    '/mingw64/lib/libpthread.a',
    # windows-default-manifest: default-manifest.o, also linked implicitly.
    '/mingw64/lib/default-manifest.o'
)
$pkgs = & $bash -lc "for f in $($probeFiles -join ' '); do pacman -Qoq `$f 2>/dev/null; done | sort -u"
$pkgs = @($pkgs | Where-Object { $_ -and $_.Trim() } | ForEach-Object { $_.Trim() })
if (-not $pkgs) { Write-Error "Could not resolve the GCC packages via pacman; is $Msys2 a working MSYS2 install?" }
Write-Host ("    packages: " + ($pkgs -join ', '))

$fileList = & $bash -lc "pacman -Ql $($pkgs -join ' ') 2>/dev/null | awk '{print `$2}'"
$copied = 0
foreach ($f in $fileList) {
    if (-not $f) { continue }
    $f = $f.Trim()
    # Directories come through with a trailing slash; only files matter, the
    # directory structure is recreated below.
    if ($f.EndsWith('/')) { continue }
    if (-not $f.StartsWith('/mingw64/')) { continue }
    $rel = $f.Substring('/mingw64/'.Length) -replace '/','\'
    $src = Join-Path $Mingw $rel
    if (-not (Test-Path $src -PathType Leaf)) { continue }
    $dst = Join-Path $TC $rel
    $dstDir = Split-Path $dst -Parent
    if (-not (Test-Path $dstDir)) { New-Item -ItemType Directory -Force -Path $dstDir | Out-Null }
    Copy-Item $src $dst -Force
    $copied++
}
Write-Host "    + $copied toolchain files"

# An unstripped GCC is mostly debug info - cc1plus, cc1 and lto1 alone are over
# 110 MB of it. Same reasoning as package-linux.sh; nobody debugging their game
# needs symbols for the compiler.
#
# Deliberately the HOST strip, not the one just copied into the bundle: Windows
# locks a DLL while it is loaded, and the bundled strip.exe loads its own
# dependencies (libiconv, libintl, ...) from the directory it is being asked to
# strip, so it fails on them with "Permission denied". Running the host copy
# keeps the bundle's files closed.
$strip = Join-Path $Mingw 'bin\strip.exe'
if (Test-Path $strip) {
    Write-Host "    stripping"
    $before = (Get-ChildItem $TC -Recurse -File | Measure-Object Length -Sum).Sum
    # strip writes to stderr for anything it does not recognise, which with a
    # Stop preference would abort the package over a file that did not need
    # stripping anyway. Best-effort by design.
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        Get-ChildItem $TC -Recurse -Include '*.exe','*.dll' -File | ForEach-Object {
            & $strip --strip-unneeded $_.FullName 2>&1 | Out-Null
        }
    } finally {
        $ErrorActionPreference = $saved
    }
    $after = (Get-ChildItem $TC -Recurse -File | Measure-Object Length -Sum).Sum
    Write-Host ("    saved {0:N0} MB" -f (($before - $after) / 1MB))
} else {
    Write-Host "    ! strip not found; shipping unstripped"
}

# --- build tools -----------------------------------------------------------
Write-Host "==> Staging build tools"

# ninja is a single self-contained binary.
$ninja = Join-Path $Mingw 'bin\ninja.exe'
if (Test-Path $ninja) { Copy-Item $ninja "$TC\bin\" -Force; Write-Host "    + ninja" }
else { Write-Host "    ! ninja not found; the editor will fall back to PATH" }

# cmake is not just its binary: it resolves CMAKE_ROOT relative to the
# executable to find its Modules directory, and without it every invocation dies
# with "CMake Error: Could not find CMAKE_ROOT !!!". share\cmake-<ver> has to
# come along at the matching path.
$cmakeExe = Join-Path $Mingw 'bin\cmake.exe'
if (Test-Path $cmakeExe) {
    Copy-Item $cmakeExe "$TC\bin\" -Force
    # --system-information scribbles a __cmake_systeminformation\ scratch tree
    # into the working directory, so run it somewhere disposable rather than
    # leaving it in the repo on every package run.
    $sysInfoDir = Join-Path ([System.IO.Path]::GetTempPath()) "deki-cmake-sysinfo-$PID"
    New-Item -ItemType Directory -Force -Path $sysInfoDir | Out-Null
    $root = try {
        Push-Location $sysInfoDir
        (& $cmakeExe --system-information 2>$null |
         Select-String -Pattern '^CMAKE_ROOT "(.*)"$' |
         Select-Object -First 1).Matches[0].Groups[1].Value
    } finally {
        Pop-Location
        # Only when it is there: a Remove-Item of a missing path stops the script
        # under ErrorActionPreference Stop, SilentlyContinue or not (pwsh 7.5).
        if (Test-Path $sysInfoDir) { Remove-Item -Recurse -Force $sysInfoDir }
    }
    if ($root -and (Test-Path $root)) {
        New-Item -ItemType Directory -Force -Path "$TC\share" | Out-Null
        Copy-Item $root "$TC\share\$(Split-Path $root -Leaf)" -Recurse -Force
        Write-Host "    + cmake (with $(Split-Path $root -Leaf))"
    } else {
        Write-Host "    ! could not locate CMAKE_ROOT; dropping bundled cmake so PATH is used"
        Remove-Item "$TC\bin\cmake.exe" -Force
    }
} else {
    Write-Host "    ! cmake not found; the editor will fall back to PATH"
}

# cmake links a set of its own (libcurl, libarchive, libuv, jsoncpp, ...). They
# were resolved with GCC's above, before cmake was copied, so they never came
# along: the bundled cmake ran only where MSYS2 was on PATH, and elsewhere
# exited at once with STATUS_DLL_NOT_FOUND and no output.
$toolBins = @(Get-ChildItem "$TC\bin" -File | Where-Object { $_.Name -in 'cmake.exe','ninja.exe' } |
              ForEach-Object { $_.FullName })
$toolDlls = 0
foreach ($d in (Resolve-MingwDlls $toolBins)) {
    if (-not (Test-Path "$TC\bin\$d")) { Copy-Item (Join-Path $Mingw "bin\$d") "$TC\bin\" -Force; $toolDlls++ }
}
Write-Host "    + $toolDlls build tool DLLs"

# --- sanity check ----------------------------------------------------------
# The whole point is that this works on a machine with nothing installed, so
# prove it here rather than shipping and finding out: compile with C++26
# reflection using an emptied PATH, so anything the bundle forgot fails now.
Write-Host "==> Verifying the bundle is self-contained"
$probe   = Join-Path ([System.IO.Path]::GetTempPath()) "deki-tc-check-$PID"
$probeCc = "$probe.cpp"; $probeOut = "$probe.exe"
'#include <string>
#include <vector>
int main() { std::vector<std::string> v{"ok"}; return v.empty(); }' | Set-Content $probeCc -Encoding ascii

$savedPath = $env:PATH
try {
    # Only the bundle's own bin - a leak from the host toolchain would make a
    # broken bundle look fine.
    $env:PATH = "$TC\bin"
    & "$TC\bin\g++.exe" -std=c++26 -freflection $probeCc -o $probeOut 2>&1 | ForEach-Object { "    $_" }
    $rc = $LASTEXITCODE
    # The build tools have DLLs of their own; a missing one fails before any output.
    $toolRc = 0
    foreach ($tool in @('cmake.exe','ninja.exe')) {
        if (Test-Path "$TC\bin\$tool") {
            & "$TC\bin\$tool" --version | Out-Null
            if ($LASTEXITCODE -ne 0) { Write-Host "    ! $tool --version failed ($LASTEXITCODE)"; $toolRc = 1 }
        }
    }
} finally {
    $env:PATH = $savedPath
}

Remove-Item $probeCc, $probeOut -Force -ErrorAction SilentlyContinue
if ($rc -ne 0) {
    Write-Error "The bundled toolchain could not compile with an empty PATH - the bundle is incomplete."
}
if ($toolRc -ne 0) {
    Write-Error "A bundled build tool could not run with an empty PATH - the bundle is incomplete."
}
Write-Host "    compiled C++26 + reflection and ran cmake and ninja with PATH set to the bundle only"

$size = (Get-ChildItem $Dist -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
Write-Host ""
Write-Host ("Done: $Dist  ({0:N0} MB)" -f $size)

# --- archive ----------------------------------------------------------------
# One versioned zip plus its SHA-256, the pair a GitHub release carries. The
# version is the editor's project() version, so the archive name and the
# `-V` output of the binary inside agree. tar.exe (bsdtar, in every Windows
# since 1803) writes zips without Compress-Archive's 2 GB ceiling; the
# toolchain alone is a large part of that.
$verMatch = Select-String -Path "$RepoRoot\CMakeLists.txt" -Pattern 'project\(DekiEditor VERSION ([0-9]+\.[0-9]+\.[0-9]+)'
if (-not $verMatch) { Write-Error "Could not read project(DekiEditor VERSION ...) from CMakeLists.txt" }
$Version = $verMatch.Matches[0].Groups[1].Value
$archiveName = "DekiEditor-$Version-windows-x86_64.zip"
$archive = Join-Path (Split-Path -Parent $Dist) $archiveName
Remove-Item $archive -Force -ErrorAction SilentlyContinue
Write-Host "==> Writing $archiveName"
# Full path on purpose: with MSYS2 or Git on PATH, a bare `tar` resolves to GNU
# tar, which reads "C:\..." as a remote host.
$bsdtar = Join-Path $env:SystemRoot 'System32\tar.exe'
& $bsdtar -a -c -f $archive -C (Split-Path -Parent $Dist) (Split-Path -Leaf $Dist)
if ($LASTEXITCODE -ne 0) { Write-Error "$bsdtar failed to write $archive" }
$hash = (Get-FileHash -Algorithm SHA256 $archive).Hash.ToLower()
# sha256sum's own format, so `sha256sum -c` and `certutil` both verify it.
"$hash *$archiveName`n" | Set-Content "$archive.sha256" -Encoding ascii -NoNewline
$zipSize = (Get-Item $archive).Length / 1MB
Write-Host ("    {0}  ({1:N0} MB)" -f $archive, $zipSize)
Write-Host "    sha256 $hash"

# --- project check ---------------------------------------------------------
# What a user does first: unpack the archive, create a project and build its
# code, with a PATH holding only Windows and git (which installs the project's
# packages). CI's smoke jobs run the editor from its build tree, where the
# engine, the import libraries and MSYS2's DLLs are all at hand, so only this
# proves the archive works on its own. It runs from the unpacked zip in the
# temp folder rather than from dist\: two levels above dist\'s editor is this
# repository, whose deki-engine the editor's source-checkout lookup would find
# in place of a missing sdk\. Skipped with -SkipProjectCheck; it compiles every
# starter package, which takes minutes. A failure deletes the archive.
if (-not $SkipProjectCheck) {
    Write-Host "==> Building a new project with the unpacked archive"
    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) { Write-Error "git is needed for the project check (it installs the starter packages)." }
    $checkRoot = Join-Path ([System.IO.Path]::GetTempPath()) "deki-archive-check-$PID"
    New-Item -ItemType Directory -Force -Path "$checkRoot\projects" | Out-Null
    $savedPath = $env:PATH
    $failure = $null
    try {
        & $bsdtar -x -f $archive -C $checkRoot
        if ($LASTEXITCODE -ne 0) { throw "$bsdtar could not unpack $archive" }
        $exe = Join-Path $checkRoot "$(Split-Path -Leaf $Dist)\DekiEditor.exe"
        $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem;" +
                    (Split-Path $git.Source -Parent)
        # The editor is a GUI-subsystem program, which the call operator does not wait for.
        foreach ($verb in @(@('--new-project', 'deki-check', "$checkRoot\projects"),
                            @('--build-code', "$checkRoot\projects\deki-check"))) {
            $p = Start-Process -FilePath $exe -ArgumentList ($verb | ForEach-Object { "`"$_`"" }) `
                               -NoNewWindow -Wait -PassThru
            if ($p.ExitCode -ne 0) {
                throw "The packaged editor failed '$($verb[0])' (exit $($p.ExitCode)); see %APPDATA%\DekiEditor\Editor.log."
            }
            Write-Host "    $($verb[0]): ok"
        }
    } catch {
        $failure = $_.Exception.Message
    } finally {
        $env:PATH = $savedPath
        Remove-Item -Recurse -Force $checkRoot -ErrorAction SilentlyContinue
    }
    if ($failure) {
        Remove-Item $archive, "$archive.sha256" -Force -ErrorAction SilentlyContinue
        Write-Error "$failure The archive was deleted."
    }
}
