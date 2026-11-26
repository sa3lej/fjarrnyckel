// The keying link: sends every key edge to the receiver by the radio (CWL2 over UDP) and follows its answers.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "psa/crypto.h"
#include "microlink.h"
#include "cwl2.h"
#include "fn.h"

static const char *TAG = "link";
typedef struct { int64_t t; bool down; } edge_t;
static QueueHandle_t edges;
static TaskHandle_t task;
static volatile uint32_t generation;       // changes when the link settings do
static volatile link_info_t info = { .state = LINK_OFF, .rtt_ms = -1 };
static cwl2_t client;

void cwl2_hmac(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[32]) {
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
  psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
  psa_key_id_t id; size_t n = 0;
  memset(out, 0, 32);
  if (psa_import_key(&attr, key, key_len, &id) != PSA_SUCCESS) return;
  psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), data, len, out, 32, &n);
  psa_destroy_key(id);
}

link_info_t link_info(void) { return info; }
// Things for the radio that are not keying wait here for the link task. They go out in packets of their own,
// after any key movement that is waiting, and nothing about the key movements' packets changes.
typedef struct { uint8_t command; uint32_t arg; } radio_t;
static QueueHandle_t radio;
void link_radio(uint8_t command, uint32_t arg) {
  if (!radio) return;
  radio_t r = { command, arg };
  xQueueSend(radio, &r, 0);
  if (task) xTaskNotifyGive(task);
}

// The last stretch of keying: counted here, from the receiver's running totals, so the page can say how that
// stretch went (and not how every stretch since the receiver started went). A stretch ends after a minute
// without a key movement. Kept across reconnects.
#define STRETCH_GAP_US 60000000
static int64_t k_last_edge;
static uint32_t k_edges;
static uint16_t k_base_late, k_base_lost, k_peak, seen_late, seen_lost;
static bool k_any, seen_counts, k_base_due;
void link_restart(void) { generation++; }

// The round trip as the page shows it. One reading says little on a path with short spikes, so the page gets the
// usual one (the median of the last readings) and the longest of the last minute (kept as twelve 5 s slots).
#define RTT_KEEP 64
#define RTT_SLOTS 12
#define RTT_SLOT_US 5000000
static uint32_t rtt_last[RTT_KEEP], rtt_peak[RTT_SLOTS];
static int rtt_n, rtt_at;
static int64_t rtt_slot;
static uint64_t rtt_echo;                // the reading already counted: a status that echoes nothing new adds nothing
static void rtt_forget(void) { rtt_n = rtt_at = 0; memset(rtt_peak, 0, sizeof rtt_peak); }
static void rtt_add(int64_t us, int64_t now) {
  uint32_t ms = us < 0 ? 0 : (uint32_t)((us + 500) / 1000);
  int64_t slot = now / RTT_SLOT_US;
  if (slot - rtt_slot >= RTT_SLOTS) memset(rtt_peak, 0, sizeof rtt_peak);
  else for (int64_t k = rtt_slot + 1; k <= slot; k++) rtt_peak[k % RTT_SLOTS] = 0;
  rtt_slot = slot;
  if (ms > rtt_peak[slot % RTT_SLOTS]) rtt_peak[slot % RTT_SLOTS] = ms;
  rtt_last[rtt_at] = ms; rtt_at = (rtt_at + 1) % RTT_KEEP;
  if (rtt_n < RTT_KEEP) rtt_n++;
}
static int rtt_usual(void) {
  if (!rtt_n) return -1;
  uint32_t v[RTT_KEEP]; memcpy(v, rtt_last, sizeof v);
  for (int a = 1; a < rtt_n; a++) { uint32_t x = v[a]; int b = a; while (b && v[b - 1] > x) { v[b] = v[b - 1]; b--; } v[b] = x; }
  return (int)v[rtt_n / 2];
}
static int rtt_longest(void) {
  if (!rtt_n) return -1;
  uint32_t m = 0;
  for (int k = 0; k < RTT_SLOTS; k++) if (rtt_peak[k] > m) m = rtt_peak[k];
  return (int)m;
}

void link_edge(int64_t t_us, bool down) {
  if (!edges) return;
  edge_t e = { t_us, down };
  xQueueSend(edges, &e, 0);
  if (task) xTaskNotifyGive(task);         // wake the link at once: the edge goes out without waiting for the next tick
}

