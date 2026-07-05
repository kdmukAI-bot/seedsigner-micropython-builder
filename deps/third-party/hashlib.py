# Frozen shim that extends MicroPython's *extensible* built-in `hashlib` with
# SHA-512 and PBKDF2-HMAC (both mbedtls-backed, from the native `_hashlib_ext`
# C module). MicroPython's built-in hashlib ships only SHA-256; embit/SeedSigner
# need SHA-512 (BIP32 CKD HMAC-SHA512) and PBKDF2-HMAC-SHA512 (BIP39
# mnemonic_to_seed, Electrum seeds).
#
# hashlib is registered with MP_REGISTER_EXTENSIBLE_MODULE, so this frozen module
# shadows the built-in on `import hashlib`. `from uhashlib import *` forces the
# built-in (via its u-prefix alias) to pull SHA-256 (and any other built-in
# algorithms) back in; we then add the mbedtls-backed extras on top.
from uhashlib import *  # noqa: F401,F403  (built-in hashes: sha256, ...)
from _hashlib_ext import sha512, pbkdf2_hmac  # noqa: F401
