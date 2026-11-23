// Fjärrnyckel: shared declarations.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// ---- wiring (ESP32-S3-DevKitC-1) ----
#define PIN_TIP   4    // key jack tip: straight key, or one paddle
#define PIN_RING  5    // key jack ring: the other paddle
#define PIN_BCLK  15   // MAX98357A
#define PIN_LRC   16
#define PIN_DIN   17

// ---- settings (kept in flash) ----
typedef struct {
  char ssid[33];
  char pass[65];
  uint8_t key_type;     // 0 = straight key, 1 = paddle
  uint8_t swap;         // paddle: 1 = ring is dit, tip is dah
  uint8_t wpm;          // paddle speed
  uint16_t tone_hz;     // sidetone pitch
  uint8_t volume;       // sidetone volume, 0-100
  uint8_t reserved;
  // New fields go at the end: an older, shorter stored copy then still loads and the rest keep their defaults.
  char link_host[64];   // the receiver: a name or an address
  uint16_t link_port;
  char link_key[96];    // the shared key, the same text as the receiver's key file
  uint8_t iambic_b;     // paddle: 1 = iambic mode B, 0 = mode A
} settings_t;
extern settings_t settings;
void settings_load(void);
void settings_save(void);

// ---- sidetone ----
void audio_start(void);
void audio_beep(int hz, int ms);          // a beep outside keying, e.g. at power-on

// ---- keyer ----
extern volatile bool key_down;            // what the sidetone follows and what goes to the radio
void keyer_start(void);

// ---- network ----
typedef enum { NET_NO_SETUP, NET_CONNECTING, NET_CONNECTED, NET_FAILED } net_state_t;
net_state_t net_state(void);
const char *net_ip(void);                 // our address on the Wi-Fi network, "" when not connected
const char *net_ap_name(void);            // name of the setup network, "" when it is off
const char *net_fail_reason(void);
void net_start(void);
void net_join(const char *ssid, const char *pass);   // save and try new Wi-Fi details
int net_scan(char *json, int size);       // writes a JSON array of networks in range

// ---- the keying link to the receiver ----
typedef enum { LINK_OFF, LINK_NO_WIFI, LINK_NOT_FOUND, LINK_NO_ANSWER, LINK_WAITING, LINK_READY } link_state_t;
typedef struct {
  link_state_t state;
  int rtt_ms;            // round trip to the receiver, -1 when unknown
  uint32_t freq_hz;      // the radio's frequency, 0 when unknown
  bool rig_valid, cw;    // the receiver knows the radio's mode; the radio is in CW
  uint16_t late, lost;   // key edges that reached the receiver too late, or not at all
} link_info_t;
void link_start(void);
void link_restart(void);                  // after the link settings changed
void link_edge(int64_t t_us, bool down);  // called by the keyer on every key change
link_info_t link_info(void);

void web_start(void);
void update_started_ok(void);             // tells the bootloader this firmware works
const char *firmware_version(void);
