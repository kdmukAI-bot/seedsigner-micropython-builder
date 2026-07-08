# Frozen shim that extends MicroPython's *extensible* built-in `hashlib` with
# SHA-512, PBKDF2-HMAC, and RIPEMD-160 (all mbedtls-backed, from the native
# `_hashlib_ext` C module). MicroPython's built-in hashlib ships only SHA-256;
# embit/SeedSigner need SHA-512 (BIP32 CKD HMAC-SHA512), PBKDF2-HMAC-SHA512 (BIP39
# mnemonic_to_seed, Electrum seeds), and RIPEMD-160 (hash160 = ripemd160(sha256)).
#
# hashlib is registered with MP_REGISTER_EXTENSIBLE_MODULE, so this frozen module
# shadows the built-in on `import hashlib`. `from uhashlib import *` forces the
# built-in (via its u-prefix alias) to pull SHA-256 (and any other built-in
# algorithms) back in; we then add the mbedtls-backed extras on top.
from uhashlib import *  # noqa: F401,F403  (built-in hashes: sha256, ...)
from _hashlib_ext import sha512, pbkdf2_hmac, ripemd160  # noqa: F401


def new(name, data=b""):
    """hashlib.new(name, data=b"") — mirrors CPython's factory. embit's
    hashes.py calls hashlib.new("ripemd160", msg).digest(); routing "ripemd160"
    to the native type makes hash160 fully native (its try/except fallback to
    util.py_ripemd160 stays intact for CPython/host). Other names resolve to the
    built-in/extended constructors already on this module."""
    if name == "ripemd160":
        return ripemd160(data)
    ctor = globals().get(name)
    if ctor is None:
        raise ValueError("unsupported hash type " + name)
    return ctor(data)
