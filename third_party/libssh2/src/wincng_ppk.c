/* Copyright (C) 2026 rshell contributors
 *
 * WinCNG 后端的 PuTTY PPK（v2/v3）私钥支持。
 * v3 的 Argon2id 来自内嵌的 phc-winner-argon2 参考实现。
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "libssh2_priv.h"
#include "crypto.h"

#if LIBSSH2_RSA || LIBSSH2_ECDSA || LIBSSH2_ED25519

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>

#include "argon2.h"

#define PPK_MAX_FILE (4 * 1024 * 1024)

typedef struct {
    int version;
    char algorithm[48];
    char encryption[24];
    char comment[512];
    unsigned char *public_blob;
    size_t public_len;
    unsigned char *private_blob;
    size_t private_len;
    unsigned char mac[32];
    size_t mac_len;
    int argon_type; /* 0=id, 1=i, 2=d */
    uint32_t argon_mem;
    uint32_t argon_passes;
    uint32_t argon_parallelism;
    unsigned char argon_salt[64];
    size_t argon_salt_len;
} ppk_file;

static int
ppk_b64_value(char c)
{
    if(c >= 'A' && c <= 'Z') return c - 'A';
    if(c >= 'a' && c <= 'z') return c - 'a' + 26;
    if(c >= '0' && c <= '9') return c - '0' + 52;
    if(c == '+') return 62;
    if(c == '/') return 63;
    return -1;
}

static int
ppk_b64_decode(const char *text, size_t len, unsigned char **out,
               size_t *out_len)
{
    unsigned char *buf = malloc(len / 4 * 3 + 4);
    size_t produced = 0;
    int buffer = 0, bits = 0;
    size_t i;

    if(!buf)
        return -1;
    for(i = 0; i < len; i++) {
        int v;
        if(text[i] == '=' || text[i] == '\r' || text[i] == '\n' ||
           text[i] == ' ' || text[i] == '\t')
            continue;
        v = ppk_b64_value(text[i]);
        if(v < 0) {
            free(buf);
            return -1;
        }
        buffer = (buffer << 6) | v;
        bits += 6;
        if(bits >= 8) {
            bits -= 8;
            buf[produced++] = (unsigned char)((buffer >> bits) & 0xFF);
        }
    }
    *out = buf;
    *out_len = produced;
    return 0;
}

static int
ppk_hex_decode(const char *text, size_t len, unsigned char *out,
               size_t out_cap, size_t *out_len)
{
    size_t i;
    if(len % 2 != 0 || len / 2 > out_cap)
        return -1;
    for(i = 0; i < len; i += 2) {
        int hi, lo;
        char a = text[i], b = text[i + 1];
        hi = (a >= '0' && a <= '9') ? a - '0' :
             (a >= 'a' && a <= 'f') ? a - 'a' + 10 :
             (a >= 'A' && a <= 'F') ? a - 'A' + 10 : -1;
        lo = (b >= '0' && b <= '9') ? b - '0' :
             (b >= 'a' && b <= 'f') ? b - 'a' + 10 :
             (b >= 'A' && b <= 'F') ? b - 'A' + 10 : -1;
        if(hi < 0 || lo < 0)
            return -1;
        out[i / 2] = (unsigned char)((hi << 4) | lo);
    }
    *out_len = len / 2;
    return 0;
}

static const char *
ppk_next_line(const char *p, const char *end, const char **line,
              size_t *line_len)
{
    const char *nl;

    if(p >= end)
        return NULL;
    nl = memchr(p, '\n', (size_t)(end - p));
    if(!nl)
        nl = end;
    *line = p;
    *line_len = (size_t)(nl - p);
    if(*line_len > 0 && p[*line_len - 1] == '\r')
        (*line_len)--;
    return nl < end ? nl + 1 : end;
}

static int
ppk_header(const char *line, size_t line_len, const char *name,
           const char **value, size_t *value_len)
{
    size_t name_len = strlen(name);

    if(line_len < name_len + 1 || memcmp(line, name, name_len) != 0 ||
       line[name_len] != ':')
        return -1;
    *value = line + name_len + 1;
    *value_len = line_len - name_len - 1;
    if(*value_len > 0 && **value == ' ') {
        (*value)++;
        (*value_len)--;
    }
    return 0;
}

