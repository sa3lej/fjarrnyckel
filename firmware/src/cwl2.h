// CWL2 client: the operator's end of the remote CW keying link. The packet format and the rules are
// described in ../cwproto.py and ../cwsend.py; this is the same client in C, free of any platform code
// so it also builds on a computer for the tests in ../tests/.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CWL2_MAX_PACKET 120          // 40-byte header, 8 edges of 8 bytes, 16-byte tag
#define CWL2_STATUS_LEN 64

#define CWL2_ST_RIG_VALID 0x01       // mode and frequency are valid
#define CWL2_ST_CW_MODE   0x02       // the radio is in CW: keying is allowed
#define CWL2_ST_KEY_DOWN  0x08
#define CWL2_ST_NONCE_BAD 0x20
#define CWL2_ST_MUTED     0x40       // the receiver is waiting for a word gap

typedef struct {
  uint8_t flags, mode;
  uint32_t session, seq, echo_age, freq_hz;
  uint64_t echo_t, nonce;
  uint16_t delay_ms, late, lost, rx_pkts;
} cwl2_status_t;

typedef struct {
  uint8_t key[96]; size_t key_len;
  uint32_t session, seq, next_index;
  uint64_t nonce, last_edge, last_sent, repeats[2];
  bool key_down, has_edge, has_sent, has_status;
  uint8_t wpm, n_repeats, n_edges;
  uint16_t delay_ms;
  struct { uint32_t index; uint64_t t; bool down; } edges[64];   // oldest first
  cwl2_status_t status;
  int64_t rtt_us;                    // last measured round trip, -1 before the first
} cwl2_t;

void cwl2_init(cwl2_t *c, const uint8_t *key, size_t key_len, uint32_t session);
void cwl2_edge(cwl2_t *c, uint64_t t_us, bool down);            // the key changed at t_us on our clock
bool cwl2_due(cwl2_t *c, uint64_t now);                         // true when a packet should go out
size_t cwl2_packet(cwl2_t *c, uint64_t now, uint8_t *out);      // builds the next packet, returns its length
bool cwl2_on_status(cwl2_t *c, const uint8_t *data, size_t len, uint64_t now);
bool cwl2_linked(const cwl2_t *c);                              // the receiver runs our session and will key

// HMAC-SHA256, supplied by the platform.
void cwl2_hmac(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[32]);
