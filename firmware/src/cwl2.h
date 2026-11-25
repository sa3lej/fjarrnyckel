// CWL2 client: the operator's end of the remote CW keying link. The packet format and the rules are
// described in ../../receiver/cwproto.py and ../../receiver/cwsend.py; this is the same client in C, free of any platform code
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

// The newest receiver version this firmware knows (RECEIVER_VERSION in receiver/cwproto.py). A receiver that
// reports a lower one, or 0 (from before it had a version), is out of date and the page says so.
#define CWL2_RECEIVER_LATEST 4

// Things for the radio that are not keying (receivers from version 3). They travel in packets of their own, so the
// KEY packets and everything about their timing stay exactly as they are without them.
#define CWL2_CMD_SET_MODE 1          // arg: the mode as its CI-V byte: 0 LSB, 1 USB, 3 CW
#define CWL2_CMD_SET_FREQ 2          // arg: the frequency in Hz
#define CWL2_CMD_TUNE     3          // start the antenna tuner; the radio transmits while it tunes
#define CWL2_COMMAND_LEN  52

// What the receiver tells about the radio every two seconds (receivers from version 4), in a packet of its own.
#define CWL2_INFO_LEN     44
#define CWL2_CAP_CONTROL  0x01       // mode and frequency can be set from the box
#define CWL2_CAP_TUNER    0x02       // the radio has an antenna tuner to start

typedef struct {
  uint8_t flags, mode, receiver;     // receiver: its version, 0 = from before it had one
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
  uint8_t command; uint16_t command_seq; uint32_t command_arg;   // the newest command, sent again until command_until
  uint64_t command_until, command_next;
  struct { uint32_t index; uint64_t t; bool down; } edges[64];   // oldest first
  cwl2_status_t status;
  bool has_info; uint8_t caps; uint16_t bands; char model[17];   // from the newest INFO packet of our session
  int64_t rtt_us;                    // last measured round trip, -1 before the first
} cwl2_t;

void cwl2_init(cwl2_t *c, const uint8_t *key, size_t key_len, uint32_t session);
void cwl2_edge(cwl2_t *c, uint64_t t_us, bool down);            // the key changed at t_us on our clock
void cwl2_command(cwl2_t *c, uint8_t command, uint32_t arg, uint64_t now);   // carried out once by the receiver
bool cwl2_command_due(const cwl2_t *c, uint64_t now);           // true when a command packet should go out
size_t cwl2_command_packet(cwl2_t *c, uint64_t now, uint8_t *out);   // builds it (CWL2_COMMAND_LEN bytes)
bool cwl2_due(cwl2_t *c, uint64_t now);                         // true when a packet should go out
size_t cwl2_packet(cwl2_t *c, uint64_t now, uint8_t *out);      // builds the next packet, returns its length
bool cwl2_on_status(cwl2_t *c, const uint8_t *data, size_t len, uint64_t now);
bool cwl2_on_info(cwl2_t *c, const uint8_t *data, size_t len);
bool cwl2_linked(const cwl2_t *c);                              // the receiver runs our session and will key

// HMAC-SHA256, supplied by the platform.
void cwl2_hmac(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[32]);
