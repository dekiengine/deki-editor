# Install everything a Windows machine needs to build Deki Editor from source.
#
# SETUP.md §2 lists the prerequisites; this script installs them. The one that
# matters is GCC 16: the reflection codegen needs C++26 static reflection,
# nothing on Windows ships it, and an MSVC build still needs it. MSYS2 provides
# GCC, CMake and Ninja together, which is the MinGW route SETUP.md §3 describes
# - so with this script no Visual Studio is needed at all.
#
# It checks the machine first, prints what it is about to change and asks
# before changing anything (-Yes skips the question). Only what is missing is
# installed, so it is safe to run again.
#
#   1. git + git-lfs              winget, if not on PATH
#   2. MSYS2                      winget, into -Msys2 (default C:\msys64)
#   3. gcc, cmake, ninja          pacman, mingw-w64-x86_64-*
#   4. GCC 16 check               must compile a C++26 reflection test
#   5. git lfs install            for the current user
#   6. <msys2>\mingw64\bin        appended to the user PATH (-SkipPath to skip)
#   7. tools/sync-repos.sh        only with -SyncRepos (packages, demo, registry)
#   8. configure + build          only with -Build, with MSYS2's tools by full path
#
# What it changes on the system, and nothing else:
#   - installs Git / Git LFS / MSYS2 with winget when missing (UAC prompt)
#   - updates MSYS2's own packages (pacman -Syu); everything stays in C:\msys64
#   - appends to the user PATH (not the machine PATH), at the end, so tools
#     already on PATH keep precedence - the build itself never relies on PATH
#   - git lfs install for the current user (~\.gitconfig)
#
# Usage: tools\setup-windows.ps1 [-Yes] [-Msys2 C:\msys64] [-SkipPath]
#                                [-SyncRepos [-Ssh]] [-Build [-BuildType Debug]]
#
# Run from a normal (non-admin) PowerShell. A log is written to
# %LOCALAPPDATA%\deki (printed at the start and on failure). Open a new terminal
# afterwards so the PATH change applies.

[CmdletBinding()]
param(
    [switch]$Yes,
    [string]$Msys2     = "C:\msys64",
    [switch]$SkipPath,
    [switch]$SyncRepos,
    [switch]$Ssh,
    [switch]$Build,
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$BuildType = "Debug"
)

$ErrorActionPreference = 'Stop'

$RepoRoot   = Split-Path -Parent $PSScriptRoot
$Mingw      = Join-Path $Msys2 'mingw64'
$MingwBin   = Join-Path $Mingw 'bin'
$MsysBash   = Join-Path $Msys2 'usr\bin\bash.exe'
$MinGcc     = [version]'16.0.0'   # first filter only; the reflection test decides
$MinFreeGB  = 15     # MSYS2 + toolchain + FetchContent deps + a Debug build
$MsysPkgs   = @('mingw-w64-x86_64-gcc', 'mingw-w64-x86_64-cmake', 'mingw-w64-x86_64-ninja')

