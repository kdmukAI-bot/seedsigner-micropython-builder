// Plain-C SHA-512 + PBKDF2-HMAC-SHA512 API, backed by mbedtls. The mbedtls
// headers/config live only inside this ESP-IDF component (REQUIRES mbedtls); the
// MicroPython binding (bindings/modhashlibext.c) includes ONLY this header, so no
// mbedtls generator-expression include dirs leak into the usermod QSTR scan.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Size (bytes) of an opaque SHA-512 context; the binding allocates this inline in
// its GC object so no separate heap alloc / finaliser is needed (the mbedtls
// SHA-512 context holds no heap-owned internals).
size_t hlx_sha512_ctx_size(void);

void hlx_sha512_init(void *ctx);                                   // init + start SHA-512
void hlx_sha512_update(void *ctx, const uint8_t *data, size_t len);
void hlx_sha512_digest(const void *ctx, uint8_t out[64]);         // non-destructive
void hlx_sha512_clone(void *dst, const void *src);                // dst init'd inside
void hlx_sha512_free(void *ctx);

// PBKDF2-HMAC-SHA512. Returns 0 on success. dklen bytes written to out.
int hlx_pbkdf2_sha512(const uint8_t *password, size_t plen,
                      const uint8_t *salt, size_t slen,
                      unsigned int iterations, uint8_t *out, size_t dklen);

// One-shot HMAC-SHA512. Returns 0 on success; writes 64 bytes to out. This is the
// hot path — embit's BIP32 CKD calls hmac.new(key, data, digestmod="sha512").digest()
// (one-shot), rerouted here via the frozen hmac.py shim.
int hlx_hmac_sha512(const uint8_t *key, size_t klen,
                    const uint8_t *msg, size_t mlen, uint8_t out[64]);

// RIPEMD-160 as an incremental type, mirroring the SHA-512 type above (mbedtls
// ripemd160 context holds no heap-owned internals, so no finaliser is needed).
// Makes embit's hash160 = ripemd160(sha256(x)) fully native via hashlib.new().
size_t hlx_ripemd160_ctx_size(void);
void hlx_ripemd160_init(void *ctx);
void hlx_ripemd160_update(void *ctx, const uint8_t *data, size_t len);
void hlx_ripemd160_digest(const void *ctx, uint8_t out[20]);   // non-destructive
void hlx_ripemd160_clone(void *dst, const void *src);
void hlx_ripemd160_free(void *ctx);

#ifdef __cplusplus
}
#endif
