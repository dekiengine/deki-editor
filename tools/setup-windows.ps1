# Set up a Windows machine to build and develop Deki Editor.
#
# The Windows counterpart of tools/setup-linux.sh. Run it with no options in a
# terminal and it asks what to do:
#
#   1) Build tools only       MSYS2 with GCC 16, CMake and Ninja; git + git-lfs
#   2) Build tools + editor   ... and build DekiEditor
#   3) Everything             ... and download every Deki package to develop with
#   4) Deki packages only     download / update the packages, demo and registry
#
# That is the Setup tab of a full-screen menu driven with the arrow keys and
# Tab, or the mouse (click a tab, a line or a button; the wheel scrolls); the Options tab sets the build type, the clone method and the PATH
# change, and the Status tab shows what is installed already.
#
# With options (or without a terminal) it does what the options say instead, so
# CI and scripts never see the menu.
#
# SETUP.md section 2 lists the prerequisites. The one that matters is GCC 16: the
# reflection codegen needs C++26 static reflection, nothing on Windows ships
# it, and an MSVC build still needs it. MSYS2 provides GCC, CMake and Ninja
# together, which is the MinGW route SETUP.md section 3 describes - so with this
# script no Visual Studio is needed at all.
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
#   7. configure + build          only with -Build, with MSYS2's tools by full path
#   8. tools/get-packages.sh      only with -GetPackages (packages, demo, registry)
#
# What it changes on the system, and nothing else:
#   - installs Git / Git LFS / MSYS2 with winget when missing (UAC prompt)
#   - updates MSYS2's own packages (pacman -Syu); everything stays in C:\msys64
#   - appends to the user PATH (not the machine PATH), at the end, so tools
#     already on PATH keep precedence - the build itself never relies on PATH
#   - git lfs install for the current user (~\.gitconfig)
#
# Usage: tools\setup-windows.ps1                       (menu)
#        tools\setup-windows.ps1 [-Yes] [-Msys2 C:\msys64] [-SkipPath]
#                                [-Build [-BuildType Debug]] [-GetPackages [-Ssh]]
# With no options and no terminal it installs the build tools only.
#
# Run from a normal (non-admin) PowerShell. A log is written to
# %LOCALAPPDATA%\deki (printed at the start and on failure). Open a new terminal
# afterwards so the PATH change applies.

