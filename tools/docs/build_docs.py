#!/usr/bin/env python3
"""Build the Deki documentation: the main site (engine core, project settings,
the Doxygen API reference of the engine and the editor DLL, and one link per
package) plus one small site per package (its components, properties and
settings sections), all generated from the live reflection metadata.

    build_docs.py [--editor <exe>] [--build-dir build] [--reuse-project] [--reuse-metadata] [--publish]

Steps:
  1. docs project   build/docs/project/deki-docs declares every registry package
  2. metadata       DekiEditor --dump-metadata build/docs/metadata.json <project>
                    (opens the project: restores and builds every package; the
                    first run takes minutes, later ones seconds)
  3. markdown       tools/docs/gen_component_docs.py -> build/docs/md/ (main site)
                    and build/docs/md-packages/<id>/ (package sites)
  4. doxygen        tools/docs/Doxyfile -> build/docs/html/ (main site);
                    tools/docs/Doxyfile.package -> build/docs/packages/<id>/html/
                    (Doxygen is found on PATH or in build/docs/doxygen/, else
                    downloaded there on Windows)
  5. publish        --publish hands both to tools/docs/publish_docs.py: the main
                    site to dekiengine/docs, each package site to its repository's
                    gh-pages branch

Run from the deki-editor checkout, or pass --root.
"""

import argparse
import hashlib
import io
import json
import os
import platform
import shutil
import stat
import subprocess
import sys
import urllib.request
import zipfile

def _rmtree(path):
    """shutil.rmtree that also removes read-only files.

    The package clones under the docs project carry read-only .git pack files,
    which Windows refuses to unlink until the bit is cleared. Clearing it on the
    file alone is not always enough: the containing directory can be read-only
    too, and Windows checks that before letting an entry be removed.

    `onerror` was deprecated in Python 3.12 in favour of `onexc` and is removed
    in 3.14, so the right one is chosen for the interpreter in hand. On 3.12
    passing the deprecated name also raised a DeprecationWarning that turned the
    handler into a no-op under -W error, which is how a docs rebuild came to
    fail on files this function exists to delete.
    """
    def _clear_readonly_and_retry(func, p, _exc):
        for target in (p, os.path.dirname(p)):
            try:
                os.chmod(target, stat.S_IWRITE)
            except OSError:
                pass
        func(p)

    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=_clear_readonly_and_retry)
    else:
        shutil.rmtree(path, onerror=_clear_readonly_and_retry)


DOXYGEN_VERSION = "1.18.0"
DOXYGEN_WINDOWS_ZIP = (
    f"https://github.com/doxygen/doxygen/releases/download/Release_{DOXYGEN_VERSION.replace('.', '_')}/"
    f"doxygen-{DOXYGEN_VERSION}.windows.x64.bin.zip")
DOXYGEN_WINDOWS_SHA256 = "e84f54cfd49ef06b0b16536056dbec0c496323de28abcce53a4269463de35eaf"
MAIN_URL = "https://dekiengine.github.io/docs/"


def log(msg):
    print(f"[docs] {msg}", flush=True)


def run(cmd, cwd=None, env=None):
    log(" ".join(str(c) for c in cmd))
    rc = subprocess.run(cmd, cwd=cwd, env=env)
    if rc.returncode != 0:
        raise SystemExit(f"command failed with exit {rc.returncode}: {cmd[0]}")


def find_editor(build_dir, explicit):
    if explicit:
        return os.path.abspath(explicit)
    exe = "DekiEditor.exe" if platform.system() == "Windows" else "DekiEditor"
    for cand in (os.path.join(build_dir, "bin", exe), os.path.join(build_dir, "bin", "Debug", exe)):
        if os.path.isfile(cand):
            return os.path.abspath(cand)
    raise SystemExit(f"DekiEditor not found under {build_dir}/bin; build the editor first or pass --editor")


