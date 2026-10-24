/* Copyright (C) 2026 rshell contributors
 *
 * WinCNG 后端的额外私钥格式支持：
 *   - OpenSSH 新格式 RSA 私钥（需要补算 CRT 指数）
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "libssh2_priv.h"
#include "crypto.h"


#if LIBSSH2_RSA

#include <string.h>

#define RSA_BN_MAX 512 /* 4096 bit */

static size_t bn_trim(const unsigned char *a, size_t n)
{
    size_t i = 0;
    while(i < n && a[i] == 0)
        i++;
    return n - i;
}

static int bn_cmp(const unsigned char *a, size_t an,
                  const unsigned char *b, size_t bn)
{
    size_t at = bn_trim(a, an), bt = bn_trim(b, bn);
    if(at != bt)
        return at < bt ? -1 : 1;
    return memcmp(a + (an - at), b + (bn - bt), at);
}

static void bn_sub(unsigned char *a, size_t an,
                   const unsigned char *b, size_t bn)
{
    int borrow = 0;
    size_t i = an, j = bn;
    while(i > 0) {
        int av = a[i - 1];
        int bv = (j > 0) ? b[j - 1] : 0;
        int v = av - bv - borrow;
        if(v < 0) {
            v += 256;
            borrow = 1;
        }
        else {
            borrow = 0;
        }
        a[i - 1] = (unsigned char)v;
        i--;
        if(j > 0)
            j--;
    }
}

/*
 * dmp1 = d mod (p-1)，用于 OpenSSH RSA 私钥补算 CNG 需要的 CRT 参数。
 * 用二进制长除法，输入都是大端无符号整数。
 */
static int rsa_mod_reduce(const unsigned char *d, size_t dlen,
                          const unsigned char *modulus, size_t mlen,
                          unsigned char *out, size_t outlen)
{
    unsigned char m[RSA_BN_MAX];
    unsigned char rem[RSA_BN_MAX + 1];
    size_t i;
    int bit;

    if(mlen == 0 || mlen > RSA_BN_MAX || outlen < mlen)
        return -1;

    memcpy(m, modulus, mlen);
    /* m = p - 1 */
    for(i = mlen; i > 0; i--) {
        if(m[i - 1] != 0) {
            m[i - 1]--;
            break;
        }
        m[i - 1] = 0xFF;
    }
    if(bn_trim(m, mlen) == 0)
        return -1;

    memset(rem, 0, sizeof(rem));

    for(i = 0; i < dlen; i++) {
        for(bit = 7; bit >= 0; bit--) {
            unsigned carry = (unsigned)((d[i] >> bit) & 1);
            size_t j;
            for(j = mlen; j > 0; j--) {
                unsigned v = ((unsigned)rem[j] << 1) | carry;
                rem[j] = (unsigned char)(v & 0xFF);
                carry = v >> 8;
            }
            rem[0] = (unsigned char)((rem[0] << 1 | carry) & 0xFF);

            if(bn_cmp(rem + 1, mlen, m, mlen) >= 0)
                bn_sub(rem + 1, mlen, m, mlen);
        }
    }

    /* 右对齐输出 */
    memset(out, 0, outlen);
    memcpy(out + outlen - mlen, rem + 1, mlen);
    return 0;
}

static int build_rsa_public_blob(LIBSSH2_SESSION *session,
                                 const unsigned char *e, size_t elen,
                                 const unsigned char *n, size_t nlen,
                                 unsigned char **method, size_t *method_len,
                                 unsigned char **pubkeydata,
                                 size_t *pubkeydata_len)
{
    static const char name[] = "ssh-rsa";
    const size_t name_len = sizeof(name) - 1;
    size_t blob_len = 4 + name_len + 4 + elen + 4 + nlen;
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
    p[2] = (unsigned char)(elen >> 8);
    p[3] = (unsigned char)elen;
    p += 4;
    memcpy(p, e, elen);
    p += elen;

    p[0] = 0;
    p[1] = 0;
    p[2] = (unsigned char)(nlen >> 8);
    p[3] = (unsigned char)nlen;
    p += 4;
    memcpy(p, n, nlen);

    *method = mth;
    *method_len = name_len;
    *pubkeydata = blob;
    *pubkeydata_len = blob_len;
    return 0;
}

typedef struct {
    unsigned char *type;
    size_t type_len;
    unsigned char *e;
    size_t e_len;
    unsigned char *n;
    size_t n_len;
    unsigned char *d;
    size_t d_len;
    unsigned char *iqmp;
    size_t iqmp_len;
    unsigned char *p;
    size_t p_len;
    unsigned char *q;
    size_t q_len;
} openssh_rsa_parts;