static int
ppk_collect_lines(const char **cursor, const char *end, int count,
                  unsigned char **blob, size_t *blob_len)
{
    unsigned char *acc = NULL;
    size_t acc_len = 0;
    int i;

    for(i = 0; i < count; i++) {
        const char *line;
        size_t line_len;
        unsigned char *part = NULL;
        size_t part_len = 0;
        unsigned char *grown;

        *cursor = ppk_next_line(*cursor, end, &line, &line_len);
        if(!*cursor && i + 1 < count)
            return -1;
        if(ppk_b64_decode(line, line_len, &part, &part_len)) {
            free(acc);
            return -1;
        }
        grown = realloc(acc, acc_len + part_len);
        if(!grown) {
            free(part);
            free(acc);
            return -1;
        }
        acc = grown;
        memcpy(acc + acc_len, part, part_len);
        acc_len += part_len;
        free(part);
    }
    *blob = acc;
    *blob_len = acc_len;
    return 0;
}

static int
ppk_read_file(const char *filename, char **text_out, size_t *len_out)
{
    FILE *fp;
    long size;
    char *text;
    size_t got;

    fp = fopen(filename, "rb");
    if(!fp)
        return -1;
    if(fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    size = ftell(fp);
    if(size <= 0 || size > PPK_MAX_FILE) {
        fclose(fp);
        return -1;
    }
    rewind(fp);
    text = malloc((size_t)size + 1);
    if(!text) {
        fclose(fp);
        return -1;
    }
    got = fread(text, 1, (size_t)size, fp);
    fclose(fp);
    if(got != (size_t)size) {
        free(text);
        return -1;
    }
    text[got] = '\0';
    *text_out = text;
    *len_out = got;
    return 0;
}

static void
ppk_free(ppk_file *file)
{
    free(file->public_blob);
    free(file->private_blob);
    memset(file, 0, sizeof(*file));
}

int
_libssh2_wincng_ppk_is_file(const char *filename)
{
    FILE *fp;
    char head[24] = { 0 };
    size_t got;

    fp = fopen(filename, "rb");
    if(!fp)
        return 0;
    got = fread(head, 1, sizeof(head) - 1, fp);
    fclose(fp);
    return got >= 20 && memcmp(head, "PuTTY-User-Key-File-", 20) == 0;
}

static int
ppk_parse(const char *filename, int need_private, ppk_file *file)
{
    char *text = NULL;
    size_t text_len = 0;
    const char *p, *end, *line;
    size_t line_len;
    int public_lines = -1, private_lines = -1;
    int i;

    if(ppk_read_file(filename, &text, &text_len))
        return -1;

    memset(file, 0, sizeof(*file));
    p = text;
    end = text + text_len;

    p = ppk_next_line(p, end, &line, &line_len);
    if(!p)
        goto fail;
    if(line_len > 22 && memcmp(line, "PuTTY-User-Key-File-", 20) == 0) {
        const char *colon;
        size_t alg_off;

        if(line[20] == '3')
            file->version = 3;
        else if(line[20] == '2')
            file->version = 2;
        else
            goto fail;

        colon = memchr(line, ':', line_len);
        if(!colon)
            goto fail;
        alg_off = (size_t)(colon - line) + 1;
        while(alg_off < line_len &&
              (line[alg_off] == ' ' || line[alg_off] == '\t'))
            alg_off++;
        if(line_len - alg_off == 0 ||
           line_len - alg_off >= sizeof(file->algorithm))
            goto fail;
        memcpy(file->algorithm, line + alg_off, line_len - alg_off);
        file->algorithm[line_len - alg_off] = '\0';
    }
    else {
        goto fail;
    }

    p = ppk_next_line(p, end, &line, &line_len);
    if(!p || ppk_header(line, line_len, "Encryption", &line, &line_len) ||
       line_len >= sizeof(file->encryption))
        goto fail;
    memcpy(file->encryption, line, line_len);
    file->encryption[line_len] = '\0';
    if(strcmp(file->encryption, "none") != 0 &&
       strcmp(file->encryption, "aes256-cbc") != 0)
        goto fail;

    p = ppk_next_line(p, end, &line, &line_len);
    if(!p || ppk_header(line, line_len, "Comment", &line, &line_len) ||
       line_len >= sizeof(file->comment))
        goto fail;
    memcpy(file->comment, line, line_len);
    file->comment[line_len] = '\0';

    p = ppk_next_line(p, end, &line, &line_len);
    if(!p || ppk_header(line, line_len, "Public-Lines", &line, &line_len))
        goto fail;
    public_lines = atoi(line);
    if(public_lines <= 0 || public_lines > 4096)
        goto fail;
    if(public_lines <= 0 ||
       ppk_collect_lines(&p, end, public_lines, &file->public_blob,
                         &file->public_len))
        goto fail;

    if(file->version >= 3 && strcmp(file->encryption, "none") != 0) {
        p = ppk_next_line(p, end, &line, &line_len);
        if(!p || ppk_header(line, line_len, "Key-Derivation", &line, &line_len))
            goto fail;
        if(line_len == 8 && memcmp(line, "Argon2id", 8) == 0)
            file->argon_type = 0;
        else if(line_len == 7 && memcmp(line, "Argon2i", 7) == 0)
            file->argon_type = 1;
        else if(line_len == 7 && memcmp(line, "Argon2d", 7) == 0)
            file->argon_type = 2;
        else
            goto fail;
        p = ppk_next_line(p, end, &line, &line_len);
        if(!p || ppk_header(line, line_len, "Argon2-Memory", &line, &line_len))
            goto fail;
        file->argon_mem = (uint32_t)strtoul(line, NULL, 10);
        p = ppk_next_line(p, end, &line, &line_len);
        if(!p || ppk_header(line, line_len, "Argon2-Passes", &line, &line_len))
            goto fail;
        file->argon_passes = (uint32_t)strtoul(line, NULL, 10);
        p = ppk_next_line(p, end, &line, &line_len);
        if(!p ||
           ppk_header(line, line_len, "Argon2-Parallelism", &line, &line_len))
            goto fail;
        file->argon_parallelism = (uint32_t)strtoul(line, NULL, 10);
        p = ppk_next_line(p, end, &line, &line_len);
        if(!p || ppk_header(line, line_len, "Argon2-Salt", &line, &line_len) ||
           ppk_hex_decode(line, line_len, file->argon_salt,
                          sizeof(file->argon_salt), &file->argon_salt_len))
            goto fail;
    }
    (void)i;

    p = ppk_next_line(p, end, &line, &line_len);
    if(!p || ppk_header(line, line_len, "Private-Lines", &line, &line_len))
        goto fail;
    private_lines = atoi(line);
    if(private_lines <= 0 || private_lines > 4096)
        goto fail;
    if(private_lines <= 0 ||
       ppk_collect_lines(&p, end, private_lines, &file->private_blob,
                         &file->private_len))
        goto fail;

    p = ppk_next_line(p, end, &line, &line_len);
    if(!p || ppk_header(line, line_len, "Private-MAC", &line, &line_len) ||
       ppk_hex_decode(line, line_len, file->mac, sizeof(file->mac),
                      &file->mac_len))
        goto fail;

    free(text);
    return need_private ? 1 : 0;

fail:
    free(text);
    ppk_free(file);
    return -1;
}

static int
ppk_aes256_cbc_decrypt(unsigned char *data, size_t len,
                       const unsigned char key[32],
                       const unsigned char iv_in[16])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key_handle = NULL;
    unsigned char iv[16];
    unsigned char *out = NULL;
    ULONG out_len = 0;
    NTSTATUS status;
    int rc = -1;

    if(len == 0 || len % 16 != 0)
        return -1;
    memcpy(iv, iv_in, sizeof(iv));

    status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0);
    if(!BCRYPT_SUCCESS(status))
        goto out;
    status = BCryptSetProperty(
        alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
        (ULONG)((wcslen(BCRYPT_CHAIN_MODE_CBC) + 1) * sizeof(wchar_t)), 0);
    if(!BCRYPT_SUCCESS(status))
        goto out;
    status = BCryptGenerateSymmetricKey(alg, &key_handle, NULL, 0,
                                        (PUCHAR)key, 32, 0);
    if(!BCRYPT_SUCCESS(status))
        goto out;

    out = malloc(len);
    if(!out)
        goto out;
    status = BCryptDecrypt(key_handle, data, (ULONG)len, NULL, iv,
                           (ULONG)sizeof(iv), out, (ULONG)len, &out_len, 0);
    if(!BCRYPT_SUCCESS(status))
        goto out;
    memcpy(data, out, len);
    rc = 0;

