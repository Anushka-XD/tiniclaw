/*
 * tiniclaw-c — secrets (ChaCha20-Poly1305 via OpenSSL 3)
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include "security.h"
#include "util.h"
#include "platform.h"

/* ── ChaCha20-Poly1305 ──────────────────────────────────────────── */

int nc_chacha20_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                        const uint8_t *plain, size_t plain_len, uint8_t *out) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;
    int rc = -1;
    if (!EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL)) goto done;
    if (!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL)) goto done;
    if (!EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce)) goto done;
    int outl = 0;
    if (!EVP_EncryptUpdate(ctx, out, &outl, plain, (int)plain_len)) goto done;
    int finl = 0;
    if (!EVP_EncryptFinal_ex(ctx, out + outl, &finl)) goto done;
    /* Append 16-byte auth tag */
    if (!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, out + outl + finl)) goto done;
    rc = 0;
done:
    EVP_CIPHER_CTX_free(ctx);
    return rc;
}

int nc_chacha20_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                        const uint8_t *cipher, size_t cipher_len, uint8_t *out) {
    if (cipher_len < 16) return -1;
    size_t data_len = cipher_len - 16;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;
    int rc = -1;
    if (!EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL)) goto done;
    if (!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL)) goto done;
    if (!EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce)) goto done;
    int outl = 0;
    if (!EVP_DecryptUpdate(ctx, out, &outl, cipher, (int)data_len)) goto done;
    /* Set tag */
    if (!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16,
                               (void *)(cipher + data_len))) goto done;
    int finl = 0;
    if (!EVP_DecryptFinal_ex(ctx, out + outl, &finl)) goto done; /* tag check here */
    rc = 0;
done:
    EVP_CIPHER_CTX_free(ctx);
    return rc;
}

void nc_hmac_sha256(const uint8_t *key, size_t key_len,
                    const uint8_t *msg, size_t msg_len, uint8_t out[32]) {
    unsigned int outl = 32;
    HMAC(EVP_sha256(), key, (int)key_len, msg, msg_len, out, &outl);
}

/* ── Key file management ────────────────────────────────────────── */

void nc_secret_store_init(NcSecretStore *s, const char *dir, bool enabled) {
    snprintf(s->key_path, sizeof s->key_path, "%s/.secret_key", dir);
    s->enabled = enabled;
}

static int load_or_create_key(const NcSecretStore *s, uint8_t key[32]) {
    FILE *f = fopen(s->key_path, "rb");
    if (f) {
        size_t n = fread(key, 1, 32, f);
        fclose(f);
        return n == 32 ? 0 : -1;
    }
    /* Create new key */
    nc_random_bytes(key, 32);
    f = fopen(s->key_path, "wb");
    if (!f) return -1;
    fwrite(key, 1, 32, f);
    fclose(f);
    chmod(s->key_path, 0600);
    return 0;
}

char *nc_secret_store_encrypt(const NcSecretStore *s, const char *plaintext) {
    if (!s->enabled || !plaintext || !*plaintext)
        return nc_strdup(plaintext ? plaintext : "");

    uint8_t key[32];
    if (load_or_create_key(s, key) != 0) return nc_strdup(plaintext);

    uint8_t nonce[NC_NONCE_LEN];
    nc_random_bytes(nonce, sizeof nonce);

    size_t plain_len = strlen(plaintext);
    size_t ct_len = plain_len + NC_TAG_LEN;
    uint8_t *ct = malloc(ct_len);
    if (!ct) return nc_strdup(plaintext);

    if (nc_chacha20_encrypt(key, nonce, (const uint8_t *)plaintext, plain_len, ct) != 0) {
        free(ct);
        return nc_strdup(plaintext);
    }

    /* Encode: enc2:<hex(nonce || ciphertext+tag)> */
    size_t blob_len = NC_NONCE_LEN + ct_len;
    uint8_t *blob = malloc(blob_len);
    memcpy(blob, nonce, NC_NONCE_LEN);
    memcpy(blob + NC_NONCE_LEN, ct, ct_len);
    free(ct);

    char *hex = malloc(blob_len * 2 + 1);
    nc_hex_encode(blob, blob_len, hex);
    free(blob);

    size_t out_len = 5 + blob_len * 2 + 1;
    char *out = malloc(out_len);
    snprintf(out, out_len, "enc2:%s", hex);
    free(hex);
    return out;
}

char *nc_secret_store_decrypt(const NcSecretStore *s, const char *ciphertext) {
    if (!ciphertext) return NULL;
    if (strncmp(ciphertext, NC_ENC_PREFIX, 5) != 0) return nc_strdup(ciphertext);
    if (!s->enabled) return nc_strdup(ciphertext + 5);  /* strip prefix passthrough */

    const char *hex = ciphertext + 5;
    size_t hex_len = strlen(hex);
    size_t blob_len = hex_len / 2;
    uint8_t *blob = malloc(blob_len);
    if (nc_hex_decode(hex, hex_len, blob, blob_len) < 0) { free(blob); return nc_strdup(""); }

    if (blob_len < NC_NONCE_LEN + NC_TAG_LEN) { free(blob); return nc_strdup(""); }

    uint8_t key[32];
    if (load_or_create_key(s, key) != 0) { free(blob); return nc_strdup(""); }

    uint8_t nonce[NC_NONCE_LEN];
    memcpy(nonce, blob, NC_NONCE_LEN);
    const uint8_t *ct = blob + NC_NONCE_LEN;
    size_t ct_len = blob_len - NC_NONCE_LEN;
    size_t plain_len = ct_len - NC_TAG_LEN;

    uint8_t *plain = malloc(plain_len + 1);
    if (nc_chacha20_decrypt(key, nonce, ct, ct_len, plain) != 0) {
        free(blob); free(plain);
        return nc_strdup("");
    }
    plain[plain_len] = '\0';
    free(blob);
    char *result = nc_strdup((char *)plain);
    free(plain);
    return result;
}
