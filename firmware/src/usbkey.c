// The box's USB port: a keyboard and a MIDI device for Morse practice sites (training mode), and a sound card
// that plays the computer's audio (the radio, through wfview) in the headphones together with the sidetone.
//
// Training mode: the keyboard and MIDI device are the two ways Morse practice sites take a real key. It follows the conventions of the VBand and Vail adapters, so those sites need no setup:
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
#include "class/audio/audio_device.h"
#include "device/usbd_pvt.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "fn.h"

static const char *TAG = "usb";
enum { ITF_HID, ITF_MIDI, ITF_MIDI_STREAMING, ITF_AUDIO_CONTROL, ITF_AUDIO_STREAMING, ITF_COUNT };
#define AUDIO_RATE 48000
// The sound card: TinyUSB's TUD_AUDIO20_SPEAKER_MONO_FB_DESCRIPTOR without its feedback endpoint, and with an
// adaptive instead of an asynchronous data endpoint. On this chip an isochronous IN packet is only delivered in
// the very frame it was loaded for; macOS polls the feedback endpoint at its own pace, the feedback transfers
// failed, and macOS stopped sending after 64 ms (seen on the box in 0.11.2: 63 packets, then the OUT endpoint
// armed and waiting, the feedback endpoint disabled). Adaptive means the box follows the computer's clock
// instead; usb_audio_take() does that with a gentle resampler.
#define SPEAKER_DESC_LEN (TUD_AUDIO20_DESC_IAD_LEN + TUD_AUDIO20_DESC_STD_AC_LEN + TUD_AUDIO20_DESC_CS_AC_LEN \
  + TUD_AUDIO20_DESC_CLK_SRC_LEN + TUD_AUDIO20_DESC_INPUT_TERM_LEN + TUD_AUDIO20_DESC_OUTPUT_TERM_LEN \
  + TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(1) + TUD_AUDIO20_DESC_STD_AS_LEN + TUD_AUDIO20_DESC_STD_AS_LEN \
  + TUD_AUDIO20_DESC_CS_AS_INT_LEN + TUD_AUDIO20_DESC_TYPE_I_FORMAT_LEN + TUD_AUDIO20_DESC_STD_AS_ISO_EP_LEN \
  + TUD_AUDIO20_DESC_CS_AS_ISO_EP_LEN)
#define SPEAKER_DESCRIPTOR(_itfnum, _epoutsize) \
  TUD_AUDIO20_DESC_IAD(_itfnum, 0x02, 0x00), \
  TUD_AUDIO20_DESC_STD_AC(_itfnum, 0x00, 0x00), \
  TUD_AUDIO20_DESC_CS_AC(0x0200, AUDIO20_FUNC_DESKTOP_SPEAKER, TUD_AUDIO20_DESC_CLK_SRC_LEN + TUD_AUDIO20_DESC_INPUT_TERM_LEN \
    + TUD_AUDIO20_DESC_OUTPUT_TERM_LEN + TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(1), AUDIO20_CS_AS_INTERFACE_CTRL_LATENCY_POS), \
  TUD_AUDIO20_DESC_CLK_SRC(0x04, AUDIO20_CLOCK_SOURCE_ATT_INT_FIX_CLK, (AUDIO20_CTRL_R << AUDIO20_CLOCK_SOURCE_CTRL_CLK_FRQ_POS), 0x01, 0x00), \
  TUD_AUDIO20_DESC_INPUT_TERM(0x01, AUDIO_TERM_TYPE_USB_STREAMING, 0x00, 0x04, 0x01, AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED, 0x00, 0, 0x00), \
  TUD_AUDIO20_DESC_OUTPUT_TERM(0x03, AUDIO_TERM_TYPE_OUT_DESKTOP_SPEAKER, 0x01, 0x02, 0x04, 0x0000, 0x00), \
  TUD_AUDIO20_DESC_FEATURE_UNIT(0x02, 0x01, 0x00, \
    AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS | AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS, \
    AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS | AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS), \
  TUD_AUDIO20_DESC_STD_AS_INT((uint8_t)((_itfnum) + 1), 0x00, 0x00, 0x00), \
  TUD_AUDIO20_DESC_STD_AS_INT((uint8_t)((_itfnum) + 1), 0x01, 0x01, 0x00), \
  TUD_AUDIO20_DESC_CS_AS_INT(0x01, AUDIO20_CTRL_NONE, AUDIO20_FORMAT_TYPE_I, AUDIO20_DATA_FORMAT_TYPE_I_PCM, 0x01, AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED, 0x00), \
  TUD_AUDIO20_DESC_TYPE_I_FORMAT(2, 16), \
  TUD_AUDIO20_DESC_STD_AS_ISO_EP(0x03, (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_ADAPTIVE | (uint8_t)TUSB_ISO_EP_ATT_DATA), _epoutsize, 0x01), \
  TUD_AUDIO20_DESC_CS_AS_ISO_EP(AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO20_CTRL_NONE, AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0x0000)
