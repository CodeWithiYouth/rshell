/* Copyright (C) 2026 rshell contributors
 *
 * Windows CNG 后端的 Ed25519 / X25519 支持。
 * 算法本身由内嵌的 Monocypher（BSD-2-Clause / CC0）提供，
 * 这里只负责 libssh2 的上下文、OpenSSH 私钥解析和签名接口。
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "libssh2_priv.h"
#include "crypto.h"

#if LIBSSH2_ED25519

#include <string.h>

#include "monocypher.h"
#include "monocypher-ed25519.h"

#define ED25519_KEY_LEN 32
#define ED25519_SECRET_LEN 64
#define ED25519_SIG_LEN 64

static int
text_contains(const char *data, size_t data_len, const char *needle)
{
    size_t needle_len = strlen(needle);
    size_t i;

    if(!data || data_len < needle_len)
        return 0;
    for(i = 0; i + needle_len <= data_len; i++) {
        if(memcmp(data + i, needle, needle_len) == 0)
            return 1;
    }
    return 0;
}

static int
file_is_openssh_key(const char *filename)
{
    FILE *fp;
    char head[128] = { 0 };
    size_t nread;

    fp = fopen(filename, "rb");
    if(!fp)
        return 0;
    nread = fread(head, 1, sizeof(head), fp);
    fclose(fp);

    return text_contains(head, nread, "-----BEGIN OPENSSH PRIVATE KEY-----");
}

void
_libssh2_wincng_ed25519_free(libssh2_ed25519_ctx *ctx)
{
    LIBSSH2_SESSION *session;

    if(!ctx)
        return;
    session = ctx->session;
    _libssh2_explicit_zero(ctx, sizeof(*ctx));
    LIBSSH2_FREE(session, ctx);
}

/*
 * 解密后的 OpenSSH 私钥内容：
 *   string  "ssh-ed25519"
 *   string  public key (32)
 *   string  private key (64 = seed || public)
 *   string  comment
 */
static libssh2_ed25519_ctx *
ctx_from_openssh_data(LIBSSH2_SESSION *session, struct string_buf *decrypted,
                      unsigned char *out_public_key)
{
    unsigned char *type = NULL, *pub = NULL, *priv = NULL, *comment = NULL;
    size_t type_len = 0, pub_len = 0, priv_len = 0, comment_len = 0;
    libssh2_ed25519_ctx *ctx;

    if(_libssh2_get_string(decrypted, &type, &type_len) ||
       !type || type_len != 11 ||
       memcmp(type, "ssh-ed25519", 11) != 0)
        return NULL;

    if(_libssh2_get_string(decrypted, &pub, &pub_len) ||
       pub_len != ED25519_KEY_LEN)
        return NULL;

    if(_libssh2_get_string(decrypted, &priv, &priv_len) ||
       priv_len != ED25519_SECRET_LEN)
        return NULL;

    /* comment 可以为空，但字段必须存在 */
    if(_libssh2_get_string(decrypted, &comment, &comment_len))
        return NULL;

    ctx = LIBSSH2_CALLOC(session, sizeof(*ctx));
    if(!ctx)
        return NULL;

    ctx->session = session;
    memcpy(ctx->secret, priv, ED25519_SECRET_LEN);
    memcpy(ctx->public_key, pub, ED25519_KEY_LEN);
    ctx->has_private = 1;

    if(out_public_key)
        memcpy(out_public_key, pub, ED25519_KEY_LEN);

    return ctx;
}

