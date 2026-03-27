#include "nvs_config.h"

#include <cstring>

#include <esp_log.h>
#include <nvs.h>

static const char *TAG = "nvs_cfg";

bool nvs_load_wifi(WifiConfig &out) {
    nvs_handle_t handle;
    if (nvs_open("net", NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    std::memset(&out, 0, sizeof(out));
    size_t ssid_len = sizeof(out.ssid);
    size_t pass_len = sizeof(out.password);

    bool ok = nvs_get_str(handle, "ssid", out.ssid, &ssid_len) == ESP_OK &&
              nvs_get_str(handle, "pass", out.password, &pass_len) == ESP_OK &&
              ssid_len > 1; // at least 1 char + null

    nvs_close(handle);
    return ok;
}

void nvs_save_wifi(const char *ssid, const char *password) {
    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open("net", NVS_READWRITE, &handle));
    ESP_ERROR_CHECK(nvs_set_str(handle, "ssid", ssid));
    ESP_ERROR_CHECK(nvs_set_str(handle, "pass", password));
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);
    ESP_LOGI(TAG, "WiFi credentials saved (SSID: %s)", ssid);
}

AuthConfig nvs_load_auth() {
    AuthConfig config = {};
    nvs_handle_t handle;

    if (nvs_open("auth", NVS_READONLY, &handle) == ESP_OK) {
        size_t user_len = sizeof(config.username);
        size_t pass_len = sizeof(config.password);

        if (nvs_get_str(handle, "user", config.username, &user_len) != ESP_OK) {
            std::strncpy(config.username, "admin", sizeof(config.username) - 1);
        }
        if (nvs_get_str(handle, "pass", config.password, &pass_len) != ESP_OK) {
            std::strncpy(config.password, "admin", sizeof(config.password) - 1);
        }
        nvs_close(handle);
    } else {
        std::strncpy(config.username, "admin", sizeof(config.username) - 1);
        std::strncpy(config.password, "admin", sizeof(config.password) - 1);
    }

    return config;
}

void nvs_save_auth(const char *username, const char *password) {
    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open("auth", NVS_READWRITE, &handle));
    ESP_ERROR_CHECK(nvs_set_str(handle, "user", username));
    ESP_ERROR_CHECK(nvs_set_str(handle, "pass", password));
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);
    ESP_LOGI(TAG, "Auth credentials saved (user: %s)", username);
}