static const uint8_t hid_report[] = { TUD_HID_REPORT_DESC_KEYBOARD() };
static const uint8_t config_desc[] = {
  TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_MIDI_DESC_LEN + SPEAKER_DESC_LEN, 0, 100),
  TUD_HID_DESCRIPTOR(ITF_HID, 0, HID_ITF_PROTOCOL_KEYBOARD, sizeof hid_report, 0x81, 8, 1),
  TUD_MIDI_DESCRIPTOR(ITF_MIDI, 0, 0x02, 0x82, 64),
  // the sound card: 16-bit mono on OUT endpoint 3
  SPEAKER_DESCRIPTOR(ITF_AUDIO_CONTROL, CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX),
};
_Static_assert(CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX == TUD_AUDIO_EP_SIZE(false, AUDIO_RATE, 2, 1), "endpoint size and rate disagree");
// With the sound card in it the port is a composite device with interface associations, which needs the
// "miscellaneous" device class; Espressif's default descriptor only uses it when there is a serial port.
static const tusb_desc_device_t device_desc = {
  .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE, .bcdUSB = 0x0200,
  .bDeviceClass = TUSB_CLASS_MISC, .bDeviceSubClass = MISC_SUBCLASS_COMMON, .bDeviceProtocol = MISC_PROTOCOL_IAD,
  .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
  .idVendor = 0x303A, .idProduct = 0x4000 | (CFG_TUD_HID << 2) | (CFG_TUD_MIDI << 3) | (CFG_TUD_AUDIO << 4),   // Espressif's scheme
  .bcdDevice = 0x0200, .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3, .bNumConfigurations = 1,
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

// Back to how the box starts: a keyboard, with the box's own keyer settings. A site that wants MIDI says so again
// when it next opens the box. Closing a site's tab sends nothing over USB, so the box cannot notice that on its own:
// it happens when the computer lets go of the box (sleep, a USB reset) or when asked from the page.
void usb_keyboard_mode(void) {
  if (midi_mode) ESP_LOGI(TAG, "back to keyboard");
  midi_mode = false; host_raw = -1; host_iambic = -1; host_wpm = 0;
}
void tud_suspend_cb(bool remote_wakeup_en) { usb_keyboard_mode(); }
static void usb_event(tinyusb_event_t *event, void *arg) { if (event->id == TINYUSB_EVENT_DETACHED) usb_keyboard_mode(); }

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

// ---- the sound card ----
// The computer sends 1 ms packets at its own clock into TinyUSB's buffer; the audio task takes 2 ms blocks at the
// box's I2S clock. The two clocks differ by a few hundredths of a percent, so the audio task plays the computer's
// samples a tiny bit faster or slower (linear interpolation) to keep about 8 ms buffered: no gaps, no clicks.
// Mute and volume from the computer are honoured on top of the box's own radio volume.
static volatile bool streaming, primed;
static int16_t host_vol[2] = { 0, 0 };   // 1/256 dB, master and channel 1
static bool host_mute[2];
static volatile float host_gain = 1;
#define RING 2048          // samples held by the box itself (power of two)
#define TARGET 384         // samples to keep buffered: 8 ms
static int16_t ring[RING];
static int r_tail, r_count; // r_tail: oldest sample
static float frac, err_avg; // position between the two oldest samples; smoothed buffer error
static void update_gain(void) {
  float db = (host_vol[0] + host_vol[1]) / 256.0f;
  host_gain = (host_mute[0] || host_mute[1]) ? 0 : powf(10, db / 20);
}
bool usb_audio_playing(void) { return streaming && primed; }

// What the sound card is doing, for the status page: how often the computer switched the stream, the last
// alternate setting, bytes received, the buffer fill, and how often it ran dry.
static volatile uint32_t itf_sets, rx_bytes, rx_packets, underruns;
static volatile uint8_t last_alt = 0xFF;
bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t n, uint8_t func_id, uint8_t ep_out, uint8_t alt) {
  rx_bytes += n; rx_packets++;
  return true;
}
// The USB controller's own view of the sound card's OUT endpoint (3): control and transfer-size registers, and
// the core interrupt status. Read-only.
#define OTG(off) (*(volatile uint32_t *)(0x60080000 + (off)))
int usb_audio_report(char *out, int size) {
  return snprintf(out, size, "{\"streaming\":%s,\"primed\":%s,\"sets\":%lu,\"alt\":%d,\"rxBytes\":%lu,\"rxPackets\":%lu,\"fill\":%d,\"rate\":%.5f,\"underruns\":%lu,\"gain\":%.3f,"
                  "\"busyOut\":%s,\"doepctl\":\"%08lx\",\"doeptsiz\":\"%08lx\",\"gintsts\":\"%08lx\"}",
                  streaming ? "true" : "false", primed ? "true" : "false", (unsigned long)itf_sets, last_alt == 0xFF ? -1 : last_alt,
                  (unsigned long)rx_bytes, (unsigned long)rx_packets, r_count, 1 + fminf(fmaxf(err_avg * 0.003f, -0.003f), 0.003f),
                  (unsigned long)underruns, host_gain, ready && usbd_edpt_busy(0, 0x03) ? "true" : "false",
                  (unsigned long)OTG(0xB60), (unsigned long)OTG(0xB70), (unsigned long)OTG(0x014));
}