static void set_state(link_state_t s) { link_info_t i = info; if (i.state != s) { i.state = s; if (s < LINK_WAITING) { i.rtt_ms = i.rtt_max_ms = -1; i.freq_hz = 0; i.rig_valid = i.cw = i.tailscale = false; i.delay_ms = 0; i.receiver = 0; i.has_radio = false; } info = i; } }

// The receiver is reached either straight over the network (a UDP socket) or through Tailscale.
typedef struct { int sock; microlink_udp_socket_t *ts; uint32_t ts_ip; uint16_t port; } path_t;
static volatile bool on_tailscale;
bool link_uses_tailscale(void) { return on_tailscale; }

static void send_packet(path_t *p, int64_t now) {
  uint8_t pkt[CWL2_MAX_PACKET];
  size_t n = cwl2_packet(&client, (uint64_t)now, pkt);
  if (p->ts) microlink_udp_send(p->ts, p->ts_ip, p->port, pkt, n); else send(p->sock, pkt, n, 0);
}

static int receive(path_t *p, uint8_t *buf, size_t size) {
  if (!p->ts) return recv(p->sock, buf, size, 0);
  for (;;) {
    uint32_t from = 0; size_t n = size;
    if (microlink_udp_recv(p->ts, &from, NULL, buf, &n, 0) != ESP_OK) return -1;
    if (from == p->ts_ip) return (int)n;
  }
}

static void pause_as(link_state_t s) { set_state(s); vTaskDelay(pdMS_TO_TICKS(500)); }

