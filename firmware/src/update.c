// Firmware updates. The flash holds two copies of the firmware; a new one is written to the unused copy and
// started once. If it does not get as far as update_started_ok(), the bootloader goes back to the old copy.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include <stdlib.h>
#include "esp_timer.h"
#include "fn.h"

static const char *TAG = "update";

const char *firmware_version(void) { return esp_app_get_description()->version; }

void update_started_ok(void) {
  esp_ota_img_states_t st;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "the new firmware started properly and is kept");
  }
}

static void restart_cb(void *arg) { esp_restart(); }

// POST /api/update with the firmware file as the request body.
esp_err_t update_upload(httpd_req_t *req) {
  const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
  esp_ota_handle_t ota;
  const char *error = NULL;
  if (!target || esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) error = "the box could not prepare for the update";
  static char buf[4096];
  int left = req->content_len;
  while (!error && left > 0) {
    int n = httpd_req_recv(req, buf, left < (int)sizeof buf ? left : (int)sizeof buf);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) { error = "the upload was interrupted"; break; }
    if (esp_ota_write(ota, buf, n) != ESP_OK) { error = "this is not a Fjarrnyckel firmware file"; break; }
    left -= n;
  }
  if (!error && esp_ota_end(ota) != ESP_OK) error = "the file is damaged or not a Fjarrnyckel firmware file";
  else if (error && target) esp_ota_abort(ota);
  if (!error && !service_mode() && !firmware_signed(target)) error = "this firmware is not signed by the Fjarrnyckel project";
  if (!error && esp_ota_set_boot_partition(target) != ESP_OK) error = "the new firmware could not be selected";
  httpd_resp_set_type(req, "application/json");
  if (error) {
    ESP_LOGW(TAG, "update refused: %s", error);
    httpd_resp_set_status(req, "400 Bad Request");
    char body[120]; snprintf(body, sizeof body, "{\"error\":\"%s\"}", error);
    return httpd_resp_sendstr(req, body);
  }
  ESP_LOGI(TAG, "new firmware stored, restarting");
  httpd_resp_sendstr(req, "{\"ok\":true}");
  static esp_timer_handle_t timer;          // restart a moment later, so the answer reaches the browser first
  const esp_timer_create_args_t args = { .callback = restart_cb, .name = "restart" };
  esp_timer_create(&args, &timer); esp_timer_start_once(timer, 1500000);
  return ESP_OK;
}

// ---- updates from the internet ----
// Every release on GitHub carries two fixed-name files: version.txt and fjarrnyckel-update.bin. The box reads the
// first to learn the newest version and, when the owner asks for it, installs the second. Nothing installs by
// itself: a restart in the middle of a contact is not something to spring on anybody.
#define RELEASES "https://github.com/sa3lej/fjarrnyckel/releases/latest/download/"
static TaskHandle_t task;
static volatile update_info_t info = { .progress = -1 };
static volatile bool want_check, want_install;

update_info_t update_info(void) { return info; }
void update_check(void) { want_check = true; if (task) xTaskNotifyGive(task); }
void update_install(void) { want_install = true; if (task) xTaskNotifyGive(task); }

static void set_error(const char *e) { update_info_t i = info; strlcpy(i.error, e, sizeof i.error); i.checking = false; i.progress = -1; info = i; }

// GitHub answers with a redirect and some very long header lines; the buffers are sized for those.
static esp_http_client_config_t http_config(const char *url) {
  return (esp_http_client_config_t){ .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .buffer_size = 8192, .buffer_size_tx = 4096,
                                     .user_agent = "fjarrnyckel", .max_redirection_count = 5, .keep_alive_enable = true };
}

typedef struct { char text[32]; int len; } reply_t;
static esp_err_t on_http(esp_http_client_event_t *e) {
  reply_t *r = e->user_data;
  if (e->event_id != HTTP_EVENT_ON_DATA || esp_http_client_get_status_code(e->client) != 200) return ESP_OK;
  int n = e->data_len < (int)sizeof r->text - 1 - r->len ? e->data_len : (int)sizeof r->text - 1 - r->len;
  memcpy(r->text + r->len, e->data, n); r->len += n; r->text[r->len] = 0;
  return ESP_OK;
}

