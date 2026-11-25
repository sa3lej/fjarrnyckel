// Wi-Fi. With saved details the box joins that network. While it is not connected it also runs its own
// open setup network, "Fjarrnyckel-XXXX", where http://192.168.4.1 offers the setup page; every name
// looked up on that network answers with the box itself, so phones open the page on their own.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "fn.h"

static const char *TAG = "net";
static volatile net_state_t state = NET_NO_SETUP;
static char ip[16], ap_name[24], fail[48];
static volatile bool ap_on;
static volatile int ap_clients;
static volatile int64_t connected_at, down_since, last_try;

net_state_t net_state(void) { return state; }
const char *net_ip(void) { return ip; }
const char *net_ap_name(void) { return ap_on ? ap_name : ""; }
const char *net_fail_reason(void) { return fail; }

// The network being tried or in use. One asked for from the page ("pending") is only saved once it has connected,
// so a mistyped password leaves nothing behind.
static char cand_ssid[33], cand_pass[65], join_fail[33], join_reason[48];
static volatile bool pending, promote_due;
static uint8_t tried;                      // known networks tried since the last connection, one bit each
static const char *reason_text(int reason) {
  switch (reason) {
    case WIFI_REASON_NO_AP_FOUND: return "network not found";
    case WIFI_REASON_AUTH_FAIL: case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_EXPIRE: return "wrong password";
    default: return "could not connect";
  }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t *d = data;
    if (d->reason == WIFI_REASON_ASSOC_LEAVE) return;   // we left on purpose, to join another network
    if (state == NET_CONNECTED || !down_since) down_since = esp_timer_get_time();
    ip[0] = 0;
    if (cand_ssid[0]) {
      state = NET_FAILED; snprintf(fail, sizeof fail, "%s", reason_text(d->reason));
      if (pending) { pending = false; strlcpy(join_fail, cand_ssid, sizeof join_fail); strlcpy(join_reason, fail, sizeof join_reason); }
    }
    ESP_LOGW(TAG, "not connected to \"%s\": %s (reason %d)", cand_ssid, reason_text(d->reason), d->reason);
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *got = data;
    snprintf(ip, sizeof ip, IPSTR, IP2STR(&got->ip_info.ip));
    state = NET_CONNECTED; fail[0] = 0; connected_at = esp_timer_get_time(); down_since = 0;
    pending = false; join_fail[0] = join_reason[0] = 0; tried = 0; promote_due = true;
    ESP_LOGI(TAG, "connected to \"%s\", address %s", cand_ssid, ip);
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) ap_clients++;
  else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) { if (ap_clients > 0) ap_clients--; }
}

static void set_ap(bool on) {
  if (on == ap_on) return;
  esp_wifi_set_mode(on ? WIFI_MODE_APSTA : WIFI_MODE_STA);
  ap_on = on; if (!on) ap_clients = 0;
  if (on) ESP_LOGI(TAG, "setup network \"%s\" is up: open http://192.168.4.1", ap_name);
  else ESP_LOGI(TAG, "setup network \"%s\" is off", ap_name);
}

// ---- the known networks ----
// settings.ssid/pass is the first of them, settings.more_* the rest; no gaps, the one used last comes first.
#define KNOWN (1 + WIFI_MORE)
static char *known_ssid(int i) { return i ? settings.more_ssid[i - 1] : settings.ssid; }
static char *known_pass(int i) { return i ? settings.more_pass[i - 1] : settings.pass; }
static int known_count(void) { int n = 0; while (n < KNOWN && known_ssid(n)[0]) n++; return n; }
static int known_find(const char *ssid) { for (int i = 0, n = known_count(); i < n; i++) if (!strcmp(known_ssid(i), ssid)) return i; return -1; }
static void known_copy(int to, const char *ssid, const char *pass) {
  char s[33], p[65]; strlcpy(s, ssid, sizeof s); strlcpy(p, pass, sizeof p);
  memset(known_ssid(to), 0, 33); memset(known_pass(to), 0, 65);
  strlcpy(known_ssid(to), s, 33); strlcpy(known_pass(to), p, 65);
}
static void known_remove(int i) {
  int n = known_count();
  for (; i < n - 1; i++) known_copy(i, known_ssid(i + 1), known_pass(i + 1));
  known_copy(n - 1, "", "");
}
// Puts a network first, new or known. With all places taken, the one used longest ago makes room.
static void known_promote(const char *ssid, const char *pass) {
  char s[33], p[65]; strlcpy(s, ssid, sizeof s); strlcpy(p, pass, sizeof p);
  int at = known_find(s);
  if (at >= 0) known_remove(at);
  int n = known_count();
  if (n == KNOWN) n = KNOWN - 1;
  for (int i = n; i > 0; i--) known_copy(i, known_ssid(i - 1), known_pass(i - 1));
  known_copy(0, s, p);
}

static int rotate;
static SemaphoreHandle_t lock;             // the manager and the page both choose networks

