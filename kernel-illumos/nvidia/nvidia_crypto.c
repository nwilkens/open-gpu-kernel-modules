/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * AES-GCM, randomness, base64, ASN.1 and the unsupported public-key and
 * X.509 entry points of the libspdm crypto backend.
 */

#include "nvidia_crypto.h"

/* AES-GCM */

static bool
nv_gcm_encrypt(crypto_mech_type_t mech_type,
               const uint8_t *key, size_t key_size, const uint8_t *iv,
               const uint8_t *a_data, size_t a_data_size,
               const uint8_t *in, size_t len, uint8_t *out,
               uint8_t *tag, size_t tag_size)
{
    uint8_t empty = 0;
    CK_AES_GCM_PARAMS params;
    crypto_mechanism_t mech;
    crypto_key_t ckey;
    crypto_data_t plain, cipher;
    iovec_t iov[2];
    uio_t uio;
    uint_t iovcnt = 0;

    bzero(&params, sizeof (params));
    params.pIv = (uchar_t *)(uintptr_t)iv;
    params.ulIvLen = NV_GCM_IV_SIZE;
    params.ulIvBits = CRYPTO_BYTES2BITS(NV_GCM_IV_SIZE);
    params.pAAD = (uchar_t *)(uintptr_t)(a_data_size != 0 ? a_data : &empty);
    params.ulAADLen = a_data_size;
    params.ulTagBits = CRYPTO_BYTES2BITS(tag_size);

    bzero(&mech, sizeof (mech));
    mech.cm_type = mech_type;
    mech.cm_param = (caddr_t)&params;
    mech.cm_param_len = sizeof (params);

    bzero(&ckey, sizeof (ckey));
    ckey.ck_format = CRYPTO_KEY_RAW;
    ckey.ck_data = (void *)(uintptr_t)key;
    ckey.ck_length = CRYPTO_BYTES2BITS(key_size);

    bzero(&plain, sizeof (plain));
    plain.cd_format = CRYPTO_DATA_RAW;
    plain.cd_length = len;
    plain.cd_raw.iov_base = (caddr_t)(uintptr_t)(len != 0 ? in : &empty);
    plain.cd_raw.iov_len = len;

    if (len != 0)
    {
        iov[iovcnt].iov_base = (caddr_t)out;
        iov[iovcnt].iov_len = len;
        iovcnt++;
    }
    iov[iovcnt].iov_base = (caddr_t)tag;
    iov[iovcnt].iov_len = tag_size;
    iovcnt++;

    bzero(&uio, sizeof (uio));
    uio.uio_iov = iov;
    uio.uio_iovcnt = iovcnt;
    uio.uio_segflg = UIO_SYSSPACE;
    uio.uio_resid = len + tag_size;

    bzero(&cipher, sizeof (cipher));
    cipher.cd_format = CRYPTO_DATA_UIO;
    cipher.cd_length = len + tag_size;
    cipher.cd_uio = &uio;

    return crypto_encrypt(&mech, &plain, &ckey, NULL, &cipher, NULL) ==
        CRYPTO_SUCCESS;
}

static bool
nv_gcm_check_args(const uint8_t *key, size_t key_size,
                  const uint8_t *iv, size_t iv_size,
                  const uint8_t *a_data, size_t a_data_size,
                  const uint8_t *data_in, size_t data_in_size,
                  const uint8_t *tag, size_t tag_size,
                  const uint8_t *data_out, const size_t *data_out_size)
{
    if (key == NULL || iv == NULL || tag == NULL)
        return false;
    if (key_size != 16 && key_size != 24 && key_size != 32)
        return false;
    if (iv_size != NV_GCM_IV_SIZE)
        return false;
    if (tag_size < 12 || tag_size > NV_GCM_MAX_TAG_SIZE)
        return false;
    if (a_data_size > INT_MAX || data_in_size > INT_MAX)
        return false;
    if (a_data == NULL && a_data_size != 0)
        return false;
    if (data_in_size != 0 && (data_in == NULL || data_out == NULL))
        return false;
    /* libspdm passes no output at all for MAC-only records. */
    if (data_out_size == NULL ? data_in_size != 0 :
        *data_out_size < data_in_size)
        return false;

    return true;
}

