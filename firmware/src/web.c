// The setup page and its small JSON interface.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "fn.h"

// The page and the logo are linked into the firmware from src/page.html and src/logo.png.
esp_err_t update_upload(httpd_req_t *req);
extern const char page_start[] asm("_binary_page_html_start");
extern const char page_end[] asm("_binary_page_html_end");
extern const char logo_start[] asm("_binary_logo_png_start");
extern const char logo_end[] asm("_binary_logo_png_end");

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
  static const char *links[] = { "off", "no-wifi", "not-found", "no-answer", "waiting", "ready" };
  char ssid[80], host[140], body[760];
  json_text(ssid, sizeof ssid, settings.ssid);
  json_text(host, sizeof host, settings.link_host);
  link_info_t l = link_info();
  snprintf(body, sizeof body,
    "{\"version\":\"%s\",\"wifi\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"reason\":\"%s\",\"setupNetwork\":\"%s\","
    "\"keyType\":%d,\"swap\":%d,\"iambic\":%d,\"wpm\":%d,\"tone\":%d,\"volume\":%d,\"keyDown\":%s,"
    "\"linkHost\":\"%s\",\"linkPort\":%u,\"linkKeySet\":%s,"
    "\"link\":\"%s\",\"rtt\":%d,\"freq\":%lu,\"rigValid\":%s,\"cw\":%s,\"late\":%u,\"lost\":%u}",
    firmware_version(), names[net_state()], ssid, net_ip(), net_fail_reason(), net_ap_name(),
    settings.key_type, settings.swap, settings.iambic_b, settings.wpm, settings.tone_hz, settings.volume, key_down ? "true" : "false",
    host, settings.link_port, settings.link_key[0] ? "true" : "false",
    links[l.state], l.rtt_ms, (unsigned long)l.freq_hz, l.rig_valid ? "true" : "false", l.cw ? "true" : "false", l.late, l.lost);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
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

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static esp_err_t post_settings(httpd_req_t *req) {
  char body[300], v[12];
  if (read_body(req, body, sizeof body) < 0) return ESP_FAIL;
  if (field(body, "keyType", v, sizeof v)) settings.key_type = atoi(v) ? 1 : 0;
  if (field(body, "swap", v, sizeof v)) settings.swap = atoi(v) ? 1 : 0;
  if (field(body, "iambic", v, sizeof v)) settings.iambic_b = atoi(v) ? 1 : 0;
  if (field(body, "wpm", v, sizeof v)) settings.wpm = clamp(atoi(v), 5, 60);
  if (field(body, "tone", v, sizeof v)) settings.tone_hz = clamp(atoi(v), 300, 1200);
  if (field(body, "volume", v, sizeof v)) settings.volume = clamp(atoi(v), 0, 100);
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
  cfg.max_uri_handlers = 12; cfg.recv_wait_timeout = 15; cfg.stack_size = 8192; cfg.lru_purge_enable = true;
  httpd_handle_t server;
  if (httpd_start(&server, &cfg) != ESP_OK) { ESP_LOGE("web", "the setup page could not start"); return; }
  const httpd_uri_t routes[] = {
    { .uri = "/", .method = HTTP_GET, .handler = get_page },
    { .uri = "/logo.png", .method = HTTP_GET, .handler = get_logo },
    { .uri = "/api/status", .method = HTTP_GET, .handler = get_status },
    { .uri = "/api/scan", .method = HTTP_GET, .handler = get_scan },
    { .uri = "/api/wifi", .method = HTTP_POST, .handler = post_wifi },
    { .uri = "/api/settings", .method = HTTP_POST, .handler = post_settings },
    { .uri = "/api/link", .method = HTTP_POST, .handler = post_link },
    { .uri = "/api/update", .method = HTTP_POST, .handler = update_upload },
  };
  for (int i = 0; i < 8; i++) httpd_register_uri_handler(server, &routes[i]);
  httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect);
}
