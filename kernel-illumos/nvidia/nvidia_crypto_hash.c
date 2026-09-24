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
 * SHA-2, HMAC and HKDF for the libspdm crypto backend.
 */

#include "nvidia_crypto.h"

static const nv_sha_alg_t nv_sha256 =
    { SHA256, SHA256_DIGEST_LENGTH, SHA256_HMAC_BLOCK_SIZE };
static const nv_sha_alg_t nv_sha384 =
    { SHA384, SHA384_DIGEST_LENGTH, SHA512_HMAC_BLOCK_SIZE };

/* SHA-256 / SHA-384 */

static void *
nv_sha_new(const nv_sha_alg_t *alg)
{
    nv_sha_ctx_t *ctx = kmem_zalloc(sizeof (*ctx), KM_SLEEP);

    ctx->alg = alg;
    SHA2Init(alg->mech, &ctx->sha);
    ctx->ready = true;

    return ctx;
}

static void
nv_sha_free(void *context)
{
    if (context == NULL)
        return;

    nv_crypto_zero(context, sizeof (nv_sha_ctx_t));
    kmem_free(context, sizeof (nv_sha_ctx_t));
}

static bool
nv_sha_init(void *context, const nv_sha_alg_t *alg)
{
    nv_sha_ctx_t *ctx = context;

    if (ctx == NULL || ctx->alg != alg)
        return false;

    SHA2Init(alg->mech, &ctx->sha);
    ctx->ready = true;

    return true;
}

static bool
nv_sha_duplicate(const void *context, void *new_context,
                 const nv_sha_alg_t *alg)
{
    const nv_sha_ctx_t *src = context;
    nv_sha_ctx_t *dst = new_context;

    if (src == NULL || dst == NULL || src->alg != alg || dst->alg != alg)
        return false;

    *dst = *src;

    return true;
}

static bool
nv_sha_update(void *context, const void *data, size_t data_size,
              const nv_sha_alg_t *alg)
{
    nv_sha_ctx_t *ctx = context;

    if (ctx == NULL || ctx->alg != alg || !ctx->ready)
        return false;
    if (data == NULL && data_size != 0)
        return false;

    SHA2Update(&ctx->sha, data, data_size);

    return true;
}

static bool
nv_sha_final(void *context, uint8_t *hash_value, const nv_sha_alg_t *alg)
{
    nv_sha_ctx_t *ctx = context;

    if (ctx == NULL || ctx->alg != alg || !ctx->ready || hash_value == NULL)
        return false;

    SHA2Final(hash_value, &ctx->sha);
    ctx->ready = false;

    return true;
}

static bool
nv_sha_all(const nv_sha_alg_t *alg, const void *data, size_t data_size,
           uint8_t *hash_value)
{
    SHA2_CTX sha;

    if (hash_value == NULL || (data == NULL && data_size != 0))
        return false;

    SHA2Init(alg->mech, &sha);
    SHA2Update(&sha, data, data_size);
    SHA2Final(hash_value, &sha);

    return true;
}

void *libspdm_sha256_new(void)
{
    return nv_sha_new(&nv_sha256);
}

void libspdm_sha256_free(void *sha256_context)
{
    nv_sha_free(sha256_context);
}

bool libspdm_sha256_init(void *sha256_context)
{
    return nv_sha_init(sha256_context, &nv_sha256);
}

bool libspdm_sha256_duplicate(const void *sha256_context,
                              void *new_sha256_context)
{
    return nv_sha_duplicate(sha256_context, new_sha256_context, &nv_sha256);
}

bool libspdm_sha256_update(void *sha256_context, const void *data,
                           size_t data_size)
{
    return nv_sha_update(sha256_context, data, data_size, &nv_sha256);
}

bool libspdm_sha256_final(void *sha256_context, uint8_t *hash_value)
{
    return nv_sha_final(sha256_context, hash_value, &nv_sha256);
}

