#include "device_config_service.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs.h>

static const char *TAG = "dev_cfg_svc";

/** NVS namespace. 기기별 설정 전용. */
static constexpr const char *kNvsNamespace = "dev";

/** 캐시 최대 항목 수. */
static constexpr int kMaxEntries = 15;

/**
 * 인메모리 캐시 항목.
 * used == false인 슬롯은 빈 슬롯.
 */
struct DeviceConfigEntry {
    uint8_t    mac[6];
    DeviceConfig config;
    bool       used;
};

static DeviceConfigEntry   s_entries[kMaxEntries] = {};
static SemaphoreHandle_t   s_mutex                = nullptr;
static std::atomic<bool>   s_changed{false};

// ── 내부 유틸리티 ────────────────────────────────────────────────────────────

/**
 * MAC 바이트 배열을 12자리 대문자 hex 문자열로 변환한다.
 * out에는 최소 13바이트 공간이 필요하다 (12자 + null).
 */
static void mac_to_key(const uint8_t (&mac)[6], char (&out)[13]) {
    snprintf(out, sizeof(out), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/**
 * 캐시에서 MAC 주소로 항목을 찾는다.
 * 없으면 nullptr 반환.
 * 호출자가 mutex를 이미 획득한 상태여야 한다.
 */
static DeviceConfigEntry *find_entry(const uint8_t (&mac)[6]) {
    for (int i = 0; i < kMaxEntries; ++i) {
        if (s_entries[i].used && memcmp(s_entries[i].mac, mac, 6) == 0) {
            return &s_entries[i];
        }
    }
    return nullptr;
}

/**
 * 빈 슬롯을 찾는다.
 * 없으면 nullptr 반환 (캐시 가득 찬 상태).
 * 호출자가 mutex를 이미 획득한 상태여야 한다.
 */
static DeviceConfigEntry *find_free_slot() {
    for (int i = 0; i < kMaxEntries; ++i) {
        if (!s_entries[i].used) {
            return &s_entries[i];
        }
    }
    return nullptr;
}

// ── NVS 저장/삭제 ────────────────────────────────────────────────────────────

/**
 * NVS에 DeviceConfig blob을 저장한다.
 * 실패 시 로그를 남기고 반환한다(캐시는 이미 반영된 상태).
 */
static void save_entry_to_nvs(const uint8_t (&mac)[6], const DeviceConfig &cfg) {
    char key[13];
    mac_to_key(mac, key);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed for write (%s): %s", key, esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(handle, key, &cfg, sizeof(DeviceConfig));
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        ESP_LOGW(TAG, "NVS full — could not save device config for %s", key);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob failed for %s: %s", key, esp_err_to_name(err));
    } else {
        nvs_commit(handle);
        ESP_LOGI(TAG, "Device config saved: %s", key);
    }

    nvs_close(handle);
}

/**
 * NVS에서 DeviceConfig blob을 삭제한다.
 * 키가 없으면 조용히 무시한다.
 */
static void delete_entry_from_nvs(const uint8_t (&mac)[6]) {
    char key[13];
    mac_to_key(mac, key);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed for delete (%s): %s", key, esp_err_to_name(err));
        return;
    }

    err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // 없는 키 삭제는 정상 케이스
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_erase_key failed for %s: %s", key, esp_err_to_name(err));
    } else {
        nvs_commit(handle);
        ESP_LOGI(TAG, "Device config deleted: %s", key);
    }

    nvs_close(handle);
}

// ── init 시 NVS 전체 로드 ────────────────────────────────────────────────────

/**
 * NVS namespace "dev"의 모든 blob 항목을 읽어 캐시에 적재한다.
 * key는 12자리 대문자 hex MAC 문자열이어야 한다.
 */
