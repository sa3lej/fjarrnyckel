// Firmware updates. The flash holds two copies of the firmware; a new one is written to the unused copy and
// started once. If it does not get as far as update_started_ok(), the bootloader goes back to the old copy.
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
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
