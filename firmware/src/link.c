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
void link_restart(void) { generation++; }

void link_edge(int64_t t_us, bool down) {
  if (!edges) return;
  edge_t e = { t_us, down };
  xQueueSend(edges, &e, 0);
  if (task) xTaskNotifyGive(task);         // wake the link at once: the edge goes out without waiting for the next tick
}

static void set_state(link_state_t s) { link_info_t i = info; if (i.state != s) { i.state = s; if (s < LINK_WAITING) { i.rtt_ms = -1; i.freq_hz = 0; i.rig_valid = i.cw = false; } info = i; } }

static void send_packet(int sock, int64_t now) {
  uint8_t pkt[CWL2_MAX_PACKET];
  size_t n = cwl2_packet(&client, (uint64_t)now, pkt);
  send(sock, pkt, n, 0);
}

// Runs one session with the receiver; returns when the settings change or Wi-Fi drops.
static void session(uint32_t gen) {
  char port[8]; snprintf(port, sizeof port, "%u", settings.link_port);
  struct addrinfo hints = { .ai_socktype = SOCK_DGRAM }, *res = NULL;
  if (getaddrinfo(settings.link_host, port, &hints, &res) || !res) { set_state(LINK_NOT_FOUND); vTaskDelay(pdMS_TO_TICKS(2000)); return; }
  int sock = socket(res->ai_family, SOCK_DGRAM, 0);
  int ok = sock >= 0 && connect(sock, res->ai_addr, res->ai_addrlen) == 0;
  freeaddrinfo(res);
  if (!ok) { if (sock >= 0) close(sock); set_state(LINK_NOT_FOUND); vTaskDelay(pdMS_TO_TICKS(2000)); return; }
  fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
  cwl2_init(&client, (const uint8_t *)settings.link_key, strlen(settings.link_key), esp_random() | 1);
  xQueueReset(edges);
  ESP_LOGI(TAG, "talking to the receiver at %s port %s", settings.link_host, port);
  int64_t heard = 0;
  set_state(LINK_NO_ANSWER);
  while (generation == gen && net_state() == NET_CONNECTED) {
    int64_t now = esp_timer_get_time();
    client.wpm = settings.key_type ? settings.wpm : 0;
    edge_t e;
    while (xQueueReceive(edges, &e, 0)) { cwl2_edge(&client, (uint64_t)e.t, e.down); send_packet(sock, esp_timer_get_time()); }
    if (cwl2_due(&client, (uint64_t)now)) send_packet(sock, now);
    uint8_t buf[96]; int n;
    while ((n = recv(sock, buf, sizeof buf, 0)) > 0) {
      now = esp_timer_get_time();
      if (!cwl2_on_status(&client, buf, (size_t)n, (uint64_t)now)) continue;
      heard = now;
      link_info_t i = { .state = cwl2_linked(&client) ? LINK_READY : LINK_WAITING,
        .rtt_ms = client.rtt_us < 0 ? -1 : (int)((client.rtt_us + 500) / 1000), .freq_hz = client.status.freq_hz,
        .rig_valid = client.status.flags & CWL2_ST_RIG_VALID, .cw = client.status.flags & CWL2_ST_CW_MODE,
        .late = client.status.late, .lost = client.status.lost };
      info = i;
    }
    if (now - heard > 1000000) set_state(LINK_NO_ANSWER);    // the receiver answers ten times a second when it is there
    ulTaskNotifyTake(pdTRUE, 1);
  }
  close(sock);
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
  xTaskCreatePinnedToCore(link_task, "link", 6144, NULL, 19, &task, 0);
}
