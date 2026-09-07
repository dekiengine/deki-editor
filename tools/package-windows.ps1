# Assemble a self-contained Deki Editor distribution for Windows.
#
# The Windows counterpart of tools/package-linux.sh, and it exists for the same
# reason: Deki compiles C++ to open a project — packages and the game plugin are
# native code — so it cannot ship as a single binary. What it can do is carry the
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
#         libdeki-*.dll, SDL3.dll, resources\, ninja.exe, sccache.exe
#         toolchain\bin\{g++.exe,gcc.exe,as.exe,ld.exe,cmake.exe,ninja.exe,...}
#         toolchain\lib\gcc\...            <- cc1plus, found via argv[0]
#         toolchain\include\c++\...
#         toolchain\x86_64-w64-mingw32\...
#
# The editor prefers toolchain\ and falls back to PATH, so a build from source
# with no bundle keeps working (see services/BundledToolchain.h).
#
# Usage: tools\package-windows.ps1 [-BuildDir build] [-Msys2 C:\msys64]

[CmdletBinding()]
param(
    [string]$BuildDir = "",
    [string]$Msys2    = "C:\msys64"
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $RepoRoot 'build' }
$Dist   = Join-Path $RepoRoot 'dist\DekiEditor-windows-x86_64'
$Mingw  = Join-Path $Msys2 'mingw64'

$editorExe = Join-Path $BuildDir 'bin\DekiEditor.exe'
if (-not (Test-Path $editorExe)) {
    Write-Error @"
$editorExe not found — build first:
  cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build build --target DekiEditor
"@
}
if (-not (Test-Path "$Mingw\bin\g++.exe")) {
    Write-Error "No MinGW toolchain at $Mingw. Pass -Msys2 <path>, or install MSYS2 (see SETUP.md §2)."
}

# --- ABI guard -------------------------------------------------------------
# The bundled compiler builds the project's packages, which are then loaded into
# this editor binary — std::string, std::vector and the ImGui context all cross
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
# distribution: *.locked.exe is the copy hot reload shunts aside while the editor
# holds the original open (a second ~280 MB of editor), and crash\ is local crash
# dumps, which may contain paths and memory from whatever was being worked on.
Get-ChildItem $Dist -Filter '*.locked.exe' -Recurse | Remove-Item -Force -ErrorAction SilentlyContinue
Remove-Item "$Dist\crash" -Recurse -Force -ErrorAction SilentlyContinue
# Debug info doubles the size and is not useful without matching sources.
Get-ChildItem $Dist -Filter '*.pdb' -Recurse | Remove-Item -Force -ErrorAction SilentlyContinue
# The test binaries land in bin\ when they were built; they are not part of the
# product.
Get-ChildItem $Dist -Filter '*Tests.exe' | Remove-Item -Force -ErrorAction SilentlyContinue

$TC = Join-Path $Dist 'toolchain'
New-Item -ItemType Directory -Force -Path "$TC\bin" | Out-Null

# --- toolchain -------------------------------------------------------------
# GCC locates cc1plus, its headers and its libs relative to argv[0]
# (`g++ -print-prog-name=cc1plus` reports bin\..\lib\gcc\...), so preserving the
# mingw64 layout under toolchain\ is what makes the copy relocatable.
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
$objdump = Join-Path $Mingw 'bin\objdump.exe'
$needed  = New-Object System.Collections.Generic.HashSet[string]
$queue   = New-Object System.Collections.Generic.Queue[string]

Get-ChildItem "$TC\bin" -Filter *.exe | ForEach-Object { $queue.Enqueue($_.FullName) }
# cc1plus and friends pull in their own set (libisl, libmpc, ...).
Get-ChildItem "$Mingw\lib\gcc\x86_64-w64-mingw32" -Recurse -Filter 'cc1*.exe' -ErrorAction SilentlyContinue |
    ForEach-Object { $queue.Enqueue($_.FullName) }

while ($queue.Count -gt 0) {
    $bin = $queue.Dequeue()
    $deps = & $objdump -p $bin 2>$null | Select-String -Pattern '^\s*DLL Name:\s*(.+)$' |
            ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() }
    foreach ($d in $deps) {
        $cand = Join-Path $Mingw "bin\$d"
        if ((Test-Path $cand) -and $needed.Add($d)) { $queue.Enqueue($cand) }
    }
}
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
    # needed because GCC links -lpthread implicitly — without it every link
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

# An unstripped GCC is mostly debug info — cc1plus, cc1 and lto1 alone are over
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
        Remove-Item -Recurse -Force $sysInfoDir -ErrorAction SilentlyContinue
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
    # Only the bundle's own bin — a leak from the host toolchain would make a
    # broken bundle look fine.
    $env:PATH = "$TC\bin"
    & "$TC\bin\g++.exe" -std=c++26 -freflection $probeCc -o $probeOut 2>&1 | ForEach-Object { "    $_" }
    $rc = $LASTEXITCODE
} finally {
    $env:PATH = $savedPath
}

Remove-Item $probeCc, $probeOut -Force -ErrorAction SilentlyContinue
if ($rc -ne 0) {
    Write-Error "The bundled toolchain could not compile with an empty PATH — the bundle is incomplete."
}
Write-Host "    compiled C++26 + reflection with PATH set to the bundle only"

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