static int
build_pubkey_blob(LIBSSH2_SESSION *session, const unsigned char *public_key,
                  unsigned char **method, size_t *method_len,
                  unsigned char **pubkeydata, size_t *pubkeydata_len)
{
    static const char name[] = "ssh-ed25519";
    const size_t name_len = sizeof(name) - 1;
    const size_t blob_len = 4 + name_len + 4 + ED25519_KEY_LEN;
    unsigned char *mth = NULL, *blob = NULL, *p;

    mth = LIBSSH2_ALLOC(session, name_len);
    blob = LIBSSH2_ALLOC(session, blob_len);
    if(!mth || !blob) {
        if(mth)
            LIBSSH2_FREE(session, mth);
        if(blob)
            LIBSSH2_FREE(session, blob);
        return -1;
    }

    memcpy(mth, name, name_len);

    p = blob;
    p[0] = 0;
    p[1] = 0;
    p[2] = 0;
    p[3] = (unsigned char)name_len;
    p += 4;
    memcpy(p, name, name_len);
    p += name_len;
    p[0] = 0;
    p[1] = 0;
    p[2] = 0;
    p[3] = ED25519_KEY_LEN;
    p += 4;
    memcpy(p, public_key, ED25519_KEY_LEN);

    *method = mth;
    *method_len = name_len;
    *pubkeydata = blob;
    *pubkeydata_len = blob_len;
    return 0;
}

int
_libssh2_wincng_ed25519_pubkey_from_file(LIBSSH2_SESSION *session,
                                         unsigned char **method,
                                         size_t *method_len,
                                         unsigned char **pubkeydata,
                                         size_t *pubkeydata_len,
                                         const char *filename,
                                         const char *passphrase)
{
    FILE *fp;
    struct string_buf *decrypted = NULL;
    libssh2_ed25519_ctx *ctx;
    int rc;

    if(!file_is_openssh_key(filename))
        return -1;

    fp = fopen(filename, "rb");
    if(!fp)
        return -1;

    rc = _libssh2_openssh_pem_parse(session,
                                    (const unsigned char *)passphrase,
                                    fp, &decrypted);
    fclose(fp);
    if(rc)
        return -1;

    ctx = ctx_from_openssh_data(session, decrypted, NULL);
    _libssh2_string_buf_free(session, decrypted);
    if(!ctx)
        return -1;

    rc = build_pubkey_blob(session, ctx->public_key, method, method_len,
                           pubkeydata, pubkeydata_len);
    _libssh2_ed25519_free(ctx);
    return rc;
}

int
_libssh2_wincng_ed25519_pubkey_from_memory(LIBSSH2_SESSION *session,
                                           unsigned char **method,
                                           size_t *method_len,
                                           unsigned char **pubkeydata,
                                           size_t *pubkeydata_len,
                                           const char *filedata,
                                           size_t filedata_len,
                                           const char *passphrase)
{
    struct string_buf *decrypted = NULL;
    libssh2_ed25519_ctx *ctx;
    int rc;

    if(!filedata ||
       !text_contains(filedata, filedata_len,
                      "-----BEGIN OPENSSH PRIVATE KEY-----"))
        return -1;

    rc = _libssh2_openssh_pem_parse_memory(session,
                                           (const unsigned char *)passphrase,
                                           filedata, filedata_len, &decrypted);
    if(rc)
        return -1;

    ctx = ctx_from_openssh_data(session, decrypted, NULL);
    _libssh2_string_buf_free(session, decrypted);
    if(!ctx)
        return -1;

    rc = build_pubkey_blob(session, ctx->public_key, method, method_len,
                           pubkeydata, pubkeydata_len);
    _libssh2_ed25519_free(ctx);
    return rc;
}

