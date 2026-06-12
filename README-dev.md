# Developer Guide

Day-to-day build/flash usage — Docker build targets, supported boards, flashing
— lives in [README.md](README.md) and `CLAUDE.md`. This document covers the parts
of the workflow that are **not** part of a normal build, chiefly the prebaked
base image.

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
