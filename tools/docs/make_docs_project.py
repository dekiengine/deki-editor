#!/usr/bin/env python3
"""Create the throwaway project the docs build opens: one that declares every
package in the registry, so a single `--dump-metadata` sees them all.

    make_docs_project.py --editor <DekiEditor.exe> --out <dir> [--registry <url|file>] [--only a,b,c]

The project is created with `DekiEditor --new-project` (the same scaffold a
user gets) and its deki-packages.json is then rewritten to declare every
registry package at "*". Opening it restores and builds all of them; a
package that does not build on desktop fails that open and is reported by the
editor, not silently skipped.

This is also the "does every package still compile" check, run by hand:

    DekiEditor --build-code <dir>/<name>
"""

import argparse
import json
import os
import shutil
import stat
import subprocess
import sys
import urllib.request

DEFAULT_REGISTRY = "https://dekiengine.github.io/registry/index.json"


def load_registry(source):
    if source.startswith("http://") or source.startswith("https://"):
        with urllib.request.urlopen(source, timeout=30) as r:
            return json.load(r)
    with open(source, encoding="utf-8") as f:
        return json.load(f)


def registry_ids(index):
    ids = sorted({p["id"] for p in index.get("packages", []) if p.get("id")})
    if not ids:
        raise SystemExit("no package ids in the registry index")
    return ids


def rmtree_force(path):
    """Delete a tree containing read-only files.

    The package clones under the docs project carry read-only .git pack files,
    which Windows refuses to unlink until the bit is cleared, and clearing it on
    the file alone is not always enough: the containing directory is checked
    too. Without this a second docs build fails on the leftovers of the first,
    so the docs could only ever be built once per checkout.

    shutil's callback was renamed from onerror to onexc in 3.12 and the old name
    is gone in 3.14, which this runs under.
    """
    def clear_readonly_and_retry(func, target, _exc):
        for path_to_fix in (target, os.path.dirname(target)):
            try:
                os.chmod(path_to_fix, stat.S_IWRITE)
            except OSError:
                pass
        func(target)

    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=clear_readonly_and_retry)
    else:
        shutil.rmtree(path, onerror=clear_readonly_and_retry)


def create_project(editor, parent, name):
    project = os.path.join(parent, name)
    if os.path.isdir(project):
        rmtree_force(project)
    os.makedirs(parent, exist_ok=True)
    rc = subprocess.run([editor, "--new-project", name, parent], capture_output=True, text=True)
    if rc.returncode != 0 or not os.path.isfile(os.path.join(project, "deki.json")):
        sys.stderr.write(rc.stdout[-2000:] + rc.stderr[-2000:])
        raise SystemExit(f"--new-project failed (exit {rc.returncode})")
    return project


def declare_packages(project, ids):
    manifest = {"schemaVersion": 1, "packages": {pid: {"version": "*"} for pid in ids}}
    with open(os.path.join(project, "deki-packages.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--editor", required=True, help="path to DekiEditor executable")
    ap.add_argument("--out", required=True, help="parent directory; the project is <out>/<name>")
    ap.add_argument("--name", default="deki-docs")
    ap.add_argument("--registry", default=os.environ.get("DEKI_REGISTRY_INDEX", DEFAULT_REGISTRY),
                    help="registry index.json URL or file (default: the public registry)")
    ap.add_argument("--only", help="comma-separated package ids instead of the whole registry")
    a = ap.parse_args(argv)

    if a.only:
        ids = [s.strip() for s in a.only.split(",") if s.strip()]
    else:
        ids = registry_ids(load_registry(a.registry))
    project = create_project(a.editor, a.out, a.name)
    declare_packages(project, ids)
    print(f"{project}")
    print(f"  {len(ids)} package(s) declared: " + ", ".join(ids))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