def find_doxygen(docs_dir):
    on_path = shutil.which("doxygen")
    if on_path:
        return on_path
    local = os.path.join(docs_dir, "doxygen", "doxygen.exe" if platform.system() == "Windows" else "doxygen")
    if os.path.isfile(local):
        return local
    if platform.system() != "Windows":
        raise SystemExit("doxygen is not on PATH; install it (apt install doxygen / brew install doxygen)")
    log(f"downloading Doxygen {DOXYGEN_VERSION} to {os.path.dirname(local)}")
    with urllib.request.urlopen(DOXYGEN_WINDOWS_ZIP, timeout=120) as r:
        data = r.read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != DOXYGEN_WINDOWS_SHA256:
        raise SystemExit(f"Doxygen download hash mismatch: {digest}")
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        z.extractall(os.path.dirname(local))
    return local


def engine_version(root):
    path = os.path.join(root, "deki-engine", "CMakeLists.txt")
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                if line.strip().startswith("project(") and "VERSION" in line:
                    return line.split("VERSION", 1)[1].split(")")[0].strip().split()[0]
    except OSError:
        pass
    return ""


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=None, help="deki-editor checkout (default: this script's grandparent)")
    ap.add_argument("--build-dir", default="build", help="the editor's CMake build directory")
    ap.add_argument("--editor", help="DekiEditor executable (default: <build-dir>/bin)")
    ap.add_argument("--packages", help="directory of package checkouts for summaries (default: ../packages if present)")
    ap.add_argument("--registry", help="registry index.json URL or file (default: ../deki-registry/index.json, else the public one)")
    ap.add_argument("--reuse-project", action="store_true", help="keep an existing docs project")
    ap.add_argument("--reuse-metadata", action="store_true", help="skip the editor run; use the existing metadata.json")
    ap.add_argument("--no-packages", action="store_true", help="build only the main site")
    ap.add_argument("--allow-no-packages", action="store_true",
                    help="proceed even when the metadata contains no packages")
    ap.add_argument("--publish", action="store_true", help="push the main site and the package sites")
    a = ap.parse_args(argv)

    root = os.path.abspath(a.root or os.path.join(os.path.dirname(__file__), "..", ".."))
    build_dir = os.path.join(root, a.build_dir) if not os.path.isabs(a.build_dir) else a.build_dir
    docs_dir = os.path.join(build_dir, "docs")
    project_parent = os.path.join(docs_dir, "project")
    project = os.path.join(project_parent, "deki-docs")
    metadata = os.path.join(docs_dir, "metadata.json")
    md_dir = os.path.join(docs_dir, "md")
    md_pkg_dir = os.path.join(docs_dir, "md-packages")
    html_dir = os.path.join(docs_dir, "html")
    pkg_out = os.path.join(docs_dir, "packages")
    tools = os.path.join(root, "tools", "docs")
    os.makedirs(docs_dir, exist_ok=True)

    if not a.reuse_metadata:
        editor = find_editor(build_dir, a.editor)
        if not (a.reuse_project and os.path.isfile(os.path.join(project, "deki.json"))):
            cmd = [sys.executable, os.path.join(tools, "make_docs_project.py"), "--editor", editor,
                   "--out", project_parent, "--name", "deki-docs"]
            if a.registry:
                cmd += ["--registry", a.registry]
            run(cmd)
        log("dumping reflection metadata (opens the docs project: restore + build of every package)")
        run([editor, "--dump-metadata", metadata, project])
    elif not os.path.isfile(metadata):
        raise SystemExit(f"--reuse-metadata given but {metadata} does not exist")

    # A metadata dump with no packages in it still exits 0, and every step
    # after this one happily produces a site: the main page's package table
    # renders as literal "| --- |" markdown and not a single package site is
    # written. That shipped once, unnoticed, because the build looked fine.
    # The usual cause is a stale docs project whose package clones predate the
    # current engine, so the editor refuses them and CMake never configures
    # them -- rerun without --reuse-project to re-clone.
    with open(metadata, encoding="utf-8") as f:
        found = len(json.load(f).get("packages") or [])
    if found == 0 and not a.allow_no_packages:
        raise SystemExit(
            f"{metadata} lists no packages, so the docs would publish with an empty "
            "catalog and no package sites. Check %APPDATA%/DekiEditor/Editor.log for "
            "'Package compatibility' errors, then rebuild the docs project by running "
            "this without --reuse-project. Pass --allow-no-packages if an empty catalog "
            "is genuinely what you want.")
    log(f"metadata lists {found} package(s)")

    packages = a.packages
    if packages is None:
        # "packages" is the name everywhere in the tooling. The checkout beside
        # this repo was called "modules" until 2026-09-19, so a checkout that
        # was never renamed still works. Without a hit the package pages silently lose everything
        # that comes from package.json and CHANGELOG.md -- minEngine, the
        # feature list and the changelog section -- with no warning, which is
        # how they shipped without one.
        for name in ("packages", "modules"):
            cand = os.path.join(root, "..", name)
            if os.path.isdir(cand):
                packages = cand
                break
        if packages is None:
            print("[docs] no package checkout beside this repo: package pages "
                  "will have no changelog, minEngine or feature list")
    registry_file = None
    if a.registry and os.path.isfile(a.registry):
        registry_file = a.registry
    else:
        cand = os.path.join(root, "..", "deki-registry", "index.json")
        registry_file = cand if os.path.isfile(cand) else None

    for d in (md_dir, md_pkg_dir, html_dir, pkg_out):
        if os.path.isdir(d):
            _rmtree(d)
    cmd = [sys.executable, os.path.join(tools, "gen_component_docs.py"), metadata, md_dir, "--main-url", MAIN_URL]
    if not a.no_packages:
        cmd += ["--packages-out", md_pkg_dir]
    if packages:
        cmd += ["--packages", packages]
    if registry_file:
        cmd += ["--registry", registry_file]
    run(cmd)

    doxygen = find_doxygen(docs_dir)
    version = engine_version(root)
    env = dict(os.environ)
    env["DEKI_DOCS_VERSION"] = version
    env["DEKI_DOCS_OUT"] = docs_dir
    env["DEKI_DOCS_MD"] = md_dir
    run([doxygen, os.path.join(tools, "Doxyfile")], cwd=root, env=env)
    shutil.copyfile(os.path.join(tools, "nojekyll"), os.path.join(html_dir, ".nojekyll"))
    index = os.path.join(html_dir, "index.html")
    if not os.path.isfile(index):
        raise SystemExit("doxygen produced no index.html")
    log(f"main site: {index}")

    if not a.no_packages and os.path.isdir(md_pkg_dir):
        for pkg_id in sorted(os.listdir(md_pkg_dir)):
            penv = dict(env)
            penv["DEKI_PKG_NAME"] = pkg_id
            penv["DEKI_PKG_MD"] = os.path.join(md_pkg_dir, pkg_id)
            penv["DEKI_PKG_OUT"] = os.path.join(pkg_out, pkg_id)
            os.makedirs(penv["DEKI_PKG_OUT"], exist_ok=True)  # doxygen does not create nested output dirs
            log(f"package site: {pkg_id}")
            rc = subprocess.run([doxygen, os.path.join(tools, "Doxyfile.package")], cwd=root, env=penv)
            if rc.returncode != 0:
                raise SystemExit(f"doxygen failed for package {pkg_id}")
            shutil.copyfile(os.path.join(tools, "nojekyll"), os.path.join(pkg_out, pkg_id, "html", ".nojekyll"))
        log(f"package sites: {pkg_out}/<id>/html")

    if a.publish:
        cmd = [sys.executable, os.path.join(tools, "publish_docs.py"), "--html", html_dir, "--root", root]
        if not a.no_packages:
            cmd += ["--packages-dir", pkg_out]
            if registry_file:
                cmd += ["--registry", registry_file]
        run(cmd)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
