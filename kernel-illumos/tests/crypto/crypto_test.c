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
 * Userland test for nvidia/nvidia_crypto_hash.c and nvidia_crypto.c.  SHA-2 comes from libmd, which
 * shares its implementation with the kernel's misc/sha2.  kmem, random and
 * the KCF AES-GCM encrypt call are emulated below; the KCF emulation uses
 * OpenSSL, so the GCM tests check the glue in nvidia_crypto.c, not KCF.
 */

#include <sys/types.h>
#include <sys/kmem.h>
#include <sys/uio.h>
#include <sys/crypto/common.h>
#include <sys/crypto/api.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>

/* kmem emulation: checks that every free passes the allocation size. */

#define TEST_KMEM_MAGIC 0x6b6d656d616c6c63ULL

static long test_kmem_outstanding;
static int test_kmem_fail_nosleep;

typedef struct
{
    uint64_t magic;
    size_t size;
} test_kmem_hdr_t;

void *
kmem_alloc(size_t size, int flags)
{
    test_kmem_hdr_t *h;

    if ((flags & KM_NOSLEEP) && test_kmem_fail_nosleep)
        return NULL;

    h = malloc(sizeof (*h) + size);
    if (h == NULL)
        abort();
    h->magic = TEST_KMEM_MAGIC;
    h->size = size;
    memset(h + 1, 0xa5, size);
    test_kmem_outstanding++;

    return h + 1;
}

void *
kmem_zalloc(size_t size, int flags)
{
    void *p = kmem_alloc(size, flags);

    if (p != NULL)
        memset(p, 0, size);
    return p;
}

void
kmem_free(void *p, size_t size)
{
    test_kmem_hdr_t *h = (test_kmem_hdr_t *)p - 1;

    if (h->magic != TEST_KMEM_MAGIC || h->size != size)
    {
        fprintf(stderr, "kmem_free: bad free (size %zu, expected %zu)\n",
                size, h->size);
        abort();
    }
    h->magic = 0;
    test_kmem_outstanding--;
    free(h);
}

/* random_get_bytes emulation */

static int test_rand_fail;

int
random_get_bytes(uint8_t *p, size_t len)
{
    if (test_rand_fail)
    {
        memset(p, 0xab, len);
        return EAGAIN;
    }
    arc4random_buf(p, len);
    return 0;
}

/* KCF emulation of crypto_mech2id() and crypto_encrypt() for CKM_AES_GCM. */

#define TEST_GCM_MECH ((crypto_mech_type_t)0x4743)

static int test_mech_invalid;
static int test_encrypt_calls;

crypto_mech_type_t
crypto_mech2id(const char *name)
{
    if (!test_mech_invalid && strcmp(name, SUN_CKM_AES_GCM) == 0)
        return TEST_GCM_MECH;
    return CRYPTO_MECH_INVALID;
}

static int
test_uio_put(crypto_data_t *cd, const uint8_t *buf, size_t len)
{
    uio_t *uio = cd->cd_uio;
    off_t off = cd->cd_offset;
    int i = 0;
    size_t n;

    if (uio->uio_segflg != UIO_SYSSPACE)
        return CRYPTO_ARGUMENTS_BAD;
    while (i < uio->uio_iovcnt && off >= (off_t)uio->uio_iov[i].iov_len)
        off -= uio->uio_iov[i++].iov_len;
    while (len != 0 && i < uio->uio_iovcnt)
    {
        n = uio->uio_iov[i].iov_len - off;
        if (n > len)
            n = len;
        memcpy(uio->uio_iov[i].iov_base + off, buf, n);
        buf += n;
        len -= n;
        off = 0;
        i++;
    }
    return len == 0 ? CRYPTO_SUCCESS : CRYPTO_DATA_LEN_RANGE;
}

int
crypto_encrypt(crypto_mechanism_t *mech, crypto_data_t *pt,
               crypto_key_t *key, crypto_ctx_template_t tmpl,
               crypto_data_t *ct, crypto_call_req_t *cr)
{
    CK_AES_GCM_PARAMS *p = (CK_AES_GCM_PARAMS *)mech->cm_param;
    const EVP_CIPHER *cipher;
    EVP_CIPHER_CTX *ctx;
    uint8_t *tmp;
    size_t len, taglen;
    int outl, rv;

    test_encrypt_calls++;

    if (mech->cm_type != TEST_GCM_MECH || cr != NULL || tmpl != NULL ||
        mech->cm_param_len != sizeof (CK_AES_GCM_PARAMS))
        return CRYPTO_ARGUMENTS_BAD;
    switch (p->ulTagBits)
    {
    case 32: case 64: case 96: case 104: case 112: case 120: case 128:
        break;
    default:
        return CRYPTO_MECHANISM_PARAM_INVALID;
    }
    /* KCF indexes pIv and pAAD even for zero lengths. */
    if (p->ulIvLen != 12 || p->ulIvBits != 96 || p->pIv == NULL ||
        p->pAAD == NULL)
        return CRYPTO_MECHANISM_PARAM_INVALID;
    if (key->ck_format != CRYPTO_KEY_RAW)
        return CRYPTO_KEY_TYPE_INCONSISTENT;
    switch (key->ck_length)
    {
    case 128: cipher = EVP_aes_128_gcm(); break;
    case 192: cipher = EVP_aes_192_gcm(); break;
    case 256: cipher = EVP_aes_256_gcm(); break;
    default: return CRYPTO_KEY_SIZE_RANGE;
    }
    if (pt->cd_format != CRYPTO_DATA_RAW || ct->cd_format != CRYPTO_DATA_UIO ||
        pt->cd_raw.iov_base == NULL ||
        pt->cd_raw.iov_len < pt->cd_offset + pt->cd_length)
        return CRYPTO_ARGUMENTS_BAD;

    len = pt->cd_length;
    taglen = p->ulTagBits / 8;
    if (ct->cd_length < len + taglen)
        return CRYPTO_BUFFER_TOO_SMALL;

    tmp = malloc(len + 16);
    ctx = EVP_CIPHER_CTX_new();
    if (tmp == NULL || ctx == NULL)
        abort();
    if (EVP_EncryptInit_ex(ctx, cipher, NULL, NULL, NULL) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1 ||
        EVP_EncryptInit_ex(ctx, NULL, NULL, key->ck_data, p->pIv) != 1)
        abort();
    if (p->ulAADLen != 0 &&
        EVP_EncryptUpdate(ctx, NULL, &outl, p->pAAD, p->ulAADLen) != 1)
        abort();
    if (len != 0 &&
        EVP_EncryptUpdate(ctx, tmp, &outl,
                          (uint8_t *)pt->cd_raw.iov_base + pt->cd_offset,
                          len) != 1)
        abort();
    if (EVP_EncryptFinal_ex(ctx, tmp + len, &outl) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tmp + len) != 1)
        abort();
    EVP_CIPHER_CTX_free(ctx);

    rv = test_uio_put(ct, tmp, len + taglen);
    free(tmp);
    return rv;
}