static void ring_drop(int k) { r_tail = (r_tail + k) & (RING - 1); r_count -= k; }

// Fills n samples of radio audio; silence when nothing is coming. Called by the audio task every block.
void usb_audio_take(int16_t *out, int n) {
  if (!ready || !streaming) { primed = false; r_count = 0; frac = 0; err_avg = 0; memset(out, 0, n * 2); return; }
  // take everything TinyUSB holds
  int16_t chunk[96];
  for (;;) {
    uint16_t have = tud_audio_available() & ~1;
    if (!have) break;
    int got = tud_audio_read(chunk, have < sizeof chunk ? have : sizeof chunk) / 2;
    for (int i = 0; i < got; i++) {
      if (r_count == RING) ring_drop(1);   // far too much: drop the oldest
      ring[(r_tail + r_count) & (RING - 1)] = chunk[i]; r_count++;
    }
  }
  if (!primed) {
    if (r_count < TARGET) { memset(out, 0, n * 2); return; }
    primed = true; frac = 0; err_avg = 0;
  }
  // Play slightly faster when more than the target is buffered, slower when less: at most 0.3 %, smoothed over
  // about a tenth of a second, so the pitch never wobbles audibly.
  err_avg += ((float)(r_count - TARGET) / TARGET - err_avg) * 0.02f;
  float step = 1 + fminf(fmaxf(err_avg * 0.003f, -0.003f), 0.003f);
  float g = host_gain;
  for (int i = 0; i < n; i++) {
    if (r_count < 2) { memset(out + i, 0, (n - i) * 2); underruns++; primed = false; return; }   // ran dry: build up again
    float a = ring[r_tail], b = ring[(r_tail + 1) & (RING - 1)];
    out[i] = (int16_t)((a + (b - a) * frac) * g);
    frac += step;
    while (frac >= 1 && r_count >= 2) { ring_drop(1); frac -= 1; }
  }
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *req) {
  itf_sets++; last_alt = TU_U16_LOW(req->wValue);
  if (TU_U16_LOW(req->wIndex) == ITF_AUDIO_STREAMING) { streaming = TU_U16_LOW(req->wValue) != 0; primed = false; }
  return true;
}
bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *req) {
  if (TU_U16_LOW(req->wIndex) == ITF_AUDIO_STREAMING) { streaming = false; primed = false; }
  return true;
}