out:
    if(out)
        free(out);
    if(key_handle)
        BCryptDestroyKey(key_handle);
    if(alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    return rc;
}

static int
ppk_sha1_parts(const void *a, size_t a_len, const void *b, size_t b_len,
               unsigned char out[20])
{
    unsigned char *tmp = malloc(a_len + b_len);
    int rc;

    if(!tmp)
        return -1;
    memcpy(tmp, a, a_len);
    memcpy(tmp + a_len, b, b_len);
    rc = libssh2_sha1(tmp, a_len + b_len, out) == 0 ? 0 : -1;
    free(tmp);
    return rc;
}

static int
ppk_hmac(int use_sha256, const unsigned char *key, size_t key_len,
         const unsigned char *data, size_t data_len, unsigned char *out)
{
    libssh2_hmac_ctx ctx;
    int rc;

    if(use_sha256) {
        rc = _libssh2_hmac_sha256_init(&ctx, (void *)key, key_len);
        if(!rc)
            return -1;
    }
    else {
        rc = _libssh2_hmac_sha1_init(&ctx, (void *)key, key_len);
        if(!rc)
            return -1;
    }
    _libssh2_hmac_update(&ctx, data, data_len);
    _libssh2_hmac_final(&ctx, out);
    _libssh2_hmac_cleanup(&ctx);
    return 0;
}