int
_libssh2_ed25519_new_private(libssh2_ed25519_ctx **ed_ctx,
                             LIBSSH2_SESSION *session,
                             const char *filename, const uint8_t *passphrase)
{
    FILE *fp;
    struct string_buf *decrypted = NULL;
    libssh2_ed25519_ctx *ctx;
    int rc;

    if(_libssh2_wincng_ppk_is_file(filename) &&
       _libssh2_wincng_ed25519_new_private_from_ppk(
           ed_ctx, session, filename, (const char *)passphrase) == 0)
        return 0;

    fp = fopen(filename, "rb");
    if(!fp) {
        return _libssh2_error(session, LIBSSH2_ERROR_FILE,
                              "Unable to open Ed25519 private key file");
    }

    rc = _libssh2_openssh_pem_parse(session, passphrase, fp, &decrypted);
    fclose(fp);
    if(rc)
        return rc;

    ctx = ctx_from_openssh_data(session, decrypted, NULL);
    _libssh2_string_buf_free(session, decrypted);
    if(!ctx) {
        return _libssh2_error(session, LIBSSH2_ERROR_PROTO,
                              "Unable to parse Ed25519 private key");
    }

    if(ed_ctx)
        *ed_ctx = ctx;
    else
        _libssh2_ed25519_free(ctx);

    return 0;
}

int
_libssh2_ed25519_new_private_frommemory(libssh2_ed25519_ctx **ed_ctx,
                                        LIBSSH2_SESSION *session,
                                        const char *filedata,
                                        size_t filedata_len,
                                        const unsigned char *passphrase)
{
    struct string_buf *decrypted = NULL;
    libssh2_ed25519_ctx *ctx;
    int rc;

    rc = _libssh2_openssh_pem_parse_memory(session, passphrase, filedata,
                                           filedata_len, &decrypted);
    if(rc)
        return rc;

    ctx = ctx_from_openssh_data(session, decrypted, NULL);
    _libssh2_string_buf_free(session, decrypted);
    if(!ctx) {
        return _libssh2_error(session, LIBSSH2_ERROR_PROTO,
                              "Unable to parse Ed25519 private key");
    }

    if(ed_ctx)
        *ed_ctx = ctx;
    else
        _libssh2_ed25519_free(ctx);

    return 0;
}

int
_libssh2_wincng_ed25519_new_private_raw(libssh2_ed25519_ctx **ed_ctx,
                                        LIBSSH2_SESSION *session,
                                        const unsigned char secret[64])
{
    libssh2_ed25519_ctx *ctx;

    if(!ed_ctx || !secret)
        return -1;

    ctx = LIBSSH2_CALLOC(session, sizeof(*ctx));
    if(!ctx)
        return -1;

    ctx->session = session;
    memcpy(ctx->secret, secret, ED25519_SECRET_LEN);
    memcpy(ctx->public_key, secret + ED25519_KEY_LEN, ED25519_KEY_LEN);
    ctx->has_private = 1;
    *ed_ctx = ctx;
    return 0;
}

int
_libssh2_ed25519_new_public(libssh2_ed25519_ctx **ed_ctx,
                            LIBSSH2_SESSION *session,
                            const unsigned char *raw_pub_key,
                            const size_t key_len)
{
    libssh2_ed25519_ctx *ctx;

    if(!ed_ctx || !raw_pub_key || key_len != ED25519_KEY_LEN)
        return -1;

    ctx = LIBSSH2_CALLOC(session, sizeof(*ctx));
    if(!ctx)
        return -1;

    ctx->session = session;
    memcpy(ctx->public_key, raw_pub_key, ED25519_KEY_LEN);
    ctx->has_private = 0;
    *ed_ctx = ctx;
    return 0;
}

int
_libssh2_ed25519_sign(libssh2_ed25519_ctx *ctx, LIBSSH2_SESSION *session,
                      uint8_t **out_sig, size_t *out_sig_len,
                      const uint8_t *message, size_t message_len)
{
    unsigned char *sig;

    if(!ctx || !ctx->has_private || !out_sig || !out_sig_len)
        return -1;

    sig = LIBSSH2_CALLOC(session, ED25519_SIG_LEN);
    if(!sig)
        return -1;

    crypto_ed25519_sign(sig, ctx->secret, message, message_len);

    *out_sig = sig;
    *out_sig_len = ED25519_SIG_LEN;
    return 0;
}