static int parse_openssh_rsa(LIBSSH2_SESSION *session,
                             struct string_buf *decrypted,
                             openssh_rsa_parts *parts)
{
    unsigned char *comment = NULL;
    size_t comment_len = 0;

    memset(parts, 0, sizeof(*parts));
    if(_libssh2_get_string(decrypted, &parts->type, &parts->type_len) ||
       parts->type_len != 7 || memcmp(parts->type, "ssh-rsa", 7) != 0)
        return -1;
    if(_libssh2_get_string(decrypted, &parts->n, &parts->n_len) ||
       _libssh2_get_string(decrypted, &parts->e, &parts->e_len) ||
       _libssh2_get_string(decrypted, &parts->d, &parts->d_len) ||
       _libssh2_get_string(decrypted, &parts->iqmp, &parts->iqmp_len) ||
       _libssh2_get_string(decrypted, &parts->p, &parts->p_len) ||
       _libssh2_get_string(decrypted, &parts->q, &parts->q_len))
        return -1;
    if(_libssh2_get_string(decrypted, &comment, &comment_len))
        return -1;
    if(parts->p_len == 0 || parts->p_len > RSA_BN_MAX)
        return -1;
    return 0;
}

int
_libssh2_wincng_rsa_new_from_parts(libssh2_rsa_ctx **rsa,
                                   const unsigned char *e, size_t e_len,
                                   const unsigned char *n, size_t n_len,
                                   const unsigned char *d, size_t d_len,
                                   const unsigned char *p, size_t p_len,
                                   const unsigned char *q, size_t q_len,
                                   const unsigned char *iqmp,
                                   size_t iqmp_len)
{
    unsigned char dmp1[RSA_BN_MAX];
    unsigned char dmq1[RSA_BN_MAX];
    int rc;

    if(p_len == 0 || p_len > RSA_BN_MAX || q_len == 0 ||
       q_len > RSA_BN_MAX)
        return -1;

    rc = rsa_mod_reduce(d, d_len, p, p_len, dmp1, p_len);
    if(rc == 0)
        rc = rsa_mod_reduce(d, d_len, q, q_len, dmq1, q_len);
    if(rc)
        return rc;

    return _libssh2_wincng_rsa_new(rsa, e, (unsigned long)e_len,
                                   n, (unsigned long)n_len,
                                   d, (unsigned long)d_len,
                                   p, (unsigned long)p_len,
                                   q, (unsigned long)q_len,
                                   dmp1, (unsigned long)p_len,
                                   dmq1, (unsigned long)q_len,
                                   iqmp, (unsigned long)iqmp_len);
}

int
_libssh2_wincng_rsa_new_from_openssh(libssh2_rsa_ctx **rsa,
                                     LIBSSH2_SESSION *session,
                                     const char *filename,
                                     const unsigned char *passphrase)
{
    FILE *fp;
    struct string_buf *decrypted = NULL;
    openssh_rsa_parts parts;
    int rc;

    fp = fopen(filename, "rb");
    if(!fp)
        return -1;
    rc = _libssh2_openssh_pem_parse(session, passphrase, fp, &decrypted);
    fclose(fp);
    if(rc)
        return -1;

    rc = parse_openssh_rsa(session, decrypted, &parts);
    if(rc == 0) {
        rc = _libssh2_wincng_rsa_new_from_parts(rsa, parts.e, parts.e_len,
                                                parts.n, parts.n_len,
                                                parts.d, parts.d_len,
                                                parts.p, parts.p_len,
                                                parts.q, parts.q_len,
                                                parts.iqmp, parts.iqmp_len);
    }

    _libssh2_string_buf_free(session, decrypted);
    return rc;
}

int
_libssh2_wincng_openssh_rsa_pubkey(LIBSSH2_SESSION *session,
                                   unsigned char **method,
                                   size_t *method_len,
                                   unsigned char **pubkeydata,
                                   size_t *pubkeydata_len,
                                   const char *filename,
                                   const char *passphrase)
{
    FILE *fp;
    struct string_buf *decrypted = NULL;
    openssh_rsa_parts parts;
    int rc;

    fp = fopen(filename, "rb");
    if(!fp)
        return -1;
    rc = _libssh2_openssh_pem_parse(session,
                                    (const unsigned char *)passphrase,
                                    fp, &decrypted);
    fclose(fp);
    if(rc)
        return -1;

    rc = parse_openssh_rsa(session, decrypted, &parts);
    if(rc == 0)
        rc = build_rsa_public_blob(session, parts.e, parts.e_len,
                                   parts.n, parts.n_len,
                                   method, method_len,
                                   pubkeydata, pubkeydata_len);

    _libssh2_string_buf_free(session, decrypted);
    return rc;
}