bool libspdm_sha256_hash_all(const void *data, size_t data_size,
                             uint8_t *hash_value)
{
    return nv_sha_all(&nv_sha256, data, data_size, hash_value);
}

void *libspdm_sha384_new(void)
{
    return nv_sha_new(&nv_sha384);
}

void libspdm_sha384_free(void *sha384_context)
{
    nv_sha_free(sha384_context);
}

bool libspdm_sha384_init(void *sha384_context)
{
    return nv_sha_init(sha384_context, &nv_sha384);
}

bool libspdm_sha384_duplicate(const void *sha384_context,
                              void *new_sha384_context)
{
    return nv_sha_duplicate(sha384_context, new_sha384_context, &nv_sha384);
}

bool libspdm_sha384_update(void *sha384_context, const void *data,
                           size_t data_size)
{
    return nv_sha_update(sha384_context, data, data_size, &nv_sha384);
}

bool libspdm_sha384_final(void *sha384_context, uint8_t *hash_value)
{
    return nv_sha_final(sha384_context, hash_value, &nv_sha384);
}

bool libspdm_sha384_hash_all(const void *data, size_t data_size,
                             uint8_t *hash_value)
{
    return nv_sha_all(&nv_sha384, data, data_size, hash_value);
}

/* HMAC (RFC 2104) */

static bool
nv_hmac_key(nv_hmac_ctx_t *ctx, const nv_sha_alg_t *alg,
            const uint8_t *key, size_t key_size)
{
    uint8_t pad[NV_SHA_MAX_BLOCK];
    size_t i;

    if (key == NULL && key_size != 0)
        return false;

    bzero(pad, sizeof (pad));
    if (key_size > alg->block_len)
    {
        SHA2Init(alg->mech, &ctx->inner);
        SHA2Update(&ctx->inner, key, key_size);
        SHA2Final(pad, &ctx->inner);
    }
    else if (key_size != 0)
    {
        bcopy(key, pad, key_size);
    }

    for (i = 0; i < alg->block_len; i++)
        pad[i] ^= 0x36;
    SHA2Init(alg->mech, &ctx->inner);
    SHA2Update(&ctx->inner, pad, alg->block_len);

    for (i = 0; i < alg->block_len; i++)
        pad[i] ^= 0x36 ^ 0x5c;
    SHA2Init(alg->mech, &ctx->outer);
    SHA2Update(&ctx->outer, pad, alg->block_len);

    nv_crypto_zero(pad, sizeof (pad));
    ctx->alg = alg;
    ctx->keyed = true;

    return true;
}

static void
nv_hmac_finish(nv_hmac_ctx_t *ctx, uint8_t *hmac_value)
{
    uint8_t digest[NV_SHA_MAX_DIGEST];

    SHA2Final(digest, &ctx->inner);
    SHA2Update(&ctx->outer, digest, ctx->alg->digest_len);
    SHA2Final(hmac_value, &ctx->outer);
    nv_crypto_zero(digest, sizeof (digest));
    ctx->keyed = false;
}

static void *
nv_hmac_new(const nv_sha_alg_t *alg)
{
    nv_hmac_ctx_t *ctx = kmem_zalloc(sizeof (*ctx), KM_SLEEP);

    ctx->alg = alg;

    return ctx;
}

static void
nv_hmac_free(void *context)
{
    if (context == NULL)
        return;

    nv_crypto_zero(context, sizeof (nv_hmac_ctx_t));
    kmem_free(context, sizeof (nv_hmac_ctx_t));
}

static bool
nv_hmac_set_key(void *context, const uint8_t *key, size_t key_size,
                const nv_sha_alg_t *alg)
{
    nv_hmac_ctx_t *ctx = context;

    if (ctx == NULL || ctx->alg != alg)
        return false;

    return nv_hmac_key(ctx, alg, key, key_size);
}

