# Developer Guide

Day-to-day build/flash usage — Docker build targets, supported boards, flashing
— lives in [README.md](README.md) and `CLAUDE.md`. This document covers the
workflow around the build: how the frozen app is staged and versioned, and how the
prebaked base image is rebuilt and published.

## Frozen app build & versioning

The firmware freezes a copy of the SeedSigner app + `embit` into the image. Staging and
version-baking are handled by `tools/stage_frozen_app.py`, driven by `make stage-app` — which
`make docker-build-all` runs automatically on the host (the Docker build mounts only this repo, so
the app/embit sources must be mirrored into `frozen_app/` first).

### Staging

```bash
make stage-app BOARD=<board>       # mirror app+embit -> frozen_app/, bake version + provenance
```

Sources resolve via `tools/_devenv`: `SS_APP_DIR` (default `../seedsigner`) and `SS_EMBIT_DIR`
(default `../embit`), both `.env`-overridable — so local dev stages your **sibling working trees**
(the live dev tip), while CI points these at the `deps/seedsigner` / `deps/embit` submodules. The
stager clean-mirrors `seedsigner/` and `embit/` into `frozen_app/` (excluding `resources/`,
`__pycache__`, `*.pyc`; a wipe-then-copy, so a removed module can't linger in the freeze), then
writes two frozen modules:

- `frozen_app/seedsigner/_version.py` — the on-device version (auto-frozen with the package).
- `frozen_app/seedsigner_frozen_build.py` — the provenance marker (SHAs, dirty count, build time,
  board).

`frozen_app/` is gitignored and regenerated on every stage.

### Version source: env override → git fallback

`tools/_version_bake.py` determines the version, in precedence order:

1. **Env override** — set `SEEDSIGNER_VERSION` (plus optional `SEEDSIGNER_VERSION_FORK`,
   `SEEDSIGNER_SHORT_COMMIT_HASH`, `SEEDSIGNER_VERSION_TIMESTAMP`). Used by release / CI builds,
   where the version is decided up front and the build host must not import the app:
   ```bash
   SEEDSIGNER_VERSION=v0.8.7 make stage-app BOARD=<board>
   ```
2. **Git fallback** — otherwise the app's own `helpers/version.py` logic runs in an isolated
   subprocess against the app source, capturing its git / CI state.

Frozen firmware content is import-only (it can't `open()` a JSON file), so the version ships as a
`.py` module — the on-device analogue of the app's `version.json`. The app reads it via
`from seedsigner import _version` under `IS_MICROPYTHON` (guarded; falls back to
`Controller.VERSION = "unknown"`).

### Bumping the app / embit pins

`deps/seedsigner` and `deps/embit` are branch-tracked submodules (see the main README). Advance a
pin to its branch tip with `git submodule update --remote <path>`, then commit the bump. Local dev
doesn't use the submodules — the stager reads your sibling working trees — so `--remote` is for
capturing a pin for CI.

### Overlay dev lane and CI dist (in progress)

Two follow-ups from the frozen-build plan are not yet wired: a `/overlay` dev-iteration lane (push
just the `seedsigner` package to a flash dir that shadows the frozen copy, for fast edits without a
firmware rebuild) and a merge-only CI job that publishes a downloadable, self-launching
`dist/<BOARD>/` (the `/main.py` launcher baked into a VFS partition). Until then, a locally built
image is launched by provisioning `/main.py` with `tools/set_p4_boot_app.py` (see the main README).

## The prebaked base image

Every firmware build runs inside a prebaked Docker image (`Dockerfile`)
containing a pinned **ESP-IDF** baseline (tools installed) plus the common
host/build dependencies. This keeps builds reproducible and fast across local
dev machines and all CI platforms.

MicroPython is **not** baked into the image: it is supplied at build time from
the `deps/micropython/upstream` submodule (which is the real version pin), and
`mpy-cross` is built there during the firmware build. So a MicroPython bump is
just a submodule pointer change — **no image rebuild needed**. The image only
changes when the ESP-IDF baseline moves.

The image is mirrored to three registries:

| Consumer | Registry it pulls |
|----------|-------------------|
| GitHub Actions + local `make docker-*` | `ghcr.io/kdmukai-bot/seedsigner-micropython-builder-base:latest` |
| GitLab CI | `registry.gitlab.com/kdmukai-bot/seedsigner-micropython-builder/base:latest` |
| Forgejo CI | `codeberg.org/kdmukai-bot/seedsigner-micropython-builder-base:latest` |

GHCR is the primary. The GitLab and Codeberg copies are **backups** so builds
keep working if the GitHub account is suspended or otherwise compromised — so
all three follow the same publish procedure and stay in sync. Every copy is
owned by the `kdmukAI-bot` account and must be **public** so every consumer
(including forks and other accounts' CI) can pull it.

> Note the registry paths differ slightly: GHCR and Codeberg use the image name
> `seedsigner-micropython-builder-base`, while GitLab nests it as
> `seedsigner-micropython-builder/base`. Tag exactly as shown below.

## Rebuilding and publishing the base image

The base image is built and published **manually from a trusted local machine**,
not by a CI job. Publishing requires a registry write token, and keeping that
token off CI runners avoids exposing it to a compromised build-time dependency
that could exfiltrate it. The image changes rarely — only when the pinned ESP-IDF
ref in `Dockerfile` is bumped — so a manual publish is a worthwhile trade.
Publish to all three registries so every consumer stays in sync.

### 1. Build locally

`ESP_IDF_REF` is a build arg; the default matches `Dockerfile`. Bump it here (and
in `Dockerfile`) when moving to a new ESP-IDF baseline. The build is long (ESP-IDF
clone + toolchain install); expect tens of minutes.

The `Dockerfile` has no `COPY`/`ADD` (it clones everything), so build it
**context-less** — piping it on stdin avoids tarring the whole repo (`deps/`,
`build/`) as a build context:

```bash
IDF_REF=v5.5.1
docker build --build-arg ESP_IDF_REF="$IDF_REF" -t ss-mpb-base:local - < Dockerfile
```

### 2. Tag for all three registries

Tag both a moving `:latest` and an immutable, traceable `:idf-<IDF_REF>` tag on
every registry:

```bash
for repo in \
  ghcr.io/kdmukai-bot/seedsigner-micropython-builder-base \
  registry.gitlab.com/kdmukai-bot/seedsigner-micropython-builder/base \
  codeberg.org/kdmukai-bot/seedsigner-micropython-builder-base; do
  docker tag ss-mpb-base:local "$repo:latest"
  docker tag ss-mpb-base:local "$repo:idf-${IDF_REF}"
done
```

### 3. Create the publish tokens (one-time)

You need one token per registry, each scoped to **registry write only** and
**short-lived** — a short expiry where the registry offers one, otherwise deleted
right after use. They're used rarely, so regenerate on demand rather than keeping
a long-lived token around. Store them in your password manager, never on disk.
Create all three as the `kdmukAI-bot` account.

*GHCR — classic PAT:*
1. GitHub → **Settings → Developer settings → Personal access tokens → Tokens
   (classic) → Generate new token (classic)**.
2. Name it (e.g. `ghcr-base-image-publish`); set a short **Expiration** (e.g. 7 days).
3. Under **Select scopes**, check **`write:packages`** (implies `read:packages`).
4. **Generate token** → copy the `ghp_…` value into your password manager.
   - Use a **classic** PAT — fine-grained PATs are unreliable for GHCR, and the
     token must belong to `kdmukAI-bot` (the package owner).

*GitLab — Personal Access Token:*
1. GitLab → avatar → **Edit profile → Access Tokens**
   (`gitlab.com/-/user_settings/personal_access_tokens`).
2. Name it; set an **Expiration date**.
3. Scopes: check **`write_registry`** and **`read_registry`** only.
4. **Create** → copy the `glpat-…` value into your password manager.
   - More-scoped alternative: a **project deploy token** (project → **Settings →
     Repository → Deploy tokens**) with `read_registry` + `write_registry`.

*Codeberg — access token (Forgejo permission-scoped):*
1. Codeberg → **Settings → Applications → Access Tokens**
   (`codeberg.org/user/settings/applications`).
2. Name it.
3. Set **Repository and organization access** to **"Public only"** — this field is
   required (it cannot be empty). Choose "Public only", not "All (public, private
   and limited)"; public-only is least-privilege and does not gate the registry push.
4. Set the **`package`** permission to **Read and write** (this is what authorizes
   the push); leave the other permission scopes at **No Access**.
5. **Generate Token** → copy the value into your password manager.
6. Codeberg tokens have no expiry field, so **delete this token right after publishing**
   (same page) to keep it short-lived.

### 4. Log in and push — without leaving a token on disk or in shell history

Pull each token from your password manager. Paste it at a hidden prompt and use a
throwaway Docker config so nothing persists in `~/.docker/config.json`:

```bash
export DOCKER_CONFIG="$(mktemp -d)"          # throwaway, not ~/.docker
IDF_REF=v5.5.1                                # match the build above
TAG="idf-${IDF_REF}"

# --- GHCR (primary) ---
read -rs TOKEN && echo                        # paste GHCR PAT; hidden, not in history
printf '%s' "$TOKEN" | docker login ghcr.io -u kdmukAI-bot --password-stdin
docker push ghcr.io/kdmukai-bot/seedsigner-micropython-builder-base:latest
docker push "ghcr.io/kdmukai-bot/seedsigner-micropython-builder-base:$TAG"
docker logout ghcr.io

# --- GitLab (backup) ---
read -rs TOKEN && echo                        # paste GitLab token
printf '%s' "$TOKEN" | docker login registry.gitlab.com -u kdmukAI-bot --password-stdin
docker push registry.gitlab.com/kdmukai-bot/seedsigner-micropython-builder/base:latest
docker push "registry.gitlab.com/kdmukai-bot/seedsigner-micropython-builder/base:$TAG"
docker logout registry.gitlab.com

# --- Codeberg (backup) ---
read -rs TOKEN && echo                        # paste Codeberg token
printf '%s' "$TOKEN" | docker login codeberg.org -u kdmukAI-bot --password-stdin
docker push codeberg.org/kdmukai-bot/seedsigner-micropython-builder-base:latest
docker push "codeberg.org/kdmukai-bot/seedsigner-micropython-builder-base:$TAG"
docker logout codeberg.org

rm -rf "$DOCKER_CONFIG"; unset DOCKER_CONFIG TOKEN
```

> **Never** `echo "<token>" | docker login …` — that writes the literal token
> into `~/.bash_history`. Always read it into a variable via `read -rs` (hidden,
> not recorded) and pipe with `--password-stdin`.

### 5. Verify visibility

A newly created package may default to private. Each consumer (including
cross-account CI and forks) pulls anonymously, so confirm all three are
**public**:

- GHCR: `github.com/users/kdmukAI-bot/packages/container/seedsigner-micropython-builder-base/settings` → visibility **Public**
- GitLab/Codeberg: the package/registry visibility follows the project; ensure the backing project is public.

Quick anonymous pull check (no login):

```bash
docker logout ghcr.io 2>/dev/null
docker pull ghcr.io/kdmukai-bot/seedsigner-micropython-builder-base:latest
```