const char *net_ssid(void) { return cand_ssid[0] ? cand_ssid : settings.ssid; }
const char *net_join_failed(void) { return join_fail; }
const char *net_join_reason(void) { return join_reason; }
int net_rssi(void) { wifi_ap_record_t ap; return state == NET_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0; }

static void connect_to(const char *ssid, const char *pass) {
  if (ssid != cand_ssid) { strlcpy(cand_ssid, ssid, sizeof cand_ssid); strlcpy(cand_pass, pass, sizeof cand_pass); }
  wifi_config_t sta = {0};
  strlcpy((char *)sta.sta.ssid, cand_ssid, sizeof sta.sta.ssid);
  strlcpy((char *)sta.sta.password, cand_pass, sizeof sta.sta.password);
  esp_wifi_set_config(WIFI_IF_STA, &sta);
  state = NET_CONNECTING; last_try = esp_timer_get_time();
  esp_wifi_connect();
}

// Joins one of the known networks: the strongest one in range that has not been tried since the last
// connection. With none in range it goes through them in turn, for networks that hide their names.
static void try_known(void) {
  int n = known_count(), pick = 0;
  if (!n) return;
  if (n > 1) {
    static wifi_ap_record_t found[24];
    wifi_scan_config_t cfg = { .show_hidden = false };
    uint16_t count = 0;
    if (esp_wifi_scan_start(&cfg, true) == ESP_OK) { count = 24; esp_wifi_scan_get_ap_records(&count, found); }
    bool seen[KNOWN] = {0}, any = false; int8_t level[KNOWN] = {0};
    for (int i = 0; i < count; i++) {
      int k = known_find((char *)found[i].ssid);
      if (k >= 0 && (!seen[k] || found[i].rssi > level[k])) { seen[k] = any = true; level[k] = found[i].rssi; }
    }
    pick = -1;
    for (int round = 0; round < 2 && pick < 0 && any; round++) {
      for (int k = 0; k < n; k++) if (seen[k] && !(tried >> k & 1) && (pick < 0 || level[k] > level[pick])) pick = k;
      if (pick < 0) tried = 0;               // every one in range has had its turn: start again
    }
    if (pick < 0) pick = rotate++ % n;
    ESP_LOGI(TAG, "%d known networks, %s in range: trying \"%s\"", n, any ? "some" : "none", known_ssid(pick));
  }
  tried |= 1 << pick;
  connect_to(known_ssid(pick), known_pass(pick));
}

void net_join(const char *ssid, const char *pass) {
  xSemaphoreTake(lock, portMAX_DELAY);
  strlcpy(cand_ssid, ssid, sizeof cand_ssid); strlcpy(cand_pass, pass, sizeof cand_pass);
  pending = true; promote_due = false; tried = 0;
  fail[0] = join_fail[0] = join_reason[0] = 0;
  esp_wifi_disconnect();
  connect_to(cand_ssid, cand_pass);
  xSemaphoreGive(lock);
}

void net_forget(const char *ssid) {
  xSemaphoreTake(lock, portMAX_DELAY);
  int at = known_find(ssid);
  if (at >= 0) {
    known_remove(at); settings_save(); tried = 0;
    ESP_LOGI(TAG, "\"%s\" is no longer a known network", ssid);
    if (!pending && !strcmp(cand_ssid, ssid)) {      // the one in use: leave it and look for another
      cand_ssid[0] = cand_pass[0] = ip[0] = 0;
      esp_wifi_disconnect();
      state = known_count() ? NET_FAILED : NET_NO_SETUP; fail[0] = 0;
      last_try = -20000000; down_since = esp_timer_get_time();
    }
  }
  xSemaphoreGive(lock);
}

static int put_name(char *json, int at, int size, const uint8_t *c) {
  for (; *c && at < size - 16; c++) {
    if (*c == '"' || *c == '\\') at += snprintf(json + at, size - at, "\\%c", *c);
    else if (*c < 0x20) at += snprintf(json + at, size - at, "\\u%04x", *c);
    else json[at++] = *c;
  }
  return at;
}

int net_known(char *json, int size) {
  int at = snprintf(json, size, "[");
  for (int i = 0, n = known_count(); i < n && at < size - 220; i++) {
    at += snprintf(json + at, size - at, "%s\"", i ? "," : "");
    at = put_name(json, at, size, (const uint8_t *)known_ssid(i));
    at += snprintf(json + at, size - at, "\"");
  }
  return at + snprintf(json + at, size - at, "]");
}