static int
ppk_derive_and_verify(ppk_file *file, const char *passphrase)
{
    const unsigned char *pass = (const unsigned char *)(passphrase ? passphrase : "");
    const size_t pass_len = passphrase ? strlen(passphrase) : 0;
    unsigned char cipher_key[32];
    unsigned char cipher_iv[16];
    unsigned char mac_key[32];
    size_t mac_key_len = 0;
    int use_sha256 = file->version >= 3;
    int encrypted = strcmp(file->encryption, "aes256-cbc") == 0;
    unsigned char *mac_data;
    unsigned char mac[32];
    size_t mac_data_len, name_len;
    unsigned char *p;

    memset(cipher_key, 0, sizeof(cipher_key));
    memset(cipher_iv, 0, sizeof(cipher_iv));
    memset(mac_key, 0, sizeof(mac_key));

    if(file->version == 2) {
        unsigned char *input;
        unsigned char digest[20];
        size_t have = 0;
        uint32_t ctr = 0;

        input = malloc(4 + pass_len);
        if(!input)
            return -1;
        while(have < sizeof(cipher_key)) {
            size_t take = sizeof(cipher_key) - have;
            input[0] = (unsigned char)(ctr >> 24);
            input[1] = (unsigned char)(ctr >> 16);
            input[2] = (unsigned char)(ctr >> 8);
            input[3] = (unsigned char)ctr;
            memcpy(input + 4, pass, pass_len);
            if(libssh2_sha1(input, 4 + pass_len, digest) != 0) {
                free(input);
                return -1;
            }
            if(take > sizeof(digest))
                take = sizeof(digest);
            memcpy(cipher_key + have, digest, take);
            have += take;
            ctr++;
        }
        free(input);

        if(ppk_sha1_parts("putty-private-key-file-mac-key",
                          strlen("putty-private-key-file-mac-key"),
                          pass, pass_len, mac_key) == 0)
            mac_key_len = 20;
        else
            return -1;
    }
    else if(encrypted) {
        unsigned char tag[80];
        int argon_rc;
        if(file->argon_type == 1)
            argon_rc = argon2i_hash_raw(file->argon_passes, file->argon_mem,
                                        file->argon_parallelism, pass, pass_len,
                                        file->argon_salt, file->argon_salt_len,
                                        tag, sizeof(tag));
        else if(file->argon_type == 2)
            argon_rc = argon2d_hash_raw(file->argon_passes, file->argon_mem,
                                        file->argon_parallelism, pass, pass_len,
                                        file->argon_salt, file->argon_salt_len,
                                        tag, sizeof(tag));
        else
            argon_rc = argon2id_hash_raw(file->argon_passes, file->argon_mem,
                                         file->argon_parallelism, pass, pass_len,
                                         file->argon_salt, file->argon_salt_len,
                                         tag, sizeof(tag));
        if(argon_rc != 0)
            return -1;
        memcpy(cipher_key, tag, 32);
        memcpy(cipher_iv, tag + 32, 16);
        memcpy(mac_key, tag + 48, 32);
        mac_key_len = 32;
    }
    /* v3 未加密：MAC key 为空 */

    if(encrypted) {
        if(ppk_aes256_cbc_decrypt(file->private_blob, file->private_len,
                                  cipher_key, cipher_iv)) {
            return -1;
        }
    }

    name_len = strlen(file->algorithm);
    mac_data_len = 4 + name_len +
                   4 + strlen(file->encryption) +
                   4 + strlen(file->comment) +
                   4 + file->public_len +
                   4 + file->private_len;
    mac_data = malloc(mac_data_len);
    if(!mac_data)
        return -1;
    p = mac_data;
    _libssh2_store_str(&p, file->algorithm, name_len);
    _libssh2_store_str(&p, file->encryption, strlen(file->encryption));
    _libssh2_store_str(&p, file->comment, strlen(file->comment));
    _libssh2_store_str(&p, (const char *)file->public_blob, file->public_len);
    _libssh2_store_str(&p, (const char *)file->private_blob, file->private_len);

    if(ppk_hmac(use_sha256, mac_key, mac_key_len, mac_data, mac_data_len,
                mac)) {
        free(mac_data);
        return -1;
    }
    free(mac_data);

    {
        size_t expected = use_sha256 ? 32 : 20;
        if(file->mac_len != expected)
            return -1;
        if(memcmp(file->mac, mac, expected) != 0)
            return -1;
    }
    return 0;
}

