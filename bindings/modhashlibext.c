// Native hashlib extension: SHA-512 + PBKDF2-HMAC, exposed as the private helper
// module `_hashlib_ext`. A frozen `hashlib.py` (`from uhashlib import * ; from
// _hashlib_ext import sha512, pbkdf2_hmac`) shadows the extensible built-in
// `hashlib` and merges these in — so `hashlib.sha512` / `hashlib.pbkdf2_hmac` work
// with NO patch to modhashlib.c.
//
// MicroPython's built-in hashlib ships only SHA-256; SeedSigner/embit need SHA-512
// (BIP32 CKD HMAC-SHA512) and PBKDF2-HMAC-SHA512 (BIP39 mnemonic_to_seed, Electrum
// seeds). The actual crypto lives in the __idf_esp-hashlib-ext component (mbedtls);
// this TU only calls its plain-C API (hashlib_ext.h) so no mbedtls headers enter the
// usermod QSTR scan.
//
// HMAC correctness note: the frozen hmac reads `hash.block_size` (default 64).
// SHA-512's block size is 128, so the sha512 type MUST expose block_size=128 or
// every HMAC-SHA512 (and thus all BIP32 derivation) would be computed wrong.

#include <string.h>

#include "py/objstr.h"
#include "py/runtime.h"

#include "hashlib_ext.h"

#ifndef MP_ERROR_TEXT
#define MP_ERROR_TEXT(x) (x)
#endif

// ---------------------------------------------------------------------------
// sha512 hash object. The opaque mbedtls context is stored inline in the GC
// object (flexible array), sized via hlx_sha512_ctx_size(); no finaliser needed.
// ---------------------------------------------------------------------------
typedef struct {
    mp_obj_base_t base;
    uint8_t ctx[];
} mp_obj_sha512_t;

static const mp_obj_type_t hashlib_ext_sha512_type;

static mp_obj_t sha512_update(mp_obj_t self_in, mp_obj_t arg);

static mp_obj_t sha512_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw,
                                const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 1, false);
    mp_obj_sha512_t *o = mp_obj_malloc_var(mp_obj_sha512_t, ctx, uint8_t,
                                           hlx_sha512_ctx_size(), type);
    hlx_sha512_init(o->ctx);
    if (n_args == 1) {
        sha512_update(MP_OBJ_FROM_PTR(o), args[0]);
    }
    return MP_OBJ_FROM_PTR(o);
}

static mp_obj_t sha512_update(mp_obj_t self_in, mp_obj_t arg) {
    mp_obj_sha512_t *self = MP_OBJ_TO_PTR(self_in);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(arg, &bufinfo, MP_BUFFER_READ);
    hlx_sha512_update(self->ctx, bufinfo.buf, bufinfo.len);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sha512_update_obj, sha512_update);

static mp_obj_t sha512_digest(mp_obj_t self_in) {
    mp_obj_sha512_t *self = MP_OBJ_TO_PTR(self_in);
    vstr_t vstr;
    vstr_init_len(&vstr, 64);
    hlx_sha512_digest(self->ctx, (uint8_t *)vstr.buf);
    return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sha512_digest_obj, sha512_digest);

static mp_obj_t sha512_copy(mp_obj_t self_in) {
    mp_obj_sha512_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_sha512_t *o = mp_obj_malloc_var(mp_obj_sha512_t, ctx, uint8_t,
                                           hlx_sha512_ctx_size(), &hashlib_ext_sha512_type);
    hlx_sha512_clone(o->ctx, self->ctx);
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sha512_copy_obj, sha512_copy);

static const mp_rom_map_elem_t sha512_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_update), MP_ROM_PTR(&sha512_update_obj)},
    {MP_ROM_QSTR(MP_QSTR_digest), MP_ROM_PTR(&sha512_digest_obj)},
    {MP_ROM_QSTR(MP_QSTR_copy), MP_ROM_PTR(&sha512_copy_obj)},
    // Exposed for hmac (block_size=128 is REQUIRED for correct HMAC-SHA512).
    {MP_ROM_QSTR(MP_QSTR_digest_size), MP_ROM_INT(64)},
    {MP_ROM_QSTR(MP_QSTR_block_size), MP_ROM_INT(128)},
};
static MP_DEFINE_CONST_DICT(sha512_locals_dict, sha512_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    hashlib_ext_sha512_type,
    MP_QSTR_sha512,
    MP_TYPE_FLAG_NONE,
    make_new, sha512_make_new,
    locals_dict, &sha512_locals_dict
    );

// ---------------------------------------------------------------------------
// pbkdf2_hmac(hash_name, password, salt, iterations, dklen=None) -> bytes
// Only 'sha512' is supported (all SeedSigner/embit callers use it).
// ---------------------------------------------------------------------------
static mp_obj_t mod_pbkdf2_hmac(size_t n_args, const mp_obj_t *args) {
    const char *hash_name = mp_obj_str_get_str(args[0]);
    if (strcmp(hash_name, "sha512") != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("only 'sha512' is supported"));
    }
    mp_buffer_info_t pw, salt;
    mp_get_buffer_raise(args[1], &pw, MP_BUFFER_READ);
    mp_get_buffer_raise(args[2], &salt, MP_BUFFER_READ);
    mp_int_t iters = mp_obj_get_int(args[3]);
    if (iters <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("iterations must be positive"));
    }
    mp_int_t dklen = 64;
    if (n_args > 4 && args[4] != mp_const_none) {
        dklen = mp_obj_get_int(args[4]);
        if (dklen <= 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("dklen must be positive"));
        }
    }
    vstr_t vstr;
    vstr_init_len(&vstr, dklen);
    int rc = hlx_pbkdf2_sha512((const uint8_t *)pw.buf, pw.len,
                               (const uint8_t *)salt.buf, salt.len,
                               (unsigned int)iters, (uint8_t *)vstr.buf, dklen);
    if (rc != 0) {
        vstr_clear(&vstr);
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("pbkdf2_hmac failed"));
    }
    return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_pbkdf2_hmac_obj, 4, 5, mod_pbkdf2_hmac);

// ---------------------------------------------------------------------------
// module `_hashlib_ext`
// ---------------------------------------------------------------------------
static const mp_rom_map_elem_t hashlib_ext_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__hashlib_ext)},
    {MP_ROM_QSTR(MP_QSTR_sha512), MP_ROM_PTR(&hashlib_ext_sha512_type)},
    {MP_ROM_QSTR(MP_QSTR_pbkdf2_hmac), MP_ROM_PTR(&mod_pbkdf2_hmac_obj)},
};
static MP_DEFINE_CONST_DICT(hashlib_ext_globals, hashlib_ext_globals_table);

const mp_obj_module_t hashlib_ext_user_cmodule = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&hashlib_ext_globals,
};

MP_REGISTER_MODULE(MP_QSTR__hashlib_ext, hashlib_ext_user_cmodule);
