#include "auth/auth.h"

#include <cstring>

#include <esp_log.h>
#include <nvs.h>

static const char *TAG = "auth";

AuthConfig auth_load() {
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

void auth_save(const char *username, const char *password) {
    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open("auth", NVS_READWRITE, &handle));
    ESP_ERROR_CHECK(nvs_set_str(handle, "user", username));
    ESP_ERROR_CHECK(nvs_set_str(handle, "pass", password));
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);
    ESP_LOGI(TAG, "Auth credentials saved (user: %s)", username);
}
