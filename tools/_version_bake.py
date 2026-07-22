"""Shared version determination + `_version.py` emission.

Used by both frozen-build lanes:
  * the frozen-app stager  (tools/stage_frozen_app.py) -> frozen_app/seedsigner/_version.py
  * the /overlay dev deploy (tools/deploy_app.py)       -> /overlay/seedsigner/_version.py

One source of truth so a frozen build and a live overlay report the version the
same way.

Version source precedence (per the structured-frozen-build design):

  1. env-var override -- $SEEDSIGNER_VERSION (+ optional $SEEDSIGNER_VERSION_FORK /
     $SEEDSIGNER_SHORT_COMMIT_HASH / $SEEDSIGNER_VERSION_TIMESTAMP). Used by release
     and CI builds, where the version is decided up front and the build host must
     NOT import the app (CI computes it from the pinned deps/seedsigner git and
     exports it).
  2. git fallback -- run the app's OWN version logic
     (seedsigner.helpers.version.Version) in an isolated subprocess against the app
     source tree. This is version.py's git-state explorer; it auto-takes the
     GitHub-Actions env path when $CI is set. It runs in a subprocess because
     version.py imports `traceback` / `datetime` at module top (fine on CPython,
     fatal on MicroPython) and shells out to git in the app repo's cwd -- neither
     of which should touch our process.

The result is baked into a frozen-importable `<pkg>/_version.py` -- the on-device
analogue of the app's `seedsigner/version.json`. Frozen firmware content is
import-only (it can't `open()` a json file), so the version data must ship as a
`.py` module the import machinery can reach. The app reads it mode-agnostically
via `from seedsigner import _version` under IS_MICROPYTHON.
"""
import json
import os
import subprocess
import sys

# Env-var override names (release / CI path). Only SEEDSIGNER_VERSION is required
# to trigger the override; the rest default to None when unset.
ENV_VERSION = "SEEDSIGNER_VERSION"
ENV_FORK = "SEEDSIGNER_VERSION_FORK"
ENV_HASH = "SEEDSIGNER_SHORT_COMMIT_HASH"
ENV_TIMESTAMP = "SEEDSIGNER_VERSION_TIMESTAMP"

# The four keys the app's Version.to_dict() emits (the version.json schema).
KEY_NAME = "name"
KEY_FORK = "fork"
KEY_HASH = "short_commit_hash"
KEY_TIMESTAMP = "timestamp"
_KEYS = (KEY_NAME, KEY_FORK, KEY_HASH, KEY_TIMESTAMP)


def _from_env():
    """Return the override dict if $SEEDSIGNER_VERSION is set, else None."""
    name = os.environ.get(ENV_VERSION)
    if not name:
        return None
    return {
        KEY_NAME: name,
        KEY_FORK: os.environ.get(ENV_FORK),
        KEY_HASH: os.environ.get(ENV_HASH),
        KEY_TIMESTAMP: os.environ.get(ENV_TIMESTAMP),
    }


def _from_app_git(app_dir, src_root):
    """Run the app's version.py logic in a subprocess and return its to_dict().

    `app_dir` is the app repo root (the git cwd); `src_root` is the sys.path root
    that makes `import seedsigner...` resolve (e.g. <app>/src). We replicate
    write_versionfile.py's 3-line to_dict() call (that script writes into the app
    repo, which is off-limits) and capture stdout instead.
    """
    if not os.path.isdir(app_dir):
        raise RuntimeError("app dir not found: %s" % app_dir)
    code = (
        "import json;"
        "from seedsigner.helpers.version import Version;"
        "print(json.dumps(Version.get_instance().to_dict()))"
    )
    env = dict(os.environ)
    env["PYTHONPATH"] = src_root + os.pathsep + env.get("PYTHONPATH", "")
    try:
        proc = subprocess.run(
            [sys.executable, "-c", code],
            cwd=app_dir, env=env,
            capture_output=True, text=True, check=True,
        )
    except subprocess.CalledProcessError as e:
        raise RuntimeError(
            "the app's version.py failed to resolve a version (cwd=%s).\n"
            "Set $%s to supply it explicitly, or check the app source.\n"
            "--- version.py stderr ---\n%s"
            % (app_dir, ENV_VERSION, (e.stderr or "").strip())
        )
    # version.py logs to stderr; the JSON is the last non-empty stdout line.
    lines = [ln for ln in proc.stdout.splitlines() if ln.strip()]
    if not lines:
        raise RuntimeError(
            "the app's version.py produced no output (cwd=%s)." % app_dir)
    try:
        return json.loads(lines[-1])
    except ValueError as e:
        raise RuntimeError(
            "could not parse version.py output as JSON: %s\ngot: %r"
            % (e, lines[-1]))


def resolve_version(app_dir, src_root):
    """Determine the version: env-var override, else app git fallback.

    Returns `(version_dict, source)` where `version_dict` always has the four
    version.json keys (values may be None when neither env nor git supplies one)
    and `source` is "env" or "git".
    """
    v = _from_env()
    source = "env"
    if v is None:
        v = _from_app_git(app_dir, src_root)
        source = "git"
    # Normalize: guarantee exactly the four keys exist.
    return {k: v.get(k) for k in _KEYS}, source


def render_version_module(version):
    """Render the text of a frozen `_version.py` for the given version dict."""
    return (
        "# Auto-generated by the builder's frozen-app stager"
        " (tools/stage_frozen_app.py).\n"
        "# DO NOT EDIT. On-device analogue of seedsigner/version.json.\n"
        "#\n"
        "# Frozen firmware content is import-only (it can't open() a json file), so\n"
        "# the app's version data ships as this frozen .py module. The app reads it\n"
        "# via `from seedsigner import _version` under IS_MICROPYTHON. It exists ONLY\n"
        "# in a builder-produced package (never in the app repo / never on Pi), so\n"
        "# that import is guarded (falls back to Controller.VERSION = 'unknown').\n"
        "VERSION_NAME = %r\n"
        "VERSION_FORK = %r\n"
        "SHORT_COMMIT_HASH = %r\n"
        "VERSION_TIMESTAMP = %r\n"
    ) % (version.get(KEY_NAME), version.get(KEY_FORK),
         version.get(KEY_HASH), version.get(KEY_TIMESTAMP))


def write_version_module(pkg_dir, version):
    """Write `<pkg_dir>/_version.py` and return its path.

    `pkg_dir` is the `seedsigner` package dir: frozen_app/seedsigner for a frozen
    build, /overlay/seedsigner for a device overlay. Called AFTER the package
    mirror so a mirror `--delete` pass can't remove the freshly baked module.
    """
    path = os.path.join(pkg_dir, "_version.py")
    with open(path, "w") as f:
        f.write(render_version_module(version))
    return path
