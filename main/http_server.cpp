#include "http_server.h"

#include <algorithm>
#include <cstring>

#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "httpd";

extern const char index_html_start[] asm("_binary_index_html_start");

static bool upload_in_progress = false;

static esp_err_t send_text_response(httpd_req_t *req, const char *status, const char *message) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, message);
}

static esp_err_t index_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ota_upload_handler(httpd_req_t *req) {
    esp_err_t result = ESP_FAIL;
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = nullptr;
    bool ota_session_open = false;
    bool upload_claimed = false;
    int remaining = 0;
    const char *response_status = "500 Internal Server Error";
    const char *response_message = "Upload failed";

    if (upload_in_progress) {
        response_status = "409 Conflict";
        response_message = "Upload already in progress";
        goto cleanup;
    }
    upload_in_progress = true;
    upload_claimed = true;

    {
        char content_type[64] = {};
        if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
            std::strcmp(content_type, "application/octet-stream") != 0) {
            response_status = "415 Unsupported Media Type";
            response_message = "Expected application/octet-stream";
            goto cleanup;
        }
    }

    if (req->content_len <= 0) {
        response_status = "400 Bad Request";
        response_message = "No content";
        goto cleanup;
    }

    update_partition = esp_ota_get_next_update_partition(nullptr);
    if (update_partition == nullptr) {
        response_message = "No OTA partition";
        goto cleanup;
    }

    if (req->content_len > static_cast<int>(update_partition->size)) {
        response_status = "400 Bad Request";
        response_message = "Firmware too large";
        goto cleanup;
    }

    ESP_LOGI(TAG, "OTA target partition=%s size=%d bytes", update_partition->label, req->content_len);

    result = esp_ota_begin(update_partition, req->content_len, &ota_handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(result));
        response_message = "OTA begin failed";
        goto cleanup;
    }
    ota_session_open = true;

    char buffer[4096];
    remaining = req->content_len;
    while (remaining > 0) {
        const int to_read = std::min(remaining, static_cast<int>(sizeof(buffer)));
        const int received = httpd_req_recv(req, buffer, to_read);

        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            ESP_LOGE(TAG, "httpd_req_recv failed: %d", received);
            response_message = "Receive failed";
            goto cleanup;
        }

        result = esp_ota_write(ota_handle, buffer, received);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(result));
            response_message = "Flash write failed";
            goto cleanup;
        }

        remaining -= received;
    }

    result = esp_ota_end(ota_handle);
    ota_session_open = false;
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(result));
        response_status = "400 Bad Request";
        response_message = "Invalid firmware image";
        goto cleanup;
    }

    result = esp_ota_set_boot_partition(update_partition);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(result));
        response_message = "Set boot partition failed";
        goto cleanup;
    }

    upload_in_progress = false;
    upload_claimed = false;

    if (send_text_response(req, "200 OK", "OK") != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send OTA success response");
    }

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK;

cleanup:
    if (ota_session_open) {
        esp_ota_abort(ota_handle);
    }
    if (upload_claimed) {
        upload_in_progress = false;
    }
    send_text_response(req, response_status, response_message);
    return result == ESP_OK ? ESP_FAIL : result;
}

httpd_handle_t start_webserver() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.recv_wait_timeout = 30;
    config.lru_purge_enable = true;

    httpd_handle_t server = nullptr;
    const esp_err_t start_result = httpd_start(&server, &config);
    if (start_result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(start_result));
        return nullptr;
    }

    httpd_uri_t index_uri = {};
    index_uri.uri = "/";
    index_uri.method = HTTP_GET;
    index_uri.handler = index_get_handler;
    index_uri.user_ctx = nullptr;
    if (httpd_register_uri_handler(server, &index_uri) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register index handler");
        httpd_stop(server);
        return nullptr;
    }

    httpd_uri_t ota_uri = {};
    ota_uri.uri = "/api/firmware/upload";
    ota_uri.method = HTTP_POST;
    ota_uri.handler = ota_upload_handler;
    ota_uri.user_ctx = nullptr;
    if (httpd_register_uri_handler(server, &ota_uri) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register OTA handler");
        httpd_stop(server);
        return nullptr;
    }

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
    return server;
}