#endif /* LIBSSH2_RSA */

#if LIBSSH2_ECDSA


struct _libssh2_wincng_ecdsa_key;

int
_libssh2_wincng_parse_ecdsa_privatekey(struct _libssh2_wincng_ecdsa_key **key,
                                       unsigned char *privatekey,
                                       size_t privatekey_len);

typedef struct {
    unsigned char d[66];
    size_t d_len;
    unsigned char point[133];
    size_t point_len;
    const char *keytype;
    const char *curve;
} sec1_ecdsa_key;

static int
der_next(const unsigned char **cursor, const unsigned char *end,
         unsigned char *tag, const unsigned char **value, size_t *value_len)
{
    const unsigned char *p = *cursor;
    size_t len;
    unsigned char len_byte;

    if(p + 2 > end)
        return -1;
    *tag = *p++;
    len_byte = *p++;
    if(len_byte & 0x80) {
        int count = len_byte & 0x7F;
        if(count == 0 || count > 3 || p + count > end)
            return -1;
        len = 0;
        while(count-- > 0)
            len = (len << 8) | *p++;
    }
    else {
        len = len_byte;
    }
    if(p + len > end)
        return -1;
    *value = p;
    *value_len = len;
    *cursor = p + len;
    return 0;
}

static int
sec1_select_curve(const unsigned char *oid, size_t oid_len, sec1_ecdsa_key *key)
{
    static const unsigned char oid_p256[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07 };
    static const unsigned char oid_p384[] = { 0x2B, 0x81, 0x04, 0x00, 0x22 };
    static const unsigned char oid_p521[] = { 0x2B, 0x81, 0x04, 0x00, 0x23 };

    if(oid_len == sizeof(oid_p256) && memcmp(oid, oid_p256, oid_len) == 0) {
        key->keytype = "ecdsa-sha2-nistp256";
        key->curve = "nistp256";
        return 0;
    }
    if(oid_len == sizeof(oid_p384) && memcmp(oid, oid_p384, oid_len) == 0) {
        key->keytype = "ecdsa-sha2-nistp384";
        key->curve = "nistp384";
        return 0;
    }
    if(oid_len == sizeof(oid_p521) && memcmp(oid, oid_p521, oid_len) == 0) {
        key->keytype = "ecdsa-sha2-nistp521";
        key->curve = "nistp521";
        return 0;
    }
    return -1;
}

static int
parse_sec1(const unsigned char *der, size_t der_len, sec1_ecdsa_key *key)
{
    const unsigned char *p = der;
    const unsigned char *end = der + der_len;
    const unsigned char *seq, *value;
    unsigned char tag;
    size_t seq_len, value_len;
    const unsigned char *q;

    memset(key, 0, sizeof(*key));
    if(der_next(&p, end, &tag, &seq, &seq_len) || tag != 0x30)
        return -1;

    q = seq;
    end = seq + seq_len;

    /* version INTEGER */
    if(der_next(&q, end, &tag, &value, &value_len) || tag != 0x02)
        return -1;

    /* privateKey OCTET STRING */
    if(der_next(&q, end, &tag, &value, &value_len) || tag != 0x04 ||
       value_len == 0 || value_len > sizeof(key->d))
        return -1;
    memcpy(key->d, value, value_len);
    key->d_len = value_len;

    while(q < end) {
        if(der_next(&q, end, &tag, &value, &value_len))
            return -1;
        if(tag == 0xA0) {
            const unsigned char *op = value;
            const unsigned char *oend = value + value_len;
            const unsigned char *oid;
            unsigned char otag;
            size_t oid_len;
            if(der_next(&op, oend, &otag, &oid, &oid_len) || otag != 0x06)
                return -1;
            if(sec1_select_curve(oid, oid_len, key))
                return -1;
        }
        else if(tag == 0xA1) {
            const unsigned char *op = value;
            const unsigned char *oend = value + value_len;
            const unsigned char *bits;
            unsigned char otag;
            size_t bits_len;
            if(der_next(&op, oend, &otag, &bits, &bits_len) || otag != 0x03 ||
               bits_len < 2 || bits[0] != 0 ||
               bits_len - 1 > sizeof(key->point))
                return -1;
            memcpy(key->point, bits + 1, bits_len - 1);
            key->point_len = bits_len - 1;
        }
    }

    if(key->d_len == 0 || key->point_len == 0 || !key->keytype)
        return -1;

    if(key->point_len != 65 && key->point_len != 97 && key->point_len != 133)
        return -1;
    if(key->point[0] != 0x04)
        return -1;

    return 0;
}

