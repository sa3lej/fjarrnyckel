// Tailscale: lets the box reach the receiver across the internet as if both were on one network.
// MicroLink does the work; this file starts it when it is switched on and reports how it is going.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "microlink.h"
#include "fn.h"

static const char *TAG = "tailscale";
static microlink_t *ml;                      // only this file's task creates and destroys it
static microlink_t *volatile shared;         // what the keying link may use; NULL while it must keep off
static volatile ts_state_t state = TS_OFF;
static volatile uint32_t generation;         // changes when the Tailscale settings do
static volatile bool forget;
static char ip[16], url[MICROLINK_AUTH_URL_MAX];
static volatile int home_region, peer_region;
static uint16_t region_ids[32]; static char region_codes[32][8]; static volatile int region_count;
int ts_home_region(void) { return home_region; }
int ts_peer_region(void) { return peer_region; }
int ts_regions(char *json, int size) {
  int at = snprintf(json, size, "[");
  for (int i = 0; i < region_count && at < size - 40; i++) at += snprintf(json + at, size - at, "%s{\"id\":%u,\"code\":\"%s\"}", i ? "," : "", region_ids[i], region_codes[i]);
  return at + snprintf(json + at, size - at, "]");
}
// Kept across a restart but not across a power cut: set while Tailscale is starting. If the box crashes with it
// set, Tailscale is switched off at the next start, so a fault there cannot keep the box restarting for ever.
#define STARTING 0x7A11CA1E
static RTC_NOINIT_ATTR uint32_t starting;
static int64_t started_at;
static volatile bool crashed;
bool ts_crashed(void) { return crashed; }

ts_state_t ts_state(void) { return state; }
const char *ts_ip(void) { return ip; }
const char *ts_login_url(void) { return url; }
void *ts_handle(void) { return shared; }
void ts_apply(bool forget_identity) { if (forget_identity) forget = true; crashed = false; generation++; }

bool ts_is_tailnet_address(uint32_t host_order) { return (host_order & 0xFFC00000) == 0x64400000; }   // 100.64.0.0/10

static void stop(void) {
  if (!ml) return;
  shared = NULL;                             // the link sees this, closes its socket and says so
  link_restart();
  while (link_uses_tailscale()) vTaskDelay(pdMS_TO_TICKS(20));
  microlink_destroy(ml);
  ml = NULL; ip[0] = url[0] = 0; home_region = peer_region = 0;
}

static bool start(void) {
  microlink_config_t cfg = { .auth_key = settings.ts_key, .device_name = "fjarrnyckel",
                             .enable_derp = true, .enable_stun = true, .enable_disco = true };
  // The receiver always keeps its place among the peers, however many devices the tailnet has.
  struct in_addr a;
  if (inet_aton(settings.link_host, &a) && ts_is_tailnet_address(ntohl(a.s_addr))) cfg.priority_peer_ip = ntohl(a.s_addr);
  starting = STARTING; started_at = esp_timer_get_time();
  // The box has to be at home on the same Tailscale relay as the receiver: MicroLink only listens and calls on
  // its own home relay, and the receiver only on its own until it has something to say to us.
  microlink_set_home_region(settings.ts_region ? settings.ts_region : settings.ts_region_auto);
  ml = microlink_init(&cfg);
  if (!ml) { ESP_LOGE(TAG, "could not start"); return false; }
  if (microlink_start(ml) != ESP_OK) { ESP_LOGE(TAG, "could not start connecting"); microlink_destroy(ml); ml = NULL; return false; }
  ESP_LOGI(TAG, "connecting");
  return true;
}

static void ts_task(void *arg) {
  uint32_t seen = generation;
  bool had_wifi = false;
  for (;;) {
    if (starting == STARTING && (!ml || esp_timer_get_time() - started_at > 60000000)) starting = 0;
    if (generation != seen) { seen = generation; stop(); }
    if (forget && !ml) { forget = false; microlink_factory_reset(); ESP_LOGI(TAG, "the box's Tailscale identity is erased"); }
    bool wifi = net_state() == NET_CONNECTED;
    if (!settings.ts_on) state = TS_OFF;
    else if (!ml) {
      if (!wifi) state = TS_NO_WIFI;
      else { state = start() ? TS_CONNECTING : TS_ERROR; had_wifi = true; if (!ml) vTaskDelay(pdMS_TO_TICKS(5000)); }
    } else {
      if (wifi && !had_wifi) microlink_rebind(ml);       // Wi-Fi came back, perhaps with another address
      had_wifi = wifi;
      microlink_state_t s = microlink_get_state(ml);
      uint32_t vpn = microlink_get_vpn_ip(ml);
      if (vpn) microlink_ip_to_str(vpn, ip); else ip[0] = 0;
      if (s == ML_STATE_NEEDS_LOGIN) { if (microlink_get_auth_url(ml, url, sizeof url) != ESP_OK) url[0] = 0; } else url[0] = 0;
      ts_state_t now = !wifi ? TS_NO_WIFI : s == ML_STATE_CONNECTED ? TS_CONNECTED : s == ML_STATE_NEEDS_LOGIN ? (url[0] ? TS_NEEDS_LOGIN : TS_CONNECTING)
                     : s == ML_STATE_AUTH_FAILED ? TS_AUTH_FAILED : s == ML_STATE_ERROR ? TS_ERROR : TS_CONNECTING;
      if (now != state && now == TS_CONNECTED) ESP_LOGI(TAG, "connected, address %s", ip);
      state = now;
      home_region = microlink_get_home_region(ml);
      if (!region_count && s == ML_STATE_CONNECTED) region_count = microlink_get_derp_regions(ml, region_ids, region_codes, 32);
      // Find the receiver among the peers and see where it is at home. Left to itself (ts_region 0) the box moves there.
      struct in_addr a; uint32_t peer = 0;
      if (inet_aton(settings.link_host, &a)) peer = ntohl(a.s_addr); else if (settings.link_host[0]) peer = microlink_resolve(ml, settings.link_host);
      peer_region = microlink_peer_home_region(ml, peer);
      if (!settings.ts_region && peer_region && peer_region != (settings.ts_region_auto ? settings.ts_region_auto : CONFIG_ML_DERP_REGION)) {
        ESP_LOGI(TAG, "the receiver is at home on relay %d: moving there", peer_region);
        settings.ts_region_auto = peer_region; settings_save(); generation++;
      }
      shared = microlink_is_connected(ml) ? ml : shared;  // once up, the link keeps its socket through short reconnects
    }
    vTaskDelay(pdMS_TO_TICKS(300));
  }
}

void ts_start(void) {
  esp_reset_reason_t why = esp_reset_reason();
  bool fault = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT;
  if (fault && starting == STARTING && settings.ts_on) {
    ESP_LOGE(TAG, "the box restarted after a fault while Tailscale was starting: Tailscale is switched off");
    settings.ts_on = 0; settings_save(); crashed = true;
  }
  starting = 0;
  xTaskCreatePinnedToCore(ts_task, "tailscale", 8192, NULL, 5, NULL, 0);
}
