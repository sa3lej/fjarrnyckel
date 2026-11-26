// The setup page and its small JSON interface.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include "fn.h"
#include "backup.h"
#include "cwl2.h"

// The page and the logo are linked into the firmware from src/page.html and src/logo.png.
esp_err_t update_upload(httpd_req_t *req);
extern const char page_start[] asm("_binary_page_html_start");
extern const char page_end[] asm("_binary_page_html_end");
extern const char logo_start[] asm("_binary_logo_png_start");
extern const char logo_end[] asm("_binary_logo_png_end");

static int clamp(int v, int lo, int hi);
static void json_text(char *out, int size, const char *in) {      // a C string as JSON string contents
  int at = 0;
  for (; *in && at < size - 8; in++) {
    unsigned char c = *in;
    if (c == '"' || c == '\\') { out[at++] = '\\'; out[at++] = c; }
    else if (c < 0x20) at += snprintf(out + at, size - at, "\\u%04x", c);
    else out[at++] = c;
  }
  out[at] = 0;
}

// ---- the page's optional password ----
// Off unless the owner sets one. With it on, the page itself still loads but every /api call needs the session
// cookie that /api/login hands out for the right password. The cookie's value is made anew at every start and
// whenever the password changes, so old sessions end there.
static char session[33];
static void new_session(void) { for (int i = 0; i < 16; i++) snprintf(session + 2 * i, 3, "%02x", (unsigned)(esp_random() & 0xFF)); }

static void hash_password(const char *pass, const uint8_t salt[8], uint8_t out[32]) {
  uint8_t in[8 + 64]; size_t len = strnlen(pass, 64), n = 0;
  memcpy(in, salt, 8); memcpy(in + 8, pass, len);
  memset(out, 0, 32);
  psa_crypto_init();
  psa_hash_compute(PSA_ALG_SHA_256, in, 8 + len, out, 32, &n);
}

static bool same(const uint8_t *a, const uint8_t *b, size_t n) { uint8_t d = 0; while (n--) d |= *a++ ^ *b++; return d == 0; }   // takes the same time whatever differs

static bool unlocked(httpd_req_t *req) {
  if (!settings.page_lock) return true;
  char value[40]; size_t len = sizeof value;
  if (httpd_req_get_cookie_val(req, "fn", value, &len) != ESP_OK || strlen(value) != 32) return false;
  return same((const uint8_t *)value, (const uint8_t *)session, 32);
}

static void give_cookie(httpd_req_t *req) {
  static char header[90];
  snprintf(header, sizeof header, "fn=%s; Path=/; HttpOnly; SameSite=Strict", session);
  httpd_resp_set_hdr(req, "Set-Cookie", header);
}

// Every protected route goes through here; the real handler travels in user_ctx.
static esp_err_t guarded(httpd_req_t *req) {
  if (unlocked(req)) return ((esp_err_t (*)(httpd_req_t *))req->user_ctx)(req);
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"error\":\"locked\"}");
}

static esp_err_t get_page(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, page_start, page_end - page_start);
}

static esp_err_t get_logo(httpd_req_t *req) {
  httpd_resp_set_type(req, "image/png");
  httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
  return httpd_resp_send(req, logo_start, logo_end - logo_start);
}

