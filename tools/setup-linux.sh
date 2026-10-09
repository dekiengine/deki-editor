#!/usr/bin/env bash
#
# Set up a Linux machine to build and develop Deki Editor.
#
# The Linux counterpart of tools/setup-windows.ps1. Run it with no options in a
# terminal and it asks what to do:
#
#   1) Build tools only       GCC 16, CMake, Ninja, git-lfs, SDL3's build deps
#   2) Build tools + editor   ... and build DekiEditor
#   3) Everything             ... and download every Deki package to develop with
#   4) Deki packages only     download / update the packages, demo and registry
#
# That is the Setup tab of a full-screen menu driven with the arrow keys and
# Tab, or the mouse (click a tab, a line or a button; the wheel scrolls); the Options tab sets the build type and the clone method, and the Status
# tab shows what is installed already.
#
# With options (or without a terminal) it does what the options say instead, so
# CI and scripts never see the menu.
#
# SETUP.md §2 lists the prerequisites. The one that matters is GCC 16: the
# reflection codegen needs C++26 static reflection (-freflection), which no
# current distribution ships by default. On Linux SDL3 is also built from
# source, so its X11/Wayland/audio development headers are needed too.
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
#   4. tools/get-packages.sh   only with --get-packages (packages, demo, registry)
#   5. configure + build       only with --build, with the exact compiler from 2
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
# Usage: tools/setup-linux.sh                      (menu)
#        tools/setup-linux.sh [--yes] [--build [--build-type T]] [--get-packages [--ssh]]
#     --yes             do not ask before installing (for CI and scripts)
#     --build           configure and build DekiEditor
#     --build-type T    Debug (default), Release, RelWithDebInfo or MinSizeRel
#     --get-packages    then run tools/get-packages.sh
#     --ssh             with --get-packages: clone over SSH
#     --allow-root      run as root (containers); the build will be owned by root
# With no options and no terminal it installs the build tools only.
#
# Run as your normal user; it calls sudo for the package installs only and asks
# for the password once, at the start. A log is written to $LOG_DIR (printed).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MIN_GCC="16.0"      # first filter only; the reflection test decides
MIN_FREE_GB=10      # toolchain + FetchContent deps + a Debug build
YES=0
TOOLS=1
GET_PACKAGES=0
USE_SSH=0
BUILD=0
BUILD_TYPE="Debug"
ALLOW_ROOT=0
ANY_OPTION=0

while [[ $# -gt 0 ]]; do
    ANY_OPTION=1
    case "$1" in
        --yes|-y) YES=1; shift ;;
        --get-packages) GET_PACKAGES=1; shift ;;
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
# on someone else's machine can be diagnosed from one file. Whether this is a
# terminal is decided before stdout becomes the tee pipe.
INTERACTIVE=0
[[ -t 0 && -t 1 ]] && INTERACTIVE=1
LOG_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/deki"
mkdir -p "$LOG_DIR"
LOG="$LOG_DIR/setup-linux-$(date +%Y%m%d-%H%M%S).log"
if [[ -t 1 ]]; then C_STEP=$'\033[36m' C_OK=$'\033[32m' C_WARN=$'\033[33m' C_ERR=$'\033[31m' C_BOLD=$'\033[1m' C_END=$'\033[0m'
else C_STEP="" C_OK="" C_WARN="" C_ERR="" C_BOLD="" C_END=""; fi
exec > >(tee -a "$LOG") 2>&1

