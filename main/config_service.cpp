#include "config_service.h"

#include <cstring>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs.h>

static const char *TAG = "cfg_svc";

/** NVS namespace. door_control과 구분되는 앱 설정 전용 네임스페이스. */
static constexpr const char *kNvsNamespace = "door";
static constexpr const char *kKeyCooldown = "cooldown";
static constexpr const char *kKeyTimeout = "timeout";

static AppConfig s_config = {};
static SemaphoreHandle_t s_mutex = nullptr;

/**
 * NVS에서 설정을 읽어온다. 키가 없으면(초기 상태) 기본값을 유지.
 * NVS open 실패 시에도 기본값으로 정상 동작한다.
 */
static void load_from_nvs() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGI(TAG, "No stored config — using defaults (cooldown=%lus, timeout=%lums)",
                 (unsigned long)s_config.cooldown_sec,
                 (unsigned long)s_config.presence_timeout_ms);
        return;
    }

    uint32_t val;
    if (nvs_get_u32(handle, kKeyCooldown, &val) == ESP_OK) {
        s_config.cooldown_sec = val;
    }
    if (nvs_get_u32(handle, kKeyTimeout, &val) == ESP_OK) {
        s_config.presence_timeout_ms = val;
    }

    nvs_close(handle);

    ESP_LOGI(TAG, "Config loaded from NVS: cooldown=%lus, timeout=%lums",
             (unsigned long)s_config.cooldown_sec,
             (unsigned long)s_config.presence_timeout_ms);
}

/** NVS에 현재 설정을 저장한다. mutex는 호출자가 이미 획득한 상태. */
static void save_to_nvs() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for write: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_u32(handle, kKeyCooldown, s_config.cooldown_sec);
    nvs_set_u32(handle, kKeyTimeout, s_config.presence_timeout_ms);
    nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "Config saved to NVS: cooldown=%lus, timeout=%lums",
             (unsigned long)s_config.cooldown_sec,
             (unsigned long)s_config.presence_timeout_ms);
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
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    std::memcpy(&s_config, &cfg, sizeof(AppConfig));
    save_to_nvs();
    xSemaphoreGive(s_mutex);
}