// "0.10.2" against "0.9.7": compares number by number.
static int compare(const char *a, const char *b) {
  for (;;) {
    long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
    if (x != y) return x > y ? 1 : -1;
    if (*a != '.' && *b != '.') return 0;
    if (*a == '.') a++;
    if (*b == '.') b++;
  }
}

static void check(void) {
  update_info_t i = info; i.checking = true; i.error[0] = 0; info = i;
  reply_t reply = {0};
  esp_http_client_config_t cfg = http_config(RELEASES "version.txt");
  cfg.event_handler = on_http; cfg.user_data = &reply;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  esp_err_t err = c ? esp_http_client_perform(c) : ESP_FAIL;
  int status = c ? esp_http_client_get_status_code(c) : 0;
  if (c) esp_http_client_cleanup(c);
  if (err != ESP_OK || status != 200 || reply.text[0] < '0' || reply.text[0] > '9') {
    ESP_LOGW(TAG, "could not look for a new version (%s, answer %d)", esp_err_to_name(err), status);
    set_error("could not reach the update server"); return;
  }
  reply.text[strcspn(reply.text, " \r\n")] = 0;
  i = info; i.checking = false; i.checked = true;
  strlcpy(i.latest, reply.text, sizeof i.latest);
  i.available = compare(i.latest, firmware_version()) > 0;
  info = i;
  ESP_LOGI(TAG, "newest version is %s, this is %s", i.latest, firmware_version());
}

static void install(void) {
  update_info_t i = info; i.progress = 0; i.error[0] = 0; info = i;
  esp_http_client_config_t http = http_config(RELEASES "fjarrnyckel-update.bin");
  esp_https_ota_config_t cfg = { .http_config = &http };
  esp_https_ota_handle_t ota = NULL;
  if (esp_https_ota_begin(&cfg, &ota) != ESP_OK) { set_error("could not download the update"); return; }
  esp_app_desc_t desc;
  if (esp_https_ota_get_img_desc(ota, &desc) != ESP_OK || strcmp(desc.project_name, esp_app_get_description()->project_name)) {
    esp_https_ota_abort(ota); set_error("this is not a Fjarrnyckel firmware file"); return;
  }
  esp_err_t err;
  while ((err = esp_https_ota_perform(ota)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
    int size = esp_https_ota_get_image_size(ota), got = esp_https_ota_get_image_len_read(ota);
    if (size > 0) { i = info; i.progress = (int)((int64_t)got * 100 / size); info = i; }
  }
  if (err != ESP_OK || !esp_https_ota_is_complete_data_received(ota)) { esp_https_ota_abort(ota); set_error("the upload was interrupted"); return; }
  if (esp_https_ota_finish(ota) != ESP_OK) { set_error("the file is damaged or not a Fjarrnyckel firmware file"); return; }
  if (!service_mode() && !firmware_signed(esp_ota_get_next_update_partition(NULL))) {
    esp_ota_set_boot_partition(esp_ota_get_running_partition());      // stay on the firmware that is running
    set_error("this firmware is not signed by the Fjarrnyckel project"); return;
  }
  i = info; i.progress = 100; info = i;
  ESP_LOGI(TAG, "version %s is installed, restarting", desc.version);
  vTaskDelay(pdMS_TO_TICKS(2500));           // long enough for the page to see that it is done
  esp_restart();
}

// Looks for a new version shortly after the box gets Wi-Fi, then twice a day, and whenever the page asks.
static void update_task(void *arg) {
  int64_t last = 0;
  { bool ok = firmware_signed(esp_ota_get_running_partition()); update_info_t i = info; i.is_signed = ok; info = i; ESP_LOGI(TAG, "the running firmware is %s", ok ? "signed" : "not signed"); }
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
    // A new firmware is on trial until it has run for a minute. If it crashes, hangs or is unplugged before
    // that, the bootloader goes back to the firmware that worked.
    if (esp_timer_get_time() > 60000000) update_started_ok();
    if (net_state() != NET_CONNECTED) { if (want_check || want_install) { want_check = want_install = false; set_error("could not reach the update server"); } continue; }
    int64_t now = esp_timer_get_time();
    if (want_install) { want_install = false; install(); continue; }
    if (want_check || !last || now - last > 12LL * 3600 * 1000000) { want_check = false; last = now ? now : 1; check(); }
  }
}

void update_start(void) { xTaskCreatePinnedToCore(update_task, "update", 12288, NULL, 4, &task, 0); }