// Looks after the connection. The setup network is up whenever the box knows no network or has been
// without a connection for 15 s, and goes away 60 s after a connection so the setup page can show the result.
// A new attempt is made every 10 s, but not while someone is using the setup network: every attempt takes
// the radio off the setup network's channel and would throw them off.
static void manager_task(void *arg) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(lock, portMAX_DELAY);
    if (promote_due) {                       // connected: this network is now the first of the known ones
      promote_due = false;
      if (strcmp(cand_ssid, settings.ssid) || strcmp(cand_pass, settings.pass)) { known_promote(cand_ssid, cand_pass); settings_save(); }
    }
    if (!settings.ssid[0] && !pending) { if (state != NET_FAILED) state = NET_NO_SETUP; set_ap(true); }
    else if (state == NET_CONNECTED) { if (ap_on && now - connected_at > 60000000) set_ap(false); }
    else {
      if (down_since && now - down_since > 15000000) set_ap(true);
      if (state != NET_CONNECTING && now - last_try > 10000000 && !(ap_on && ap_clients > 0)) try_known();
      if (state == NET_CONNECTING && now - last_try > 20000000) {     // no answer at all from the network
        state = NET_FAILED; snprintf(fail, sizeof fail, "could not connect");
        if (pending) { pending = false; strlcpy(join_fail, cand_ssid, sizeof join_fail); strlcpy(join_reason, fail, sizeof join_reason); }
      }
    }
    xSemaphoreGive(lock);
  }
}
int net_scan(char *json, int size) {
  wifi_scan_config_t cfg = { .show_hidden = false };
  if (esp_wifi_scan_start(&cfg, true) != ESP_OK) return snprintf(json, size, "[]");
  uint16_t count = 24; static wifi_ap_record_t found[24];
  esp_wifi_scan_get_ap_records(&count, found);
  int at = snprintf(json, size, "[");
  for (int i = 0; i < count && at < size - 120; i++) {
    if (!found[i].ssid[0]) continue;
    bool seen = false;                       // the same network from several access points: list it once
    for (int j = 0; j < i; j++) if (!strcmp((char *)found[j].ssid, (char *)found[i].ssid)) seen = true;
    if (seen) continue;
    at += snprintf(json + at, size - at, "%s{\"ssid\":\"", at > 1 ? "," : "");
    at = put_name(json, at, size, found[i].ssid);
    at += snprintf(json + at, size - at, "\",\"rssi\":%d,\"open\":%s}", found[i].rssi,
                   found[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
  }
  return at + snprintf(json + at, size - at, "]");
}

// Every DNS question on the setup network gets the box's own address as the answer.
static void dns_task(void *arg) {
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
  bind(sock, (struct sockaddr *)&addr, sizeof addr);
  uint8_t buf[512];
  for (;;) {
    struct sockaddr_in from; socklen_t fromlen = sizeof from;
    int len = recvfrom(sock, buf, sizeof buf - 16, 0, (struct sockaddr *)&from, &fromlen);
    if (len < 12 || !ap_on) continue;
    if ((ntohl(from.sin_addr.s_addr) >> 8) != (0xC0A80400 >> 8)) continue;   // only for the setup network, 192.168.4.x
    int q = 12; while (q < len && buf[q]) q += buf[q] + 1;                    // skip the question's name
    q += 5; if (q > len) continue;
    buf[2] = 0x81; buf[3] = 0x80; buf[6] = 0; buf[7] = 1; buf[8] = buf[9] = buf[10] = buf[11] = 0;
    static const uint8_t answer[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1 };
    memcpy(buf + q, answer, sizeof answer);
    sendto(sock, buf, q + sizeof answer, 0, (struct sockaddr *)&from, fromlen);
  }
}

void net_start(void) {
  esp_netif_init();
  esp_event_loop_create_default();
  esp_netif_create_default_wifi_sta();
  esp_netif_create_default_wifi_ap();
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&init);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);            // the details are kept with our own settings
  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL);
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL);

  uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(ap_name, sizeof ap_name, "Fjarrnyckel-%02X%02X", mac[4], mac[5]);
  wifi_config_t ap = { .ap = { .channel = 1, .max_connection = 4, .authmode = WIFI_AUTH_OPEN } };
  strlcpy((char *)ap.ap.ssid, ap_name, sizeof ap.ap.ssid);
  ap.ap.ssid_len = strlen(ap_name);

  bool setup = !settings.ssid[0];
  esp_wifi_set_mode(setup ? WIFI_MODE_APSTA : WIFI_MODE_STA);
  esp_wifi_set_config(WIFI_IF_AP, &ap);
  esp_wifi_start();
  esp_wifi_set_ps(WIFI_PS_NONE);                     // no power saving: it delays packets by up to 100 ms
  ap_on = setup; down_since = esp_timer_get_time();
  if (setup) ESP_LOGI(TAG, "no Wi-Fi details yet. Setup network \"%s\" is up: open http://192.168.4.1", ap_name);
  else last_try = -20000000;                         // the manager joins a known network at its first look
  // The box answers to http://fjarrnyckel.local on the home network, so nobody has to look up its address.
  if (mdns_init() == ESP_OK) { mdns_hostname_set("fjarrnyckel"); mdns_instance_name_set("Fjarrnyckel"); mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0); }
  lock = xSemaphoreCreateMutex();
  xTaskCreate(dns_task, "dns", 3072, NULL, 5, NULL);
  xTaskCreate(manager_task, "wifi", 4096, NULL, 5, NULL);
}
