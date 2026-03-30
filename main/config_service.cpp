#include "config_service.h"

#include <cstring>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs.h>

static const char *TAG = "cfg_svc";

/** NVS namespace. door_control과 구분되는 앱 설정 전용 네임스페이스. */
static constexpr const char *kNvsNamespace = "door";
static constexpr const char *kKeyTimeout = "timeout";
static constexpr const char *kKeyAutoUnlock = "auto";
static constexpr const char *kKeyRssiThresh = "rssi";
static constexpr const char *kKeyEnterWindow = "ent_win";
static constexpr const char *kKeyEnterCount = "ent_cnt";

static AppConfig s_config = {};
static SemaphoreHandle_t s_mutex = nullptr;

/**
 * NVS에서 설정을 읽어온다. 키가 없으면(초기 상태) 기본값을 유지.
 * NVS open 실패 시에도 기본값으로 정상 동작한다.
 */
static void load_from_nvs() {
    AppConfig loaded = s_config;
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGI(TAG, "No stored config — using defaults (timeout=%lums)",
                 (unsigned long)s_config.presence_timeout_ms);
        return;
    }

    uint32_t val;
    if (nvs_get_u32(handle, kKeyTimeout, &val) == ESP_OK) {
        loaded.presence_timeout_ms = val;
    }
    uint8_t auto_val;
    if (nvs_get_u8(handle, kKeyAutoUnlock, &auto_val) == ESP_OK) {
        loaded.auto_unlock_enabled = (auto_val != 0);
    }
    int8_t rssi_val;
    if (nvs_get_i8(handle, kKeyRssiThresh, &rssi_val) == ESP_OK) {
        loaded.rssi_threshold = rssi_val;
    }
    uint32_t ent_win;
    if (nvs_get_u32(handle, kKeyEnterWindow, &ent_win) == ESP_OK) {
        loaded.enter_window_ms = ent_win;
    }
    uint32_t ent_cnt;
    if (nvs_get_u32(handle, kKeyEnterCount, &ent_cnt) == ESP_OK) {
        loaded.enter_min_count = ent_cnt;
    }

    nvs_close(handle);

    if (!validate(loaded)) {
        ESP_LOGW(TAG, "Stored config is invalid — using defaults (timeout=%lums)",
                 (unsigned long)s_config.presence_timeout_ms);
        return;
    }

    s_config = loaded;

    ESP_LOGI(TAG, "Config loaded from NVS: timeout=%lums, auto_unlock=%s",
             (unsigned long)s_config.presence_timeout_ms,
             s_config.auto_unlock_enabled ? "on" : "off");
}

/** NVS에 현재 설정을 저장한다. mutex는 호출자가 이미 획득한 상태. */
static void save_to_nvs() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for write: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_u32(handle, kKeyTimeout, s_config.presence_timeout_ms);
    nvs_set_u8(handle, kKeyAutoUnlock, s_config.auto_unlock_enabled ? 1 : 0);
    nvs_set_i8(handle, kKeyRssiThresh, s_config.rssi_threshold);
    nvs_set_u32(handle, kKeyEnterWindow, s_config.enter_window_ms);
    nvs_set_u32(handle, kKeyEnterCount, s_config.enter_min_count);
    nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "Config saved: auto=%s rssi=%d timeout=%lums win=%lums cnt=%lu",
             s_config.auto_unlock_enabled ? "on" : "off",
             s_config.rssi_threshold,
             (unsigned long)s_config.presence_timeout_ms,
             (unsigned long)s_config.enter_window_ms,
             (unsigned long)s_config.enter_min_count);
}

void config_service_init() {
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);

    load_from_nvs();
}

AppConfig app_config_get() {
    AppConfig copy;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    std::memcpy(&copy, &s_config, sizeof(AppConfig));
    xSemaphoreGive(s_mutex);
    return copy;
}

void app_config_set(const AppConfig &cfg) {
    if (!validate(cfg)) {
        ESP_LOGW(TAG, "Rejected invalid config update: timeout=%lu",
                 (unsigned long)cfg.presence_timeout_ms);
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    std::memcpy(&s_config, &cfg, sizeof(AppConfig));
    save_to_nvs();
    xSemaphoreGive(s_mutex);
}
