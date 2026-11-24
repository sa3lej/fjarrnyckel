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
static const uint8_t hid_report[] = { TUD_HID_REPORT_DESC_KEYBOARD() };
static const uint8_t config_desc[] = {
  TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_MIDI_DESC_LEN + TUD_AUDIO20_SPEAKER_MONO_FB_DESC_LEN, 0, 100),
  TUD_HID_DESCRIPTOR(ITF_HID, 0, HID_ITF_PROTOCOL_KEYBOARD, sizeof hid_report, 0x81, 8, 1),
  TUD_MIDI_DESCRIPTOR(ITF_MIDI, 0, 0x02, 0x82, 64),
  // the sound card: 16-bit mono, OUT endpoint 3, feedback on IN endpoint 3
  TUD_AUDIO20_SPEAKER_MONO_FB_DESCRIPTOR(ITF_AUDIO_CONTROL, 0, 2, 16, 0x03, CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX, 0x83, 4),
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
// The computer sends 1 ms packets into TinyUSB's buffer; the audio task takes 2 ms blocks out of it at the pace
// of the box's own I2S clock. The feedback endpoint reports how full that buffer is, so the computer speeds up
// or slows down a little and the buffer stays half full: no clicks from the two clocks drifting apart.
// Mute and volume from the computer are honoured on top of the box's own radio volume.
static volatile bool streaming, primed;
static int16_t host_vol[2] = { 0, 0 };   // 1/256 dB, master and channel 1
static bool host_mute[2];
static volatile float host_gain = 1;
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
// The USB controller's own view of the sound card's OUT endpoint (3) and feedback endpoint (IN 3): control,
// interrupt and transfer-size registers, plus the core interrupt status and the frame number. Read-only.
#define OTG(off) (*(volatile uint32_t *)(0x60080000 + (off)))
int usb_audio_report(char *out, int size) {
  return snprintf(out, size, "{\"streaming\":%s,\"primed\":%s,\"sets\":%lu,\"alt\":%d,\"rxBytes\":%lu,\"rxPackets\":%lu,\"fill\":%u,\"underruns\":%lu,\"gain\":%.3f,"
                  "\"busyOut\":%s,\"busyFb\":%s,\"doepctl\":\"%08lx\",\"doepint\":\"%08lx\",\"doeptsiz\":\"%08lx\",\"diepctl\":\"%08lx\",\"diepint\":\"%08lx\",\"gintsts\":\"%08lx\",\"gintmsk\":\"%08lx\",\"dsts\":\"%08lx\"}",
                  streaming ? "true" : "false", primed ? "true" : "false", (unsigned long)itf_sets, last_alt == 0xFF ? -1 : last_alt,
                  (unsigned long)rx_bytes, (unsigned long)rx_packets, ready ? tud_audio_available() : 0, (unsigned long)underruns, host_gain,
                  ready && usbd_edpt_busy(0, 0x03) ? "true" : "false", ready && usbd_edpt_busy(0, 0x83) ? "true" : "false",
                  (unsigned long)OTG(0xB60), (unsigned long)OTG(0xB68), (unsigned long)OTG(0xB70),
                  (unsigned long)OTG(0x960), (unsigned long)OTG(0x968), (unsigned long)OTG(0x014), (unsigned long)OTG(0x018), (unsigned long)OTG(0x808));
}

// Fills n samples of radio audio; silence when nothing is coming. Called by the audio task every block.
void usb_audio_take(int16_t *out, int n) {
  int want = n * 2;
  if (!ready || !streaming) { primed = false; memset(out, 0, want); return; }
  uint16_t have = tud_audio_available();
  if (!primed) { if (have >= CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 2) primed = true; else { memset(out, 0, want); return; } }
  int got = tud_audio_read(out, have < want ? have & ~1 : want);
  if (got < want) { memset((uint8_t *)out + got, 0, want - got); underruns++; if (got == 0) primed = false; }   // ran dry: build up again
  float g = host_gain;
  if (g < 0.999f) for (int i = 0; i < n; i++) out[i] = (int16_t)(out[i] * g);
}

void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t *p) {
  p->method = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
  p->sample_freq = AUDIO_RATE;
  p->fifo_count.fifo_threshold = CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 2;
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
  if (tinyusb_driver_install(&cfg) != ESP_OK) { ESP_LOGE(TAG, "the USB port could not be set up as a key"); return; }
  ready = true;
  xTaskCreatePinnedToCore(midi_task, "usbmidi", 3072, NULL, 4, NULL, 0);
}