static bool
nv_gcm_seal(const uint8_t *key, size_t key_size,
            const uint8_t *iv, size_t iv_size,
            const uint8_t *a_data, size_t a_data_size,
            const uint8_t *data_in, size_t data_in_size,
            uint8_t *tag_out, size_t tag_size,
            uint8_t *data_out, size_t *data_out_size)
{
    crypto_mech_type_t mech_type;

    if (!nv_gcm_check_args(key, key_size, iv, iv_size, a_data, a_data_size,
                           data_in, data_in_size, tag_out, tag_size,
                           data_out, data_out_size))
        return false;

    mech_type = crypto_mech2id(SUN_CKM_AES_GCM);
    if (mech_type == CRYPTO_MECH_INVALID)
        return false;

    if (!nv_gcm_encrypt(mech_type, key, key_size, iv, a_data, a_data_size,
                        data_in, data_in_size, data_out, tag_out, tag_size))
        return false;

    if (data_out_size != NULL)
        *data_out_size = data_in_size;

    return true;
}

/*
 * Decryption uses only the KCF encrypt path: KCF's GCM decrypt compares the
 * tag with bcmp() and frees its plaintext buffer without clearing it.  The
 * first pass recovers the plaintext into scratch, the second re-encrypts it
 * to get the expected tag.  On failure data_out holds at most ciphertext.
 */
static bool
nv_gcm_open(nv_aead_ctx_t *ctx,
            const uint8_t *key, size_t key_size,
            const uint8_t *iv, size_t iv_size,
            const uint8_t *a_data, size_t a_data_size,
            const uint8_t *data_in, size_t data_in_size,
            const uint8_t *tag, size_t tag_size,
            uint8_t *data_out, size_t *data_out_size)
{
    crypto_mech_type_t mech_type;
    uint8_t expected[NV_GCM_MAX_TAG_SIZE];
    uint8_t computed[NV_GCM_MAX_TAG_SIZE];
    uint8_t *scratch = NULL;
    bool ret = false;

    if (!nv_gcm_check_args(key, key_size, iv, iv_size, a_data, a_data_size,
                           data_in, data_in_size, tag, tag_size,
                           data_out, data_out_size))
        return false;

    mech_type = crypto_mech2id(SUN_CKM_AES_GCM);
    if (mech_type == CRYPTO_MECH_INVALID)
        return false;

    bcopy(tag, expected, tag_size);

    if (data_in_size != 0)
    {
        if (ctx == NULL)
        {
            scratch = kmem_alloc(data_in_size, KM_NOSLEEP);
        }
        else
        {
            if (ctx->scratch_size < data_in_size)
            {
                if (ctx->scratch != NULL)
                    kmem_free(ctx->scratch, ctx->scratch_size);
                ctx->scratch_size = 0;
                ctx->scratch = kmem_alloc(data_in_size, KM_NOSLEEP);
                if (ctx->scratch != NULL)
                    ctx->scratch_size = data_in_size;
            }
            scratch = ctx->scratch;
        }
        if (scratch == NULL)
            goto out;

        if (!nv_gcm_encrypt(mech_type, key, key_size, iv, a_data,
                            a_data_size, data_in, data_in_size, scratch,
                            computed, sizeof (computed)))
            goto out;
    }

    if (!nv_gcm_encrypt(mech_type, key, key_size, iv, a_data, a_data_size,
                        scratch, data_in_size, data_out, computed, tag_size))
        goto out;

    if (!nv_crypto_equal(computed, expected, tag_size))
        goto out;

    if (data_in_size != 0)
        bcopy(scratch, data_out, data_in_size);
    if (data_out_size != NULL)
        *data_out_size = data_in_size;
    ret = true;

out:
    nv_crypto_zero(computed, sizeof (computed));
    nv_crypto_zero(expected, sizeof (expected));
    if (scratch != NULL)
    {
        nv_crypto_zero(scratch, data_in_size);
        if (ctx == NULL)
            kmem_free(scratch, data_in_size);
    }

    return ret;
}

bool libspdm_aead_aes_gcm_encrypt(const uint8_t *key, size_t key_size,
                                  const uint8_t *iv, size_t iv_size,
                                  const uint8_t *a_data, size_t a_data_size,
                                  const uint8_t *data_in, size_t data_in_size,
                                  uint8_t *tag_out, size_t tag_size,
                                  uint8_t *data_out, size_t *data_out_size)
{
    return nv_gcm_seal(key, key_size, iv, iv_size, a_data, a_data_size,
                       data_in, data_in_size, tag_out, tag_size,
                       data_out, data_out_size);
}

