#!/usr/bin/env python3
"""Publish built documentation to GitHub Pages.

    publish_docs.py --html build/docs/html [--packages-dir build/docs/packages --registry <index.json>]

Main site: --html is pushed to the dekiengine/docs repository (built output
only, served from its master branch root, like the registry).

Package sites: with --packages-dir, each <id>/html tree is pushed to that
package repository's `gh-pages` branch (the repository is looked up in the
registry index; default dekiengine/<id>) and Pages is enabled on that branch
when it is not yet. Served at https://dekiengine.github.io/<repo>/, which the
registry's `docs` field names and the main site links to.

Needs git and the GitHub CLI (gh, authenticated for the dekiengine org).
"""

import argparse
import json
import os
import shutil
import stat
import subprocess
import sys
import time
import urllib.request


def sh(cmd, cwd=None, check=True, capture=False):
    r = subprocess.run(cmd, cwd=cwd, text=True, capture_output=capture)
    if check and r.returncode != 0:
        if capture:
            sys.stderr.write((r.stdout or "") + (r.stderr or ""))
        raise SystemExit(f"command failed ({r.returncode}): {' '.join(cmd)}")
    return (r.stdout or "").strip() if capture else ""


def rmtree(path):
    """shutil.rmtree that also removes read-only files (git objects on Windows)."""
    def onexc(func, p, _exc):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    shutil.rmtree(path, onexc=onexc)


def replace_tree(site, html, keep):
    """Replace everything in `site` except `keep` with the contents of `html`."""
    for entry in os.listdir(site):
        if entry in keep:
            continue
        path = os.path.join(site, entry)
        rmtree(path) if os.path.isdir(path) else os.remove(path)
    for entry in os.listdir(html):
        src = os.path.join(html, entry)
        dst = os.path.join(site, entry)
        shutil.copytree(src, dst) if os.path.isdir(src) else shutil.copy2(src, dst)
    open(os.path.join(site, ".nojekyll"), "w").close()


def commit_and_push(site, branch, message):
    sh(["git", "add", "-A"], cwd=site)
    if not sh(["git", "status", "--porcelain"], cwd=site, capture=True):
        return False
    sh(["git", "commit", "-q", "-m", message], cwd=site)
    sh(["git", "push", "-q", "-u", "origin", branch], cwd=site)
    return True


def ensure_pages(repo, branch):
    """Enable GitHub Pages on <branch> root, or point it there if it serves another source.

    GitHub may enable Pages by itself the moment a gh-pages branch appears, and
    the site record can lag the push by a few seconds, so both the lookup and
    the update are retried rather than trusted once.
    """
    last = ""
    for attempt in range(6):
        out = sh(["gh", "api", f"repos/{repo}/pages"], capture=True, check=False)
        if out and out.startswith("{"):
            current = json.loads(out).get("source", {})
            if current.get("branch") == branch and current.get("path", "/") == "/":
                return "already enabled" if attempt == 0 else "enabled"
            r = subprocess.run(["gh", "api", "-X", "PUT", f"repos/{repo}/pages", "-f", f"source[branch]={branch}",
                                "-f", "source[path]=/"], text=True, capture_output=True)
            if r.returncode == 0:
                return "source updated"
            last = (r.stderr or r.stdout or "").strip()
        else:
            r = subprocess.run(["gh", "api", "-X", "POST", f"repos/{repo}/pages", "-f", f"source[branch]={branch}",
                                "-f", "source[path]=/"], text=True, capture_output=True)
            if r.returncode == 0 or "409" in (r.stderr or ""):
                return "enabled"
            last = (r.stderr or r.stdout or "").strip()
        time.sleep(5)
    return f"NOT enabled ({last[:120]})"


def wait_for_pages(repo, url, timeout=300):
    deadline = time.time() + timeout
    status = ""
    while time.time() < deadline:
        out = sh(["gh", "api", f"repos/{repo}/pages/builds/latest"], capture=True, check=False)
        try:
            status = json.loads(out).get("status", "") if out else ""
        except ValueError:
            status = ""
        if status == "built":
            break
        time.sleep(10)
    try:
        with urllib.request.urlopen(url, timeout=30) as r:
            return f"{status or 'unknown'}, HTTP {r.status}"
    except Exception as e:  # noqa: BLE001
        return f"{status or 'unknown'}, not reachable yet ({e})"