// Entity 4 is the clock (fixed 48 kHz), entity 2 the feature unit (mute and volume), as in TinyUSB's
// TUD_AUDIO20_SPEAKER_MONO_FB_DESCRIPTOR.
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *req) {
  uint8_t entity = TU_U16_HIGH(req->wIndex), ctrl = TU_U16_HIGH(req->wValue), ch = TU_U16_LOW(req->wValue);
  if (entity == 4) {
    if (ctrl == AUDIO20_CS_CTRL_SAM_FREQ && req->bRequest == AUDIO20_CS_REQ_CUR) {
      audio20_control_cur_4_t cur = { .bCur = AUDIO_RATE };
      return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &cur, sizeof cur);
    }
    if (ctrl == AUDIO20_CS_CTRL_SAM_FREQ && req->bRequest == AUDIO20_CS_REQ_RANGE) {
      audio20_control_range_4_n_t(1) range = { .wNumSubRanges = 1, .subrange = { { AUDIO_RATE, AUDIO_RATE, 0 } } };
      return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &range, sizeof range);
    }
    if (ctrl == AUDIO20_CS_CTRL_CLK_VALID && req->bRequest == AUDIO20_CS_REQ_CUR) {
      audio20_control_cur_1_t valid = { .bCur = 1 };
      return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &valid, sizeof valid);
    }
  }
  if (entity == 2 && ch < 2) {
    if (ctrl == AUDIO20_FU_CTRL_MUTE && req->bRequest == AUDIO20_CS_REQ_CUR) {
      audio20_control_cur_1_t m = { .bCur = host_mute[ch] };
      return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &m, sizeof m);
    }
    if (ctrl == AUDIO20_FU_CTRL_VOLUME && req->bRequest == AUDIO20_CS_REQ_CUR) {
      audio20_control_cur_2_t v = { .bCur = host_vol[ch] };
      return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &v, sizeof v);
    }
    if (ctrl == AUDIO20_FU_CTRL_VOLUME && req->bRequest == AUDIO20_CS_REQ_RANGE) {
      audio20_control_range_2_n_t(1) r = { .wNumSubRanges = 1, .subrange = { { -50 * 256, 0, 256 } } };   // -50 to 0 dB in 1 dB steps
      return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &r, sizeof r);
    }
  }
  return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *req, uint8_t *buf) {
  uint8_t entity = TU_U16_HIGH(req->wIndex), ctrl = TU_U16_HIGH(req->wValue), ch = TU_U16_LOW(req->wValue);
  if (req->bRequest != AUDIO20_CS_REQ_CUR) return false;
  if (entity == 4 && ctrl == AUDIO20_CS_CTRL_SAM_FREQ) return tu_unaligned_read32(buf) == AUDIO_RATE;
  if (entity == 2 && ch < 2 && ctrl == AUDIO20_FU_CTRL_MUTE) { host_mute[ch] = buf[0] != 0; update_gain(); return true; }
  if (entity == 2 && ch < 2 && ctrl == AUDIO20_FU_CTRL_VOLUME) { host_vol[ch] = (int16_t)tu_unaligned_read16(buf); update_gain(); return true; }
  return false;
}

void usb_start(void) {
  tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
  cfg.descriptor.device = &device_desc;
  cfg.descriptor.full_speed_config = config_desc;
  cfg.event_cb = usb_event;
  if (tinyusb_driver_install(&cfg) != ESP_OK) { ESP_LOGE(TAG, "the USB port could not be set up as a key"); return; }
  ready = true;
  xTaskCreatePinnedToCore(midi_task, "usbmidi", 3072, NULL, 4, NULL, 0);
}