static void load_all_from_nvs() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No device configs in NVS — starting empty");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed for load: %s", esp_err_to_name(err));
        return;
    }

    nvs_iterator_t it = nullptr;
    err = nvs_entry_find_in_handle(handle, NVS_TYPE_BLOB, &it);

    int loaded = 0;
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        // key 길이가 12인지 확인 (MAC 12hex)
        if (strlen(info.key) == 12) {
            // blob 크기 확인
            size_t blob_size = 0;
            esp_err_t get_err = nvs_get_blob(handle, info.key, nullptr, &blob_size);
            if (get_err == ESP_OK && blob_size == sizeof(DeviceConfig)) {
                DeviceConfig cfg;
                get_err = nvs_get_blob(handle, info.key, &cfg, &blob_size);
                if (get_err == ESP_OK && validate_device_config(cfg)) {
                    // key → MAC 바이트 변환
                    uint8_t mac[6];
                    bool parse_ok = true;
                    for (int b = 0; b < 6; ++b) {
                        unsigned int byte_val = 0;
                        if (sscanf(info.key + b * 2, "%02X", &byte_val) != 1) {
                            parse_ok = false;
                            break;
                        }
                        mac[b] = static_cast<uint8_t>(byte_val);
                    }

                    if (parse_ok) {
                        DeviceConfigEntry *slot = find_free_slot();
                        if (slot) {
                            memcpy(slot->mac, mac, 6);
                            slot->config = cfg;
                            slot->used = true;
                            ++loaded;
                        } else {
                            ESP_LOGW(TAG, "Cache full — skipping %s", info.key);
                        }
                    } else {
                        ESP_LOGW(TAG, "Invalid MAC key in NVS: %s", info.key);
                    }
                } else {
                    ESP_LOGW(TAG, "Invalid device config for %s — skipping", info.key);
                }
            } else {
                ESP_LOGW(TAG, "Unexpected blob size for %s (%d bytes) — skipping",
                         info.key, (int)blob_size);
            }
        }

        err = nvs_entry_next(&it);
    }

    nvs_release_iterator(it);
    nvs_close(handle);

    ESP_LOGI(TAG, "Loaded %d device config(s) from NVS", loaded);
}

// ── 공개 API ─────────────────────────────────────────────────────────────────

void device_config_service_init() {
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);

    load_all_from_nvs();
}

DeviceConfig device_config_get(const uint8_t (&mac)[6]) {
    DeviceConfig result = {};  // 기본값

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const DeviceConfigEntry *entry = find_entry(mac);
    if (entry) {
        result = entry->config;
    }
    xSemaphoreGive(s_mutex);

    return result;
}

void device_config_set(const uint8_t (&mac)[6], const DeviceConfig &cfg) {
    if (!validate_device_config(cfg)) {
        char key[13];
        mac_to_key(mac, key);
        ESP_LOGW(TAG, "Rejected invalid device config for %s", key);
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    DeviceConfigEntry *entry = find_entry(mac);
    if (!entry) {
        entry = find_free_slot();
        if (!entry) {
            xSemaphoreGive(s_mutex);
            char key[13];
            mac_to_key(mac, key);
            ESP_LOGE(TAG, "Cache full — cannot add device config for %s", key);
            return;
        }
        memcpy(entry->mac, mac, 6);
        entry->used = true;
    }
    entry->config = cfg;

    xSemaphoreGive(s_mutex);

    // NVS 저장은 mutex 밖에서 (I/O 지연이 길 수 있음)
    save_entry_to_nvs(mac, cfg);

    s_changed.store(true);
}

void device_config_delete(const uint8_t (&mac)[6]) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    DeviceConfigEntry *entry = find_entry(mac);
    if (entry) {
        memset(entry, 0, sizeof(DeviceConfigEntry));
        entry->used = false;
    }

    xSemaphoreGive(s_mutex);

    delete_entry_from_nvs(mac);

    s_changed.store(true);
}

int device_config_get_all(uint8_t (*macs)[6], DeviceConfig *configs, int max) {
    int count = 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < kMaxEntries && count < max; ++i) {
        if (s_entries[i].used) {
            memcpy(macs[count], s_entries[i].mac, 6);
            configs[count] = s_entries[i].config;
            ++count;
        }
    }
    xSemaphoreGive(s_mutex);

    return count;
}

bool device_config_changed() {
    return s_changed.exchange(false);
}
