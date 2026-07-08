// mbedtls-backed implementation of the plain-C SHA-512 + PBKDF2 API (hashlib_ext.h).
// Compiled as an ESP-IDF component that REQUIRES mbedtls, so it sees mbedtls's exact
// config + headers. Keeping the mbedtls surface here (not in the MicroPython usermod
// binding) avoids leaking mbedtls generator-expression include dirs into the usermod
// QSTR scan — same plain-C-lib split used by cUR / esp-secp256k1.

#include "hashlib_ext.h"

#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/ripemd160.h"
#include "mbedtls/sha512.h"

size_t hlx_sha512_ctx_size(void) {
    return sizeof(mbedtls_sha512_context);
}

void hlx_sha512_init(void *ctx) {
    mbedtls_sha512_init((mbedtls_sha512_context *)ctx);
    mbedtls_sha512_starts((mbedtls_sha512_context *)ctx, 0);  // 0 => SHA-512
}

void hlx_sha512_update(void *ctx, const uint8_t *data, size_t len) {
    mbedtls_sha512_update((mbedtls_sha512_context *)ctx, data, len);
}

// Non-destructive: clone then finalise the copy so the caller can keep updating.
void hlx_sha512_digest(const void *ctx, uint8_t out[64]) {
    mbedtls_sha512_context tmp;
    mbedtls_sha512_init(&tmp);
    mbedtls_sha512_clone(&tmp, (const mbedtls_sha512_context *)ctx);
    mbedtls_sha512_finish(&tmp, out);
    mbedtls_sha512_free(&tmp);
}

void hlx_sha512_clone(void *dst, const void *src) {
    mbedtls_sha512_init((mbedtls_sha512_context *)dst);
    mbedtls_sha512_clone((mbedtls_sha512_context *)dst, (const mbedtls_sha512_context *)src);
}

void hlx_sha512_free(void *ctx) {
    mbedtls_sha512_free((mbedtls_sha512_context *)ctx);
}

int hlx_pbkdf2_sha512(const uint8_t *password, size_t plen,
                      const uint8_t *salt, size_t slen,
                      unsigned int iterations, uint8_t *out, size_t dklen) {
    return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA512, password, plen,
                                         salt, slen, iterations,
                                         (uint32_t)dklen, out);
}

int hlx_hmac_sha512(const uint8_t *key, size_t klen,
                    const uint8_t *msg, size_t mlen, uint8_t out[64]) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA512);
    if (info == NULL) {
        return -1;
    }
    return mbedtls_md_hmac(info, key, klen, msg, mlen, out);
}

size_t hlx_ripemd160_ctx_size(void) {
    return sizeof(mbedtls_ripemd160_context);
}

void hlx_ripemd160_init(void *ctx) {
    mbedtls_ripemd160_init((mbedtls_ripemd160_context *)ctx);
    mbedtls_ripemd160_starts((mbedtls_ripemd160_context *)ctx);
}

void hlx_ripemd160_update(void *ctx, const uint8_t *data, size_t len) {
    mbedtls_ripemd160_update((mbedtls_ripemd160_context *)ctx, data, len);
}

// Non-destructive: clone then finalise the copy so the caller can keep updating.
void hlx_ripemd160_digest(const void *ctx, uint8_t out[20]) {
    mbedtls_ripemd160_context tmp;
    mbedtls_ripemd160_init(&tmp);
    mbedtls_ripemd160_clone(&tmp, (const mbedtls_ripemd160_context *)ctx);
    mbedtls_ripemd160_finish(&tmp, out);
    mbedtls_ripemd160_free(&tmp);
}

void hlx_ripemd160_clone(void *dst, const void *src) {
    mbedtls_ripemd160_init((mbedtls_ripemd160_context *)dst);
    mbedtls_ripemd160_clone((mbedtls_ripemd160_context *)dst,
                            (const mbedtls_ripemd160_context *)src);
}

void hlx_ripemd160_free(void *ctx) {
    mbedtls_ripemd160_free((mbedtls_ripemd160_context *)ctx);
}
