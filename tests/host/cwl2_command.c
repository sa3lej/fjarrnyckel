// Prints three packets from the firmware's CWL2 client as hex: before a command, right after it was given, and
// after its two seconds are over. tests/test_c_client.py reads them with the receiver's own code.
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <stdio.h>
#include <string.h>
#include "cwl2.h"

void cwl2_hmac(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[32]) {
  unsigned int n = 32;
  HMAC(EVP_sha256(), key, (int)key_len, data, len, out, &n);
}

static void show(cwl2_t *c, uint64_t now) {
  uint8_t pkt[CWL2_MAX_PACKET]; size_t n = cwl2_packet(c, now, pkt);
  for (size_t i = 0; i < n; i++) printf("%02x", pkt[i]);
  printf("\n");
}

int main(int argc, char **argv) {
  cwl2_t c; cwl2_init(&c, (const uint8_t *)argv[1], strlen(argv[1]), 0x1234);
  show(&c, 1000000);
  cwl2_command(&c, CWL2_CMD_SELECT_CW, 2000000);
  show(&c, 2100000);
  show(&c, 2000000 + 2500000);
  return 0;
}