bool libspdm_aead_aes_gcm_decrypt(const uint8_t *key, size_t key_size,
                                  const uint8_t *iv, size_t iv_size,
                                  const uint8_t *a_data, size_t a_data_size,
                                  const uint8_t *data_in, size_t data_in_size,
                                  const uint8_t *tag, size_t tag_size,
                                  uint8_t *data_out, size_t *data_out_size)
{
    return nv_gcm_open(NULL, key, key_size, iv, iv_size, a_data, a_data_size,
                       data_in, data_in_size, tag, tag_size,
                       data_out, data_out_size);
}

/*
 * The context only caches the decrypt scratch buffer; like the Linux version
 * it must not be used by two threads at once.
 */
bool libspdm_aead_gcm_prealloc(void **context)
{
    if (context == NULL)
        return false;

    *context = kmem_zalloc(sizeof (nv_aead_ctx_t), KM_SLEEP);

    return true;
}

void libspdm_aead_free(void *context)
{
    nv_aead_ctx_t *ctx = context;

    if (ctx == NULL)
        return;

    if (ctx->scratch != NULL)
    {
        nv_crypto_zero(ctx->scratch, ctx->scratch_size);
        kmem_free(ctx->scratch, ctx->scratch_size);
    }
    kmem_free(ctx, sizeof (*ctx));
}

bool libspdm_aead_aes_gcm_encrypt_prealloc(void *context,
        const uint8_t *key, size_t key_size,
        const uint8_t *iv, size_t iv_size,
        const uint8_t *a_data, size_t a_data_size,
        const uint8_t *data_in, size_t data_in_size,
        uint8_t *tag_out, size_t tag_size,
        uint8_t *data_out, size_t *data_out_size)
{
    if (context == NULL)
        return false;

    return nv_gcm_seal(key, key_size, iv, iv_size, a_data, a_data_size,
                       data_in, data_in_size, tag_out, tag_size,
                       data_out, data_out_size);
}

bool libspdm_aead_aes_gcm_decrypt_prealloc(void *context,
        const uint8_t *key, size_t key_size,
        const uint8_t *iv, size_t iv_size,
        const uint8_t *a_data, size_t a_data_size,
        const uint8_t *data_in, size_t data_in_size,
        const uint8_t *tag, size_t tag_size,
        uint8_t *data_out, size_t *data_out_size)
{
    if (context == NULL)
        return false;

    return nv_gcm_open(context, key, key_size, iv, iv_size,
                       a_data, a_data_size, data_in, data_in_size,
                       tag, tag_size, data_out, data_out_size);
}

/* Random numbers */

bool libspdm_random_bytes(uint8_t *output, size_t size)
{
    if (size == 0)
        return true;
    if (output == NULL)
        return false;

    if (random_get_bytes(output, size) != 0)
    {
        nv_crypto_zero(output, size);
        return false;
    }

    return true;
}

/* Base64 (RFC 4648) */

static const uint8_t nv_base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int
nv_base64_value(uint8_t c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;

    return -1;
}

static bool
nv_base64_space(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/*
 * Like mbedtls_base64_encode(), the output is NUL terminated and the NUL is
 * not counted.  RM's libspdm_encode_base64_with_newline() writes a newline
 * after the returned length, so that byte must exist.
 */
bool libspdm_encode_base64(const uint8_t *src, uint8_t *dst, size_t srclen,
                           size_t *p_dstlen)
{
    uint32_t v;
    size_t need, i, o;

    if (p_dstlen == NULL)
        return false;
    if (dst == NULL || (src == NULL && srclen != 0) ||
        srclen > (SIZE_MAX / 4) * 3)
        goto fail;

    need = ((srclen + 2) / 3) * 4;
    if (need >= *p_dstlen)
        goto fail;

    for (i = 0, o = 0; srclen - i >= 3; i += 3)
    {
        v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8) | src[i + 2];
        dst[o++] = nv_base64_alphabet[(v >> 18) & 63];
        dst[o++] = nv_base64_alphabet[(v >> 12) & 63];
        dst[o++] = nv_base64_alphabet[(v >> 6) & 63];
        dst[o++] = nv_base64_alphabet[v & 63];
    }

    if (srclen - i == 1)
    {
        v = (uint32_t)src[i] << 16;
        dst[o++] = nv_base64_alphabet[(v >> 18) & 63];
        dst[o++] = nv_base64_alphabet[(v >> 12) & 63];
        dst[o++] = '=';
        dst[o++] = '=';
    }
    else if (srclen - i == 2)
    {
        v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8);
        dst[o++] = nv_base64_alphabet[(v >> 18) & 63];
        dst[o++] = nv_base64_alphabet[(v >> 12) & 63];
        dst[o++] = nv_base64_alphabet[(v >> 6) & 63];
        dst[o++] = '=';
    }

    dst[o] = '\0';
    *p_dstlen = o;
    return true;

