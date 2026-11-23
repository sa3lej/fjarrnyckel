// Checks that a firmware copy in flash carries a valid signature from the Fjärrnyckel project.
// A signed file is the firmware image followed by "FNSIG001" and a 64-byte ECDSA P-256 signature over the
// SHA-256 of the image (see tools/sign_firmware.py).
#include <string.h>
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "psa/crypto.h"
#include "fn.h"
#include "signing_key.h"

bool firmware_signed(const void *partition) {
  const esp_partition_t *part = partition;
  if (!part) return false;
  esp_image_metadata_t meta;
  const esp_partition_pos_t pos = { .offset = part->address, .size = part->size };
  if (esp_image_get_metadata(&pos, &meta) != ESP_OK || meta.image_len + 72 > part->size) return false;
  uint8_t trailer[72];
  if (esp_partition_read(part, meta.image_len, trailer, sizeof trailer) != ESP_OK || memcmp(trailer, "FNSIG001", 8)) return false;

  psa_crypto_init();
  psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
  if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) return false;
  static uint8_t buf[4096];
  for (uint32_t at = 0; at < meta.image_len; ) {
    uint32_t n = meta.image_len - at < sizeof buf ? meta.image_len - at : sizeof buf;
    if (esp_partition_read(part, at, buf, n) != ESP_OK || psa_hash_update(&op, buf, n) != PSA_SUCCESS) { psa_hash_abort(&op); return false; }
    at += n;
  }
  uint8_t hash[32]; size_t len = 0;
  if (psa_hash_finish(&op, hash, sizeof hash, &len) != PSA_SUCCESS) return false;

  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
  psa_set_key_bits(&attr, 256);
  psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
  psa_key_id_t key;
  if (psa_import_key(&attr, signing_key, sizeof signing_key, &key) != PSA_SUCCESS) { ESP_LOGE("sign", "the signing key could not be loaded"); return false; }
  psa_status_t ok = psa_verify_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, sizeof hash, trailer + 8, 64);
  psa_destroy_key(key);
  return ok == PSA_SUCCESS;
}
