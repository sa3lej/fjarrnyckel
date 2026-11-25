// Prints packets from the firmware's CWL2 client as hex, one a line: a KEY packet, then the COMMAND packets a
// command produces over two seconds of 10 ms steps, then a KEY packet again. tests/test_c_client.py reads them with
// the receiver's own code.
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <stdio.h>
#include <string.h>
#include "cwl2.h"

void cwl2_hmac(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[32]) {
  unsigned int n = 32;
  HMAC(EVP_sha256(), key, (int)key_len, data, len, out, &n);
}

static void hex(const uint8_t *pkt, size_t n) { for (size_t i = 0; i < n; i++) printf("%02x", pkt[i]); printf("\n"); }

int main(int argc, char **argv) {
  cwl2_t c; cwl2_init(&c, (const uint8_t *)argv[1], strlen(argv[1]), 0x1234);
  uint8_t pkt[CWL2_MAX_PACKET];
  hex(pkt, cwl2_packet(&c, 1000000, pkt));
  cwl2_command(&c, CWL2_CMD_SET_FREQ, 7030000, 2000000);
  for (uint64_t now = 2000000; now < 4000000; now += 10000)
    if (cwl2_command_due(&c, now)) hex(pkt, cwl2_command_packet(&c, now, pkt));
  hex(pkt, cwl2_packet(&c, 4000000, pkt));
  return 0;
}