def publish_main(html, repo, branch, site, sha):
    url = f"https://github.com/{repo}.git"
    if os.path.isdir(os.path.join(site, ".git")):
        sh(["git", "fetch", "-q", "origin", branch], cwd=site)
        sh(["git", "checkout", "-q", "-B", branch, f"origin/{branch}"], cwd=site)
    else:
        if os.path.isdir(site):
            rmtree(site)
        sh(["git", "clone", "-q", "--branch", branch, url, site])
    replace_tree(site, html, keep={".git", "README.md", "LICENSE"})
    if commit_and_push(site, branch, f"Docs from deki-editor {sha}".strip()):
        print(f"main site: pushed to {repo}@{branch}")
    else:
        print("main site: unchanged")


def publish_package(pkg_id, html, repo, site, sha):
    """Push one package's site to its repository's gh-pages branch."""
    branch = "gh-pages"
    url = f"https://github.com/{repo}.git"
    if os.path.isdir(site):
        rmtree(site)
    # Clone only what is needed: the gh-pages branch when it exists, otherwise
    # start it as an orphan so the docs never mix with the source history.
    has_branch = bool(sh(["git", "ls-remote", "--heads", url, branch], capture=True))
    if has_branch:
        sh(["git", "clone", "-q", "--depth", "1", "--branch", branch, url, site])
    else:
        sh(["git", "clone", "-q", "--depth", "1", url, site])
        sh(["git", "checkout", "-q", "--orphan", branch], cwd=site)
        sh(["git", "rm", "-rfq", "."], cwd=site, check=False)
    replace_tree(site, html, keep={".git"})
    changed = commit_and_push(site, branch, f"Docs for {pkg_id} from deki-editor {sha}".strip())
    state = ensure_pages(repo, branch)
    print(f"{pkg_id}: {'pushed' if changed else 'unchanged'} ({repo}@{branch}, pages {state})")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--html", required=True, help="built main site (build/docs/html)")
    ap.add_argument("--repo", default="dekiengine/docs", help="GitHub repo serving the main site")
    ap.add_argument("--branch", default="master")
    ap.add_argument("--site-dir", default=None, help="local clone of the main site repo (default: <html>/../site)")
    ap.add_argument("--packages-dir", help="per-package sites: <dir>/<id>/html (build/docs/packages)")
    ap.add_argument("--registry", help="registry index.json, for each package's repository")
    ap.add_argument("--root", default=None, help="deki-editor checkout, for the commit message's sha")
    ap.add_argument("--no-wait", action="store_true", help="do not wait for the main site's Pages build")
    a = ap.parse_args(argv)

    html = os.path.abspath(a.html)
    if not os.path.isfile(os.path.join(html, "index.html")):
        raise SystemExit(f"no index.html under {html}")
    sha = sh(["git", "rev-parse", "--short", "HEAD"], cwd=a.root, capture=True) if a.root else ""
    site = os.path.abspath(a.site_dir or os.path.join(html, "..", "site"))

    publish_main(html, a.repo, a.branch, site, sha)

    if a.packages_dir:
        repos = {}
        if a.registry and os.path.isfile(a.registry):
            with open(a.registry, encoding="utf-8") as f:
                for entry in json.load(f).get("packages", []):
                    if entry.get("id") and entry.get("repo"):
                        repos[entry["id"]] = entry["repo"]
        sites = os.path.abspath(os.path.join(a.packages_dir, "..", "site-packages"))
        os.makedirs(sites, exist_ok=True)
        for pkg_id in sorted(os.listdir(a.packages_dir)):
            phtml = os.path.join(a.packages_dir, pkg_id, "html")
            if not os.path.isfile(os.path.join(phtml, "index.html")):
                print(f"{pkg_id}: no index.html, skipped")
                continue
            publish_package(pkg_id, phtml, repos.get(pkg_id, f"dekiengine/{pkg_id}"), os.path.join(sites, pkg_id), sha)

    if not a.no_wait:
        owner, name = a.repo.split("/", 1)
        print(f"main site: {wait_for_pages(a.repo, f'https://{owner}.github.io/{name}/')}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
