// System API: OTA, rollback acknowledge, reboot, factory reset.

#include "web_internal.h"

namespace pixfrog::web::impl {

// ── POST /api/ota ────────────────────────────────────────────────────────────
// Raw firmware binary in the request body → inactive OTA slot. esp_ota_end()
// validates the image (magic, chip, SHA); on success the boot partition is
// switched and the device restarts. With BOOTLOADER_APP_ROLLBACK_ENABLE the
// new image must confirm itself at boot-complete or the bootloader rolls
// back to the current slot — a power cut mid-flash is also safe (the running
// slot is never touched).

// Exchanged, not just read: one flight at a time, whatever the httpd task
// count is configured to be.
static std::atomic<uint32_t> g_ota_in_progress{ 0 };  // RMW: 32-bit on the P4

esp_err_t handle_ota(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    if (g_ota_in_progress.exchange(1)) return send_err(req, 500, "OTA already in progress");

    // Claimed above — every exit from here on must hand the flag back.
    const esp_partition_t* update = esp_ota_get_next_update_partition(nullptr);
    if (!update) {
        g_ota_in_progress = false;
        return send_err(req, 500, "no OTA partition (single-app table?)");
    }

    const int total = req->content_len;
    if (total <= 0) {
        g_ota_in_progress = false;
        return send_err(req, 400, "empty body");
    }
    if (static_cast<size_t>(total) > update->size) {
        g_ota_in_progress = false;
        return send_err(req, 400, "image too large");
    }

    ESP_LOGI(TAG, "OTA: %d bytes -> %s", total, update->label);

    esp_ota_handle_t ota = 0;
    esp_err_t err        = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        g_ota_in_progress = false;
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return send_err(req, 500, "esp_ota_begin failed");
    }

    // Static buffer: the httpd task stack is small and only one OTA can run
    // at a time (guarded by g_ota_in_progress).
    static char buf[4096];
    int remaining = total;
    while (remaining > 0) {
        const int n = httpd_req_recv(
            req, buf,
            remaining < static_cast<int>(sizeof(buf)) ? remaining : static_cast<int>(sizeof(buf)));
        if (n <= 0) {
            esp_ota_abort(ota);
            g_ota_in_progress = false;
            return send_err(req, 400, "upload interrupted");
        }
        err = esp_ota_write(ota, buf, n);
        if (err != ESP_OK) {
            esp_ota_abort(ota);
            g_ota_in_progress = false;
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            return send_err(req, 500, "flash write failed");
        }
        remaining -= n;
    }

    err = esp_ota_end(ota);  // validates magic / chip / image integrity
    if (err != ESP_OK) {
        g_ota_in_progress = false;
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return send_err(req, 400, "image validation failed");
    }
    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        g_ota_in_progress = false;
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return send_err(req, 500, "set boot partition failed");
    }

    // The new image overwrote the rejected slot: forget which image that was,
    // so a rollback of this upload is recorded as a new event even when it is
    // the same build (boot matches the record by image SHA).
    config::RollbackRecord rb{};
    if (config::get_rollback(rb)) {
        std::memset(rb.rejected_sha, 0, sizeof(rb.rejected_sha));
        config::set_rollback(rb);
    }

    ESP_LOGI(TAG, "OTA complete, rebooting into %s", update->label);
    httpd_resp_set_type(req, "application/json");
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"partition\":\"%s\",\"rebooting\":true}",
             update->label);
    httpd_resp_sendstr(req, resp);
    vTaskDelay(pdMS_TO_TICKS(500));  // let the response flush
    esp_restart();
    return ESP_OK;
}

// ── POST /api/rollback/ack ───────────────────────────────────────────────────
esp_err_t handle_rollback_ack(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    if (const auto* rb = rollback_record(); rb && !rb->acknowledged) {
        g_rollback.acknowledged = 1;
        config::set_rollback(g_rollback);
    }
    return send_ok(req);
}

// ── POST /api/audio/test ─────────────────────────────────────────────────────
// The speaker test chime (audio::start_test), played from its own task.
esp_err_t handle_audio_test(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    if (!audio::start_test())
        return send_err(req, 409, "no sound: no speaker, volume off, or busy");
    return send_ok(req);
}

// ── POST /api/reboot ─────────────────────────────────────────────────────────

esp_err_t handle_reboot(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    send_ok(req);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

// ── POST /api/factory-reset ──────────────────────────────────────────────────

// Factory reset then reboot (what the SPA announces): the defaults turn the
// opt-in services (web, sACN, FPP, control universe) off, and only a reboot
// stops them all, the server answering this request included.
esp_err_t handle_factory_reset(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    config::reset_to_defaults();
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "rebooting", true);
    send_json(req, resp);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

}  // namespace pixfrog::web::impl