int
_libssh2_wincng_ppk_pubkey(LIBSSH2_SESSION *session,
                           unsigned char **method, size_t *method_len,
                           unsigned char **pubkeydata, size_t *pubkeydata_len,
                           const char *filename, const char *passphrase)
{
    ppk_file file;
    unsigned char *copy;
    size_t alg_len;
    int parsed;

    (void)passphrase;
    parsed = ppk_parse(filename, 0, &file);
    if(parsed < 0)
        return -1;

    alg_len = strlen(file.algorithm);
    copy = LIBSSH2_ALLOC(session, file.public_len);
    if(!copy) {
        ppk_free(&file);
        return -1;
    }
    memcpy(copy, file.public_blob, file.public_len);

    *method = LIBSSH2_ALLOC(session, alg_len);
    if(!*method) {
        LIBSSH2_FREE(session, copy);
        ppk_free(&file);
        return -1;
    }
    memcpy(*method, file.algorithm, alg_len);
    *method_len = alg_len;
    *pubkeydata = copy;
    *pubkeydata_len = file.public_len;

    ppk_free(&file);
    return 0;
}

#if LIBSSH2_RSA
int
_libssh2_wincng_rsa_new_from_ppk(libssh2_rsa_ctx **rsa,
                                 LIBSSH2_SESSION *session,
                                 const char *filename,
                                 const char *passphrase)
{
    ppk_file file;
    struct string_buf pub, priv;
    unsigned char *alg, *e, *n, *d, *p, *q, *iqmp;
    size_t alg_len, e_len, n_len, d_len, p_len, q_len, iqmp_len;
    int rc;

    if(ppk_parse(filename, 1, &file) < 0)
        return -1;
    if(strcmp(file.algorithm, "ssh-rsa") != 0 ||
       ppk_derive_and_verify(&file, passphrase)) {
        ppk_free(&file);
        return -1;
    }

    pub.data = file.public_blob;
    pub.dataptr = file.public_blob;
    pub.len = file.public_len;
    priv.data = file.private_blob;
    priv.dataptr = file.private_blob;
    priv.len = file.private_len;

    if(_libssh2_get_string(&pub, &alg, &alg_len) ||
       _libssh2_get_string(&pub, &e, &e_len) ||
       _libssh2_get_string(&pub, &n, &n_len) ||
       _libssh2_get_string(&priv, &d, &d_len) ||
       _libssh2_get_string(&priv, &p, &p_len) ||
       _libssh2_get_string(&priv, &q, &q_len) ||
       _libssh2_get_string(&priv, &iqmp, &iqmp_len)) {
        ppk_free(&file);
        return -1;
    }

    rc = _libssh2_wincng_rsa_new_from_parts(rsa, e, e_len, n, n_len,
                                            d, d_len, p, p_len, q, q_len,
                                            iqmp, iqmp_len);
    ppk_free(&file);
    return rc;
}
#endif /* LIBSSH2_RSA */