fail:
    *p_dstlen = 0;
    return false;
}

/* Whitespace is skipped so that PEM bodies with line breaks decode. */
bool libspdm_decode_base64(const uint8_t *src, uint8_t *dst, size_t srclen,
                           size_t *p_dstlen)
{
    size_t nchars = 0, npad = 0, need, i, o = 0;
    uint32_t acc = 0;
    uint_t quad = 0;
    int v;

    if (p_dstlen == NULL)
        return false;
    if (src == NULL && srclen != 0)
        goto fail;

    for (i = 0; i < srclen; i++)
    {
        if (nv_base64_space(src[i]))
            continue;
        if (src[i] == '=')
        {
            if (++npad > 2)
                goto fail;
            continue;
        }
        if (npad != 0 || nv_base64_value(src[i]) < 0)
            goto fail;
        nchars++;
    }

    if ((nchars + npad) % 4 != 0)
        goto fail;

    need = (nchars / 4) * 3 + ((nchars % 4) != 0 ? (nchars % 4) - 1 : 0);
    if (need > *p_dstlen || (dst == NULL && need != 0))
        goto fail;

    for (i = 0; i < srclen; i++)
    {
        v = nv_base64_value(src[i]);
        if (v < 0)
            continue;
        acc = (acc << 6) | (uint32_t)v;
        if (++quad == 4)
        {
            dst[o++] = (uint8_t)(acc >> 16);
            dst[o++] = (uint8_t)(acc >> 8);
            dst[o++] = (uint8_t)acc;
            acc = 0;
            quad = 0;
        }
    }

    if (quad == 2)
    {
        dst[o++] = (uint8_t)(acc >> 4);
    }
    else if (quad == 3)
    {
        dst[o++] = (uint8_t)(acc >> 10);
        dst[o++] = (uint8_t)(acc >> 2);
    }

    *p_dstlen = o;
    return true;

fail:
    *p_dstlen = 0;
    return false;
}

/* ASN.1 */

/*
 * Same contract as the mbedTLS backend, which libspdm_crypt_cert.c relies on:
 * on success *ptr is moved past the tag and length octets and *length is the
 * content length.
 */
bool libspdm_asn1_get_tag(uint8_t **ptr, const uint8_t *end, size_t *length,
                          uint32_t tag)
{
    const uint8_t *p;
    size_t avail, len, hdr, nbytes, i;

    if (ptr == NULL || *ptr == NULL || end == NULL || length == NULL)
        return false;

    p = *ptr;
    if (p >= end)
        return false;
    avail = (size_t)(end - p);

    if (avail < 2 || p[0] != tag)
        return false;

    if (p[1] < 0x80)
    {
        len = p[1];
        hdr = 2;
    }
    else
    {
        nbytes = p[1] & 0x7f;
        if (nbytes == 0 || nbytes > 4 || avail - 2 < nbytes)
            return false;
        for (len = 0, i = 0; i < nbytes; i++)
            len = (len << 8) | p[2 + i];
        hdr = 2 + nbytes;
    }

    if (len > avail - hdr)
        return false;

    *ptr = (uint8_t *)(uintptr_t)(p + hdr);
    *length = len;

    return true;
}

/*
 * This backend has no ECDSA, ECDH, RSA or X.509 support, so SPDM cannot
 * authenticate the GPU.  Reporting that here makes RM refuse to start an SPDM
 * session, which rules out Confidential Computing.
 */
bool libspdm_check_crypto_backend(void)
{
    return false;
}

/* Elliptic curve */

void *libspdm_ec_new_by_nid(size_t nid)
{
    return NULL;
}

void libspdm_ec_free(void *ec_context)
{
}

bool libspdm_ec_generate_key(void *ec_context, uint8_t *public_key,
                             size_t *public_key_size)
{
    return false;
}

bool libspdm_ec_compute_key(void *ec_context, const uint8_t *peer_public,
                            size_t peer_public_size, uint8_t *key,
                            size_t *key_size)
{
    return false;
}

