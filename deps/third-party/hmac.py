# Frozen hmac shim. Shadows micropython-lib's pure-Python `hmac`.
#
# digestmod == "sha512" (embit BIP32 CKD's call: hmac.new(key, data,
# digestmod="sha512").digest()) routes to the native mbedtls one-shot in
# `_hashlib_ext` — eliminating the pure-Python ipad/opad XOR + two-pass glue that
# runs on every one of the ~78 CKD levels in a 10-input multisig parse. Every
# OTHER digestmod falls back to the reference pure-Python HMAC (a verbatim copy of
# micropython-lib's hmac.py), preserving the full `hmac` module API/behavior.
import binascii

try:
    from _hashlib_ext import hmac_sha512 as _native_hmac_sha512
except ImportError:  # host/CPython, or a firmware without the native module
    _native_hmac_sha512 = None


class _NativeHMACSHA512:
    """HMAC-SHA512 via the native mbedtls one-shot. Buffers the message and
    recomputes on digest() — HMAC is a pure function of (key, msg), and embit
    supplies the whole message in the constructor, so the one-shot runs once.
    Exposes the same surface (update/digest/hexdigest/copy, digest_size,
    block_size, name) the pure-Python HMAC does."""
    digest_size = 64
    block_size = 128

    def __init__(self, key, msg=None):
        self._key = bytes(key)
        self._msg = bytearray()
        if msg is not None:
            self._msg += msg

    def update(self, msg):
        self._msg += msg

    def digest(self):
        return _native_hmac_sha512(self._key, bytes(self._msg))

    def hexdigest(self):
        return str(binascii.hexlify(self.digest()), "utf-8")

    def copy(self):
        other = _NativeHMACSHA512.__new__(_NativeHMACSHA512)
        other._key = self._key
        other._msg = bytearray(self._msg)
        return other

    @property
    def name(self):
        return "hmac-sha512"


def _is_sha512(digestmod):
    if digestmod == "sha512":
        return True
    # Also route the callable form (hashlib.sha512) / a module with .name.
    name = getattr(digestmod, "__name__", None) or getattr(digestmod, "name", None)
    return name == "sha512"


# ---------------------------------------------------------------------------
# Reference pure-Python HMAC (verbatim from micropython-lib python-stdlib/hmac),
# used for every digestmod other than sha512.
# ---------------------------------------------------------------------------
class HMAC:
    def __init__(self, key, msg=None, digestmod=None):
        if not isinstance(key, (bytes, bytearray)):
            raise TypeError("key: expected bytes/bytearray")

        import hashlib

        if digestmod is None:
            # TODO: Default hash algorithm is now deprecated.
            digestmod = hashlib.md5

        if callable(digestmod):
            # A hashlib constructor returning a new hash object.
            make_hash = digestmod  # A
        elif isinstance(digestmod, str):
            # A hash name suitable for hashlib.new().
            make_hash = lambda d=b"": getattr(hashlib, digestmod)(d)
        else:
            # A module supporting PEP 247.
            make_hash = digestmod.new  # C

        self._outer = make_hash()
        self._inner = make_hash()

        self.digest_size = getattr(self._inner, "digest_size", None)
        # If the provided hash doesn't support block_size (e.g. built-in
        # hashlib), 64 is the correct default for all built-in hash
        # functions (md5, sha1, sha256).
        self.block_size = getattr(self._inner, "block_size", 64)

        # Truncate to digest_size if greater than block_size.
        if len(key) > self.block_size:
            key = make_hash(key).digest()

        # Pad to block size.
        key = key + bytes(self.block_size - len(key))

        self._outer.update(bytes(x ^ 0x5C for x in key))
        self._inner.update(bytes(x ^ 0x36 for x in key))

        if msg is not None:
            self.update(msg)

    @property
    def name(self):
        return "hmac-" + getattr(self._inner, "name", type(self._inner).__name__)

    def update(self, msg):
        self._inner.update(msg)

    def copy(self):
        if not hasattr(self._inner, "copy"):
            # Not supported for built-in hash functions.
            raise NotImplementedError()
        # Call __new__ directly to avoid the expensive __init__.
        other = self.__class__.__new__(self.__class__)
        other.block_size = self.block_size
        other.digest_size = self.digest_size
        other._inner = self._inner.copy()
        other._outer = self._outer.copy()
        return other

    def _current(self):
        h = self._outer
        if hasattr(h, "copy"):
            # built-in hash functions don't support this, and as a result,
            # digest() will finalise the hmac and further calls to
            # update/digest will fail.
            h = h.copy()
        h.update(self._inner.digest())
        return h

    def digest(self):
        h = self._current()
        return h.digest()

    def hexdigest(self):
        import binascii

        return str(binascii.hexlify(self.digest()), "utf-8")


def new(key, msg=None, digestmod=None):
    if _native_hmac_sha512 is not None and _is_sha512(digestmod):
        return _NativeHMACSHA512(key, msg)
    return HMAC(key, msg, digestmod)


def compare_digest(a, b):
    """Constant-time-ish comparison (pure addition over the frozen hmac API; the
    stdlib one isn't in micropython-lib's copy, so add it defensively)."""
    if len(a) != len(b):
        return False
    result = 0
    for x, y in zip(a, b):
        result |= x ^ y
    return result == 0
