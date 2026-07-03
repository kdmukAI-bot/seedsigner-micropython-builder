# Frozen manifest for the SeedSigner ESP32-P4 firmware.
#
# Starts from the esp32 port default, then freezes the stdlib modules the
# SeedSigner Python app imports as top-level names. These are official
# micropython-lib (python-stdlib) packages; freezing them into the firmware
# means the deploy harness no longer has to vendor them to /lib.
include("$(PORT_DIR)/boards/manifest.py")

require("logging")
require("hmac")

# urtypes==1.0.1: third-party PyPI package (NOT micropython-lib), so it can't be
# resolved via require(); freeze it from the pinned in-repo copy instead. It pulls
# the PSBT/descriptor bytes out of the UR CBOR after a UR scan completes
# (DecodeQR.get_psbt()); without it a completed ur:crypto-psbt raises ImportError.
# base_path anchors to the builder repo root via the absolute $(MPY_DIR)
# (= <repo>/deps/micropython/upstream), so it resolves to <repo>/deps/third-party.
# Unlike embit, urtypes has no native-code (secp256k1) build step, so it can be
# frozen on its own now rather than waiting for the embit/secp256k1 freeze pass.
package("urtypes", base_path="$(MPY_DIR)/../../../deps/third-party")
