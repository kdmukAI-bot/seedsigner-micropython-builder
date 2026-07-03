# deps/third-party

Pinned in-repo copies of pure-Python third-party packages that the SeedSigner app
needs on-device but that are **not** official micropython-lib (so `require()` can't
resolve them).

Vendoring a pinned copy here — rather than pulling from a developer's
`.venv/.../python3.12/site-packages/...` — keeps the firmware build reproducible and
gives the board manifest a stable `base_path` to `package()` (freeze) from.

## urtypes 1.0.1

- Source: PyPI `urtypes==1.0.1` (pinned in `seedsigner/requirements.txt`).
- Purpose: extracts the PSBT/descriptor bytes out of the UR CBOR after an animated
  `ur:crypto-psbt/...` scan completes (`DecodeQR.get_psbt()` →
  `from urtypes.crypto import PSBT`). Without it, a completed UR PSBT scan raises
  `ImportError` and the Controller's catch-all masks it as "Not Yet Implemented".
  See `docs/knowledge/ur-psbt-urtypes-missing-on-device.md`.
- Dependencies: none beyond the MicroPython stdlib (`binascii`, `hashlib`, `io`,
  `math`, `struct`). Written to run on MicroPython (Foundation Devices' Passport
  uses it), so no compat shims are needed.
- **Frozen into firmware** via the P4 board `manifest.py`:
  `package("urtypes", base_path="$(MPY_DIR)/../../../deps/third-party")`
  (`$(MPY_DIR)` = `deps/micropython/upstream`, so the base_path resolves back to
  this dir). Unlike embit, urtypes has no native-code (secp256k1) build step, so it
  is frozen on its own rather than waiting for the embit/secp256k1 freeze pass.
  Because it is frozen, it is **not** vendored to `/lib` by `tools/deploy_app.py`
  — same lifecycle as the frozen `logging`/`hmac`.

To refresh the pin: `pip download urtypes==<ver>` (or copy the `.venv` tree),
drop `__pycache__`/`*.pyc`, and update this note.
