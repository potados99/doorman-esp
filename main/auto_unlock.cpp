#include "auto_unlock.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs.h>

static const char *TAG = "auto_unlock";

/** NVS namespace. 구 config_service와 동일하게 "door"를 유지합니다 (마이그레이션 불필요). */
static constexpr const char *kNvsNamespace = "door";
static constexpr const char *kKeyAutoUnlock = "auto";

static bool s_enabled = false;
static SemaphoreHandle_t s_mutex = nullptr;

/** NVS에서 값을 읽어옵니다. 키가 없으면 기본값(false)을 유지합니다. */
static void load_from_nvs() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGI(TAG, "No stored value — using default (off)");
        return;
    }

    uint8_t val;
    if (nvs_get_u8(handle, kKeyAutoUnlock, &val) == ESP_OK) {
        s_enabled = (val != 0);
    }
    nvs_close(handle);

    ESP_LOGI(TAG, "Loaded from NVS: %s", s_enabled ? "on" : "off");
}

/** NVS에 현재 값을 저장합니다. 호출자가 mutex를 이미 획득한 상태입니다. */
static void save_to_nvs() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for write: %s", esp_err_to_name(err));
        return;
    }
    nvs_set_u8(handle, kKeyAutoUnlock, s_enabled ? 1 : 0);
    nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "Saved: %s", s_enabled ? "on" : "off");
}

void auto_unlock_init() {
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);
    load_from_nvs();
}

bool auto_unlock_is_enabled() {
    if (s_mutex == nullptr) return false;  // init 전 / NVS 로드 실패 시 안전 기본값
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool v = s_enabled;
    xSemaphoreGive(s_mutex);
    return v;
}

void auto_unlock_set(bool enabled) {
    if (s_mutex == nullptr) return;  // init 전 호출 조용히 무시
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_enabled = enabled;
    save_to_nvs();
    xSemaphoreGive(s_mutex);
}
