# Frozen manifest for the SeedSigner ESP32-P4 firmware.
#
# Starts from the esp32 port default, then freezes the stdlib modules the
# SeedSigner Python app imports as top-level names. These are official
# micropython-lib (python-stdlib) packages; freezing them into the firmware
# means the deploy harness no longer has to vendor them to /lib.
include("$(PORT_DIR)/boards/manifest.py")

require("logging")

# hmac.py: frozen shim that routes hmac.new(..., digestmod="sha512") — embit's
# BIP32 CKD hot path — to the native mbedtls one-shot in `_hashlib_ext`, and falls
# back to the reference pure-Python HMAC (micropython-lib's body) for every other
# digestmod. Replaces `require("hmac")` (which pulled the pure-Python lib); freezing
# our own shadowing copy avoids a duplicate-`hmac` name clash. See
# deps/third-party/hmac.py + bindings/modhashlibext.c (hmac_sha512).
module("hmac.py", base_path="$(MPY_DIR)/../../../deps/third-party")

# urtypes==1.0.1: third-party PyPI package (NOT micropython-lib), so it can't be
# resolved via require(); freeze it from the pinned in-repo copy instead. It pulls
# the PSBT/descriptor bytes out of the UR CBOR after a UR scan completes
# (DecodeQR.get_psbt()); without it a completed ur:crypto-psbt raises ImportError.
# base_path anchors to the builder repo root via the absolute $(MPY_DIR)
# (= <repo>/deps/micropython/upstream), so it resolves to <repo>/deps/third-party.
# Unlike embit, urtypes has no native-code (secp256k1) build step, so it can be
# frozen on its own now rather than waiting for the embit/secp256k1 freeze pass.
package("urtypes", base_path="$(MPY_DIR)/../../../deps/third-party")

# hashlib.py: frozen shim that extends the extensible built-in `hashlib` with
# mbedtls-backed SHA-512 + PBKDF2-HMAC (from the native `_hashlib_ext` C module).
# Required for BIP32 (HMAC-SHA512) and BIP39 (PBKDF2) — the built-in hashlib only
# ships SHA-256. See deps/third-party/hashlib.py and bindings/modhashlibext.c.
module("hashlib.py", base_path="$(MPY_DIR)/../../../deps/third-party")

# seedsigner_lvgl_screens.py: the public Python facade the shared app imports. It wraps
# the private C module `_seedsigner_lvgl_screens`, does the microSD language-pack I/O the
# C side can't (ESP-IDF fatfs vs MicroPython oofatfs link collision), and exposes the same
# dir-based locale API the Pi native module does — so the app calls one API on both
# platforms. See deps/third-party/seedsigner_lvgl_screens.py + bindings/modseedsigner_bindings.c
# (the module rename to `_seedsigner_lvgl_screens`).
module("seedsigner_lvgl_screens.py", base_path="$(MPY_DIR)/../../../deps/third-party")

# ---- Frozen app tree (P4-35 bring-up, 2026-07-10) ----
# Freeze the full SeedSigner app + embit into the firmware. On this VFS a
# successful stat costs ~60-70 ms and a trivial import ~130 ms, so a ~40-module
# boot import chain is ~5-6 s of pure VFS overhead; frozen imports skip the VFS
# entirely (measured on P4-43: import seedsigner.controller 5921 ms -> ~194 ms).
# `.frozen` precedes `/lib` in sys.path, so this shadows any /lib copy with no
# boot.py change — the deploy harness need only write /main.py. Package
# resolution is ATOMIC per top-level name, so a dev override of a frozen
# `seedsigner` must overlay the WHOLE package, not single files.
# `seedsigner/resources` is data (never imported) and stays off the freeze / on
# the microSD. The sources are staged into <repo>/frozen_app by
# tools/stage_frozen_app.py (`make stage-app`, which docker-build-all runs on the
# host -- the build container only mounts this repo). That stager also bakes the
# version into seedsigner/_version.py and regenerates the seedsigner_frozen_build
# provenance marker. See docs/knowledge/esp32-p4-vfs-import-overhead.md.
#
# frozen_app/ is gitignored and staged before a dev/deploy build, so a clean
# checkout that does NOT stage it -- e.g. CI, which only verifies the firmware
# compiles -- has nothing to freeze. Guard the freeze: package() chdirs into
# base_path and raises FileNotFoundError when frozen_app is absent, so catch it
# and build without the frozen tree (the app is VFS-deployed there instead).
try:
    package("seedsigner", base_path="$(MPY_DIR)/../../../frozen_app")
    package("embit", base_path="$(MPY_DIR)/../../../frozen_app")
    module("seedsigner_frozen_build.py", base_path="$(MPY_DIR)/../../../frozen_app")
except OSError:
    pass  # frozen_app not staged (e.g. CI) -> build without the frozen app tree