$script:CurrentStep = 'start'
$failed = $false
function Step($msg) { $script:CurrentStep = $msg; Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Ok($msg)   { Write-Host "    $msg" -ForegroundColor Green }
function Warn($msg) { Write-Host "    warning: $msg" -ForegroundColor Yellow }
function Fail($msg) { throw $msg }

function Have($cmd) { [bool](Get-Command $cmd -ErrorAction SilentlyContinue) }

function Refresh-Path {
    # winget installs update the registry PATH, not this session's.
    $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
                [Environment]::GetEnvironmentVariable('Path', 'User')
}

function Winget-Install($id) {
    if (-not (Have 'winget')) {
        Fail "winget not found. Install 'App Installer' from the Microsoft Store, or install $id by hand."
    }
    winget install --id $id --exact --source winget --disable-interactivity `
        --accept-package-agreements --accept-source-agreements
    if ($LASTEXITCODE -ne 0) { Fail "winget install $id failed (exit $LASTEXITCODE)" }
    Refresh-Path
}

# Run a command in an MSYS2 login shell. MSYSTEM picks the mingw64 environment,
# CHERE_INVOKING keeps the login shell from cd'ing to $HOME.
function Msys($cmd) {
    $env:MSYSTEM = 'MINGW64'
    $env:CHERE_INVOKING = '1'
    & $MsysBash -lc $cmd | Out-Host
    return $LASTEXITCODE
}

function Git-Lfs-Present {
    if (-not (Have 'git')) { return $false }
    git lfs version *> $null
    return ($LASTEXITCODE -eq 0)
}

$LogDir = Join-Path $env:LOCALAPPDATA 'deki'
New-Item -ItemType Directory -Force $LogDir | Out-Null
$Log = Join-Path $LogDir ("setup-windows-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
Start-Transcript -Path $Log | Out-Null

try {
    # --- Pre-flight --------------------------------------------------------
    Step 'Checking this machine'
    Write-Host "    log: $Log"

    if (-not [Environment]::Is64BitOperatingSystem) { Fail 'Deki needs 64-bit Windows.' }
    Ok ("{0} ({1})" -f (Get-CimInstance Win32_OperatingSystem).Caption, $env:PROCESSOR_ARCHITECTURE)

    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
                   [Security.Principal.WindowsBuiltInRole]::Administrator)
    if ($isAdmin) {
        Warn 'running as Administrator. Not needed (winget asks for UAC itself), and files the build creates may be awkward to change later from a normal terminal.'
    }

    if (-not (Test-Path (Join-Path $RepoRoot 'deki-engine\CMakeLists.txt'))) {
        Fail "deki-engine\ is empty - the submodule is missing. Run: git -C `"$RepoRoot`" submodule update --init --recursive"
    }

    $drive = Get-PSDrive -Name ((Resolve-Path $RepoRoot).Drive.Name)
    $freeGB = [math]::Floor($drive.Free / 1GB)
    if ($freeGB -lt $MinFreeGB) {
        Warn "only $freeGB GB free on $($drive.Name):; MSYS2 plus a first build needs about $MinFreeGB GB."
    }

    # What is missing, so the plan says exactly what will happen.
    $needGit   = -not (Have 'git')
    $needLfs   = -not (Git-Lfs-Present)
    $needMsys2 = -not (Test-Path $MsysBash)
    if ($needMsys2 -and $Msys2 -ne 'C:\msys64') {
        Fail "No MSYS2 at $Msys2. winget installs to C:\msys64 - install it there by hand, or omit -Msys2."
    }
    $missingPkgs = @()
    if (-not $needMsys2) {
        foreach ($p in $MsysPkgs) {
            & $MsysBash -lc "pacman -Q $p" *> $null
            if ($LASTEXITCODE -ne 0) { $missingPkgs += $p }
        }
    } else {
        $missingPkgs = $MsysPkgs
    }
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $pathEntries = @($userPath -split ';' | Where-Object { $_ })
    $needPath = (-not $SkipPath) -and -not ($pathEntries | Where-Object { $_.TrimEnd('\') -ieq $MingwBin })

    # pacman -Syu replaces MSYS2's runtime and closes its processes; a terminal
    # left open would be killed mid-session, or block the update.
    $msysProcs = @()
    if (-not $needMsys2) {
        $msysProcs = @(Get-Process -ErrorAction SilentlyContinue |
                       Where-Object { $_.Path -and $_.Path.StartsWith($Msys2, [StringComparison]::OrdinalIgnoreCase) })
    }

    # --- Plan + confirmation -----------------------------------------------
    Step 'Plan'
    if ($needGit)   { Write-Host '    install with winget: Git.Git' }
    if ($needLfs -and -not $needGit) { Write-Host '    install with winget: GitHub.GitLFS' }
    if ($needMsys2) { Write-Host "    install with winget: MSYS2.MSYS2 (into C:\msys64, UAC prompt)" }
    Write-Host '    update MSYS2 packages (pacman -Syu, twice)'
    if ($missingPkgs.Count -gt 0) { Write-Host ("    install with pacman: " + ($missingPkgs -join ' ')) }
    Write-Host "    git lfs install for $env:USERNAME"
    if ($needPath) { Write-Host "    append to user PATH: $MingwBin" }
    if ($SyncRepos) { Write-Host ("    tools/sync-repos.sh{0} into {1}" -f $(if ($Ssh) { ' --ssh' } else { '' }), (Split-Path -Parent $RepoRoot)) }
    if ($Build)     { Write-Host "    build DekiEditor ($BuildType) in $RepoRoot\build" }
    if ($msysProcs.Count -gt 0) {
        Warn ("MSYS2 programs are running ({0}); close them first - the update will end them." -f
              (($msysProcs | Select-Object -ExpandProperty ProcessName -Unique) -join ', '))
    }

    if (-not $Yes) {
        if ([Console]::IsInputRedirected) { Fail 'not running in a terminal, so cannot ask; pass -Yes to proceed without asking.' }
        $answer = Read-Host "`n    Continue? [y/N]"
        if ($answer -notmatch '^[yY]') { Write-Host '    Nothing changed.'; return }
    }

    # --- 1. git + git-lfs --------------------------------------------------
    # Package install shells out to git, and packages may keep assets in LFS.
    Step 'Installing git + git-lfs'
    if (-not (Have 'git')) { Winget-Install 'Git.Git' }
    if (-not (Git-Lfs-Present)) { Winget-Install 'GitHub.GitLFS' }
    Ok (git --version)
    Ok (git lfs version)

    # --- 2. MSYS2 ----------------------------------------------------------
    Step "Installing MSYS2 ($Msys2)"
    if (-not (Test-Path $MsysBash)) {
        Winget-Install 'MSYS2.MSYS2'
        if (-not (Test-Path $MsysBash)) { Fail "MSYS2 installed but $MsysBash is missing." }
        # First start initialises the keyring and the user's home.
        $null = Msys 'true'
    }
    Ok "found $MsysBash"

    # --- 3. gcc, cmake, ninja ----------------------------------------------
    # -Syu twice: the first pass may update the core (pacman, msys2-runtime)
    # and exit early, the second updates everything else. Its exit code is
    # ignored for that reason; the -S below fails loudly if the update broke.
    Step 'Installing gcc, cmake, ninja (pacman)'
    $null = Msys 'pacman -Syu --noconfirm'
    $null = Msys 'pacman -Syu --noconfirm'
    $rc = Msys ("pacman -S --needed --noconfirm " + ($MsysPkgs -join ' '))
    if ($rc -ne 0) { Fail "pacman -S failed (exit $rc)" }

    # --- 4. GCC with C++26 reflection --------------------------------------
    # Absolute paths from here on: the build must use exactly these, whatever
    # else (Strawberry Perl, an old MinGW, ...) is earlier on PATH.
    #
    # The version number is only a first filter. What the codegen needs is
    # -std=c++26 -freflection with <meta>, so g++ compiles a small reflection
    # program, the way the engine's configure step will.
    Step "Checking GCC (>= $MinGcc, C++26 reflection)"
    $gcc   = Join-Path $MingwBin 'gcc.exe'
    $gxx   = Join-Path $MingwBin 'g++.exe'
    $cmake = Join-Path $MingwBin 'cmake.exe'
    $ninja = Join-Path $MingwBin 'ninja.exe'
    foreach ($t in $gcc, $gxx, $cmake, $ninja) { if (-not (Test-Path $t)) { Fail "$t not found after install." } }
    $gccVer = [version](& $gxx -dumpfullversion).Trim()
    if ($gccVer -lt $MinGcc) {
        Fail "MSYS2 gave GCC $gccVer; Deki's reflection codegen needs GCC 16. Run 'pacman -Syu' in MSYS2 later, or pass -DDEKI_GXX16=<g++> to CMake."
    }
    $probe = Join-Path $env:TEMP ("deki-reflection-probe-{0}.cpp" -f $PID)
    [IO.File]::WriteAllText($probe, "#include <meta>`nstruct DekiProbe { int x; };`n" +
        "static_assert(std::meta::identifier_of(^^DekiProbe) == `"DekiProbe`");`nint main() {}`n")
    $savedPath = $env:Path
    try {
        # g++ runs cc1plus and loads its DLLs from its own bin.
        $env:Path = "$MingwBin;$env:Path"
        & $gxx -std=c++26 -freflection -fsyntax-only $probe *> $null
        $probeRc = $LASTEXITCODE
    } finally {
        $env:Path = $savedPath
        Remove-Item $probe -ErrorAction SilentlyContinue
    }
    if ($probeRc -ne 0) {
        Fail "GCC $gccVer at $gxx cannot compile C++26 static reflection (-freflection), which Deki's codegen needs. Run 'pacman -Syu' in MSYS2 for a newer GCC."
    }
    Ok "g++ $gccVer, C++26 reflection OK ($gxx)"
    Ok (((& $cmake --version) | Select-Object -First 1) + " ($cmake)")
    Ok ("ninja " + (& $ninja --version) + " ($ninja)")

    # --- 5. git-lfs --------------------------------------------------------
    Step 'Setting up git-lfs'
    git lfs install --skip-repo *> $null
    if ($LASTEXITCODE -ne 0) { Fail "git lfs install failed (exit $LASTEXITCODE)" }
    Ok 'git lfs install --skip-repo'

    # --- 6. PATH -----------------------------------------------------------
    # Appended, not prepended: for the user's own terminals, so tools they
    # already rely on keep precedence. Shadowing is reported, not overridden.
    Step "Updating PATH ($MingwBin)"
    if ($SkipPath) {
        Ok 'skipped (-SkipPath)'
    } elseif (-not $needPath) {
        Ok 'already on user PATH'
    } else {
        [Environment]::SetEnvironmentVariable('Path', (($pathEntries + $MingwBin) -join ';'), 'User')
        Ok 'appended to user PATH (open a new terminal for it to apply)'
    }
    Refresh-Path
    foreach ($tool in 'g++', 'cmake', 'ninja') {
        $first = Get-Command $tool -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($first -and -not $first.Source.StartsWith($MingwBin, [StringComparison]::OrdinalIgnoreCase)) {
            Warn "'$tool' in a new terminal will be $($first.Source), not MSYS2's. This script's build is unaffected; for manual builds use the commands printed at the end."
        }
    }

    # --- 7. Sync repos (optional) ------------------------------------------
    # sync-repos.sh needs git on its PATH, so run it with Git for Windows'
    # bash rather than MSYS2's, which has no git unless installed separately.
    if ($SyncRepos) {
        Step 'Syncing repos (tools/sync-repos.sh)'
        $gitBash = Join-Path (Split-Path -Parent (Split-Path -Parent (Get-Command git).Source)) 'bin\bash.exe'
        if (-not (Test-Path $gitBash)) { Fail "Git Bash not found at $gitBash." }
        $syncArgs = @()
        if ($Ssh) { $syncArgs += '--ssh' }
        & $gitBash (Join-Path $RepoRoot 'tools/sync-repos.sh') @syncArgs
        if ($LASTEXITCODE -ne 0) { Fail "sync-repos.sh failed (exit $LASTEXITCODE)" }
    }

    # --- 8. Build (optional) -----------------------------------------------
    # MSYS2's bin goes first on this process's PATH: g++ runs as, ld and its
    # DLLs from there, and nothing else may answer to those names mid-build.
    $configureArgs = @('-B', 'build', '-S', '.', '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$BuildType",
                       "-DCMAKE_C_COMPILER=$gcc", "-DCMAKE_CXX_COMPILER=$gxx",
                       "-DCMAKE_MAKE_PROGRAM=$ninja", "-DDEKI_GXX16=$gxx")
    if ($Build) {
        Step "Building DekiEditor ($BuildType)"
        $env:Path = "$MingwBin;$env:Path"
        Push-Location $RepoRoot
        try {
            & $cmake @configureArgs
            if ($LASTEXITCODE -ne 0) { Fail "cmake configure failed (exit $LASTEXITCODE)" }
            & $cmake --build build --target DekiEditor
            if ($LASTEXITCODE -ne 0) { Fail "cmake build failed (exit $LASTEXITCODE)" }
            $exe = Join-Path $RepoRoot 'build\bin\DekiEditor.exe'
            if (-not (Test-Path $exe)) { Fail "the build finished but $exe is missing." }
            Ok $exe
        } finally {
            Pop-Location
        }
    }

    Step 'Done'
    if (-not $Build) {
        Write-Host "    Build the editor (from $RepoRoot):"
        Write-Host ("      & `"$cmake`" " + ($configureArgs -join ' '))
        Write-Host "      & `"$cmake`" --build build --target DekiEditor"
        Write-Host '    or re-run with -Build.'
    }
    Write-Host "    log: $Log"
}
catch {
    $failed = $true
    Write-Host "`nSetup failed during: $script:CurrentStep" -ForegroundColor Red
    Write-Host "    $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Full log: $Log"
}
finally {
    Stop-Transcript | Out-Null
}
if ($failed) { exit 1 }