static esp_err_t get_status(httpd_req_t *req) {
  static const char *names[] = { "no-setup", "connecting", "connected", "failed" };
  static const char *links[] = { "off", "no-wifi", "no-tailscale", "not-found", "no-answer", "waiting", "ready" };
  static const char *tss[] = { "off", "no-wifi", "connecting", "needs-login", "connected", "auth-failed", "error" };
  char ssid[200], host[140], login[2 * 256 + 8], joined[200];
  static char body[6000], nets[1800];
  json_text(joined, sizeof joined, net_join_failed());
  net_known(nets, sizeof nets);
  json_text(login, sizeof login, ts_login_url());
  json_text(ssid, sizeof ssid, net_ssid());
  json_text(host, sizeof host, settings.link_host);
  link_info_t l = link_info();
  char model[110]; json_text(model, sizeof model, l.model);
  // Memory as it is now and at its lowest since the box started, for internal RAM (what Wi-Fi, TLS and the tasks
  // need) and PSRAM (Tailscale's buffers), plus the largest single free block of internal RAM.
  const uint32_t in_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  char mem[200];
  snprintf(mem, sizeof mem, "{\"inFree\":%u,\"inMin\":%u,\"inBlock\":%u,\"inTotal\":%u,\"psFree\":%u,\"psMin\":%u,\"psTotal\":%u,\"up\":%lld}",
           (unsigned)heap_caps_get_free_size(in_caps), (unsigned)heap_caps_get_minimum_free_size(in_caps),
           (unsigned)heap_caps_get_largest_free_block(in_caps), (unsigned)heap_caps_get_total_size(in_caps),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM), (long long)(esp_timer_get_time() / 1000000));
  char ua[480]; usb_audio_report(ua, sizeof ua);
  update_info_t u = update_info();
  snprintf(body, sizeof body,
    "{\"version\":\"%s\",\"wifi\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"nets\":%s,\"joinFailed\":\"%s\",\"joinReason\":\"%s\",\"reason\":\"%s\",\"setupNetwork\":\"%s\","
    "\"keyType\":%d,\"swap\":%d,\"iambic\":%d,\"wpm\":%d,\"tone\":%d,\"volume\":%d,\"hp\":%d,\"keyDown\":%s,"
    "\"linkHost\":\"%s\",\"linkPort\":%u,\"linkKeySet\":%s,"
    "\"link\":\"%s\",\"rtt\":%d,\"rttMax\":%d,\"freq\":%lu,\"rigValid\":%s,\"cw\":%s,\"late\":%u,\"lost\":%u,\"linkTailscale\":%s,\"buffer\":%d,\"delay\":%u,\"recv\":%d,\"recvLatest\":%d,\"mode\":%d,\"rig\":%s,\"rigModel\":\"%s\",\"rigCaps\":%d,\"rigBands\":%d,\"keyN\":%lu,\"keyLate\":%u,\"keyLost\":%u,\"keyPeak\":%u,"
    "\"ts\":\"%s\",\"tsOn\":%s,\"tsIp\":\"%s\",\"tsLogin\":\"%s\",\"tsKeySet\":%s,\"tsCrashed\":%s,\"tsRegion\":%u,\"tsHome\":%d,\"tsPeerHome\":%d,"
    "\"latest\":\"%s\",\"upChecked\":%s,\"upChecking\":%s,\"upAvailable\":%s,\"upProgress\":%d,\"upError\":\"%s\",\"lock\":%s,\"signed\":%s,\"service\":%s,\"led\":%d,\"tones\":%d,\"target\":%d,\"usbRaw\":%d,\"usb\":%s,\"usbMidi\":%s,\"radio\":%d,\"radioSpk\":%d,\"bright\":%d,\"usbAudio\":%s,\"ua\":%s,\"mem\":%s}",
    firmware_version(), names[net_state()], ssid, net_ip(), net_rssi(), nets, joined, net_join_reason(), net_fail_reason(), net_ap_name(),
    settings.key_type, settings.swap, settings.iambic_b, settings.wpm, settings.tone_hz, settings.volume, settings.hp_volume, key_down ? "true" : "false",
    host, settings.link_port, settings.link_key[0] ? "true" : "false",
    links[l.state], l.rtt_ms, l.rtt_max_ms, (unsigned long)l.freq_hz, l.rig_valid ? "true" : "false", l.cw ? "true" : "false", l.late, l.lost, l.tailscale ? "true" : "false", settings.link_delay, l.delay_ms, l.receiver, CWL2_RECEIVER_LATEST, l.mode, l.has_radio ? "true" : "false", model, l.caps, l.bands, (unsigned long)l.k_edges, l.k_late, l.k_lost, l.k_peak_ms,
    tss[ts_state()], settings.ts_on ? "true" : "false", ts_ip(), login, settings.ts_key[0] ? "true" : "false", ts_crashed() ? "true" : "false", settings.ts_region, ts_home_region(), ts_peer_region(),
    u.latest, u.checked ? "true" : "false", u.checking ? "true" : "false", u.available ? "true" : "false", u.progress, u.error, settings.page_lock ? "true" : "false",
    u.is_signed ? "true" : "false", service_mode() ? "true" : "false", !settings.led_off, !settings.tones_off,
    settings.key_target, settings.usb_raw, usb_connected() ? "true" : "false", usb_midi_mode() ? "true" : "false", settings.radio_volume, settings.radio_spk, settings.led_level >= 1 && settings.led_level <= 100 ? settings.led_level : 10, usb_audio_playing() ? "true" : "false", ua, mem);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, body);
}

