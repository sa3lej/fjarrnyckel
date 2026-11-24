// Sealing and opening backup files; see backup.h for the format.
#include <stdbool.h>
#include <string.h>
#include "psa/crypto.h"
#include "backup.h"

static const char MAGIC[8] = { 'F', 'N', 'B', 'A', 'C', 'K', 'U', 'P' };

static void wipe(void *p, size_t n) { volatile uint8_t *v = p; while (n--) *v++ = 0; }

// PBKDF2-HMAC-SHA256 with one output block, which is all a 256-bit key needs.
static bool derive(const char *pass, const uint8_t salt[16], psa_key_id_t *aes) {
  size_t plen = strlen(pass), n;
  if (!plen) return false;
  psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&a, PSA_KEY_TYPE_HMAC);
  psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE);
  psa_set_key_algorithm(&a, PSA_ALG_HMAC(PSA_ALG_SHA_256));
  psa_key_id_t mac;
  if (psa_import_key(&a, (const uint8_t *)pass, plen, &mac) != PSA_SUCCESS) return false;
  uint8_t in[20], u[32], next[32], key[32];
  memcpy(in, salt, 16); in[16] = in[17] = in[18] = 0; in[19] = 1;
  bool ok = psa_mac_compute(mac, PSA_ALG_HMAC(PSA_ALG_SHA_256), in, sizeof in, u, sizeof u, &n) == PSA_SUCCESS;
  memcpy(key, u, 32);
  for (int i = 1; ok && i < BACKUP_ROUNDS; i++) {
    ok = psa_mac_compute(mac, PSA_ALG_HMAC(PSA_ALG_SHA_256), u, sizeof u, next, sizeof next, &n) == PSA_SUCCESS;
    memcpy(u, next, 32);
    for (int j = 0; j < 32; j++) key[j] ^= u[j];
    if (i % 2000 == 0) backup_yield();
  }
  psa_destroy_key(mac);
  if (ok) {
    psa_key_attributes_t k = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&k, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&k, 256);
    psa_set_key_usage_flags(&k, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&k, PSA_ALG_GCM);
    ok = psa_import_key(&k, key, sizeof key, aes) == PSA_SUCCESS;
  }
  wipe(key, sizeof key); wipe(u, sizeof u); wipe(next, sizeof next);
  return ok;
}

int backup_seal(const char *pass, const uint8_t *plain, size_t len, uint8_t *out, size_t size) {
  if (size < len + BACKUP_OVERHEAD || psa_crypto_init() != PSA_SUCCESS) return -1;
  memcpy(out, MAGIC, 8);
  out[8] = 1; out[9] = out[10] = out[11] = 0;
  if (psa_generate_random(out + 12, 28) != PSA_SUCCESS) return -1;     // salt and nonce
  psa_key_id_t aes;
  if (!derive(pass, out + 12, &aes)) return -1;
  size_t n = 0;
  psa_status_t s = psa_aead_encrypt(aes, PSA_ALG_GCM, out + 28, 12, out, BACKUP_HEAD, plain, len, out + BACKUP_HEAD, size - BACKUP_HEAD, &n);
  psa_destroy_key(aes);
  return s == PSA_SUCCESS ? (int)(BACKUP_HEAD + n) : -1;
}

int backup_open(const char *pass, const uint8_t *file, size_t len, uint8_t *plain, size_t size) {
  if (len < BACKUP_OVERHEAD || memcmp(file, MAGIC, 8) || file[8] != 1 || psa_crypto_init() != PSA_SUCCESS) return -1;
  psa_key_id_t aes;
  if (!derive(pass, file + 12, &aes)) return -2;
  size_t n = 0;
  psa_status_t s = psa_aead_decrypt(aes, PSA_ALG_GCM, file + 28, 12, file, BACKUP_HEAD, file + BACKUP_HEAD, len - BACKUP_HEAD, plain, size, &n);
  psa_destroy_key(aes);
  return s == PSA_SUCCESS ? (int)n : -2;
}