CURRENT_STEP="start"
step() { CURRENT_STEP="$1"; printf '\n%s==> %s%s\n' "$C_STEP" "$1" "$C_END"; }
ok()   { printf '    %s%s%s\n' "$C_OK" "$1" "$C_END"; }
warn() { printf '    %swarning: %s%s\n' "$C_WARN" "$1" "$C_END"; }
die()  { printf '\n%serror: %s%s\n' "$C_ERR" "$1" "$C_END" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

on_exit() {
    local rc=$?
    # The menu hides the cursor and may be on the alternate screen; never
    # leave the terminal like that.
    if [[ $INTERACTIVE == 1 ]]; then
        { if [[ ${UI_ALT:-0} == 1 ]]; then printf '\033[?1049l' >/dev/tty; fi
          printf '\033[?1000l\033[?1006l\033[?25h' >/dev/tty; } 2>/dev/null || true
    fi
    if [[ $rc -ne 0 ]]; then
        printf '\n%sSetup failed during: %s (exit %d)%s\n' "$C_ERR" "$CURRENT_STEP" "$rc" "$C_END" >&2
        printf 'Full log: %s\n' "$LOG" >&2
    fi
}
trap on_exit EXIT

# version_ge A B: is version A >= B?
version_ge() { [[ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" == "$2" ]]; }

# Does GitHub accept this machine's SSH key? (Decides the default clone URL.)
# BatchMode: never prompts, and an unknown host key fails rather than being
# trusted unseen - then HTTPS is the default.
github_ssh_works() {
    have ssh || return 1
    ssh -o BatchMode=yes -o ConnectTimeout=8 -T git@github.com 2>&1 | grep -q 'successfully authenticated'
}

# --- Terminal UI -----------------------------------------------------------
# Everything here draws on /dev/tty, never into the log; the caller logs what
# was chosen. The main screen uses the terminal's alternate screen, so the
# scrollback is left as it was and the plan prints into it afterwards.
TTY=/dev/tty
UI_ALT=0
if [[ "$(locale charmap 2>/dev/null)" == UTF-8 ]]; then
    G_TL='╭' G_TR='╮' G_BL='╰' G_BR='╯' G_H='─' G_V='│' G_LT='├' G_RT='┤'
    G_SEL='▸' G_OK='✓' G_NO='✗' G_LR='←/→' G_UD='↑/↓' G_L='‹' G_R='›'
else
    G_TL='+' G_TR='+' G_BL='+' G_BR='+' G_H='-' G_V='|' G_LT='+' G_RT='+'
    G_SEL='>' G_OK='ok' G_NO='--' G_LR='Left/Right' G_UD='Up/Down' G_L='<' G_R='>'
fi
S_SEL=$'\033[1;36m' S_TAB=$'\033[1;30;46m' S_DIM=$'\033[2m' S_OK=$'\033[32m' S_BAD=$'\033[33m' S_END=$'\033[0m'

ui_width() {  # UI_W: inner width of the box - the terminal's, capped to read well
    local cols
    cols=$(stty size <$TTY 2>/dev/null | cut -d' ' -f2 || true)
    [[ "$cols" =~ ^[0-9]+$ ]] || cols=80
    UI_W=$(( cols - 2 ))
    if (( UI_W > 86 )); then UI_W=86; fi
    if (( UI_W < 40 )); then UI_W=40; fi
}
rep() { local s; printf -v s '%*s' "$2" ''; printf '%s' "${s// /$1}"; }
ui_top() { local t=" $1 "; printf '%s%s%s%s%s\n' "$G_TL" "$G_H" "$t" "$(rep "$G_H" $((UI_W - 1 - ${#t})))" "$G_TR"; }
ui_mid() { printf '%s%s%s\n' "$G_LT" "$(rep "$G_H" "$UI_W")" "$G_RT"; }
ui_bot() { printf '%s%s%s\n' "$G_BL" "$(rep "$G_H" "$UI_W")" "$G_BR"; }
# ui_row PLAIN [STYLED]: PLAIN's length decides the padding (bash's printf pads
# by bytes, not characters); STYLED, PLAIN with colour codes added, is drawn.
ui_row() {
    local plain=$1 styled=${2-$1} room=$((UI_W - 2))
    if (( ${#plain} > room )); then plain=${plain:0:room}; styled=$plain; fi
    printf '%s %s%*s %s\n' "$G_V" "$styled" $((room - ${#plain})) '' "$G_V"
}
pad() { local s="$1"; while (( ${#s} < $2 )); do s+=" "; done; printf '%s' "$s"; }

# What is installed now, for the Status tab - quick checks only.
ST_PLAIN=() ST_STYLED=() SSH_OK=0
st_add() {  # NAME OK TEXT
    local mark=$G_NO col=$S_BAD name
    if [[ $2 == 1 ]]; then mark=$G_OK col=$S_OK; fi
    name="$(pad "$1" 16)"
    ST_PLAIN+=("  $name$mark  $3")
    ST_STYLED+=("  $name$col$mark$S_END  $3")
}
collect_status() {
    ST_PLAIN=() ST_STYLED=()
    local c="" cand ws n=0 d cache src bt
    if have git; then st_add "git" 1 "$(git --version | awk '{print $3}')"; else st_add "git" 0 "not installed"; fi
    if git lfs version >/dev/null 2>&1; then
        st_add "git-lfs" 1 "$(git lfs version | sed 's|^git-lfs/\([^ ]*\).*|\1|')"
    else st_add "git-lfs" 0 "not installed"; fi
    for cand in g++-16 g++; do
        if have "$cand" && version_ge "$("$cand" -dumpfullversion)" "$MIN_GCC"; then c=$cand; break; fi
    done
    if [[ -n $c ]]; then st_add "GCC 16" 1 "$c $("$c" -dumpfullversion)"
    elif have g++; then st_add "GCC 16" 0 "not installed (g++ is $(g++ -dumpfullversion))"
    else st_add "GCC 16" 0 "not installed"; fi
    if have cmake; then st_add "CMake" 1 "$(cmake --version | head -n1 | awk '{print $3}')"; else st_add "CMake" 0 "not installed"; fi
    if have ninja; then st_add "Ninja" 1 "$(ninja --version)"; else st_add "Ninja" 0 "not installed"; fi
    cache="$REPO_ROOT/build/CMakeCache.txt"
    if [[ -x "$REPO_ROOT/build/bin/DekiEditor" ]]; then
        src=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache" 2>/dev/null | head -n1 || true)
        bt=$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$cache" 2>/dev/null | head -n1 || true)
        if [[ -n $src && "${src%/}" != "${REPO_ROOT%/}" ]]; then st_add "Editor build" 0 "made in another folder - will be rebuilt"
        else st_add "Editor build" 1 "${bt:-built}  build/bin/DekiEditor"; fi
    else st_add "Editor build" 0 "not built yet"; fi
    ws="$(dirname "$REPO_ROOT")"
    for d in "$ws"/packages/deki-*/; do
        if [[ -d "$d.git" ]]; then n=$((n + 1)); fi
    done
    if (( n > 0 )); then st_add "Deki packages" 1 "$n cloned in $ws/packages"; else st_add "Deki packages" 0 "none yet"; fi
    if [[ -d "$ws/deki-demo/.git" ]]; then st_add "Demo project" 1 "$ws/deki-demo"; else st_add "Demo project" 0 "not cloned"; fi
    if github_ssh_works; then SSH_OK=1; st_add "GitHub SSH" 1 "key accepted"
    else SSH_OK=0; st_add "GitHub SSH" 0 "no key accepted - packages will use HTTPS"; fi
}

UI_ROWS=11   # content rows below the tab bar, the same on every tab

# Mouse: button presses and the wheel, in the SGR form (1006), which reports
# any column. Only on while a menu is up, so the terminal's own text selection
# works everywhere else.
MOUSE_ON=$'\033[?1000h\033[?1006h'
MOUSE_OFF=$'\033[?1000l\033[?1006l'

# read_event: the next key or mouse event into EV - a character, or UP DOWN
# LEFT RIGHT BACKTAB ESC, or CLICK (left button, at EV_X / EV_Y, 1-based)
# WHEEL_UP WHEEL_DOWN; NONE for anything else.
read_event() {
    local key rest c="" seq="" b
    EV=NONE EV_X=0 EV_Y=0
    if ! IFS= read -rsn1 key <$TTY; then EV=q; return 0; fi
    if [[ $key != $'\e' ]]; then EV=$key; return 0; fi
    rest=""
    IFS= read -rsn2 -t 0.05 rest <$TTY || true
    case "$rest" in
        '[A'|'OA') EV=UP ;;
        '[B'|'OB') EV=DOWN ;;
        '[C'|'OC') EV=RIGHT ;;
        '[D'|'OD') EV=LEFT ;;
        '[Z') EV=BACKTAB ;;
        '') EV=ESC ;;
        '[<')   # mouse: ESC [ < button ; column ; row, then M (press) or m (release)
            while IFS= read -rsn1 -t 0.1 c <$TTY; do
                if [[ $c == [Mm] ]]; then break; fi
                seq+=$c
            done
            IFS=';' read -r b EV_X EV_Y <<<"$seq"
            if [[ $c == M && $b == 0 ]]; then EV=CLICK
            elif [[ $b == 64 ]]; then EV=WHEEL_UP
            elif [[ $b == 65 ]]; then EV=WHEEL_DOWN; fi ;;
    esac
    [[ $EV_X =~ ^[0-9]+$ ]] || EV_X=0
    [[ $EV_Y =~ ^[0-9]+$ ]] || EV_Y=0
    return 0
}

# Options tab: change the value on row $opt (run_tui's).
tui_change() {
    if [[ $opt == 0 ]]; then
        if [[ $BUILD_TYPE == Debug ]]; then BUILD_TYPE=Release; else BUILD_TYPE=Debug; fi
    else
        USE_SSH=$(( 1 - USE_SSH ))
    fi
}
ui_line() { ui_row "$@"; UI_N=$((UI_N + 1)); }
clone_word() { if [[ $USE_SSH == 1 ]]; then echo SSH; else echo HTTPS; fi; }

# The main screen. Sets TOOLS / BUILD / BUILD_TYPE / GET_PACKAGES / USE_SSH;
# returns 1 if the user quit.
run_tui() {
    local tab=0 sel=1 opt=0 key i x w plain styled label ws
    local tabs=("Setup" "Options" "Status")
    local act_l=("Build tools only" "Build tools + editor" "Everything" "Deki packages only")
    local act_d=("GCC 16, CMake, Ninja, git-lfs, SDL3's build deps"
                 "... and build DekiEditor"
                 "... and download every Deki package to develop with"
                 "download / update the packages, demo and registry")
    local names=("Build type" "Clone packages over")
    local vals notes
    ws="$(dirname "$REPO_ROOT")"
    UI_ALT=1
    printf '\033[?1049h\033[?25l\033[H\033[2J%s' "$MOUSE_ON" >$TTY
    while :; do
        ui_width
        {
            printf '\033[H'
            ui_top "Deki setup"
            ui_row "$REPO_ROOT" "$S_DIM$REPO_ROOT$S_END"
            ui_row ""
            plain="" styled=""
            for i in 0 1 2; do
                if [[ $i == "$tab" ]]; then styled+="$S_TAB ${tabs[i]} $S_END   "
                else styled+="$S_DIM ${tabs[i]} $S_END   "; fi
                plain+=" ${tabs[i]}    "
            done
            ui_row "$plain" "$styled"
            ui_mid
            UI_N=0
            ui_line ""
            case $tab in
                0)
                    for i in 0 1 2 3; do
                        label="$(pad "${act_l[i]}" 24)${act_d[i]}"
                        if [[ $i == "$sel" ]]; then ui_line "  $G_SEL $label" "$S_SEL  $G_SEL $label$S_END"
                        else ui_line "    $label"; fi
                    done
                    ui_line ""
                    case $sel in
                        0) plain="Installs only what is missing, and asks before changing anything." ;;
                        1) plain="$BUILD_TYPE build in build/   (Options tab to change)" ;;
                        2) plain="$BUILD_TYPE build, packages over $(clone_word) into $ws" ;;
                        3) plain="Packages over $(clone_word) into $ws" ;;
                    esac
                    ui_line "    $plain" "    $S_DIM$plain$S_END"
                    ;;
                1)
                    vals=("$BUILD_TYPE" "$(clone_word)")
                    if [[ $SSH_OK == 1 ]]; then notes=("Debug for developing, Release optimised" "GitHub accepts this machine's key")
                    else notes=("Debug for developing, Release optimised" "no SSH key on GitHub here: HTTPS works"); fi
                    for i in 0 1; do
                        if [[ $i == "$opt" ]]; then
                            label="$(pad "${names[i]}" 22)$(pad "$G_L ${vals[i]} $G_R" 12)"
                            ui_line "  $G_SEL $label${notes[i]}" "$S_SEL  $G_SEL $label$S_END$S_DIM${notes[i]}$S_END"
                        else
                            label="$(pad "${names[i]}" 22)$(pad "  ${vals[i]}" 12)"
                            ui_line "    $label${notes[i]}" "    $label$S_DIM${notes[i]}$S_END"
                        fi
                    done
                    ui_line ""
                    plain="Used by the Setup choices that build or download packages."
                    ui_line "    $plain" "    $S_DIM$plain$S_END"
                    ;;
                2)
                    for i in "${!ST_PLAIN[@]}"; do ui_line "${ST_PLAIN[i]}" "${ST_STYLED[i]}"; done
                    ;;
            esac
            while (( UI_N < UI_ROWS )); do ui_line ""; done
            ui_mid
            case $tab in
                0) plain="$G_LR Tab: tabs   $G_UD: move   Enter: start   Esc: quit" ;;
                1) plain="Tab: tabs   $G_UD: move   $G_LR Space: change   Esc: quit" ;;
                2) plain="$G_LR Tab: tabs   r: check again   Esc: quit" ;;
            esac
            ui_row "$plain" "$S_DIM$plain$S_END"
            ui_bot
            printf '\033[J'
        } >$TTY

        read_event
        key=$EV
        if [[ $key == WHEEL_UP ]]; then key=UP; fi
        if [[ $key == WHEEL_DOWN ]]; then key=DOWN; fi
        case "$key" in
            ESC|q|Q) UI_ALT=0; printf '%s\033[?25h\033[?1049l' "$MOUSE_OFF" >$TTY; return 1 ;;
            CLICK)
                # Screen rows, from the top (1-based): border, folder, blank,
                # tab bar (4), rule, blank, then the tab's lines from row 7.
                if (( EV_Y == 4 )); then
                    x=3
                    for i in 0 1 2; do
                        w=$(( ${#tabs[i]} + 2 ))
                        if (( EV_X >= x && EV_X < x + w )); then tab=$i; fi
                        x=$(( x + w + 3 ))
                    done
                elif [[ $tab == 0 ]] && (( EV_Y >= 7 && EV_Y <= 10 )); then
                    sel=$(( EV_Y - 7 )); break
                elif [[ $tab == 1 ]] && (( EV_Y >= 7 && EV_Y <= 8 )); then
                    opt=$(( EV_Y - 7 )); tui_change
                fi ;;
            $'\t') tab=$(( (tab + 1) % 3 )) ;;
            BACKTAB) tab=$(( (tab + 2) % 3 )) ;;
            UP|k|K)
                if [[ $tab == 0 ]]; then sel=$(( (sel + 3) % 4 )); fi
                if [[ $tab == 1 ]]; then opt=$(( (opt + 1) % 2 )); fi ;;
            DOWN|j|J)
                if [[ $tab == 0 ]]; then sel=$(( (sel + 1) % 4 )); fi
                if [[ $tab == 1 ]]; then opt=$(( (opt + 1) % 2 )); fi ;;
            LEFT|RIGHT|' '|'')
                if [[ $tab == 1 ]]; then tui_change
                elif [[ $key == '' && $tab == 0 ]]; then break
                elif [[ $key == RIGHT ]]; then tab=$(( (tab + 1) % 3 ))
                elif [[ $key == LEFT ]]; then tab=$(( (tab + 2) % 3 )); fi ;;
            [1-4]) if [[ $tab == 0 ]]; then sel=$((key - 1)); break; fi ;;
            r|R) if [[ $tab == 2 ]]; then collect_status; fi ;;
        esac
    done
    UI_ALT=0
    printf '%s\033[?25h\033[?1049l' "$MOUSE_OFF" >$TTY
    TOOLS=1 BUILD=0 GET_PACKAGES=0
    case $sel in
        1) BUILD=1 ;;
        2) BUILD=1; GET_PACKAGES=1 ;;
        3) TOOLS=0; GET_PACKAGES=1 ;;
    esac
    return 0
}