#include "../../nvidia/nvidia_crypto_hash.c"
#include "../../nvidia/nvidia_crypto.c"

/* Test helpers */

static int failures;
static int checks;

#define CHECK(cond)                                                     \
    do {                                                                \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                    #cond);                                             \
        }                                                               \
    } while (0)

static size_t
unhex(const char *s, uint8_t *out)
{
    size_t n = 0;
    unsigned int v;

    while (s[0] != '\0' && s[1] != '\0')
    {
        if (sscanf(s, "%2x", &v) != 1)
            abort();
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static bool
hexeq(const uint8_t *buf, size_t len, const char *hex)
{
    uint8_t want[512];

    return unhex(hex, want) == len && memcmp(buf, want, len) == 0;
}

static void
fill(uint8_t *p, size_t len, uint8_t v)
{
    memset(p, v, len);
}

static void
fill_seq(uint8_t *p, size_t len, uint8_t start)
{
    size_t i;

    for (i = 0; i < len; i++)
        p[i] = (uint8_t)(start + i);
}

/* SHA-2 */

static void
test_sha(void)
{
    uint8_t out[48], out2[48];
    void *a, *b;

    CHECK(libspdm_sha256_hash_all("abc", 3, out));
    CHECK(hexeq(out, 32, "ba7816bf8f01cfea414140de5dae2223"
                         "b00361a396177a9cb410ff61f20015ad"));
    CHECK(libspdm_sha384_hash_all("abc", 3, out));
    CHECK(hexeq(out, 48, "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded163"
                         "1a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7"));
    CHECK(libspdm_sha256_hash_all(NULL, 0, out));
    CHECK(hexeq(out, 32, "e3b0c44298fc1c149afbf4c8996fb924"
                         "27ae41e4649b934ca495991b7852b855"));
    CHECK(!libspdm_sha256_hash_all(NULL, 1, out));
    CHECK(!libspdm_sha256_hash_all("abc", 3, NULL));

    /* duplicate() forks the running state */
    a = libspdm_sha384_new();
    b = libspdm_sha384_new();
    CHECK(a != NULL && b != NULL);
    CHECK(libspdm_sha384_init(a));
    CHECK(libspdm_sha384_update(a, "ab", 2));
    CHECK(libspdm_sha384_duplicate(a, b));
    CHECK(libspdm_sha384_update(a, "c", 1));
    CHECK(libspdm_sha384_update(b, "d", 1));
    CHECK(libspdm_sha384_final(a, out));
    CHECK(libspdm_sha384_hash_all("abc", 3, out2));
    CHECK(memcmp(out, out2, 48) == 0);
    CHECK(libspdm_sha384_final(b, out));
    CHECK(libspdm_sha384_hash_all("abd", 3, out2));
    CHECK(memcmp(out, out2, 48) == 0);

    /* finalized contexts need init() */
    CHECK(!libspdm_sha384_final(b, out));
    CHECK(!libspdm_sha384_update(a, "x", 1));
    CHECK(libspdm_sha384_init(a));
    CHECK(libspdm_sha384_update(a, "abc", 3));
    CHECK(libspdm_sha384_final(a, out));
    CHECK(hexeq(out, 48, "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded163"
                         "1a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7"));

    /* a SHA-384 context is rejected by the SHA-256 entry points */
    CHECK(!libspdm_sha256_init(a));
    CHECK(!libspdm_sha256_update(a, "x", 1));
    CHECK(!libspdm_sha256_duplicate(a, b));
    libspdm_sha384_free(a);
    libspdm_sha384_free(b);
    libspdm_sha256_free(NULL);

    a = libspdm_sha256_new();
    b = libspdm_sha256_new();
    CHECK(libspdm_sha256_update(a, "a", 1));
    CHECK(libspdm_sha256_duplicate(a, b));
    CHECK(libspdm_sha256_update(b, "bc", 2));
    CHECK(libspdm_sha256_final(b, out));
    CHECK(hexeq(out, 32, "ba7816bf8f01cfea414140de5dae2223"
                         "b00361a396177a9cb410ff61f20015ad"));
    CHECK(libspdm_sha256_update(a, "bc", 2));
    CHECK(libspdm_sha256_final(a, out2));
    CHECK(memcmp(out, out2, 32) == 0);
    CHECK(!libspdm_sha256_update(a, NULL, 1));
    libspdm_sha256_free(a);
    libspdm_sha256_free(b);
}

/* HMAC: RFC 4231 test cases 1-4, 6 and 7 */

static void
test_hmac_rfc4231(void)
{
    static const struct
    {
        const char *key;
        size_t key_rep;
        const char *data;
        size_t data_rep;
        const char *sha256;
        const char *sha384;
    } tc[] = {
        { "0b", 20, "4869205468657265", 1,
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
          "afd03944d84895626b0825f4ab46907f15f9dadbe4101ec682aa034c7cebc59c"
          "faea9ea9076ede7f4af152e8b2fa9cb6" },
        { "4a656665", 1,
          "7768617420646f2079612077616e7420666f72206e6f7468696e673f", 1,
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
          "af45d2e376484031617f78d2b58a6b1b9c7ef464f5a01b47e42ec3736322445e"
          "8e2240ca5e69e2c78b3239ecfab21649" },
        { "aa", 20, "dd", 50,
          "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe",
          "88062608d3e6ad8a0aa2ace014c8a86f0aa635d947ac9febe83ef4e55966144b"
          "2a5ab39dc13814b94e3ab6e101a34f27" },
        { "0102030405060708090a0b0c0d0e0f10111213141516171819", 1, "cd", 50,
          "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b",
          "3e8a69b7783c25851933ab6290af6ca77a9981480850009cc5577c6e1f573b4e"
          "6801dd23c4a7d679ccf8a386c674cffb" },
        { "aa", 131,
          "54657374205573696e67204c6172676572205468616e20426c6f636b2d53697a"
          "65204b6579202d2048617368204b6579204669727374", 1,
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
          "4ece084485813e9088d2c63a041bc5b44f9ef1012a2b588f3cd11f05033ac4c6"
          "0c2ef6ab4030fe8296248df163f44952" },
        { "aa", 131,
          "5468697320697320612074657374207573696e672061206c6172676572207468"
          "616e20626c6f636b2d73697a65206b657920616e642061206c61726765722074"
          "68616e20626c6f636b2d73697a6520646174612e20546865206b6579206e6565"
          "647320746f20626520686173686564206265666f7265206265696e6720757365"
          "642062792074686520484d414320616c676f726974686d2e", 1,
          "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2",
          "6617178e941f020d351e2f254e8fd32c602420feb0b8fb9adccebb82461e99c5"
          "a678cc31e799176d3860e6110c46523e" },
    };
    uint8_t key[256], data[256], unit[256], out[48];
    size_t i, r, klen, dlen, n;
    void *ctx;

    for (i = 0; i < sizeof (tc) / sizeof (tc[0]); i++)
    {
        n = unhex(tc[i].key, unit);
        for (r = 0, klen = 0; r < tc[i].key_rep; r++, klen += n)
            memcpy(key + klen, unit, n);
        n = unhex(tc[i].data, unit);
        for (r = 0, dlen = 0; r < tc[i].data_rep; r++, dlen += n)
            memcpy(data + dlen, unit, n);

        CHECK(libspdm_hmac_sha256_all(data, dlen, key, klen, out));
        CHECK(hexeq(out, 32, tc[i].sha256));
        CHECK(libspdm_hmac_sha384_all(data, dlen, key, klen, out));
        CHECK(hexeq(out, 48, tc[i].sha384));

        /* streaming in two pieces gives the same answer */
        ctx = libspdm_hmac_sha384_new();
        CHECK(libspdm_hmac_sha384_set_key(ctx, key, klen));
        CHECK(libspdm_hmac_sha384_update(ctx, data, dlen / 2));
        CHECK(libspdm_hmac_sha384_update(ctx, data + dlen / 2,
                                         dlen - dlen / 2));
        CHECK(libspdm_hmac_sha384_final(ctx, out));
        CHECK(hexeq(out, 48, tc[i].sha384));
        libspdm_hmac_sha384_free(ctx);
    }
}

static void
test_hmac_semantics(void)
{
    uint8_t key[32], out[32], want[32], want2[32];
    void *a, *b;

    fill_seq(key, sizeof (key), 1);

    a = libspdm_hmac_sha256_new();
    b = libspdm_hmac_sha256_new();
    CHECK(a != NULL && b != NULL);

    /* no key yet */
    CHECK(!libspdm_hmac_sha256_update(a, "x", 1));
    CHECK(!libspdm_hmac_sha256_final(a, out));

    CHECK(libspdm_hmac_sha256_set_key(a, key, sizeof (key)));
    CHECK(libspdm_hmac_sha256_update(a, "hello ", 6));
    CHECK(libspdm_hmac_sha256_duplicate(a, b));
    CHECK(libspdm_hmac_sha256_update(a, "world", 5));
    CHECK(libspdm_hmac_sha256_update(b, "there", 5));
    CHECK(libspdm_hmac_sha256_final(a, out));
    CHECK(libspdm_hmac_sha256_all("hello world", 11, key, sizeof (key), want));
    CHECK(memcmp(out, want, 32) == 0);
    CHECK(libspdm_hmac_sha256_final(b, out));
    CHECK(libspdm_hmac_sha256_all("hello there", 11, key, sizeof (key), want2));
    CHECK(memcmp(out, want2, 32) == 0);

    /* finalized contexts need a new key */
    CHECK(!libspdm_hmac_sha256_update(a, "x", 1));
    CHECK(!libspdm_hmac_sha256_final(a, out));
    CHECK(libspdm_hmac_sha256_set_key(a, key, sizeof (key)));
    CHECK(libspdm_hmac_sha256_update(a, "hello world", 11));
    CHECK(libspdm_hmac_sha256_final(a, out));
    CHECK(memcmp(out, want, 32) == 0);

    /* type confusion is rejected */
    CHECK(!libspdm_hmac_sha384_set_key(a, key, sizeof (key)));
    CHECK(!libspdm_hmac_sha384_duplicate(a, b));
    CHECK(!libspdm_hmac_sha256_set_key(a, NULL, 1));

    /* empty key */
    CHECK(libspdm_hmac_sha256_set_key(a, NULL, 0));
    CHECK(libspdm_hmac_sha256_final(a, out));
    CHECK(HMAC(EVP_sha256(), "", 0, (const uint8_t *)"", 0, want, NULL) != NULL);
    CHECK(memcmp(out, want, 32) == 0);

    libspdm_hmac_sha256_free(a);
    libspdm_hmac_sha256_free(b);
    libspdm_hmac_sha384_free(NULL);
}

static void
test_hmac_openssl(void)
{
    uint8_t key[300], data[700], out[48], want[48];
    unsigned int wl;
    size_t klen, dlen;
    int i;

    for (i = 0; i < 400; i++)
    {
        klen = arc4random_uniform(sizeof (key));
        dlen = arc4random_uniform(sizeof (data));
        arc4random_buf(key, klen);
        arc4random_buf(data, dlen);

        CHECK(libspdm_hmac_sha256_all(data, dlen, key, klen, out));
        HMAC(EVP_sha256(), key, (int)klen, data, dlen, want, &wl);
        CHECK(wl == 32 && memcmp(out, want, 32) == 0);

        CHECK(libspdm_hmac_sha384_all(data, dlen, key, klen, out));
        HMAC(EVP_sha384(), key, (int)klen, data, dlen, want, &wl);
        CHECK(wl == 48 && memcmp(out, want, 48) == 0);
    }
}

/* HKDF: RFC 5869 test cases 1-3 plus OpenSSL cross checks for SHA-384 */

static void
test_hkdf_rfc5869(void)
{
    uint8_t ikm[80], salt[80], info[80], prk[32], okm[82];

    fill(ikm, 22, 0x0b);
    fill_seq(salt, 13, 0x00);
    fill_seq(info, 10, 0xf0);
    CHECK(libspdm_hkdf_sha256_extract(ikm, 22, salt, 13, prk, 32));
    CHECK(hexeq(prk, 32, "077709362c2e32df0ddc3f0dc47bba63"
                         "90b6c73bb50f9c3122ec844ad7c2b3e5"));
    CHECK(libspdm_hkdf_sha256_expand(prk, 32, info, 10, okm, 42));
    CHECK(hexeq(okm, 42, "3cb25f25faacd57a90434f64d0362f2a"
                         "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                         "34007208d5b887185865"));

    fill_seq(ikm, 80, 0x00);
    fill_seq(salt, 80, 0x60);
    fill_seq(info, 80, 0xb0);
    CHECK(libspdm_hkdf_sha256_extract(ikm, 80, salt, 80, prk, 32));
    CHECK(hexeq(prk, 32, "06a6b88c5853361a06104c9ceb35b45c"
                         "ef760014904671014a193f40c15fc244"));
    CHECK(libspdm_hkdf_sha256_expand(prk, 32, info, 80, okm, 82));
    CHECK(hexeq(okm, 82, "b11e398dc80327a1c8e7f78c596a4934"
                         "4f012eda2d4efad8a050cc4c19afa97c"
                         "59045a99cac7827271cb41c65e590e09"
                         "da3275600c2f09b8367793a9aca3db71"
                         "cc30c58179ec3e87c14c01d5c1f3434f"
                         "1d87"));

    fill(ikm, 22, 0x0b);
    CHECK(libspdm_hkdf_sha256_extract(ikm, 22, NULL, 0, prk, 32));
    CHECK(hexeq(prk, 32, "19ef24a32c717b167f33a91d6f648bdf"
                         "96596776afdb6377ac434c1c293ccb04"));
    CHECK(libspdm_hkdf_sha256_expand(prk, 32, NULL, 0, okm, 42));
    CHECK(hexeq(okm, 42, "8da4e775a563c18f715f802a063c5a31"
                         "b8a11f5c5ee1879ec3454e5f3c738d2d"
                         "9d201395faa4b61a96c8"));
}

static bool
openssl_hkdf(const EVP_MD *md, int mode, const uint8_t *key, size_t key_len,
             const uint8_t *salt, size_t salt_len,
             const uint8_t *info, size_t info_len,
             uint8_t *out, size_t out_len)
{
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
    bool ok;

    ok = pctx != NULL &&
        EVP_PKEY_derive_init(pctx) == 1 &&
        EVP_PKEY_CTX_set_hkdf_mode(pctx, mode) == 1 &&
        EVP_PKEY_CTX_set_hkdf_md(pctx, md) == 1 &&
        EVP_PKEY_CTX_set1_hkdf_key(pctx, key, (int)key_len) == 1 &&
        (salt_len == 0 ||
         EVP_PKEY_CTX_set1_hkdf_salt(pctx, salt, (int)salt_len) == 1) &&
        (info_len == 0 ||
         EVP_PKEY_CTX_add1_hkdf_info(pctx, info, (int)info_len) == 1) &&
        EVP_PKEY_derive(pctx, out, &out_len) == 1;
    EVP_PKEY_CTX_free(pctx);
    return ok;
}

static void
test_hkdf_openssl(void)
{
    uint8_t ikm[200], salt[200], info[200], prk[48], want_prk[48];
    uint8_t *okm, *want;
    size_t ikm_len, salt_len, info_len, out_len;
    int i;

    okm = malloc(255 * 48);
    want = malloc(255 * 48);

    for (i = 0; i < 200; i++)
    {
        ikm_len = 1 + arc4random_uniform(sizeof (ikm) - 1);
        salt_len = arc4random_uniform(sizeof (salt));
        info_len = arc4random_uniform(sizeof (info));
        arc4random_buf(ikm, ikm_len);
        arc4random_buf(salt, salt_len);
        arc4random_buf(info, info_len);

        out_len = 1 + arc4random_uniform(255 * 32);
        CHECK(libspdm_hkdf_sha256_extract(ikm, ikm_len, salt, salt_len,
                                          prk, 32));
        CHECK(openssl_hkdf(EVP_sha256(), EVP_PKEY_HKDEF_MODE_EXTRACT_ONLY,
                           ikm, ikm_len, salt, salt_len, NULL, 0,
                           want_prk, 32));
        CHECK(memcmp(prk, want_prk, 32) == 0);
        CHECK(libspdm_hkdf_sha256_expand(prk, 32, info, info_len,
                                         okm, out_len));
        CHECK(openssl_hkdf(EVP_sha256(), EVP_PKEY_HKDEF_MODE_EXPAND_ONLY,
                           prk, 32, NULL, 0, info, info_len, want, out_len));
        CHECK(memcmp(okm, want, out_len) == 0);

        out_len = 1 + arc4random_uniform(255 * 48);
        CHECK(libspdm_hkdf_sha384_extract(ikm, ikm_len, salt, salt_len,
                                          prk, 48));
        CHECK(openssl_hkdf(EVP_sha384(), EVP_PKEY_HKDEF_MODE_EXTRACT_ONLY,
                           ikm, ikm_len, salt, salt_len, NULL, 0,
                           want_prk, 48));
        CHECK(memcmp(prk, want_prk, 48) == 0);
        CHECK(libspdm_hkdf_sha384_expand(prk, 48, info, info_len,
                                         okm, out_len));
        CHECK(openssl_hkdf(EVP_sha384(), EVP_PKEY_HKDEF_MODE_EXPAND_ONLY,
                           prk, 48, NULL, 0, info, info_len, want, out_len));
        CHECK(memcmp(okm, want, out_len) == 0);
    }

    /* the output limit is 255 blocks, and the output never overruns */
    fill(okm, 255 * 48, 0x5a);
    CHECK(libspdm_hkdf_sha384_expand(prk, 48, info, 10, okm, 255 * 48));
    CHECK(!libspdm_hkdf_sha384_expand(prk, 48, info, 10, okm, 255 * 48 + 1));
    CHECK(!libspdm_hkdf_sha256_expand(prk, 32, info, 10, okm, 255 * 32 + 1));
    fill(okm, 64, 0x5a);
    CHECK(libspdm_hkdf_sha256_expand(prk, 32, info, 10, okm, 33));
    CHECK(okm[33] == 0x5a);

    /* argument checks */
    CHECK(!libspdm_hkdf_sha256_extract(ikm, 10, salt, 10, prk, 31));
    CHECK(!libspdm_hkdf_sha384_extract(ikm, 10, salt, 10, prk, 32));
    CHECK(!libspdm_hkdf_sha256_extract(NULL, 10, salt, 10, prk, 32));
    CHECK(!libspdm_hkdf_sha256_extract(ikm, 10, NULL, 10, prk, 32));
    CHECK(!libspdm_hkdf_sha256_expand(prk, 31, info, 10, okm, 32));
    CHECK(!libspdm_hkdf_sha384_expand(prk, 32, info, 10, okm, 32));
    CHECK(!libspdm_hkdf_sha256_expand(NULL, 32, info, 10, okm, 32));
    CHECK(!libspdm_hkdf_sha256_expand(prk, 32, NULL, 10, okm, 32));
    CHECK(!libspdm_hkdf_sha256_expand(prk, 32, info, 10, NULL, 32));

    free(okm);
    free(want);
}

/* AES-GCM glue */

static void
test_gcm_vector(void)
{
    /* GCM spec test case 16 (AES-256, 60-byte plaintext, 20-byte AAD) */
    uint8_t key[32], iv[12], pt[64], aad[20], ct[64], tag[16], out[64];
    size_t ptl, out_size;

    unhex("feffe9928665731c6d6a8f9467308308"
          "feffe9928665731c6d6a8f9467308308", key);
    unhex("cafebabefacedbaddecaf888", iv);
    ptl = unhex("d9313225f88406e5a55909c5aff5269a"
                "86a7a9531534f7da2e4c303d8a318a72"
                "1c3c0c95956809532fcf0e2449a6b525"
                "b16aedf5aa0de657ba637b39", pt);
    unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad);

    out_size = sizeof (ct);
    CHECK(libspdm_aead_aes_gcm_encrypt(key, 32, iv, 12, aad, 20, pt, ptl,
                                       tag, 16, ct, &out_size));
    CHECK(out_size == ptl);
    CHECK(hexeq(ct, ptl, "522dc1f099567d07f47f37a32a84427d"
                         "643a8cdcbfe5c0c97598a2bd2555d1aa"
                         "8cb08e48590dbb3da7b08b1056828838"
                         "c5f61e6393ba7a0abcc9f662"));
    CHECK(hexeq(tag, 16, "76fc6ece0f4e1768cddf8853bb2d551b"));

    out_size = sizeof (out);
    CHECK(libspdm_aead_aes_gcm_decrypt(key, 32, iv, 12, aad, 20, ct, ptl,
                                       tag, 16, out, &out_size));
    CHECK(out_size == ptl && memcmp(out, pt, ptl) == 0);

    /* MAC-only records: libspdm passes NULL data and output pointers */
    memset(key, 0, sizeof (key));
    memset(iv, 0, sizeof (iv));
    CHECK(libspdm_aead_aes_gcm_encrypt(key, 32, iv, 12, NULL, 0, NULL, 0,
                                       tag, 16, NULL, NULL));
    CHECK(hexeq(tag, 16, "530f8afbc74536b9a963b4f1c4cb738b"));
    CHECK(libspdm_aead_aes_gcm_decrypt(key, 32, iv, 12, NULL, 0, NULL, 0,
                                       tag, 16, NULL, NULL));
    CHECK(libspdm_aead_aes_gcm_encrypt(key, 32, iv, 12, aad, 20, NULL, 0,
                                       tag, 16, NULL, NULL));
    CHECK(libspdm_aead_aes_gcm_decrypt(key, 32, iv, 12, aad, 20, NULL, 0,
                                       tag, 16, NULL, NULL));
    tag[3] ^= 0x10;
    CHECK(!libspdm_aead_aes_gcm_decrypt(key, 32, iv, 12, aad, 20, NULL, 0,
                                        tag, 16, NULL, NULL));
    tag[3] ^= 0x10;
    aad[19] ^= 0x01;
    CHECK(!libspdm_aead_aes_gcm_decrypt(key, 32, iv, 12, aad, 20, NULL, 0,
                                        tag, 16, NULL, NULL));
    CHECK(!libspdm_aead_aes_gcm_encrypt(key, 32, iv, 12, aad, 20, pt, 1,
                                        tag, 16, out, NULL));
}

static void
test_gcm_roundtrip(void *pre)
{
    static const size_t lens[] = { 0, 1, 15, 16, 17, 31, 32, 33, 1000, 4099 };
    uint8_t key[32], iv[12], aad[40], tag[16], bad[16];
    uint8_t *pt, *ct, *out;
    size_t li, tag_size, aad_len, len, out_size;
    bool ok;

    pt = malloc(4099);
    ct = malloc(4099);
    out = malloc(4099);

    for (li = 0; li < sizeof (lens) / sizeof (lens[0]); li++)
    {
        for (tag_size = 12; tag_size <= 16; tag_size++)
        {
            len = lens[li];
            aad_len = (li * 7) % sizeof (aad);
            arc4random_buf(key, sizeof (key));
            arc4random_buf(iv, sizeof (iv));
            arc4random_buf(aad, sizeof (aad));
            arc4random_buf(pt, len);

            out_size = len;
            ok = pre != NULL ?
                libspdm_aead_aes_gcm_encrypt_prealloc(pre, key, 32, iv, 12,
                    aad, aad_len, pt, len, tag, tag_size, ct, &out_size) :
                libspdm_aead_aes_gcm_encrypt(key, 32, iv, 12,
                    aad, aad_len, pt, len, tag, tag_size, ct, &out_size);
            CHECK(ok && out_size == len);

            fill(out, len, 0xee);
            out_size = len;
            ok = pre != NULL ?
                libspdm_aead_aes_gcm_decrypt_prealloc(pre, key, 32, iv, 12,
                    aad, aad_len, ct, len, tag, tag_size, out, &out_size) :
                libspdm_aead_aes_gcm_decrypt(key, 32, iv, 12,
                    aad, aad_len, ct, len, tag, tag_size, out, &out_size);
            CHECK(ok && out_size == len && memcmp(out, pt, len) == 0);

            /* wrong tag: plaintext must never reach data_out */
            memcpy(bad, tag, tag_size);
            bad[tag_size - 1] ^= 1;
            fill(out, len, 0xee);
            out_size = len + 1;
            ok = pre != NULL ?
                libspdm_aead_aes_gcm_decrypt_prealloc(pre, key, 32, iv, 12,
                    aad, aad_len, ct, len, bad, tag_size, out, &out_size) :
                libspdm_aead_aes_gcm_decrypt(key, 32, iv, 12,
                    aad, aad_len, ct, len, bad, tag_size, out, &out_size);
            CHECK(!ok && out_size == len + 1);
            CHECK(len == 0 || memcmp(out, pt, len) != 0);

            /* modified AAD or ciphertext */
            if (aad_len != 0)
            {
                aad[0] ^= 0x80;
                CHECK(!libspdm_aead_aes_gcm_decrypt(key, 32, iv, 12, aad,
                        aad_len, ct, len, tag, tag_size, out, &out_size));
                aad[0] ^= 0x80;
            }
            if (len != 0)
            {
                ct[len / 2] ^= 0x01;
                fill(out, len, 0xee);
                CHECK(!libspdm_aead_aes_gcm_decrypt(key, 32, iv, 12, aad,
                        aad_len, ct, len, tag, tag_size, out, &out_size));
                CHECK(memcmp(out, pt, len) != 0);
                ct[len / 2] ^= 0x01;
            }

            /* in place */
            memcpy(out, pt, len);
            out_size = len;
            CHECK(libspdm_aead_aes_gcm_encrypt(key, 32, iv, 12, aad, aad_len,
                    out, len, bad, tag_size, out, &out_size));
            CHECK(memcmp(out, ct, len) == 0 && memcmp(bad, tag, tag_size) == 0);
            CHECK(libspdm_aead_aes_gcm_decrypt(key, 32, iv, 12, aad, aad_len,
                    out, len, tag, tag_size, out, &out_size));
            CHECK(memcmp(out, pt, len) == 0);
        }
    }

    free(pt);
    free(ct);
    free(out);
}

static void
test_gcm_args(void)
{
    uint8_t key[32] = { 0 }, iv[12] = { 0 }, tag[16], buf[32], out[32];
    size_t out_size;
    void *pre;

    memset(buf, 0x11, sizeof (buf));

    out_size = 32;
    CHECK(!libspdm_aead_aes_gcm_encrypt(key, 31, iv, 12, NULL, 0, buf, 32,
                                        tag, 16, out, &out_size));
    CHECK(!libspdm_aead_aes_gcm_encrypt(key, 32, iv, 16, NULL, 0, buf, 32,
                                        tag, 16, out, &out_size));
    CHECK(!libspdm_aead_aes_gcm_encrypt(key, 32, iv, 12, NULL, 0, buf, 32,
                                        tag, 11, out, &out_size));
    CHECK(!libspdm_aead_aes_gcm_encrypt(key, 32, iv, 12, NULL, 0, buf, 32,
                                        tag, 17, out, &out_size));
    CHECK(!libspdm_aead_aes_gcm_encrypt(key, 32, iv, 12, NULL, 1, buf, 32,
                                        tag, 16, out, &out_size));
    CHECK(!libspdm_aead_aes_gcm_encrypt(key, 32, iv, 12, NULL, 0, buf, 32,
                                        tag, 16, out, NULL));
    CHECK(!libspdm_aead_aes_gcm_encrypt(key, 32, iv, 12, NULL, 0, buf,
                                        (size_t)INT_MAX + 1, tag, 16, out,
                                        &out_size));
    out_size = 31;
    CHECK(!libspdm_aead_aes_gcm_encrypt(key, 32, iv, 12, NULL, 0, buf, 32,
                                        tag, 16, out, &out_size));
    CHECK(out_size == 31);
    out_size = 32;
    CHECK(!libspdm_aead_aes_gcm_decrypt(key, 32, iv, 12, NULL, 0, buf, 32,
                                        NULL, 16, out, &out_size));
    CHECK(!libspdm_aead_aes_gcm_encrypt_prealloc(NULL, key, 32, iv, 12, NULL,
                                        0, buf, 32, tag, 16, out, &out_size));
    CHECK(libspdm_aead_aes_gcm_encrypt(key, 16, iv, 12, NULL, 0, buf, 32,
                                       tag, 16, out, &out_size));
    CHECK(libspdm_aead_aes_gcm_encrypt(key, 24, iv, 12, NULL, 0, buf, 32,
                                       tag, 16, out, &out_size));

    test_mech_invalid = 1;
    CHECK(!libspdm_aead_aes_gcm_encrypt(key, 32, iv, 12, NULL, 0, buf, 32,
                                        tag, 16, out, &out_size));
    test_mech_invalid = 0;

    /* scratch allocation failure fails closed */
    CHECK(libspdm_aead_aes_gcm_encrypt(key, 32, iv, 12, NULL, 0, buf, 32,
                                       tag, 16, out, &out_size));
    CHECK(libspdm_aead_gcm_prealloc(&pre));
    test_kmem_fail_nosleep = 1;
    memset(buf, 0xee, sizeof (buf));
    CHECK(!libspdm_aead_aes_gcm_decrypt(key, 32, iv, 12, NULL, 0, out, 32,
                                        tag, 16, buf, &out_size));
    CHECK(!libspdm_aead_aes_gcm_decrypt_prealloc(pre, key, 32, iv, 12, NULL,
                                        0, out, 32, tag, 16, buf, &out_size));
    CHECK(buf[0] == 0xee && buf[31] == 0xee);
    test_kmem_fail_nosleep = 0;
    CHECK(libspdm_aead_aes_gcm_decrypt_prealloc(pre, key, 32, iv, 12, NULL,
                                        0, out, 32, tag, 16, buf, &out_size));
    CHECK(buf[0] == 0x11 && buf[31] == 0x11);
    libspdm_aead_free(pre);
    libspdm_aead_free(NULL);
    CHECK(!libspdm_aead_gcm_prealloc(NULL));
}

/* Base64 */

static void
test_base64(void)
{
    static const char *const rfc[][2] = {
        { "", "" }, { "f", "Zg==" }, { "fo", "Zm8=" }, { "foo", "Zm9v" },
        { "foob", "Zm9vYg==" }, { "fooba", "Zm9vYmE=" },
        { "foobar", "Zm9vYmFy" },
    };
    static const char *const bad[] = {
        "Zg=", "Z===", "Zg==Zg==", "Zm9v!", "Zg=a", "=Zg=", "Zm9vY", "Z",
        "Zm9v====", "Zm-v", "Zm_v",
    };
    uint8_t src[400], enc[700], dec[400];
    char pem[800];
    size_t i, j, n, len, p;

    for (i = 0; i < sizeof (rfc) / sizeof (rfc[0]); i++)
    {
        len = strlen(rfc[i][1]);
        n = len + 1;
        memset(enc, 0x5a, sizeof (enc));
        CHECK(libspdm_encode_base64((const uint8_t *)rfc[i][0], enc,
                                    strlen(rfc[i][0]), &n));
        CHECK(n == len && memcmp(enc, rfc[i][1], len) == 0);
        CHECK(enc[len] == '\0' && enc[len + 1] == 0x5a);

        /* the NUL needs room too */
        n = len;
        CHECK(!libspdm_encode_base64((const uint8_t *)rfc[i][0], enc,
                                     strlen(rfc[i][0]), &n));
        CHECK(n == 0);

        n = strlen(rfc[i][0]);
        memset(dec, 0x5a, sizeof (dec));
        CHECK(libspdm_decode_base64((const uint8_t *)rfc[i][1], dec, len, &n));
        CHECK(n == strlen(rfc[i][0]) && memcmp(dec, rfc[i][0], n) == 0);
        CHECK(dec[n] == 0x5a);
        if (n != 0)
        {
            n--;
            CHECK(!libspdm_decode_base64((const uint8_t *)rfc[i][1], dec,
                                         len, &n));
        }
    }

    for (i = 0; i < sizeof (bad) / sizeof (bad[0]); i++)
    {
        n = sizeof (dec);
        CHECK(!libspdm_decode_base64((const uint8_t *)bad[i], dec,
                                     strlen(bad[i]), &n));
        CHECK(n == 0);
    }

    /* round trips, compared with OpenSSL, and PEM style line breaks */
    for (len = 0; len < sizeof (src); len++)
    {
        arc4random_buf(src, len);
        n = sizeof (enc);
        CHECK(libspdm_encode_base64(src, enc, len, &n));
        CHECK(n == (size_t)EVP_EncodeBlock((uint8_t *)pem, src, (int)len));
        CHECK(memcmp(enc, pem, n) == 0);

        for (i = 0, p = 0; i < n; i += 64)
        {
            j = n - i < 64 ? n - i : 64;
            memcpy(pem + p, enc + i, j);
            p += j;
            pem[p++] = (i / 64) % 2 ? '\n' : '\r';
            if (pem[p - 1] == '\r')
                pem[p++] = '\n';
        }
        memset(dec, 0, sizeof (dec));
        j = len;
        CHECK(libspdm_decode_base64((const uint8_t *)pem, dec, p, &j));
        CHECK(j == len && memcmp(dec, src, len) == 0);
    }

    n = 16;
    CHECK(!libspdm_encode_base64(src, NULL, 3, &n));
    CHECK(!libspdm_encode_base64(NULL, enc, 3, &n));
    CHECK(!libspdm_encode_base64(src, enc, 3, NULL));
    n = 16;
    CHECK(!libspdm_decode_base64((const uint8_t *)"Zm9v", NULL, 4, &n));
    CHECK(!libspdm_decode_base64(NULL, dec, 4, &n));
    CHECK(!libspdm_decode_base64((const uint8_t *)"Zm9v", dec, 4, NULL));
}

/* ASN.1 */

static void
test_asn1(void)
{
    uint8_t buf[300];
    uint8_t *p, *end;
    size_t len;

    /* SEQUENCE { INTEGER 5 } */
    static const uint8_t seq[] = { 0x30, 0x03, 0x02, 0x01, 0x05 };
    p = (uint8_t *)seq;
    end = p + sizeof (seq);
    CHECK(libspdm_asn1_get_tag(&p, end, &len, LIBSPDM_CRYPTO_ASN1_SEQUENCE |
                               LIBSPDM_CRYPTO_ASN1_CONSTRUCTED));
    CHECK(p == seq + 2 && len == 3);
    CHECK(!libspdm_asn1_get_tag(&p, end, &len, LIBSPDM_CRYPTO_ASN1_OID));
    CHECK(p == seq + 2);
    CHECK(libspdm_asn1_get_tag(&p, end, &len, LIBSPDM_CRYPTO_ASN1_INTEGER));
    CHECK(p == seq + 4 && len == 1 && *p == 5);

    /* long forms */
    buf[0] = 0x04; buf[1] = 0x81; buf[2] = 0x80;
    p = buf;
    CHECK(libspdm_asn1_get_tag(&p, buf + 3 + 0x80, &len, 0x04));
    CHECK(p == buf + 3 && len == 0x80);
    p = buf;
    CHECK(!libspdm_asn1_get_tag(&p, buf + 3 + 0x7f, &len, 0x04));
    CHECK(p == buf);

    buf[0] = 0x30; buf[1] = 0x82; buf[2] = 0x01; buf[3] = 0x00;
    p = buf;
    CHECK(libspdm_asn1_get_tag(&p, buf + 4 + 256, &len, 0x30));
    CHECK(p == buf + 4 && len == 256);
    p = buf;
    CHECK(!libspdm_asn1_get_tag(&p, buf + 4 + 255, &len, 0x30));

    /* huge lengths must not wrap */
    buf[0] = 0x30; buf[1] = 0x84; buf[2] = 0xff; buf[3] = 0xff;
    buf[4] = 0xff; buf[5] = 0xff;
    p = buf;
    CHECK(!libspdm_asn1_get_tag(&p, buf + 10, &len, 0x30));
    buf[1] = 0x85;
    CHECK(!libspdm_asn1_get_tag(&p, buf + 10, &len, 0x30));
    /* nine length octets would shift the leading 01 out of a size_t */
    memset(buf, 0, 20);
    buf[0] = 0x30; buf[1] = 0x89; buf[2] = 0x01; buf[10] = 0x05;
    CHECK(!libspdm_asn1_get_tag(&p, buf + 20, &len, 0x30));
    CHECK(p == buf);
    buf[1] = 0x80;
    CHECK(!libspdm_asn1_get_tag(&p, buf + 10, &len, 0x30));
    buf[1] = 0x82;
    CHECK(!libspdm_asn1_get_tag(&p, buf + 3, &len, 0x30));

    /* truncated and degenerate inputs */
    p = buf;
    CHECK(!libspdm_asn1_get_tag(&p, buf + 1, &len, 0x30));
    CHECK(!libspdm_asn1_get_tag(&p, buf, &len, 0x30));
    p = buf + 2;
    CHECK(!libspdm_asn1_get_tag(&p, buf + 1, &len, 0x30));
    CHECK(p == buf + 2);
    p = buf;
    CHECK(!libspdm_asn1_get_tag(NULL, buf + 10, &len, 0x30));
    CHECK(!libspdm_asn1_get_tag(&p, NULL, &len, 0x30));
    CHECK(!libspdm_asn1_get_tag(&p, buf + 10, NULL, 0x30));
    CHECK(!libspdm_asn1_get_tag(&p, buf + 10, &len, 0x130));
    buf[0] = 0x05; buf[1] = 0x00;
    CHECK(libspdm_asn1_get_tag(&p, buf + 2, &len, 0x05));
    CHECK(len == 0 && p == buf + 2);
}

/* Everything that must fail closed */

static void
test_unsupported(void)
{
    uint8_t buf[64];
    size_t n = sizeof (buf);
    const uint8_t *cert;
    void *ctx = (void *)1;

    CHECK(!libspdm_check_crypto_backend());
    CHECK(libspdm_ec_new_by_nid(LIBSPDM_CRYPTO_NID_SECP384R1) == NULL);
    CHECK(!libspdm_ec_generate_key(NULL, buf, &n));
    CHECK(!libspdm_ec_compute_key(NULL, buf, 96, buf, &n));
    CHECK(!libspdm_ecdsa_sign(NULL, LIBSPDM_CRYPTO_NID_SHA384, buf, 48,
                              buf, &n));
    CHECK(!libspdm_ecdsa_verify(NULL, LIBSPDM_CRYPTO_NID_SHA384, buf, 48,
                                buf, 96));
    CHECK(libspdm_rsa_new() == NULL);
    CHECK(!libspdm_rsa_set_key(NULL, LIBSPDM_RSA_KEY_N, buf, 64));
    CHECK(!libspdm_rsa_pss_sign(NULL, LIBSPDM_CRYPTO_NID_SHA384, buf, 48,
                                buf, &n));
    CHECK(!libspdm_rsa_pss_verify(NULL, LIBSPDM_CRYPTO_NID_SHA384, buf, 48,
                                  buf, 64));
    CHECK(!libspdm_ec_get_public_key_from_x509(buf, 64, &ctx));
    CHECK(!libspdm_rsa_get_public_key_from_x509(buf, 64, &ctx));
    CHECK(!libspdm_x509_verify_cert(buf, 64, buf, 64));
    CHECK(!libspdm_x509_verify_cert_chain(buf, 64, buf, 64));
    CHECK(!libspdm_x509_get_cert_from_cert_chain(buf, 64, 0, &cert, &n));
    CHECK(!libspdm_x509_set_date_time("19700101000000Z", buf, &n));
    CHECK(libspdm_x509_compare_date_time(buf, buf) < 0);
    libspdm_ec_free(NULL);
    libspdm_rsa_free(NULL);
}

static void
test_random(void)
{
    uint8_t buf[64], zero[64] = { 0 };

    CHECK(libspdm_random_bytes(buf, sizeof (buf)));
    CHECK(libspdm_random_bytes(NULL, 0));
    CHECK(!libspdm_random_bytes(NULL, 1));
    test_rand_fail = 1;
    CHECK(!libspdm_random_bytes(buf, sizeof (buf)));
    CHECK(memcmp(buf, zero, sizeof (buf)) == 0);
    test_rand_fail = 0;
}

int
main(void)
{
    void *pre;

    test_sha();
    test_hmac_rfc4231();
    test_hmac_semantics();
    test_hmac_openssl();
    test_hkdf_rfc5869();
    test_hkdf_openssl();
    test_gcm_vector();
    test_gcm_roundtrip(NULL);
    CHECK(libspdm_aead_gcm_prealloc(&pre));
    test_gcm_roundtrip(pre);
    libspdm_aead_free(pre);
    test_gcm_args();
    test_base64();
    test_asn1();
    test_unsupported();
    test_random();

    CHECK(test_kmem_outstanding == 0);

    printf("%d checks, %d failures, %d KCF encrypt calls\n", checks, failures,
           test_encrypt_calls);
    if (failures != 0)
    {
        printf("FAIL\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