static bool
nv_hmac_duplicate(const void *context, void *new_context,
                  const nv_sha_alg_t *alg)
{
    const nv_hmac_ctx_t *src = context;
    nv_hmac_ctx_t *dst = new_context;

    if (src == NULL || dst == NULL || src->alg != alg || dst->alg != alg)
        return false;

    *dst = *src;

    return true;
}

static bool
nv_hmac_update(void *context, const void *data, size_t data_size,
               const nv_sha_alg_t *alg)
{
    nv_hmac_ctx_t *ctx = context;

    if (ctx == NULL || ctx->alg != alg || !ctx->keyed)
        return false;
    if (data == NULL && data_size != 0)
        return false;

    SHA2Update(&ctx->inner, data, data_size);

    return true;
}

static bool
nv_hmac_final(void *context, uint8_t *hmac_value, const nv_sha_alg_t *alg)
{
    nv_hmac_ctx_t *ctx = context;

    if (ctx == NULL || ctx->alg != alg || !ctx->keyed || hmac_value == NULL)
        return false;

    nv_hmac_finish(ctx, hmac_value);

    return true;
}

static bool
nv_hmac_all(const nv_sha_alg_t *alg, const void *data, size_t data_size,
            const uint8_t *key, size_t key_size, uint8_t *hmac_value)
{
    nv_hmac_ctx_t ctx;
    bool ret = false;

    if (hmac_value == NULL || (data == NULL && data_size != 0))
        return false;

    if (nv_hmac_key(&ctx, alg, key, key_size))
    {
        SHA2Update(&ctx.inner, data, data_size);
        nv_hmac_finish(&ctx, hmac_value);
        ret = true;
    }

    nv_crypto_zero(&ctx, sizeof (ctx));

    return ret;
}

void *libspdm_hmac_sha256_new(void)
{
    return nv_hmac_new(&nv_sha256);
}

void libspdm_hmac_sha256_free(void *hmac_sha256_ctx)
{
    nv_hmac_free(hmac_sha256_ctx);
}

bool libspdm_hmac_sha256_set_key(void *hmac_sha256_ctx, const uint8_t *key,
                                 size_t key_size)
{
    return nv_hmac_set_key(hmac_sha256_ctx, key, key_size, &nv_sha256);
}

bool libspdm_hmac_sha256_duplicate(const void *hmac_sha256_ctx,
                                   void *new_hmac_sha256_ctx)
{
    return nv_hmac_duplicate(hmac_sha256_ctx, new_hmac_sha256_ctx,
                             &nv_sha256);
}

bool libspdm_hmac_sha256_update(void *hmac_sha256_ctx, const void *data,
                                size_t data_size)
{
    return nv_hmac_update(hmac_sha256_ctx, data, data_size, &nv_sha256);
}

bool libspdm_hmac_sha256_final(void *hmac_sha256_ctx, uint8_t *hmac_value)
{
    return nv_hmac_final(hmac_sha256_ctx, hmac_value, &nv_sha256);
}

bool libspdm_hmac_sha256_all(const void *data, size_t data_size,
                             const uint8_t *key, size_t key_size,
                             uint8_t *hmac_value)
{
    return nv_hmac_all(&nv_sha256, data, data_size, key, key_size,
                       hmac_value);
}

void *libspdm_hmac_sha384_new(void)
{
    return nv_hmac_new(&nv_sha384);
}

void libspdm_hmac_sha384_free(void *hmac_sha384_ctx)
{
    nv_hmac_free(hmac_sha384_ctx);
}

bool libspdm_hmac_sha384_set_key(void *hmac_sha384_ctx, const uint8_t *key,
                                 size_t key_size)
{
    return nv_hmac_set_key(hmac_sha384_ctx, key, key_size, &nv_sha384);
}

bool libspdm_hmac_sha384_duplicate(const void *hmac_sha384_ctx,
                                   void *new_hmac_sha384_ctx)
{
    return nv_hmac_duplicate(hmac_sha384_ctx, new_hmac_sha384_ctx,
                             &nv_sha384);
}

