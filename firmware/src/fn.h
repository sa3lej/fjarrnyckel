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
#define PIN_HP_BCLK 12  // second MAX98357A, for headphones
#define PIN_HP_LRC  13
#define PIN_HP_DIN  14

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
  uint8_t ts_on;        // 1 = join the owner's Tailscale network
  char ts_key[128];     // optional Tailscale auth key; empty = sign in through the setup page
  uint8_t page_lock;    // 1 = the setup page asks for a password
  uint8_t page_salt[8]; // the password itself is not kept, only SHA-256(salt + password)
  uint8_t page_hash[32];
  uint8_t led_off;      // 1 = the status light stays dark
  uint8_t tones_off;    // 1 = no tones when keying comes on or goes off
  uint8_t key_target;   // 0 = the key goes to the radio, 1 = to the computer on the USB port (training)
  uint8_t usb_raw;      // training with a paddle: 1 = send the two paddles as they are, the site does the keying
  uint8_t hp_volume;    // sidetone volume in the headphones, 0-100
  uint16_t ts_region;   // Tailscale relay region the box calls home; 0 = follow the receiver
  uint16_t ts_region_auto;  // the receiver's region as last learned, used when ts_region is 0
  uint8_t radio_volume; // sound from the computer (the radio, through wfview) in the headphones, 0-100
  uint8_t led_level;    // status light brightness 1-100, 10 = the original level; 0 (older copies) = 10
} settings_t;
extern settings_t settings;
void settings_load(void);
void settings_save(void);
// A sealed backup of every setting and the Tailscale identity: returns its length, or -1.
int settings_backup(const char *pass, uint8_t *out, size_t size);
// Takes everything from a backup: 0, -1 not a backup, -2 wrong passphrase or changed, -3 could not store it.
// The box must restart afterwards for Wi-Fi, Tailscale and the link to use it.
int settings_restore(const char *pass, const uint8_t *file, size_t len);

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
typedef enum { LINK_OFF, LINK_NO_WIFI, LINK_NO_TAILSCALE, LINK_NOT_FOUND, LINK_NO_ANSWER, LINK_WAITING, LINK_READY } link_state_t;
typedef struct {
  link_state_t state;
  int rtt_ms;            // round trip to the receiver, -1 when unknown
  uint32_t freq_hz;      // the radio's frequency, 0 when unknown
  bool rig_valid, cw;    // the receiver knows the radio's mode; the radio is in CW
  uint16_t late, lost;   // key edges that reached the receiver too late, or not at all
  bool tailscale;        // the link runs through Tailscale
} link_info_t;
void link_start(void);
void link_restart(void);                  // after the link settings changed
void link_edge(int64_t t_us, bool down);  // called by the keyer on every key change
link_info_t link_info(void);
bool link_uses_tailscale(void);          // the link holds a socket on the Tailscale network

// ---- Tailscale ----
typedef enum { TS_OFF, TS_NO_WIFI, TS_CONNECTING, TS_NEEDS_LOGIN, TS_CONNECTED, TS_AUTH_FAILED, TS_ERROR } ts_state_t;
void ts_start(void);
void ts_apply(bool forget_identity);      // after the Tailscale settings changed; true also erases the box's identity there
ts_state_t ts_state(void);
bool ts_crashed(void);                    // Tailscale was switched off because the box crashed while starting it
const char *ts_ip(void);                  // our address on the tailnet, "" when not connected
const char *ts_login_url(void);
int ts_home_region(void);                 // the relay region in use, 0 = not known yet
int ts_peer_region(void);                 // the receiver's relay region, 0 = not known
int ts_regions(char *json, int size);     // the regions Tailscale offers, as a JSON array of {id, code}           // where the owner signs in to add the box, "" when not asked for
void *ts_handle(void);                    // the MicroLink handle while the link may use it, else NULL
bool ts_is_tailnet_address(uint32_t host_order);

void web_start(void);
// ---- the USB port as a key for Morse practice sites ----
void usb_start(void);
void usb_key(bool dit, bool dah, bool keyed);   // every millisecond: what the computer should see now
bool usb_connected(void);
bool usb_midi_mode(void);                 // a site has switched the box from keyboard to MIDI
void usb_keyboard_mode(void);             // back to a keyboard and the box's own keyer settings
bool usb_audio_playing(void);             // the computer is playing sound to the box
int usb_audio_report(char *out, int size); // the sound card's counters as JSON, for the status
void usb_audio_take(int16_t *out, int n); // n samples of that sound at 48 kHz; silence when there is none
bool usb_raw(void);                       // the paddles go out as they are
int usb_iambic(void);                     // keyer mode and speed to use in training: a MIDI site may ask for its own
int usb_wpm(void);

// ---- status light and tones, service mode, signatures ----
void led_start(void);
void led_show(int r, int g, int b);
void status_start(void);
void service_check(void);                 // at start: the key held down asks for service mode or a full erase
bool service_mode(void);
bool firmware_signed(const void *partition);                 // the firmware copy carries the project's signature

void update_started_ok(void);             // tells the bootloader this firmware works
const char *firmware_version(void);
typedef struct {
  char latest[16];       // newest released version, "" until the box has looked
  bool checked, checking, available;
  bool is_signed;        // the running firmware carries the project's signature
  int progress;          // -1 = nothing is being installed, else percent
  char error[64];
} update_info_t;
void update_start(void);
void update_check(void);                  // look for a new version now
void update_install(void);                // download and install the newest version, then restart
update_info_t update_info(void);