# select_menu VAR TITLE DEFAULT "label|description"... - a boxed menu drawn in
# place: Up/Down (or k/j) move, Enter chooses, a digit picks that line, Esc or
# q quits (VAR is then -1).
select_menu() {
    local __var=$1 title=$2 i=$3; shift 3
    local items=("$@") n=$# k label hint top="" row col
    ui_width
    # Output before this went through the tee pipe; let it reach the screen
    # first, or the box can be drawn above the end of it.
    sleep 0.1
    # Make room first, so a terminal that scrolls does it before the box's
    # row is taken; then ask the terminal which row that is, to match clicks
    # to lines. One that does not answer gets no mouse here, keys only.
    printf '\n' >$TTY
    for ((k = 0; k < n + 3; k++)); do printf '\n' >$TTY; done
    printf '\033[%dA\033[6n' $((n + 3)) >$TTY
    if IFS='[;' read -rsdR -t 1 _ row col <$TTY && [[ $row =~ ^[0-9]+$ ]]; then top=$row; fi
    printf '\033[?25l' >$TTY
    if [[ -n $top ]]; then printf '%s' "$MOUSE_ON" >$TTY; fi
    local drawn=0
    while :; do
        {
            if [[ -n $top ]]; then printf '\033[%d;1H' "$top"
            elif [[ $drawn == 1 ]]; then printf '\033[%dA' $((n + 3)); fi
            ui_top "$title"
            for ((k = 0; k < n; k++)); do
                label="$(pad "${items[k]%%|*}" 14)${items[k]#*|}"
                if [[ $k == "$i" ]]; then ui_row "  $G_SEL $label" "$S_SEL  $G_SEL $label$S_END"
                else ui_row "    $label"; fi
            done
            hint="$G_UD: move   Enter or click: choose   Esc: cancel"
            ui_row "$hint" "$S_DIM$hint$S_END"
            ui_bot
        } >$TTY
        drawn=1
        read_event
        case "$EV" in
            CLICK) if [[ -n $top ]] && (( EV_Y > top && EV_Y <= top + n )); then i=$(( EV_Y - top - 1 )); break; fi ;;
            UP|WHEEL_UP|k|K) i=$(( (i - 1 + n) % n )) ;;
            DOWN|WHEEL_DOWN|j|J) i=$(( (i + 1) % n )) ;;
            '') break ;;
            ESC|q|Q) i=-1; break ;;
            [1-9]) if [[ $EV -le $n ]]; then i=$((EV - 1)); break; fi ;;
        esac
    done
    printf '%s\033[?25h' "$MOUSE_OFF" >$TTY
    printf -v "$__var" '%s' "$i"
}