bool libspdm_hmac_sha384_update(void *hmac_sha384_ctx, const void *data,
                                size_t data_size)
{
    return nv_hmac_update(hmac_sha384_ctx, data, data_size, &nv_sha384);
}

bool libspdm_hmac_sha384_final(void *hmac_sha384_ctx, uint8_t *hmac_value)
{
    return nv_hmac_final(hmac_sha384_ctx, hmac_value, &nv_sha384);
}

bool libspdm_hmac_sha384_all(const void *data, size_t data_size,
                             const uint8_t *key, size_t key_size,
                             uint8_t *hmac_value)
{
    return nv_hmac_all(&nv_sha384, data, data_size, key, key_size,
                       hmac_value);
}

/* HKDF (RFC 5869) */

static bool
nv_hkdf_extract(const nv_sha_alg_t *alg, const uint8_t *key, size_t key_size,
                const uint8_t *salt, size_t salt_size,
                uint8_t *prk_out, size_t prk_out_size)
{
    if (prk_out == NULL || prk_out_size != alg->digest_len)
        return false;

    return nv_hmac_all(alg, key, key_size, salt, salt_size, prk_out);
}

static bool
nv_hkdf_expand(const nv_sha_alg_t *alg, const uint8_t *prk, size_t prk_size,
               const uint8_t *info, size_t info_size,
               uint8_t *out, size_t out_size)
{
    nv_hmac_ctx_t *base, *work;
    uint8_t t[NV_SHA_MAX_DIGEST];
    uint8_t counter;
    size_t done, n;

    if (prk == NULL || out == NULL || (info == NULL && info_size != 0))
        return false;
    if (prk_size < alg->digest_len || out_size > 255 * alg->digest_len)
        return false;

    base = kmem_alloc(2 * sizeof (*base), KM_SLEEP);
    work = base + 1;

    (void) nv_hmac_key(base, alg, prk, prk_size);

    for (done = 0, counter = 1; done < out_size; counter++)
    {
        *work = *base;
        if (done != 0)
            SHA2Update(&work->inner, t, alg->digest_len);
        SHA2Update(&work->inner, info, info_size);
        SHA2Update(&work->inner, &counter, 1);
        nv_hmac_finish(work, t);

        n = out_size - done;
        if (n > alg->digest_len)
            n = alg->digest_len;
        bcopy(t, out + done, n);
        done += n;
    }

    nv_crypto_zero(t, sizeof (t));
    nv_crypto_zero(base, 2 * sizeof (*base));
    kmem_free(base, 2 * sizeof (*base));

    return true;
}

bool libspdm_hkdf_sha256_extract(const uint8_t *key, size_t key_size,
                                 const uint8_t *salt, size_t salt_size,
                                 uint8_t *prk_out, size_t prk_out_size)
{
    return nv_hkdf_extract(&nv_sha256, key, key_size, salt, salt_size,
                           prk_out, prk_out_size);
}

bool libspdm_hkdf_sha256_expand(const uint8_t *prk, size_t prk_size,
                                const uint8_t *info, size_t info_size,
                                uint8_t *out, size_t out_size)
{
    return nv_hkdf_expand(&nv_sha256, prk, prk_size, info, info_size,
                          out, out_size);
}

bool libspdm_hkdf_sha384_extract(const uint8_t *key, size_t key_size,
                                 const uint8_t *salt, size_t salt_size,
                                 uint8_t *prk_out, size_t prk_out_size)
{
    return nv_hkdf_extract(&nv_sha384, key, key_size, salt, salt_size,
                           prk_out, prk_out_size);
}

bool libspdm_hkdf_sha384_expand(const uint8_t *prk, size_t prk_size,
                                const uint8_t *info, size_t info_size,
                                uint8_t *out, size_t out_size)
{
    return nv_hkdf_expand(&nv_sha384, prk, prk_size, info, info_size,
                          out, out_size);
}
