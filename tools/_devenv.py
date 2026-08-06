"""Env-driven local-dev config for the deploy / SD-stage / benchmark tools.

NO hard-coded local paths live in committed files. Instead:
  * this repo's OWN paths resolve relative to __file__ (REPO_ROOT);
  * the app and embit default to this repo's PINNED SUBMODULES (deps/…), so what
    a build freezes is decided by the pin and is reproducible from a fresh clone;
  * every path is still a one-line env override for a non-standard layout or a
    deliberate build of local working-tree edits.

Precedence: process env  >  a `.env` file at the repo root  >  the default.
`.env` is gitignored (it holds a workstation's real paths); `.env.example` is the
committed contract. See docs/language-pack-repo-integration-todo.md.

Language packs: this repo POINTS ONLY AT THE APP (finalized 2026-07-06 design). The
app bundles its deployable packs at `src/lang-packs`; we copy those bytes to the SD.
This repo does NOT know the pack repo and does NOT build packs — whatever the app
bundled is what deploys, and absent packs = a valid English-only deploy.

⚠ `src/lang-packs/` is GITIGNORED in the app repo — it is a build artifact, not
tracked content. A clean checkout (which is what the pinned submodule is) therefore
has NO packs, and a build off the default resolves to an English-only image. Set
SS_PACKS_DIR at a tree that actually holds built pack bytes to get a localized one.

This is a LOCAL-DEV helper only — SeedSigner-OS and the production MicroPython build
assemble their own pieces and do not use these scripts.
"""
import os

# This repo's own root (tools/ -> repo root) and the common dev/ parent it sits in.
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_DEV_ROOT = os.path.dirname(REPO_ROOT)


def _load_dotenv():
    vals = {}
    try:
        with open(os.path.join(REPO_ROOT, ".env")) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                vals[k.strip()] = v.strip().strip('"').strip("'")
    except OSError:
        pass
    return vals


_DOTENV = _load_dotenv()


def get(key, default=None):
    """Resolve `key`: process env, then .env, then `default` (empty == unset)."""
    return os.environ.get(key) or _DOTENV.get(key) or default


# --- app + embit sources (env-overridable; PINNED-SUBMODULE defaults) --------
# These default to this repo's OWN pinned submodules, not to a sibling working
# clone, so a build's app code is whatever the pin says and nothing else. The
# sibling default that used to live here silently froze whatever state a local
# working tree happened to be in — including local-only commits that exist on no
# remote, producing images nobody (including us, after a fresh clone) could
# reproduce. Point SS_APP_DIR at a working clone to deliberately build local
# edits; the default is the reproducible one.
SS_APP_DIR = get("SS_APP_DIR", os.path.join(REPO_ROOT, "deps", "seedsigner"))
SS_EMBIT_DIR = get("SS_EMBIT_DIR", os.path.join(REPO_ROOT, "deps", "embit"))
# btc-datagen has no submodule here — it stays a sibling-layout default.
SS_BTC_DATAGEN_DIR = get("SS_BTC_DATAGEN_DIR", os.path.join(_DEV_ROOT, "btc-datagen"))

# --- derived paths -----------------------------------------------------------
SS_SRC = os.path.join(SS_APP_DIR, "src", "seedsigner")   # the app PACKAGE (deploy_app pushes this)
SS_SRC_ROOT = os.path.join(SS_APP_DIR, "src")            # the sys.path root (host-import benches)
SS_LANGPACKS_DIR = os.path.join(SS_SRC_ROOT, "lang-packs")  # the app's bundled, deployable packs
EMBIT_SRC = os.path.join(SS_EMBIT_DIR, "src", "embit")
UR2_SRC = os.path.join(SS_SRC, "helpers", "ur2")
TOOLS_DIR = os.path.join(REPO_ROOT, "tools")             # this repo's own tools/ (relative)


def resolve_packs():
    """The dir of built pack bytes to STAGE to the SD. This repo POINTS ONLY AT THE APP:
    it copies whatever the app bundled at $SS_APP_DIR/src/lang-packs, and neither knows
    the pack repo nor builds packs. `SS_PACKS_DIR` overrides for an oddball layout. An
    absent/empty dir = a valid English-only deploy (the caller stages nothing; the app
    renders its baked English floor), so this NEVER errors on a missing dir."""
    return get("SS_PACKS_DIR") or SS_LANGPACKS_DIR