// Runs one session with the receiver; returns when the settings change or the network drops.
static void session(uint32_t gen) {
  path_t path = { .sock = -1, .port = settings.link_port };
  microlink_t *ml = NULL;
  struct in_addr lit; bool literal = inet_aton(settings.link_host, &lit);
  if (settings.ts_on) {
    // A tailnet address, or the name of a device on the tailnet, goes through Tailscale. Anything else goes direct.
    on_tailscale = true;                     // said before looking at the handle, so it is never taken away under us
    ml = ts_handle();
    if (ml) {
      if (literal) path.ts_ip = ts_is_tailnet_address(ntohl(lit.s_addr)) ? ntohl(lit.s_addr) : 0;
      else path.ts_ip = microlink_resolve(ml, settings.link_host);
      if (path.ts_ip) path.ts = microlink_udp_create(ml, 0);
      if (path.ts_ip && !path.ts) { on_tailscale = false; pause_as(LINK_NO_TAILSCALE); return; }
    } else if (!literal || ts_is_tailnet_address(ntohl(lit.s_addr))) { on_tailscale = false; pause_as(LINK_NO_TAILSCALE); return; }
    if (!path.ts) on_tailscale = false;
  }
  if (!path.ts) {
    char port[8]; snprintf(port, sizeof port, "%u", settings.link_port);
    struct addrinfo hints = { .ai_socktype = SOCK_DGRAM }, *res = NULL;
    if (getaddrinfo(settings.link_host, port, &hints, &res) || !res) { set_state(LINK_NOT_FOUND); vTaskDelay(pdMS_TO_TICKS(2000)); return; }
    path.sock = socket(res->ai_family, SOCK_DGRAM, 0);
    int ok = path.sock >= 0 && connect(path.sock, res->ai_addr, res->ai_addrlen) == 0;
    freeaddrinfo(res);
    if (!ok) { if (path.sock >= 0) close(path.sock); set_state(LINK_NOT_FOUND); vTaskDelay(pdMS_TO_TICKS(2000)); return; }
    fcntl(path.sock, F_SETFL, fcntl(path.sock, F_GETFL, 0) | O_NONBLOCK);
  }
  cwl2_init(&client, (const uint8_t *)settings.link_key, strlen(settings.link_key), esp_random() | 1);
  xQueueReset(edges); xQueueReset(radio);
  rtt_forget();
  ESP_LOGI(TAG, "talking to the receiver at %s port %u%s", settings.link_host, settings.link_port, path.ts ? " through Tailscale" : "");
  int64_t heard = 0;
  set_state(LINK_NO_ANSWER);
  while (generation == gen && net_state() == NET_CONNECTED && (!path.ts || ts_handle() == ml)) {
    int64_t now = esp_timer_get_time();
    client.wpm = settings.key_type ? settings.wpm : 0;
    client.delay_ms = settings.link_delay;                   // 0 = the receiver's own default
    edge_t e;
    while (xQueueReceive(edges, &e, 0)) {
      if (cwl2_linked(&client)) {             // only what the receiver will play counts
        if (!k_any || e.t - k_last_edge > STRETCH_GAP_US) {
          k_edges = 0; k_peak = 0; k_any = true;
          if (seen_counts) { k_base_late = seen_late; k_base_lost = seen_lost; } else k_base_due = true;
        }
        k_edges++; k_last_edge = e.t;
      }
      cwl2_edge(&client, (uint64_t)e.t, e.down); send_packet(&path, esp_timer_get_time());
    }
    radio_t r;
    while (xQueueReceive(radio, &r, 0)) cwl2_command(&client, r.command, r.arg, (uint64_t)now);
    if (cwl2_command_due(&client, (uint64_t)now)) {
      uint8_t pkt[CWL2_COMMAND_LEN]; size_t n = cwl2_command_packet(&client, (uint64_t)now, pkt);
      if (path.ts) microlink_udp_send(path.ts, path.ts_ip, path.port, pkt, n); else send(path.sock, pkt, n, 0);
    }
    if (cwl2_due(&client, (uint64_t)now)) send_packet(&path, now);
    uint8_t buf[96]; int n;
    while ((n = receive(&path, buf, sizeof buf)) > 0) {
      now = esp_timer_get_time();
      if (!cwl2_on_status(&client, buf, (size_t)n, (uint64_t)now)) { cwl2_on_info(&client, buf, (size_t)n); continue; }
      heard = now;
      if (client.rtt_us >= 0 && client.status.echo_t != rtt_echo) { rtt_echo = client.status.echo_t; rtt_add(client.rtt_us, now); }
      const cwl2_status_t *st = &client.status;
      seen_late = st->late; seen_lost = st->lost; seen_counts = true;
      if (k_base_due) { k_base_due = false; k_base_late = st->late; k_base_lost = st->lost; }
      if (st->late < k_base_late) k_base_late = 0;      // the receiver was restarted: its totals began again
      if (st->lost < k_base_lost) k_base_lost = 0;
      if (k_any && now - k_last_edge < STRETCH_GAP_US && st->delay_ms > k_peak) k_peak = st->delay_ms;
      link_info_t i = { .state = cwl2_linked(&client) ? LINK_READY : LINK_WAITING,
        .rtt_ms = rtt_usual(), .rtt_max_ms = rtt_longest(), .freq_hz = client.status.freq_hz,
        .rig_valid = client.status.flags & CWL2_ST_RIG_VALID, .cw = client.status.flags & CWL2_ST_CW_MODE,
        .late = client.status.late, .lost = client.status.lost, .tailscale = path.ts != NULL,
        .delay_ms = client.status.delay_ms, .receiver = client.status.receiver, .mode = client.status.mode,
        .k_edges = k_any ? k_edges : 0, .k_late = k_any ? (uint16_t)(st->late - k_base_late) : 0,
        .k_lost = k_any ? (uint16_t)(st->lost - k_base_lost) : 0, .k_peak_ms = k_peak,
        .has_radio = client.has_info, .caps = client.caps, .bands = client.bands };
      memcpy(i.model, client.model, sizeof i.model);
      info = i;
    }
    if (now - heard > 1000000) set_state(LINK_NO_ANSWER);    // the receiver answers ten times a second when it is there
    ulTaskNotifyTake(pdTRUE, 1);
  }
  if (path.ts) microlink_udp_close(path.ts); else close(path.sock);
  on_tailscale = false;
}

static void link_task(void *arg) {
  psa_crypto_init();
  for (;;) {
    if (!settings.link_host[0] || !settings.link_key[0]) { set_state(LINK_OFF); vTaskDelay(pdMS_TO_TICKS(300)); continue; }
    if (net_state() != NET_CONNECTED) { set_state(LINK_NO_WIFI); vTaskDelay(pdMS_TO_TICKS(300)); continue; }
    session(generation);
  }
}

void link_start(void) {
  edges = xQueueCreate(64, sizeof(edge_t));
  radio = xQueueCreate(8, sizeof(radio_t));
  xTaskCreatePinnedToCore(link_task, "link", 6144, NULL, 19, &task, 0);
}