# --- Menu ------------------------------------------------------------------
if [[ $ANY_OPTION == 0 && $INTERACTIVE == 1 ]]; then
    CURRENT_STEP="menu"
    printf '  Checking this machine...\r' >$TTY
    collect_status
    USE_SSH=$SSH_OK
    if ! run_tui; then echo "Nothing changed."; exit 0; fi
    echo "    chose: tools=$TOOLS build=$BUILD ($BUILD_TYPE) packages=$GET_PACKAGES$(if [[ $GET_PACKAGES == 1 ]]; then echo " over $(clone_word)"; fi)"
fi

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
elif [[ $TOOLS == 1 ]]; then
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

if [[ $BUILD == 1 && ! -e "$REPO_ROOT/deki-engine/CMakeLists.txt" ]]; then
    die "deki-engine/ is empty - the submodule is missing. Run: git -C \"$REPO_ROOT\" submodule update --init --recursive"
fi

# A checkout that was moved or copied keeps a build/ configured for the old
# folder, and CMake refuses it ("CMakeCache.txt directory is different").
# build/ is generated output only, so it is started again from scratch.
BUILD_DIR="$REPO_ROOT/build"
STALE_BUILD=""
if [[ $BUILD == 1 && -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    cached_src="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt" | head -n1)"
    if [[ -n "$cached_src" && "${cached_src%/}" != "${REPO_ROOT%/}" ]]; then
        STALE_BUILD="$cached_src"
    fi
fi

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
if [[ $TOOLS == 1 ]]; then
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
else
    # Packages only: get-packages.sh needs git and curl, and packages may keep
    # assets in LFS. Installing those is the build-tools step's job.
    for t in git curl; do
        have "$t" || die "'$t' is not installed. Run this again and choose 1 (build tools) first."
    done
    git lfs version >/dev/null 2>&1 || die "git-lfs is not installed. Run this again and choose 1 (build tools) first."
fi

# --- Plan + confirmation ---------------------------------------------------
step "Plan"
if [[ $TOOLS == 1 ]]; then
    if [[ ${#MISSING[@]} -eq 0 ]]; then
        ok "all ${PM} packages already installed"
    else
        echo "    install with $PM (${#MISSING[@]}): ${MISSING[*]}"
        [[ $PM == pacman ]] && echo "    pacman -Syu: upgrades the whole system first (Arch has no partial upgrades)"
    fi
    [[ $ADD_PPA == 1 ]] && echo "    add apt source: ppa:ubuntu-toolchain-r/test (GCC 16; also updates libstdc++)"
    echo "    git lfs install for $(id -un)"
fi
[[ -n "$STALE_BUILD" ]] && echo "    delete $BUILD_DIR (configured for $STALE_BUILD - the checkout moved) and build from scratch"
[[ $BUILD == 1 ]] && echo "    build DekiEditor ($BUILD_TYPE) in $BUILD_DIR"
[[ $GET_PACKAGES == 1 ]] && echo "    download / update every Deki package, deki-demo and the registry into $(dirname "$REPO_ROOT") over $([[ $USE_SSH == 1 ]] && echo SSH || echo HTTPS)"

if [[ $YES == 0 ]]; then
    [[ $INTERACTIVE == 1 ]] || die "not running in a terminal, so cannot ask; pass --yes to proceed without asking."
    select_menu go "Go ahead with this plan?" 0 \
        "Continue|make the changes above" "Cancel|change nothing"
    [[ $go == 0 ]] || { echo "    Nothing changed."; exit 0; }
    echo "    confirmed"
fi

if [[ $TOOLS == 1 ]]; then
    # --- 1. Packages -------------------------------------------------------
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

    # --- 2. GCC with C++26 reflection --------------------------------------
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

    # --- 3. git-lfs --------------------------------------------------------
    # Package install shells out to git, and packages may keep assets in LFS.
    step "Setting up git-lfs"
    git lfs version >/dev/null 2>&1 || die "git-lfs not found"
    git lfs install --skip-repo >/dev/null
    ok "$(git lfs version)"
fi

# --- 4. Build (optional) ---------------------------------------------------
if [[ $BUILD == 1 ]]; then
    step "Building DekiEditor ($BUILD_TYPE)"
    if [[ -n "$STALE_BUILD" ]]; then
        rm -rf -- "$BUILD_DIR"
        ok "removed $BUILD_DIR (was configured for $STALE_BUILD)"
    fi
    cd "$REPO_ROOT"
    "$CMAKE" -B build -S . -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
             -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
             -DCMAKE_MAKE_PROGRAM="$NINJA" -DDEKI_GXX16="$CXX"
    "$CMAKE" --build build --target DekiEditor
    [[ -x build/bin/DekiEditor ]] || die "the build finished but build/bin/DekiEditor is missing."
    ok "$REPO_ROOT/build/bin/DekiEditor"
    ok "$(build/bin/DekiEditor -V 2>/dev/null | head -n1 || true)"
fi

# --- 5. Deki packages (optional) -------------------------------------------
if [[ $GET_PACKAGES == 1 ]]; then
    step "Downloading Deki packages (tools/get-packages.sh)"
    pkg_args=()
    [[ $USE_SSH == 1 ]] && pkg_args+=(--ssh)
    "$REPO_ROOT/tools/get-packages.sh" ${pkg_args[@]+"${pkg_args[@]}"}
fi

step "Done"
if [[ $TOOLS == 1 && $BUILD == 0 ]]; then
    cat <<EOF
    Build the editor (from $REPO_ROOT):
      "$CMAKE" -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=$CC -DCMAKE_CXX_COMPILER=$CXX -DCMAKE_MAKE_PROGRAM=$NINJA -DDEKI_GXX16=$CXX
      "$CMAKE" --build build --target DekiEditor
    or run this again and choose 2.
EOF
fi
[[ $BUILD == 1 ]] && echo "    Run the editor:  $REPO_ROOT/build/bin/DekiEditor"
[[ $GET_PACKAGES == 1 ]] && echo "    Open the workspace:  code \"$(dirname "$REPO_ROOT")\""
echo "    log: $LOG"