#if LIBSSH2_ECDSA
int
_libssh2_wincng_ecdsa_new_private_from_ppk(
    struct _libssh2_wincng_ecdsa_key **key, LIBSSH2_SESSION *session,
    const char *filename, const char *passphrase)
{
    ppk_file file;
    struct string_buf pub, priv;
    unsigned char *alg, *curve, *point, *d;
    size_t alg_len, curve_len, point_len, d_len;
    char curve_name[16];
    int rc;

    if(ppk_parse(filename, 1, &file) < 0)
        return -1;
    if(strncmp(file.algorithm, "ecdsa-sha2-", 11) != 0 ||
       ppk_derive_and_verify(&file, passphrase)) {
        ppk_free(&file);
        return -1;
    }

    pub.data = file.public_blob;
    pub.dataptr = file.public_blob;
    pub.len = file.public_len;
    priv.data = file.private_blob;
    priv.dataptr = file.private_blob;
    priv.len = file.private_len;

    if(_libssh2_get_string(&pub, &alg, &alg_len) ||
       _libssh2_get_string(&pub, &curve, &curve_len) ||
       _libssh2_get_string(&pub, &point, &point_len) ||
       _libssh2_get_string(&priv, &d, &d_len) ||
       curve_len == 0 || curve_len >= sizeof(curve_name)) {
        ppk_free(&file);
        return -1;
    }
    memcpy(curve_name, curve, curve_len);
    curve_name[curve_len] = '\0';

    rc = _libssh2_wincng_ecdsa_new_from_raw(key, session,
                                            file.algorithm, curve_name,
                                            point, point_len, d, d_len);
    ppk_free(&file);
    return rc;
}
#endif /* LIBSSH2_ECDSA */

#if LIBSSH2_ED25519
int
_libssh2_wincng_ed25519_new_private_from_ppk(
    libssh2_ed25519_ctx **ctx, LIBSSH2_SESSION *session,
    const char *filename, const char *passphrase)
{
    ppk_file file;
    struct string_buf pub, priv;
    unsigned char *alg, *pub_key, *seed;
    size_t alg_len, pub_key_len, seed_len;
    unsigned char secret[64];
    int rc;

    if(ppk_parse(filename, 1, &file) < 0)
        return -1;
    if(strcmp(file.algorithm, "ssh-ed25519") != 0 ||
       ppk_derive_and_verify(&file, passphrase)) {
        ppk_free(&file);
        return -1;
    }

    pub.data = file.public_blob;
    pub.dataptr = file.public_blob;
    pub.len = file.public_len;
    priv.data = file.private_blob;
    priv.dataptr = file.private_blob;
    priv.len = file.private_len;

    if(_libssh2_get_string(&pub, &alg, &alg_len) ||
       _libssh2_get_string(&pub, &pub_key, &pub_key_len) ||
       pub_key_len != 32 ||
       _libssh2_get_string(&priv, &seed, &seed_len) ||
       seed_len != 32) {
        ppk_free(&file);
        return -1;
    }

    memcpy(secret, seed, 32);
    memcpy(secret + 32, pub_key, 32);
    rc = _libssh2_wincng_ed25519_new_private_raw(ctx, session, secret);
    ppk_free(&file);
    return rc;
}
#endif /* LIBSSH2_ED25519 */

#endif /* LIBSSH2_RSA || LIBSSH2_ECDSA || LIBSSH2_ED25519 */