static int
load_sec1_file(LIBSSH2_SESSION *session, const char *filename,
               const unsigned char *passphrase, sec1_ecdsa_key *key)
{
    FILE *fp;
    unsigned char *der = NULL;
    size_t der_len = 0;
    int rc;

    fp = fopen(filename, "rb");
    if(!fp)
        return -1;
    rc = _libssh2_pem_parse(session, "-----BEGIN EC PRIVATE KEY-----",
                            "-----END EC PRIVATE KEY-----", passphrase, fp,
                            &der, &der_len);
    fclose(fp);
    if(rc != LIBSSH2_ERROR_NONE || !der) {
        if(der)
            LIBSSH2_FREE(session, der);
        return -1;
    }

    rc = parse_sec1(der, der_len, key);
    LIBSSH2_FREE(session, der);
    return rc;
}

static int
build_ecdsa_public_blob(LIBSSH2_SESSION *session, const sec1_ecdsa_key *key,
                        unsigned char **method, size_t *method_len,
                        unsigned char **pubkeydata, size_t *pubkeydata_len)
{
    const size_t name_len = strlen(key->keytype);
    const size_t curve_len = strlen(key->curve);
    const size_t blob_len = 4 + name_len + 4 + curve_len + 4 + key->point_len;
    unsigned char *mth = LIBSSH2_ALLOC(session, name_len);
    unsigned char *blob = LIBSSH2_ALLOC(session, blob_len);
    unsigned char *p;

    if(!mth || !blob) {
        if(mth)
            LIBSSH2_FREE(session, mth);
        if(blob)
            LIBSSH2_FREE(session, blob);
        return -1;
    }

    memcpy(mth, key->keytype, name_len);
    p = blob;
    _libssh2_store_str(&p, key->keytype, name_len);
    _libssh2_store_str(&p, key->curve, curve_len);
    _libssh2_store_str(&p, (const char *)key->point, key->point_len);

    *method = mth;
    *method_len = name_len;
    *pubkeydata = blob;
    *pubkeydata_len = blob_len;
    return 0;
}

int
_libssh2_wincng_ecdsa_sec1_pubkey(LIBSSH2_SESSION *session,
                                  unsigned char **method,
                                  size_t *method_len,
                                  unsigned char **pubkeydata,
                                  size_t *pubkeydata_len,
                                  const char *filename,
                                  const char *passphrase)
{
    sec1_ecdsa_key key;

    if(load_sec1_file(session, filename, (const unsigned char *)passphrase,
                      &key))
        return -1;
    return build_ecdsa_public_blob(session, &key, method, method_len,
                                   pubkeydata, pubkeydata_len);
}

int
_libssh2_wincng_ecdsa_new_from_raw(
    struct _libssh2_wincng_ecdsa_key **key, LIBSSH2_SESSION *session,
    const char *keytype, const char *curve,
    const unsigned char *point, size_t point_len,
    const unsigned char *d, size_t d_len)
{
    unsigned char *inner;
    unsigned char *p;
    size_t cap, inner_len;
    int rc;

    cap = 8 + (4 + strlen(keytype)) + (4 + strlen(curve)) +
          (4 + point_len) + (4 + d_len + 1) + 4;
    inner = LIBSSH2_ALLOC(session, cap);
    if(!inner)
        return -1;

    p = inner;
    _libssh2_store_u32(&p, 1);
    _libssh2_store_u32(&p, 1);
    _libssh2_store_str(&p, keytype, strlen(keytype));
    _libssh2_store_str(&p, curve, strlen(curve));
    _libssh2_store_str(&p, (const char *)point, point_len);
    if(!_libssh2_store_bignum2_bytes(&p, d, d_len)) {
        LIBSSH2_FREE(session, inner);
        return -1;
    }
    _libssh2_store_str(&p, "", 0);
    inner_len = (size_t)(p - inner);

    rc = _libssh2_wincng_parse_ecdsa_privatekey(key, inner, inner_len);
    LIBSSH2_FREE(session, inner);
    return rc;
}

int
_libssh2_wincng_ecdsa_new_private_sec1(
    struct _libssh2_wincng_ecdsa_key **key, LIBSSH2_SESSION *session,
    const char *filename, const unsigned char *passphrase)
{
    sec1_ecdsa_key parts;

    if(load_sec1_file(session, filename, passphrase, &parts))
        return -1;

    return _libssh2_wincng_ecdsa_new_from_raw(key, session, parts.keytype,
                                              parts.curve, parts.point,
                                              parts.point_len, parts.d,
                                              parts.d_len);
}

#endif /* LIBSSH2_ECDSA */








