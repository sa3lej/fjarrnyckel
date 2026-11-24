// Training mode: on its USB port the box is a keyboard and a MIDI device, the two ways Morse practice sites
// take a real key. It follows the conventions of the VBand and Vail adapters, so those sites need no setup:
//   keyboard   dit = Left Ctrl, dah = Right Ctrl; an already-keyed signal (the box's own keyer, or a straight
//              key) = Left Ctrl
//   MIDI       note 1 = dit, note 2 = dah, note 0 = already keyed; channel 1
// The box starts as a keyboard. A site that speaks MIDI switches it over (control change 0) and may ask for
// the raw paddles (program change 0), a keyer type or a speed; those requests last until the box restarts.
// See github.com/Vail-CW/vail-adapter, docs/MIDI_INTEGRATION_SPEC.md.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"
#include "class/midi/midi_device.h"
#include "fn.h"

static const char *TAG = "usb";
enum { ITF_HID, ITF_MIDI, ITF_MIDI_STREAMING, ITF_COUNT };
static const uint8_t hid_report[] = { TUD_HID_REPORT_DESC_KEYBOARD() };
static const uint8_t config_desc[] = {
  TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_MIDI_DESC_LEN, 0, 100),
  TUD_HID_DESCRIPTOR(ITF_HID, 0, HID_ITF_PROTOCOL_KEYBOARD, sizeof hid_report, 0x81, 8, 1),
  TUD_MIDI_DESCRIPTOR(ITF_MIDI, 0, 0x02, 0x82, 64),
};
uint8_t const *tud_hid_descriptor_report_cb(uint8_t itf) { return hid_report; }
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t id, hid_report_type_t type, uint8_t *buf, uint16_t len) { return 0; }
void tud_hid_set_report_cb(uint8_t itf, uint8_t id, hid_report_type_t type, uint8_t const *buf, uint16_t len) {}

static volatile bool ready, midi_mode;
static volatile int host_raw = -1, host_iambic = -1, host_wpm;   // what a MIDI site asked for; -1 / 0 = nothing
static uint8_t sent_keys, sent_notes;                             // what the computer has been told so far

bool usb_connected(void) { return ready && tud_mounted(); }
bool usb_midi_mode(void) { return midi_mode; }
bool usb_raw(void) { return host_raw >= 0 ? host_raw : settings.usb_raw; }
int usb_iambic(void) { return host_iambic >= 0 ? host_iambic : settings.iambic_b; }
int usb_wpm(void) { return host_wpm ? host_wpm : settings.wpm; }

// Called every millisecond by the keyer with what the computer should see now. Nothing here waits: a change
// that cannot go out this millisecond goes out the next.
void usb_key(bool dit, bool dah, bool keyed) {
  if (!ready || !tud_mounted()) { sent_keys = sent_notes = 0; return; }
  uint8_t notes = (keyed ? 1 : 0) | (dit ? 2 : 0) | (dah ? 4 : 0);
  uint8_t keys = midi_mode ? 0 : ((keyed || dit) ? KEYBOARD_MODIFIER_LEFTCTRL : 0) | (dah ? KEYBOARD_MODIFIER_RIGHTCTRL : 0);
  if (keys != sent_keys && tud_hid_ready() && tud_hid_keyboard_report(0, keys, NULL)) sent_keys = keys;
  if (!midi_mode) notes = 0;
  for (int n = 0; n < 3 && notes != sent_notes; n++) {
    uint8_t bit = 1 << n;
    if (!((notes ^ sent_notes) & bit)) continue;
    uint8_t msg[3] = { (notes & bit) ? 0x90 : 0x80, n, (notes & bit) ? 0x7F : 0x00 };
    if (tud_midi_stream_write(0, msg, 3) == 3) sent_notes = (sent_notes & ~bit) | (notes & bit);
  }
}

static void midi_task(void *arg) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5));
    uint8_t p[4];
    while (ready && tud_midi_available() && tud_midi_packet_read(p)) {
      uint8_t kind = p[1] & 0xF0;
      if (kind == 0xB0 && p[2] == 0) { midi_mode = p[3] <= 0x3F; ESP_LOGI(TAG, "a site switched the box to %s", midi_mode ? "MIDI" : "keyboard"); }
      else if (kind == 0xB0 && p[2] == 1 && p[3]) { int wpm = 1200 / (p[3] * 2); host_wpm = wpm < 5 ? 5 : wpm > 60 ? 60 : wpm; }
      else if (kind == 0xC0) { host_raw = p[2] == 0; if (p[2] == 7) host_iambic = 0; else if (p[2] == 8) host_iambic = 1; }
    }
  }
}

void usb_start(void) {
  tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
  cfg.descriptor.full_speed_config = config_desc;
  if (tinyusb_driver_install(&cfg) != ESP_OK) { ESP_LOGE(TAG, "the USB port could not be set up as a key"); return; }
  ready = true;
  xTaskCreatePinnedToCore(midi_task, "usbmidi", 3072, NULL, 4, NULL, 0);
}