static esp_err_t get_relays(httpd_req_t *req) {
  static char body[1400];
  ts_regions(body, sizeof body);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, body);
}

static esp_err_t get_scan(httpd_req_t *req) {
  static char body[3200];
  net_scan(body, sizeof body);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, body);
}

// Reads a form body ("a=1&b=two") and gives the decoded value of one field.
static bool field(const char *body, const char *name, char *out, int size) {
  size_t n = strlen(name);
  for (const char *p = body; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
    if (strncmp(p, name, n) || p[n] != '=') continue;
    int at = 0;
    for (p += n + 1; *p && *p != '&' && at < size - 1; p++) {
      if (*p == '+') out[at++] = ' ';
      else if (*p == '%' && p[1] && p[2]) { char hex[3] = { p[1], p[2], 0 }; out[at++] = (char)strtol(hex, NULL, 16); p += 2; }
      else out[at++] = *p;
    }
    out[at] = 0;
    return true;
  }
  return false;
}

static int read_body(httpd_req_t *req, char *body, int size) {
  int total = req->content_len < size - 1 ? req->content_len : size - 1, got = 0;
  while (got < total) { int r = httpd_req_recv(req, body + got, total - got); if (r <= 0) return -1; got += r; }
  body[got] = 0;
  return got;
}

static esp_err_t post_wifi(httpd_req_t *req) {
  char body[400], ssid[33] = "", pass[65] = "";
  if (read_body(req, body, sizeof body) < 0) return ESP_FAIL;
  field(body, "ssid", ssid, sizeof ssid); field(body, "pass", pass, sizeof pass);
  if (!ssid[0]) { httpd_resp_set_status(req, "400 Bad Request"); return httpd_resp_sendstr(req, "{\"error\":\"no network name\"}"); }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}");
  net_join(ssid, pass);
  return ESP_OK;
}