[CmdletBinding()]
param(
    [switch]$Yes,
    [string]$Msys2     = "C:\msys64",
    [switch]$SkipPath,
    [switch]$GetPackages,
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
$Tools      = $true
$Interactive = -not [Console]::IsInputRedirected -and -not [Console]::IsOutputRedirected

$script:CurrentStep = 'start'
$failed = $false
function Step($msg) { $script:CurrentStep = $msg; Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Ok($msg)   { Write-Host "    $msg" -ForegroundColor Green }
function Warn($msg) { Write-Host "    warning: $msg" -ForegroundColor Yellow }
function Fail($msg) { throw $msg }

function Have($cmd) { [bool](Get-Command $cmd -ErrorAction SilentlyContinue) }

# --- Terminal UI -----------------------------------------------------------
# Drawn with [Console] directly, so none of it lands in the transcript; the
# caller logs what was chosen. Only characters that every Windows console font
# has (code page 437's box drawing), so it draws the same in conhost and
# Windows Terminal; they are written as [char] codes because Windows
# PowerShell 5.1 reads a script without a BOM as ANSI.
$G = @{
    TL = [char]0x250C; TR = [char]0x2510; BL = [char]0x2514; BR = [char]0x2518
    H  = [char]0x2500; V  = [char]0x2502; LT = [char]0x251C; RT = [char]0x2524
    SEL = [char]0x25BA; OK = [char]0x221A; NO = [char]0x00D7
    L = [char]0x00AB; R = [char]0x00BB
    LR = "$([char]0x2190)/$([char]0x2192)"; UD = "$([char]0x2191)/$([char]0x2193)"
}
$UiRows = 12   # content rows below the tab bar, the same on every tab

# Keys and the mouse. [Console]::ReadKey only sees keys, so the menus read the
# console's input records themselves: key presses, left clicks and the wheel.
# Anything that goes wrong here falls back to ReadKey - keys only, no mouse.
$script:MouseOk = $false
$script:ModeWas = $null
try {
    if (-not ('DekiConsoleInput' -as [type])) {
        Add-Type -ErrorAction Stop -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class DekiConsoleInput {
    [StructLayout(LayoutKind.Sequential)] public struct KEY_EVENT_RECORD {
        public int KeyDown; public ushort RepeatCount; public ushort VirtualKeyCode;
        public ushort VirtualScanCode; public ushort UnicodeChar; public uint ControlKeyState; }
    [StructLayout(LayoutKind.Sequential)] public struct MOUSE_EVENT_RECORD {
        public short X; public short Y; public uint ButtonState; public uint ControlKeyState; public uint EventFlags; }
    [StructLayout(LayoutKind.Explicit)] public struct INPUT_RECORD {
        [FieldOffset(0)] public ushort EventType;
        [FieldOffset(4)] public KEY_EVENT_RECORD Key;
        [FieldOffset(4)] public MOUSE_EVENT_RECORD Mouse; }
    [DllImport("kernel32.dll", SetLastError = true)] public static extern IntPtr GetStdHandle(int n);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool GetConsoleMode(IntPtr h, out uint mode);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool SetConsoleMode(IntPtr h, uint mode);
    [DllImport("kernel32.dll", SetLastError = true, EntryPoint = "ReadConsoleInputW")]
    public static extern bool ReadConsoleInput(IntPtr h, [Out] INPUT_RECORD[] buf, uint len, out uint read);
}
'@
    }
    $script:MouseOk = $true
} catch { }

# Mouse input on and QuickEdit (click-to-select) off while a menu is up -
# QuickEdit would take the clicks. Mouse-Off puts the console back as it was.
function Mouse-On {
    if (-not $script:MouseOk) { return }
    $h = [DekiConsoleInput]::GetStdHandle(-10)
    $m = [uint32]0
    if (-not [DekiConsoleInput]::GetConsoleMode($h, [ref]$m)) { $script:MouseOk = $false; return }
    $script:ModeWas = $m
    # ENABLE_MOUSE_INPUT 0x10 | ENABLE_EXTENDED_FLAGS 0x80, minus ENABLE_QUICK_EDIT_MODE 0x40
    [void][DekiConsoleInput]::SetConsoleMode($h, [uint32](($m -bor 0x90) -band 0xFFFFFFBF))
}
function Mouse-Off {
    if ($null -ne $script:ModeWas) {
        [void][DekiConsoleInput]::SetConsoleMode([DekiConsoleInput]::GetStdHandle(-10), [uint32]$script:ModeWas)
        $script:ModeWas = $null
    }
}

# The next key or mouse event: Kind Key (Key = a [ConsoleKey] name, Char,
# Shift), Click (X, Y: left button, in console cells) or Wheel (Delta > 0 up).
function Read-UiEvent {
    if (-not $script:MouseOk -or $null -eq $script:ModeWas) {
        $k = [Console]::ReadKey($true)
        return [pscustomobject]@{ Kind = 'Key'; Key = [string]$k.Key; Char = $k.KeyChar
                                  Shift = [bool]($k.Modifiers -band [ConsoleModifiers]::Shift) }
    }
    $h = [DekiConsoleInput]::GetStdHandle(-10)
    $buf = New-Object 'DekiConsoleInput+INPUT_RECORD[]' 1
    $read = [uint32]0
    while ($true) {
        if (-not [DekiConsoleInput]::ReadConsoleInput($h, $buf, 1, [ref]$read)) {
            $script:MouseOk = $false
            return Read-UiEvent
        }
        $r = $buf[0]
        if ($r.EventType -eq 1 -and $r.Key.KeyDown -ne 0) {
            $vk = [int]$r.Key.VirtualKeyCode
            if ($vk -in 16, 17, 18, 20, 91, 92) { continue }   # Shift, Ctrl, Alt, Caps Lock, Windows alone
            return [pscustomobject]@{ Kind = 'Key'; Key = [string][ConsoleKey]$vk; Char = [char]$r.Key.UnicodeChar
                                      Shift = [bool]($r.Key.ControlKeyState -band 0x10) }
        }
        if ($r.EventType -eq 2) {
            $m = $r.Mouse
            if ($m.EventFlags -eq 4) {   # MOUSE_WHEELED: the high word is the signed distance
                $up = -not (($m.ButtonState -shr 16) -band 0x8000)
                return [pscustomobject]@{ Kind = 'Wheel'; Delta = $(if ($up) { 1 } else { -1 }) }
            }
            if (($m.EventFlags -eq 0 -or $m.EventFlags -eq 2) -and ($m.ButtonState -band 1)) {
                return [pscustomobject]@{ Kind = 'Click'; X = [int]$m.X; Y = [int]$m.Y }
            }
        }
    }
}

function Ui-Width { $script:UiW = [Math]::Min(86, [Math]::Max(40, [Console]::WindowWidth - 3)) }
function Seg($t, $f = $null, $b = $null) { [pscustomobject]@{ T = [string]$t; F = $f; B = $b } }
function Ui-Top($title) {
    $t = " $title "
    [Console]::WriteLine("$($G.TL)$($G.H)$t" + ([string]$G.H * ($script:UiW - 1 - $t.Length)) + "$($G.TR)")
}
function Ui-Mid { [Console]::WriteLine("$($G.LT)" + ([string]$G.H * $script:UiW) + "$($G.RT)") }
function Ui-Bot { [Console]::WriteLine("$($G.BL)" + ([string]$G.H * $script:UiW) + "$($G.BR)") }
# A row inside the box, from segments (Seg text foreground background).
function Ui-Row([object[]]$Segs) {
    $room = $script:UiW - 2
    $len = 0
    [Console]::Write("$($G.V) ")
    foreach ($s in $Segs) {
        $t = $s.T
        if ($len + $t.Length -gt $room) { $t = $t.Substring(0, [Math]::Max(0, $room - $len)) }
        if ($s.F) { [Console]::ForegroundColor = $s.F }
        if ($s.B) { [Console]::BackgroundColor = $s.B }
        [Console]::Write($t)
        [Console]::ResetColor()
        $len += $t.Length
    }
    [Console]::WriteLine((' ' * ($room - $len)) + " $($G.V)")
    $script:UiN++
}

# What is installed now, for the Status tab - quick checks only.
function Collect-Status {
    $script:Status = New-Object System.Collections.ArrayList
    function Add($name, $ok, $text) { [void]$script:Status.Add(@($name, $ok, $text)) }
    if (Have 'git') { Add 'git' $true ((git --version) -replace '^git version ', '') } else { Add 'git' $false 'not installed' }
    if (Git-Lfs-Present) { Add 'git-lfs' $true ((git lfs version) -replace '^git-lfs/(\S+).*', '$1') } else { Add 'git-lfs' $false 'not installed' }
    if (Test-Path $MsysBash) { Add 'MSYS2' $true $Msys2 } else { Add 'MSYS2' $false 'not installed' }
    $gxx = Join-Path $MingwBin 'g++.exe'
    if (Test-Path $gxx) {
        $v = (& $gxx -dumpfullversion).Trim()
        if ([version]$v -ge $MinGcc) { Add 'GCC 16' $true $v } else { Add 'GCC 16' $false "$v is too old" }
    } else { Add 'GCC 16' $false 'not installed' }
    $c = Join-Path $MingwBin 'cmake.exe'
    if (Test-Path $c) { Add 'CMake' $true (((& $c --version) | Select-Object -First 1) -replace '^cmake version ', '') } else { Add 'CMake' $false 'not installed' }
    $nj = Join-Path $MingwBin 'ninja.exe'
    if (Test-Path $nj) { Add 'Ninja' $true (& $nj --version) } else { Add 'Ninja' $false 'not installed' }
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    if (@($userPath -split ';') | Where-Object { $_.TrimEnd('\') -ieq $MingwBin }) { Add 'PATH' $true "has $MingwBin" }
    else { Add 'PATH' $false "no $MingwBin" }
    $exe = Join-Path $RepoRoot 'build\bin\DekiEditor.exe'
    $cache = Join-Path $RepoRoot 'build\CMakeCache.txt'
    if (Test-Path $exe) {
        $src = (Select-String -Path $cache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)' -ErrorAction SilentlyContinue | Select-Object -First 1)
        $bt  = (Select-String -Path $cache -Pattern '^CMAKE_BUILD_TYPE:STRING=(.*)' -ErrorAction SilentlyContinue | Select-Object -First 1)
        $srcDir = if ($src) { $src.Matches[0].Groups[1].Value.Replace('/', '\').TrimEnd('\') } else { '' }
        if ($srcDir -and $srcDir -ine $RepoRoot.TrimEnd('\')) { Add 'Editor build' $false 'made in another folder - will be rebuilt' }
        else { Add 'Editor build' $true ("{0}  build\bin\DekiEditor.exe" -f $(if ($bt) { $bt.Matches[0].Groups[1].Value } else { 'built' })) }
    } else { Add 'Editor build' $false 'not built yet' }
    $ws = Split-Path -Parent $RepoRoot
    $n = @(Get-ChildItem (Join-Path $ws 'packages') -Directory -Filter 'deki-*' -ErrorAction SilentlyContinue |
           Where-Object { Test-Path (Join-Path $_.FullName '.git') }).Count
    if ($n -gt 0) { Add 'Deki packages' $true "$n cloned in $ws\packages" } else { Add 'Deki packages' $false 'none yet' }
    if (Test-Path (Join-Path $ws 'deki-demo\.git')) { Add 'Demo project' $true "$ws\deki-demo" } else { Add 'Demo project' $false 'not cloned' }
    $script:SshOk = GitHub-Ssh-Works
    if ($script:SshOk) { Add 'GitHub SSH' $true 'key accepted' } else { Add 'GitHub SSH' $false 'no key accepted - packages will use HTTPS' }
}

# The main screen. Sets $Tools / $Build / $BuildType / $GetPackages / $Ssh /
# $SkipPath; returns $false if the user quit.
function Run-Tui {
    $tabs  = @('Setup', 'Options', 'Status')
    $actL  = @('Build tools only', 'Build tools + editor', 'Everything', 'Deki packages only')
    $actD  = @('MSYS2 with GCC 16, CMake and Ninja; git + git-lfs', '... and build DekiEditor',
               '... and download every Deki package to develop with', 'download / update the packages, demo and registry')
    $names = @('Build type', 'Clone packages over', 'Add MSYS2 to PATH')
    $ws = Split-Path -Parent $RepoRoot
    $tab = 0; $sel = 1; $opt = 0
    $lastW = -1
    while ($true) {
        Ui-Width
        if ($script:UiW -ne $lastW) { [Console]::Clear(); $lastW = $script:UiW }
        [Console]::SetCursorPosition(0, 0)
        $clone = if ($script:Ssh) { 'SSH' } else { 'HTTPS' }

        Ui-Top 'Deki setup'
        Ui-Row (Seg $RepoRoot 'DarkGray')
        Ui-Row (Seg '')
        $segs = @()
        for ($i = 0; $i -lt 3; $i++) {
            if ($i -eq $tab) { $segs += Seg " $($tabs[$i]) " 'Black' 'Cyan' } else { $segs += Seg " $($tabs[$i]) " 'Gray' }
            $segs += Seg '   '
        }
        Ui-Row $segs
        Ui-Mid
        $script:UiN = 0
        Ui-Row (Seg '')
        switch ($tab) {
            0 {
                for ($i = 0; $i -lt 4; $i++) {
                    $label = $actL[$i].PadRight(24) + $actD[$i]
                    if ($i -eq $sel) { Ui-Row (Seg "  $($G.SEL) $label" 'Cyan') } else { Ui-Row (Seg "    $label") }
                }
                Ui-Row (Seg '')
                $summary = switch ($sel) {
                    0 { 'Installs only what is missing, and asks before changing anything.' }
                    1 { "$script:BuildType build in build\   (Options tab to change)" }
                    2 { "$script:BuildType build, packages over $clone into $ws" }
                    3 { "Packages over $clone into $ws" }
                }
                Ui-Row (Seg "    $summary" 'DarkGray')
            }
            1 {
                $vals  = @($script:BuildType, $clone, $(if ($script:SkipPath) { 'no' } else { 'yes' }))
                $notes = @('Debug for developing, Release optimised',
                           $(if ($script:SshOk) { "GitHub accepts this machine's key" } else { 'no SSH key on GitHub here: HTTPS works' }),
                           "so new terminals find g++, cmake and ninja")
                for ($i = 0; $i -lt 3; $i++) {
                    if ($i -eq $opt) {
                        Ui-Row (Seg ("  $($G.SEL) " + $names[$i].PadRight(22) + "$($G.L) $($vals[$i]) $($G.R)".PadRight(12)) 'Cyan'), (Seg $notes[$i] 'DarkGray')
                    } else {
                        Ui-Row (Seg ("    " + $names[$i].PadRight(22) + "  $($vals[$i])".PadRight(12))), (Seg $notes[$i] 'DarkGray')
                    }
                }
                Ui-Row (Seg '')
                Ui-Row (Seg '    Used by the Setup choices that build or download packages.' 'DarkGray')
            }
            2 {
                foreach ($s in $script:Status) {
                    if ($s[1]) { $mark = Seg $G.OK 'Green' } else { $mark = Seg $G.NO 'Yellow' }
                    Ui-Row (Seg ("  " + $s[0].PadRight(16))), $mark, (Seg "  $($s[2])")
                }
            }
        }
        while ($script:UiN -lt $UiRows) { Ui-Row (Seg '') }
        Ui-Mid
        $hint = switch ($tab) {
            0 { "$($G.LR) Tab: tabs   $($G.UD): move   Enter: start   Esc: quit" }
            1 { "Tab: tabs   $($G.UD): move   $($G.LR) Space: change   Esc: quit" }
            2 { "$($G.LR) Tab: tabs   R: check again   Esc: quit" }
        }
        Ui-Row (Seg $hint 'DarkGray')
        Ui-Bot

        $change = { if ($opt -eq 0) { $script:BuildType = if ($script:BuildType -eq 'Debug') { 'Release' } else { 'Debug' } }
                    elseif ($opt -eq 1) { $script:Ssh = -not $script:Ssh }
                    else { $script:SkipPath = -not $script:SkipPath } }
        $start = $false
        $ev = Read-UiEvent
        if ($ev.Kind -eq 'Click') {
            # Screen rows, from the top: border, folder, blank, tab bar (3),
            # rule, blank, then the tab's lines from row 6.
            if ($ev.Y -eq 3) {
                $tx = 2
                for ($i = 0; $i -lt 3; $i++) {
                    $w = $tabs[$i].Length + 2
                    if ($ev.X -ge $tx -and $ev.X -lt $tx + $w) { $tab = $i }
                    $tx += $w + 3
                }
            } elseif ($tab -eq 0 -and $ev.Y -ge 6 -and $ev.Y -le 9) {
                $sel = $ev.Y - 6; $start = $true
            } elseif ($tab -eq 1 -and $ev.Y -ge 6 -and $ev.Y -le 8) {
                $opt = $ev.Y - 6; & $change
            }
        } else {
            $k = $ev.Key
            if ($ev.Kind -eq 'Wheel') { $k = if ($ev.Delta -gt 0) { 'UpArrow' } else { 'DownArrow' } }
            if ($k -eq 'Tab' -and $ev.Shift) { $k = 'BackTab' }
            switch ($k) {
                { $_ -in 'Escape', 'Q' } { [Console]::Clear(); return $false }
                'Tab'     { $tab = ($tab + 1) % 3 }
                'BackTab' { $tab = ($tab + 2) % 3 }
                { $_ -in 'UpArrow', 'K' } {
                    if ($tab -eq 0) { $sel = ($sel + 3) % 4 }
                    if ($tab -eq 1) { $opt = ($opt + 2) % 3 }
                }
                { $_ -in 'DownArrow', 'J' } {
                    if ($tab -eq 0) { $sel = ($sel + 1) % 4 }
                    if ($tab -eq 1) { $opt = ($opt + 1) % 3 }
                }
                { $_ -in 'LeftArrow', 'RightArrow', 'Spacebar', 'Enter' } {
                    if ($tab -eq 1) { & $change }
                    elseif ($_ -eq 'Enter' -and $tab -eq 0) { $start = $true }
                    elseif ($_ -eq 'RightArrow') { $tab = ($tab + 1) % 3 }
                    elseif ($_ -eq 'LeftArrow') { $tab = ($tab + 2) % 3 }
                }
                'R' { if ($tab -eq 2) { Collect-Status } }
            }
            $d = [int]$ev.Char - [int][char]'1'
            if ($tab -eq 0 -and $d -ge 0 -and $d -lt 4) { $sel = $d; $start = $true }
        }
        if ($start) {
            [Console]::Clear()
            $script:Tools = $true; $script:Build = $false; $script:GetPackages = $false
            switch ($sel) {
                1 { $script:Build = $true }
                2 { $script:Build = $true; $script:GetPackages = $true }
                3 { $script:Tools = $false; $script:GetPackages = $true }
            }
            return $true
        }
    }
}

# A boxed menu drawn in place: Up/Down (or k/j) move, Enter chooses, a digit
# picks that line, Esc or q cancels. $items is a list of @(label, description).
# Returns the chosen index, or -1.
function Select-Menu($title, $items, [int]$default = 0) {
    $i = $default
    $n = $items.Count
    Ui-Width
    [Console]::WriteLine()
    # Reserve the lines first, so a console that scrolls does so before $top
    # is taken and the redraws land in place.
    for ($k = 0; $k -lt $n + 3; $k++) { [Console]::WriteLine() }
    $top = [Console]::CursorTop - ($n + 3)
    while ($true) {
        [Console]::SetCursorPosition(0, $top)
        Ui-Top $title
        for ($k = 0; $k -lt $n; $k++) {
            $label = $items[$k][0].PadRight(14) + $items[$k][1]
            if ($k -eq $i) { Ui-Row (Seg "  $($G.SEL) $label" 'Cyan') } else { Ui-Row (Seg "    $label") }
        }
        Ui-Row (Seg "$($G.UD): move   Enter or click: choose   Esc: cancel" 'DarkGray')
        Ui-Bot
        $ev = Read-UiEvent
        if ($ev.Kind -eq 'Click') {
            if ($ev.Y -gt $top -and $ev.Y -le $top + $n) { return ($ev.Y - $top - 1) }
            continue
        }
        $k = $ev.Key
        if ($ev.Kind -eq 'Wheel') { $k = if ($ev.Delta -gt 0) { 'UpArrow' } else { 'DownArrow' } }
        switch ($k) {
            { $_ -in 'UpArrow', 'K' }   { $i = ($i - 1 + $n) % $n }
            { $_ -in 'DownArrow', 'J' } { $i = ($i + 1) % $n }
            'Enter' { return $i }
            { $_ -in 'Escape', 'Q' } { return -1 }
        }
        $d = [int]$ev.Char - [int][char]'1'
        if ($d -ge 0 -and $d -lt $n) { return $d }
    }
}

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

# Git for Windows' bash: get-packages.sh needs git and curl on its PATH, which
# MSYS2's own bash does not have.
function Git-Bash {
    if (-not (Have 'git')) { return $null }
    $p = Join-Path (Split-Path -Parent (Split-Path -Parent (Get-Command git).Source)) 'bin\bash.exe'
    if (Test-Path $p) { return $p } else { return $null }
}

# Does GitHub accept this machine's SSH key, through the ssh git itself uses?
# (Decides the default clone URL.) BatchMode never prompts, and an unknown host
# key fails rather than being trusted unseen - then HTTPS is the default.
function GitHub-Ssh-Works {
    if (-not (Have 'git')) { return $false }
    $saved = $env:GIT_SSH_COMMAND
    try {
        $env:GIT_SSH_COMMAND = 'ssh -o BatchMode=yes -o ConnectTimeout=8'
        git ls-remote git@github.com:dekiengine/deki-editor.git HEAD *> $null
        return ($LASTEXITCODE -eq 0)
    } finally {
        $env:GIT_SSH_COMMAND = $saved
    }
}

$LogDir = Join-Path $env:LOCALAPPDATA 'deki'
New-Item -ItemType Directory -Force $LogDir | Out-Null
$Log = Join-Path $LogDir ("setup-windows-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
Start-Transcript -Path $Log | Out-Null

# The menus draw box characters; the console must take them as UTF-8.
$encodingWas = [Console]::OutputEncoding
if ($Interactive) { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false }

try {
    # --- Menu --------------------------------------------------------------
    if ($PSBoundParameters.Count -eq 0 -and $Interactive) {
        $script:CurrentStep = 'menu'
        [Console]::Write('  Checking this machine...')
        Collect-Status
        $Ssh = $script:SshOk
        $cursorWas = [Console]::CursorVisible
        [Console]::CursorVisible = $false
        Mouse-On
        try { $go = Run-Tui } finally { Mouse-Off; [Console]::CursorVisible = $cursorWas }
        if (-not $go) { Write-Host 'Nothing changed.'; return }
        Write-Host ("    chose: tools={0} build={1} ({2}) packages={3}{4} path={5}" -f $Tools, [bool]$Build, $BuildType,
                    [bool]$GetPackages, $(if ($GetPackages) { $(if ($Ssh) { ' over SSH' } else { ' over HTTPS' }) } else { '' }),
                    $(if ($SkipPath) { 'skip' } else { 'add' }))
    }

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

    if ($Build -and -not (Test-Path (Join-Path $RepoRoot 'deki-engine\CMakeLists.txt'))) {
        Fail "deki-engine\ is empty - the submodule is missing. Run: git -C `"$RepoRoot`" submodule update --init --recursive"
    }

    # A checkout that was moved or copied keeps a build\ configured for the old
    # folder, and CMake refuses it ("CMakeCache.txt directory is different").
    # build\ is generated output only, so it is started again from scratch.
    $buildDir = Join-Path $RepoRoot 'build'
    $staleBuild = $null
    $cache = Join-Path $buildDir 'CMakeCache.txt'
    if ($Build -and (Test-Path $cache)) {
        $line = Select-String -Path $cache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=' | Select-Object -First 1
        if ($line) {
            $cachedSrc = ($line.Line -replace '^[^=]*=', '').Trim()
            $norm = { param($p) $p.Replace('/', '\').TrimEnd('\') }
            if ((& $norm $cachedSrc) -ine (& $norm $RepoRoot)) { $staleBuild = $cachedSrc }
        }
    }

    $drive = Get-PSDrive -Name ((Resolve-Path $RepoRoot).Drive.Name)
    $freeGB = [math]::Floor($drive.Free / 1GB)
    if ($freeGB -lt $MinFreeGB) {
        Warn "only $freeGB GB free on $($drive.Name):; MSYS2 plus a first build needs about $MinFreeGB GB."
    }

    # What is missing, so the plan says exactly what will happen.
    $needGit   = -not (Have 'git')
    $needLfs   = -not (Git-Lfs-Present)
    $needMsys2 = $false
    $missingPkgs = @()
    $needPath  = $false
    $msysProcs = @()
    if ($Tools) {
        $needMsys2 = -not (Test-Path $MsysBash)
        if ($needMsys2 -and $Msys2 -ne 'C:\msys64') {
            Fail "No MSYS2 at $Msys2. winget installs to C:\msys64 - install it there by hand, or omit -Msys2."
        }
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

        # pacman -Syu replaces MSYS2's runtime and closes its processes; a
        # terminal left open would be killed mid-session, or block the update.
        if (-not $needMsys2) {
            $msysProcs = @(Get-Process -ErrorAction SilentlyContinue |
                           Where-Object { $_.Path -and $_.Path.StartsWith($Msys2, [StringComparison]::OrdinalIgnoreCase) })
        }
    } else {
        # Packages only: installing git and git-lfs is the build-tools step's job.
        if ($needGit -or $needLfs) { Fail 'git or git-lfs is not installed. Run this again and choose 1 (build tools) first.' }
        if (-not (Git-Bash)) { Fail 'Git Bash (part of Git for Windows) not found; get-packages.sh runs in it.' }
    }

    # --- Plan + confirmation -----------------------------------------------
    Step 'Plan'
    if ($Tools) {
        if ($needGit)   { Write-Host '    install with winget: Git.Git' }
        if ($needLfs -and -not $needGit) { Write-Host '    install with winget: GitHub.GitLFS' }
        if ($needMsys2) { Write-Host "    install with winget: MSYS2.MSYS2 (into C:\msys64, UAC prompt)" }
        Write-Host '    update MSYS2 packages (pacman -Syu, twice)'
        if ($missingPkgs.Count -gt 0) { Write-Host ("    install with pacman: " + ($missingPkgs -join ' ')) }
        Write-Host "    git lfs install for $env:USERNAME"
        if ($needPath) { Write-Host "    append to user PATH: $MingwBin" }
    }
    if ($staleBuild) { Write-Host "    delete $buildDir (configured for $staleBuild - the checkout moved) and build from scratch" }
    if ($Build) { Write-Host "    build DekiEditor ($BuildType) in $buildDir" }
    if ($GetPackages) {
        Write-Host ("    download / update every Deki package, deki-demo and the registry into {0} over {1}" -f
                    (Split-Path -Parent $RepoRoot), $(if ($Ssh) { 'SSH' } else { 'HTTPS' }))
    }
    if ($msysProcs.Count -gt 0) {
        Warn ("MSYS2 programs are running ({0}); close them first - the update will end them." -f
              (($msysProcs | Select-Object -ExpandProperty ProcessName -Unique) -join ', '))
    }

    if (-not $Yes) {
        if (-not $Interactive) { Fail 'not running in a terminal, so cannot ask; pass -Yes to proceed without asking.' }
        Mouse-On
        try {
            $go = Select-Menu 'Go ahead with this plan?' @(
                @('Continue', 'make the changes above'),
                @('Cancel',   'change nothing')
            ) 0
        } finally { Mouse-Off }
        if ($go -ne 0) { Write-Host '    Nothing changed.'; return }
        Write-Host '    confirmed'
    }

    if ($Tools) {
        # --- 1. git + git-lfs ----------------------------------------------
        # Package install shells out to git, and packages may keep assets in LFS.
        Step 'Installing git + git-lfs'
        if (-not (Have 'git')) { Winget-Install 'Git.Git' }
        if (-not (Git-Lfs-Present)) { Winget-Install 'GitHub.GitLFS' }
        Ok (git --version)
        Ok (git lfs version)

        # --- 2. MSYS2 ------------------------------------------------------
        Step "Installing MSYS2 ($Msys2)"
        if (-not (Test-Path $MsysBash)) {
            Winget-Install 'MSYS2.MSYS2'
            if (-not (Test-Path $MsysBash)) { Fail "MSYS2 installed but $MsysBash is missing." }
            # First start initialises the keyring and the user's home.
            $null = Msys 'true'
        }
        Ok "found $MsysBash"

        # --- 3. gcc, cmake, ninja ------------------------------------------
        # -Syu twice: the first pass may update the core (pacman, msys2-runtime)
        # and exit early, the second updates everything else. Its exit code is
        # ignored for that reason; the -S below fails loudly if the update broke.
        Step 'Installing gcc, cmake, ninja (pacman)'
        $null = Msys 'pacman -Syu --noconfirm'
        $null = Msys 'pacman -Syu --noconfirm'
        $rc = Msys ("pacman -S --needed --noconfirm " + ($MsysPkgs -join ' '))
        if ($rc -ne 0) { Fail "pacman -S failed (exit $rc)" }

        # --- 4. GCC with C++26 reflection ----------------------------------
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

        # --- 5. git-lfs ----------------------------------------------------
        Step 'Setting up git-lfs'
        git lfs install --skip-repo *> $null
        if ($LASTEXITCODE -ne 0) { Fail "git lfs install failed (exit $LASTEXITCODE)" }
        Ok 'git lfs install --skip-repo'

        # --- 6. PATH -------------------------------------------------------
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

        $configureArgs = @('-B', 'build', '-S', '.', '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$BuildType",
                           "-DCMAKE_C_COMPILER=$gcc", "-DCMAKE_CXX_COMPILER=$gxx",
                           "-DCMAKE_MAKE_PROGRAM=$ninja", "-DDEKI_GXX16=$gxx")
    }

    # --- 7. Build (optional) -----------------------------------------------
    # MSYS2's bin goes first on this process's PATH: g++ runs as, ld and its
    # DLLs from there, and nothing else may answer to those names mid-build.
    if ($Build) {
        Step "Building DekiEditor ($BuildType)"
        if ($staleBuild) {
            Remove-Item -Recurse -Force $buildDir
            Ok "removed $buildDir (was configured for $staleBuild)"
        }
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

    # --- 8. Deki packages (optional) ---------------------------------------
    if ($GetPackages) {
        Step 'Downloading Deki packages (tools/get-packages.sh)'
        $gitBash = Git-Bash
        if (-not $gitBash) { Fail 'Git Bash (part of Git for Windows) not found; get-packages.sh runs in it.' }
        $pkgArgs = @()
        if ($Ssh) { $pkgArgs += '--ssh' }
        & $gitBash (Join-Path $RepoRoot 'tools/get-packages.sh') @pkgArgs
        if ($LASTEXITCODE -ne 0) { Fail "get-packages.sh reported failures (exit $LASTEXITCODE) - see its table above." }
    }

    Step 'Done'
    if ($Tools -and -not $Build) {
        Write-Host "    Build the editor (from $RepoRoot):"
        Write-Host ("      & `"$cmake`" " + ($configureArgs -join ' '))
        Write-Host "      & `"$cmake`" --build build --target DekiEditor"
        Write-Host '    or run this again and choose 2.'
    }
    if ($Build) { Write-Host "    Run the editor:  $RepoRoot\build\bin\DekiEditor.exe" }
    if ($GetPackages) { Write-Host ("    Open the workspace:  code `"{0}`"" -f (Split-Path -Parent $RepoRoot)) }
    Write-Host "    log: $Log"
}
catch {
    $failed = $true
    Write-Host "`nSetup failed during: $script:CurrentStep" -ForegroundColor Red
    Write-Host "    $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Full log: $Log"
}
finally {
    [Console]::OutputEncoding = $encodingWas
    Stop-Transcript | Out-Null
}
if ($failed) { exit 1 }
