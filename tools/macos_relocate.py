#!/usr/bin/env python3
"""Make a macOS distribution folder relocatable.

Homebrew's GCC and the editor's own libraries name their dependencies by
absolute path (/opt/homebrew/..., the build tree), which exist only on the
machine that built them. This walks a folder, copies in whatever Homebrew
library something still needs, and rewrites every Mach-O file to find its
dependencies relative to itself. Each file is then signed again, ad hoc: on
Apple Silicon a binary whose signature no longer matches is killed at launch.

It fails when anything still points outside the folder, so an archive that
only works on the machine that packaged it is never produced.

    python3 tools/macos_relocate.py <dist-folder> <folder-for-copied-libraries>
"""
import os
import shutil
import struct
import subprocess
import sys

MH_MAGICS = {0xFEEDFACF, 0xCFFAEDFE}
FAT_MAGICS = {0xCAFEBABE, 0xBEBAFECA}
LOADABLE = {2, 6, 8}  # MH_EXECUTE, MH_DYLIB, MH_BUNDLE; never object files
FOREIGN = ("/opt/homebrew/", "/usr/local/", "/Users/", "/private/", "/tmp/", "/opt/")


def run(*cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def is_loadable(path):
    try:
        with open(path, "rb") as f:
            head = f.read(16)
    except OSError:
        return False
    if len(head) < 16:
        return False
    magic = struct.unpack(">I", head[:4])[0]
    if magic in FAT_MAGICS:
        return True
    return magic in MH_MAGICS and struct.unpack("<I", head[12:16])[0] in LOADABLE


def deps_of(path):
    lines = run("otool", "-L", path).splitlines()[1:]
    names = [line.strip().rsplit(" (compatibility", 1)[0] for line in lines]
    install_id = id_of(path)
    return [n for n in names if n != install_id]


def id_of(path):
    lines = run("otool", "-D", path).splitlines()
    return lines[1].strip() if len(lines) > 1 else None


def rpaths_of(path):
    out = run("otool", "-l", path).splitlines()
    found = []
    for i, line in enumerate(out):
        if line.strip() == "cmd LC_RPATH":
            found.append(out[i + 2].strip().split(" ", 1)[1].rsplit(" (offset", 1)[0])
    return found


def scan(root):
    """Real Mach-O files, and every name (links too) a library can be found by."""
    files, index = [], {}
    for folder, _, names in os.walk(root):
        for name in names:
            path = os.path.join(folder, name)
            if not is_loadable(path):
                continue
            real = os.path.realpath(path)
            index.setdefault(name, real)
            if not os.path.islink(path):
                files.append(path)
    return files, index


def main():
    root, copies = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    os.makedirs(copies, exist_ok=True)

    # Bring in what is missing; each copy can need more.
    while True:
        files, index = scan(root)
        missing = {}
        for path in files:
            for dep in deps_of(path):
                base = os.path.basename(dep)
                if dep.startswith(FOREIGN) and base not in index:
                    missing[base] = dep
        if not missing:
            break
        for base, dep in sorted(missing.items()):
            if not os.path.exists(dep):
                sys.exit(f"error: {dep}, needed by the distribution, does not exist")
            target = os.path.join(copies, base)
            shutil.copy2(os.path.realpath(dep), target)
            os.chmod(target, 0o755)
            print(f"    + {base}")

    for path in files:
        os.chmod(path, 0o755)
        args, folders = [], []
        for dep in deps_of(path):
            base = os.path.basename(dep)
            if base not in index or not (dep.startswith(FOREIGN) or dep.startswith("@")):
                continue
            new = "@rpath/" + base
            if dep != new:
                args += ["-change", dep, new]
            folder = os.path.relpath(os.path.dirname(index[base]), os.path.dirname(os.path.realpath(path)))
            folders.append("@loader_path" if folder == "." else "@loader_path/" + folder)

        install_id = id_of(path)
        if install_id:
            wanted = "@rpath/" + os.path.basename(install_id)
            if install_id != wanted:
                args += ["-id", wanted]

        current = rpaths_of(path)
        for rpath in current:
            if rpath not in folders:
                args += ["-delete_rpath", rpath]
        for folder in sorted(set(folders)):
            if folder not in current:
                args += ["-add_rpath", folder]

        if args:
            run("install_name_tool", *args, path)
        run("codesign", "--force", "--sign", "-", path)

    # Nothing may still point outside.
    files, _ = scan(root)
    bad = []
    for path in files:
        bad += [f"{path}: {d}" for d in deps_of(path) if d.startswith(FOREIGN)]
        bad += [f"{path}: rpath {r}" for r in rpaths_of(path) if r.startswith(FOREIGN)]
    if bad:
        sys.exit("error: still points outside the distribution:\n  " + "\n  ".join(bad))
    print(f"    {len(files)} files relocated and signed")


if __name__ == "__main__":
    main()