static esp_err_t post_wifi_forget(httpd_req_t *req) {
  char body[200], ssid[33] = "";
  if (read_body(req, body, sizeof body) < 0) return ESP_FAIL;
  field(body, "ssid", ssid, sizeof ssid);
  net_forget(ssid);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// The shared key is only ever written, never sent back: an empty key field keeps the stored one.
static esp_err_t post_link(httpd_req_t *req) {
  char body[420], host[64] = "", port[8] = "", key[96] = "";
  if (read_body(req, body, sizeof body) < 0) return ESP_FAIL;
  field(body, "host", host, sizeof host); field(body, "port", port, sizeof port); field(body, "key", key, sizeof key);
  strlcpy(settings.link_host, host, sizeof settings.link_host);
  int p = atoi(port); settings.link_port = p > 0 && p < 65536 ? p : 7373;
  if (key[0]) strlcpy(settings.link_key, key, sizeof settings.link_key);
  settings_save();
  link_restart();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// The radio from the box, for when wfview is out of reach: mode=0|1|3 (LSB, USB, CW as CI-V bytes), freq=Hz, tune=1.
static esp_err_t post_radio(httpd_req_t *req) {
  char body[80], v[16];
  if (read_body(req, body, sizeof body) < 0) return ESP_FAIL;
  if (field(body, "mode", v, sizeof v)) { int m = atoi(v); if (m == 0 || m == 1 || m == 3) link_radio(CWL2_CMD_SET_MODE, (uint32_t)m); }
  if (field(body, "freq", v, sizeof v)) { long hz = strtol(v, NULL, 10); if (hz > 0) link_radio(CWL2_CMD_SET_FREQ, (uint32_t)hz); }
  if (field(body, "tune", v, sizeof v) && atoi(v)) link_radio(CWL2_CMD_TUNE, 0);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// Training: puts the USB port back to a keyboard after a MIDI site, without unplugging the box.
static esp_err_t post_usb_keyboard(httpd_req_t *req) {
  usb_keyboard_mode();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// Switches Tailscale on or off. An auth key is optional: without one the page offers a sign-in link instead.
// "forget" erases the box's identity on the tailnet, so the next start registers it anew.
static esp_err_t post_tailscale(httpd_req_t *req) {
  char body[420], v[8], key[128] = "";
  if (read_body(req, body, sizeof body) < 0) return ESP_FAIL;
  if (field(body, "on", v, sizeof v)) settings.ts_on = atoi(v) ? 1 : 0;
  if (field(body, "key", key, sizeof key) && key[0]) strlcpy(settings.ts_key, key, sizeof settings.ts_key);
  if (field(body, "region", v, sizeof v)) settings.ts_region = (uint16_t)clamp(atoi(v), 0, 999);
  bool forget = field(body, "forget", v, sizeof v) && atoi(v);
  if (forget) settings.ts_key[0] = 0;
  settings_save();
  ts_apply(forget);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// "check" looks for a new version; "install" downloads and installs the newest one.
static esp_err_t post_update_online(httpd_req_t *req) {
  char body[60], v[8];
  if (read_body(req, body, sizeof body) < 0) return ESP_FAIL;
  if (field(body, "install", v, sizeof v) && atoi(v)) update_install(); else update_check();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t post_login(httpd_req_t *req) {
  char body[260], pass[65] = ""; uint8_t hash[32];
  if (read_body(req, body, sizeof body) < 0) return ESP_FAIL;
  field(body, "pass", pass, sizeof pass);
  hash_password(pass, settings.page_salt, hash);
  httpd_resp_set_type(req, "application/json");
  if (settings.page_lock && !same(hash, settings.page_hash, 32)) {
    httpd_resp_set_status(req, "403 Forbidden");
    return httpd_resp_sendstr(req, "{\"error\":\"wrong password\"}");
  }
  give_cookie(req);
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// Sets, changes or (with an empty password) removes the page's password.
// ---- backups ----
static esp_err_t answer(httpd_req_t *req, const char *status, const char *json) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, json);
}

static void wipe(void *p, size_t n) { volatile char *v = p; while (n--) *v++ = 0; }

// POST /api/backup with the passphrase: the sealed backup file comes back. Deriving the key takes a few seconds.
static esp_err_t post_backup(httpd_req_t *req) {
  static char body[1024], pass[1024];
  static uint8_t file[sizeof(settings_t) + 512];
  if (req->content_len >= sizeof body) return answer(req, "400 Bad Request", "{\"error\":\"passphrase too long\"}");
  if (read_body(req, body, sizeof body) < 0) return ESP_FAIL;
  if (!field(body, "pass", pass, sizeof pass) || !pass[0]) return answer(req, "400 Bad Request", "{\"error\":\"no passphrase\"}");
  int n = settings_backup(pass, file, sizeof file);
  wipe(body, sizeof body); wipe(pass, sizeof pass);
  if (n < 0) return answer(req, "500 Internal Server Error", "{\"error\":\"the backup could not be made\"}");
  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"fjarrnyckel.fnb\"");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t r = httpd_resp_send(req, (const char *)file, n);
  wipe(file, sizeof file);
  return r;
}

static void restart_cb(void *arg) { esp_restart(); }

static int unhex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

// POST /api/restore with the passphrase and the file in hex: the box takes everything from it and restarts.
static esp_err_t post_restore(httpd_req_t *req) {
  // The largest file format 1 can hold (a 16-bit settings length), in hex, with room for the passphrase field.
  const size_t most = 2 * (BACKUP_OVERHEAD + 512 + 65535) + 1024;
  if (req->content_len > most) return answer(req, "400 Bad Request", "{\"error\":\"not a backup\"}");
  size_t size = req->content_len + 1;
  char *body = malloc(size), *data = malloc(size), *pass = malloc(size);
  int result = -3;
  if (body && data && pass && read_body(req, body, size) >= 0 && field(body, "pass", pass, size) && field(body, "data", data, size)) {
    size_t n = strlen(data) / 2; bool ok = strlen(data) % 2 == 0;
    for (size_t i = 0; ok && i < n; i++) {
      int hi = unhex(data[2 * i]), lo = unhex(data[2 * i + 1]);
      if (hi < 0 || lo < 0) ok = false; else ((uint8_t *)data)[i] = (uint8_t)(hi << 4 | lo);
    }
    result = !ok ? -1 : !pass[0] ? -2 : settings_restore(pass, (const uint8_t *)data, n);
  }
  if (body) { wipe(body, size); free(body); }
  if (data) { wipe(data, size); free(data); }
  if (pass) { wipe(pass, size); free(pass); }
  if (result == -1) return answer(req, "400 Bad Request", "{\"error\":\"not a backup\"}");
  if (result == -2) return answer(req, "403 Forbidden", "{\"error\":\"wrong passphrase\"}");
  if (result < 0) return answer(req, "500 Internal Server Error", "{\"error\":\"the backup could not be stored\"}");
  ESP_LOGI("web", "a backup was restored: restarting");
  answer(req, "200 OK", "{\"ok\":true}");
  static esp_timer_handle_t timer;          // restart a moment later, so the answer reaches the browser first
  const esp_timer_create_args_t args = { .callback = restart_cb, .name = "restart" };
  esp_timer_create(&args, &timer); esp_timer_start_once(timer, 1500000);
  return ESP_OK;
}

static esp_err_t post_password(httpd_req_t *req) {
  char body[260], pass[65] = "";
  if (read_body(req, body, sizeof body) < 0) return ESP_FAIL;
  field(body, "pass", pass, sizeof pass);
  new_session();                             // everyone else who was let in has to give the new password
  if (pass[0]) {
    esp_fill_random(settings.page_salt, sizeof settings.page_salt);
    hash_password(pass, settings.page_salt, settings.page_hash);
    settings.page_lock = 1;
    give_cookie(req);
  } else settings.page_lock = 0;
  settings_save();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static esp_err_t post_settings(httpd_req_t *req) {
  char body[300], v[12];
  if (read_body(req, body, sizeof body) < 0) return ESP_FAIL;
  if (field(body, "keyType", v, sizeof v)) settings.key_type = atoi(v) ? 1 : 0;
  if (field(body, "swap", v, sizeof v)) settings.swap = atoi(v) ? 1 : 0;
  if (field(body, "iambic", v, sizeof v)) settings.iambic_b = atoi(v) ? 1 : 0;
  if (field(body, "wpm", v, sizeof v)) settings.wpm = clamp(atoi(v), 5, 60);
  if (field(body, "tone", v, sizeof v)) settings.tone_hz = clamp(atoi(v), 300, 1200);
  if (field(body, "target", v, sizeof v)) settings.key_target = atoi(v) ? 1 : 0;
  if (field(body, "usbRaw", v, sizeof v)) settings.usb_raw = atoi(v) ? 1 : 0;
  if (field(body, "led", v, sizeof v)) settings.led_off = !atoi(v);
  if (field(body, "tones", v, sizeof v)) settings.tones_off = !atoi(v);
  if (field(body, "volume", v, sizeof v)) settings.volume = clamp(atoi(v), 0, 100);
  if (field(body, "hp", v, sizeof v)) settings.hp_volume = clamp(atoi(v), 0, 100);
  if (field(body, "radio", v, sizeof v)) settings.radio_volume = clamp(atoi(v), 0, 100);
  if (field(body, "radioSpk", v, sizeof v)) settings.radio_spk = clamp(atoi(v), 0, 100);
  if (field(body, "buffer", v, sizeof v)) settings.link_delay = clamp(atoi(v), 10, 200);
  if (field(body, "bright", v, sizeof v)) settings.led_level = clamp(atoi(v), 1, 100);
  if (field(body, "save", v, sizeof v)) settings_save();     // sliders send live values; only the last one is stored
  if (field(body, "beep", v, sizeof v)) audio_beep(settings.tone_hz, 150);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// Anything else is a phone checking for internet access: send it to the setup page.
static esp_err_t redirect(httpd_req_t *req, httpd_err_code_t err) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  return httpd_resp_send(req, NULL, 0);
}

void web_start(void) {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.max_uri_handlers = 20; cfg.recv_wait_timeout = 15; cfg.stack_size = 8192; cfg.lru_purge_enable = true;
  httpd_handle_t server;
  if (httpd_start(&server, &cfg) != ESP_OK) { ESP_LOGE("web", "the setup page could not start"); return; }
  const httpd_uri_t routes[] = {
    { .uri = "/", .method = HTTP_GET, .handler = get_page },
    { .uri = "/logo.png", .method = HTTP_GET, .handler = get_logo },
    { .uri = "/api/status", .method = HTTP_GET, .handler = get_status },
    { .uri = "/api/scan", .method = HTTP_GET, .handler = get_scan },
    { .uri = "/api/relays", .method = HTTP_GET, .handler = get_relays },
    { .uri = "/api/wifi", .method = HTTP_POST, .handler = post_wifi },
    { .uri = "/api/settings", .method = HTTP_POST, .handler = post_settings },
    { .uri = "/api/link", .method = HTTP_POST, .handler = post_link },
    { .uri = "/api/tailscale", .method = HTTP_POST, .handler = post_tailscale },
    { .uri = "/api/update/online", .method = HTTP_POST, .handler = post_update_online },
    { .uri = "/api/update", .method = HTTP_POST, .handler = update_upload },
    { .uri = "/api/backup", .method = HTTP_POST, .handler = post_backup },
    { .uri = "/api/restore", .method = HTTP_POST, .handler = post_restore },
    { .uri = "/api/usb/keyboard", .method = HTTP_POST, .handler = post_usb_keyboard },
    { .uri = "/api/radio", .method = HTTP_POST, .handler = post_radio },
    { .uri = "/api/wifi/forget", .method = HTTP_POST, .handler = post_wifi_forget },
  };
  new_session();
  for (int i = 0; i < sizeof routes / sizeof routes[0]; i++) {
    httpd_uri_t r = routes[i];
    if (i >= 2) { r.user_ctx = (void *)r.handler; r.handler = guarded; }     // the page and the logo are always shown
    httpd_register_uri_handler(server, &r);
  }
  const httpd_uri_t login = { .uri = "/api/login", .method = HTTP_POST, .handler = post_login };
  const httpd_uri_t password = { .uri = "/api/password", .method = HTTP_POST, .handler = guarded, .user_ctx = (void *)post_password };
  httpd_register_uri_handler(server, &login); httpd_register_uri_handler(server, &password);
  httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect);
}