int
_libssh2_ed25519_verify(libssh2_ed25519_ctx *ctx, const uint8_t *s,
                        size_t s_len, const uint8_t *m, size_t m_len)
{
    if(!ctx || !s || s_len != ED25519_SIG_LEN)
        return -1;
    return crypto_ed25519_check(s, ctx->public_key, m, m_len) == 0 ? 0 : -1;
}

int
_libssh2_curve25519_new(LIBSSH2_SESSION *session,
                        unsigned char **out_public_key,
                        unsigned char **out_private_key)
{
    unsigned char priv[ED25519_KEY_LEN];
    unsigned char pub[ED25519_KEY_LEN];
    unsigned char *priv_copy = NULL, *pub_copy = NULL;

    if(_libssh2_random(priv, sizeof(priv)))
        return -1;

    crypto_x25519_public_key(pub, priv);

    if(out_private_key) {
        priv_copy = LIBSSH2_ALLOC(session, sizeof(priv));
        if(!priv_copy)
            goto fail;
        memcpy(priv_copy, priv, sizeof(priv));
    }

    if(out_public_key) {
        pub_copy = LIBSSH2_ALLOC(session, sizeof(pub));
        if(!pub_copy)
            goto fail;
        memcpy(pub_copy, pub, sizeof(pub));
    }

    if(out_private_key)
        *out_private_key = priv_copy;
    if(out_public_key)
        *out_public_key = pub_copy;

    _libssh2_explicit_zero(priv, sizeof(priv));
    return 0;

fail:
    if(priv_copy)
        LIBSSH2_FREE(session, priv_copy);
    if(pub_copy)
        LIBSSH2_FREE(session, pub_copy);
    _libssh2_explicit_zero(priv, sizeof(priv));
    return -1;
}

int
_libssh2_curve25519_gen_k(_libssh2_bn **k,
                          uint8_t private_key[ED25519_KEY_LEN],
                          uint8_t server_public_key[ED25519_KEY_LEN])
{
    unsigned char shared[ED25519_KEY_LEN];
    int rc;

    if(!k || !*k)
        return -1;

    crypto_x25519(shared, private_key, server_public_key);
    rc = _libssh2_bn_from_bin(*k, sizeof(shared), shared);
    _libssh2_explicit_zero(shared, sizeof(shared));
    return rc;
}

/* 安全密钥（FIDO sk-ssh-ed25519）不支持；保留符号供 libssh2 编译。 */
int
_libssh2_ed25519_new_private_sk(libssh2_ed25519_ctx **ed_ctx,
                                unsigned char *flags,
                                const char **application,
                                const unsigned char **key_handle,
                                size_t *handle_len,
                                LIBSSH2_SESSION *session,
                                const char *filename,
                                const uint8_t *passphrase)
{
    (void)flags;
    (void)application;
    (void)key_handle;
    (void)handle_len;
    (void)filename;
    (void)passphrase;
    if(ed_ctx)
        *ed_ctx = NULL;
    return _libssh2_error(session, LIBSSH2_ERROR_METHOD_NOT_SUPPORTED,
                          "sk-ssh-ed25519 is not supported");
}

int
_libssh2_ed25519_new_private_frommemory_sk(libssh2_ed25519_ctx **ed_ctx,
                                           unsigned char *flags,
                                           const char **application,
                                           const unsigned char **key_handle,
                                           size_t *handle_len,
                                           LIBSSH2_SESSION *session,
                                           const char *filedata,
                                           size_t filedata_len,
                                           const unsigned char *passphrase)
{
    (void)flags;
    (void)application;
    (void)key_handle;
    (void)handle_len;
    (void)filedata;
    (void)filedata_len;
    (void)passphrase;
    if(ed_ctx)
        *ed_ctx = NULL;
    return _libssh2_error(session, LIBSSH2_ERROR_METHOD_NOT_SUPPORTED,
                          "sk-ssh-ed25519 is not supported");
}

#endif /* LIBSSH2_ED25519 */
