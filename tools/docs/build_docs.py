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

Steps 3 and 4 run once per language (--languages, default all): English at
the root of every site, Japanese, Chinese and Korean in html/<code>/ beside
it, each with the editor guides (docs/ and docs/<code>/) as pages. The API
reference is built in English only. See localize.py.
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

import localize

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
    # The overrides carry Japanese, Chinese and Korean text, which a Windows
    # console or a redirect in its code page cannot print.
    encoding = sys.stdout.encoding or "utf-8"
    print(f"[docs] {msg}".encode(encoding, "backslashreplace").decode(encoding), flush=True)


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


def doxygen_run(doxygen, doxyfile, overrides, cwd, env):
    """Doxygen with a few settings replaced: the Doxyfile is read, the
    overrides are appended (a later value wins) and the result goes in on stdin."""
    with open(doxyfile, encoding="utf-8") as f:
        config = f.read()
    # Forward slashes: a backslash inside a quoted Doxygen value is an escape.
    config += "\n" + "".join(f'{key} = "{str(value).replace(chr(92), "/")}"\n' for key, value in overrides.items())
    log(f"{doxygen} {os.path.basename(doxyfile)} " + " ".join(f"{k}={v}" for k, v in overrides.items()))
    rc = subprocess.run([doxygen, "-"], cwd=cwd, env=env, input=config.encode("utf-8"))
    if rc.returncode != 0:
        raise SystemExit(f"doxygen failed: {doxyfile}")


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
    ap.add_argument("--languages", help="comma-separated language codes to build (default: en,ja,zh,ko)")
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

    site_dir = os.path.join(root, "..", "hugo-dekiengine")
    languages = a.languages.split(",") if a.languages else list(localize.LABELS)
    doxygen = find_doxygen(docs_dir)
    version = engine_version(root)
    # English owns the root of each site, so building it starts every site
    # afresh; building only other languages replaces just their folders.
    stale = [os.path.join(docs_dir, "theme")]
    if "en" in languages:
        stale += [md_dir, md_pkg_dir, html_dir, pkg_out]
    else:
        stale += [os.path.join(html_dir, lang) for lang in languages]
        if os.path.isdir(pkg_out):
            stale += [os.path.join(pkg_out, p, "html", lang) for p in os.listdir(pkg_out) for lang in languages]
    for d in stale:
        if os.path.isdir(d):
            _rmtree(d)
    with open(os.path.join(tools, "theme", "header.html"), encoding="utf-8") as f:
        header_template = f.read()

    # English at the root of each site, every other language in its own
    # /<code>/ folder of the same site, so one push publishes them all.
    for lang in languages:
        suffix = "" if lang == "en" else f"-{lang}"
        lang_md = md_dir + suffix
        lang_md_pkg = md_pkg_dir + suffix
        html_output = "html" if lang == "en" else f"html/{lang}"
        if lang != "en":
            for d in (lang_md, lang_md_pkg):
                if os.path.isdir(d):
                    _rmtree(d)
        cmd = [sys.executable, os.path.join(tools, "gen_component_docs.py"), metadata, lang_md, "--main-url", MAIN_URL,
               "--language", lang, "--guides", os.path.join(root, "docs"), "--editor-root", root]
        if not a.no_packages:
            cmd += ["--packages-out", lang_md_pkg]
        if packages:
            cmd += ["--packages", packages]
        if registry_file:
            cmd += ["--registry", registry_file]
        if os.path.isdir(site_dir):
            cmd += ["--site", site_dir]
        run(cmd)

        header = os.path.join(docs_dir, "theme", lang, "header.html")
        os.makedirs(os.path.dirname(header), exist_ok=True)
        with open(header, "w", encoding="utf-8", newline="\n") as f:
            f.write(localize.header(header_template, lang))
        overrides = {"HTML_HEADER": header, "HTML_OUTPUT": html_output,
                     "OUTPUT_LANGUAGE": localize.DOXYGEN_LANGUAGES[lang]}

        env = dict(os.environ)
        env["DEKI_DOCS_VERSION"] = version
        env["DEKI_DOCS_OUT"] = docs_dir
        env["DEKI_DOCS_MD"] = lang_md
        main_overrides = dict(overrides)
        if lang != "en":
            # The API reference is English only; each language links to it.
            main_overrides["INPUT"] = lang_md
            main_overrides["PROJECT_BRIEF"] = localize.bar_word(lang, "brief", "")
        os.makedirs(os.path.join(docs_dir, html_output), exist_ok=True)  # doxygen does not create nested output dirs
        doxygen_run(doxygen, os.path.join(tools, "Doxyfile"), main_overrides, root, env)
        out = os.path.join(docs_dir, html_output)
        shutil.copyfile(os.path.join(tools, "nojekyll"), os.path.join(out, ".nojekyll"))
        index = os.path.join(out, "index.html")
        if not os.path.isfile(index):
            raise SystemExit(f"doxygen produced no index.html ({lang})")
        log(f"main site ({lang}): {index}")

        if not a.no_packages and os.path.isdir(lang_md_pkg):
            for pkg_id in sorted(os.listdir(lang_md_pkg)):
                penv = dict(env)
                penv["DEKI_PKG_NAME"] = pkg_id
                penv["DEKI_PKG_MD"] = os.path.join(lang_md_pkg, pkg_id)
                penv["DEKI_PKG_OUT"] = os.path.join(pkg_out, pkg_id)
                pkg_overrides = dict(overrides)
                if lang != "en":
                    pkg_overrides["PROJECT_BRIEF"] = localize.bar_word(lang, "package brief", "")
                os.makedirs(os.path.join(pkg_out, pkg_id, html_output), exist_ok=True)
                log(f"package site ({lang}): {pkg_id}")
                doxygen_run(doxygen, os.path.join(tools, "Doxyfile.package"), pkg_overrides, root, penv)
                shutil.copyfile(os.path.join(tools, "nojekyll"),
                                os.path.join(pkg_out, pkg_id, html_output, ".nojekyll"))
            log(f"package sites ({lang}): {pkg_out}/<id>/{html_output}")

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