bool libspdm_ecdsa_sign(void *ec_context, size_t hash_nid,
                        const uint8_t *message_hash, size_t hash_size,
                        uint8_t *signature, size_t *sig_size)
{
    return false;
}

bool libspdm_ecdsa_verify(void *ec_context, size_t hash_nid,
                          const uint8_t *message_hash, size_t hash_size,
                          const uint8_t *signature, size_t sig_size)
{
    return false;
}

/* RSA */

void *libspdm_rsa_new(void)
{
    return NULL;
}

void libspdm_rsa_free(void *rsa_context)
{
}

bool libspdm_rsa_set_key(void *rsa_context, const libspdm_rsa_key_tag_t key_tag,
                         const uint8_t *big_number, size_t bn_size)
{
    return false;
}

bool libspdm_rsa_pss_sign(void *rsa_context, size_t hash_nid,
                          const uint8_t *message_hash, size_t hash_size,
                          uint8_t *signature, size_t *sig_size)
{
    return false;
}

bool libspdm_rsa_pss_verify(void *rsa_context, size_t hash_nid,
                            const uint8_t *message_hash, size_t hash_size,
                            const uint8_t *signature, size_t sig_size)
{
    return false;
}

/* X.509 */

bool libspdm_ec_get_public_key_from_x509(const uint8_t *cert, size_t cert_size,
                                         void **ec_context)
{
    return false;
}

bool libspdm_rsa_get_public_key_from_x509(const uint8_t *cert, size_t cert_size,
                                          void **rsa_context)
{
    return false;
}

bool libspdm_x509_get_subject_name(const uint8_t *cert, size_t cert_size,
                                   uint8_t *cert_subject,
                                   size_t *subject_size)
{
    return false;
}

bool libspdm_x509_get_version(const uint8_t *cert, size_t cert_size,
                              size_t *version)
{
    return false;
}

bool libspdm_x509_get_serial_number(const uint8_t *cert, size_t cert_size,
                                    uint8_t *serial_number,
                                    size_t *serial_number_size)
{
    return false;
}

bool libspdm_x509_get_signature_algorithm(const uint8_t *cert,
                                          size_t cert_size, uint8_t *oid,
                                          size_t *oid_size)
{
    return false;
}

bool libspdm_x509_get_issuer_name(const uint8_t *cert, size_t cert_size,
                                  uint8_t *cert_issuer,
                                  size_t *issuer_size)
{
    return false;
}

bool libspdm_x509_get_extension_data(const uint8_t *cert, size_t cert_size,
                                     const uint8_t *oid, size_t oid_size,
                                     uint8_t *extension_data,
                                     size_t *extension_data_size)
{
    return false;
}

bool libspdm_x509_get_validity(const uint8_t *cert, size_t cert_size,
                               uint8_t *from, size_t *from_size, uint8_t *to,
                               size_t *to_size)
{
    return false;
}

bool libspdm_x509_set_date_time(const char *date_time_str, void *date_time,
                                size_t *date_time_size)
{
    return false;
}

int32_t libspdm_x509_compare_date_time(const void *date_time1,
                                       const void *date_time2)
{
    return -2;
}

bool libspdm_x509_get_key_usage(const uint8_t *cert, size_t cert_size,
                                size_t *usage)
{
    return false;
}

bool libspdm_x509_get_extended_key_usage(const uint8_t *cert,
                                         size_t cert_size, uint8_t *usage,
                                         size_t *usage_size)
{
    return false;
}

bool libspdm_x509_get_extended_basic_constraints(const uint8_t *cert,
                                                 size_t cert_size,
                                                 uint8_t *basic_constraints,
                                                 size_t *basic_constraints_size)
{
    return false;
}

bool libspdm_x509_verify_cert(const uint8_t *cert, size_t cert_size,
                              const uint8_t *ca_cert, size_t ca_cert_size)
{
    return false;
}

bool libspdm_x509_verify_cert_chain(const uint8_t *root_cert,
                                    size_t root_cert_length,
                                    const uint8_t *cert_chain,
                                    size_t cert_chain_length)
{
    return false;
}

bool libspdm_x509_get_cert_from_cert_chain(const uint8_t *cert_chain,
                                           size_t cert_chain_length,
                                           const int32_t cert_index,
                                           const uint8_t **cert,
                                           size_t *cert_length)
{
    return false;
}
