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

#ifndef _NVIDIA_CRYPTO_H_
#define _NVIDIA_CRYPTO_H_

/*
 * libspdm crypto backend (library/cryptlib.h) for the illumos kernel
 * interface layer.  SHA-2 comes from misc/sha2 and AES-GCM from KCF; HMAC and
 * HKDF are built on SHA-2 here so that their contexts can be duplicated.
 * Callers must be in thread context.
 */

#include <sys/types.h>
#include <sys/stdbool.h>
#include <sys/int_limits.h>
#include <sys/kmem.h>
#include <sys/random.h>
#include <sys/uio.h>
#include <sys/crypto/common.h>
#include <sys/crypto/api.h>

#ifdef _KERNEL
#include <sys/systm.h>
#include <sys/sha2.h>
#else
#include <strings.h>
#include <sha2.h>
#endif

#include "library/cryptlib.h"
#include "nvspdm_cryptlib_extensions.h"

#define NV_SHA_MAX_DIGEST       SHA384_DIGEST_LENGTH
#define NV_SHA_MAX_BLOCK        SHA512_HMAC_BLOCK_SIZE

#define NV_GCM_IV_SIZE          12
#define NV_GCM_MAX_TAG_SIZE     16

typedef struct nv_sha_alg
{
    uint64_t mech;
    size_t   digest_len;
    size_t   block_len;
} nv_sha_alg_t;


typedef struct nv_sha_ctx
{
    const nv_sha_alg_t *alg;
    bool                ready;
    SHA2_CTX            sha;
} nv_sha_ctx_t;

typedef struct nv_hmac_ctx
{
    const nv_sha_alg_t *alg;
    bool                keyed;
    SHA2_CTX            inner;
    SHA2_CTX            outer;
} nv_hmac_ctx_t;

typedef struct nv_aead_ctx
{
    uint8_t *scratch;
    size_t   scratch_size;
} nv_aead_ctx_t;

static inline void
nv_crypto_zero(void *p, size_t len)
{
    bzero(p, len);
    __asm__ __volatile__("" : : "r"(p) : "memory");
}

static inline bool
nv_crypto_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    volatile uint8_t diff = 0;
    size_t i;

    for (i = 0; i < len; i++)
        diff |= a[i] ^ b[i];

    return diff == 0;
}

#endif /* _NVIDIA_CRYPTO_H_ */
