#include "statemachine.h"

#ifdef ESP_PLATFORM
#include <esp_log.h>
static const char *TAG = "sm";
#else
#define ESP_LOGI(tag, fmt, ...)
#endif

#include <cstdio>

static const uint32_t kStaleThresholdMs = 86400000;  // 24시간

StateMachine::StateMachine(AppConfig cfg) : config_(cfg) {}

void StateMachine::mac_to_str(const uint8_t *mac, char *buf, size_t buf_size) {
    snprintf(buf, buf_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

StateMachine::DeviceState *StateMachine::find_device(const uint8_t (&mac)[6]) {
    for (auto &d : devices_) {
        if (d.valid && std::memcmp(d.mac, mac, 6) == 0) {
            return &d;
        }
    }
    return nullptr;
}

StateMachine::DeviceState *StateMachine::find_or_create(const uint8_t (&mac)[6]) {
    DeviceState *existing = find_device(mac);
    if (existing) {
        return existing;
    }
    for (auto &d : devices_) {
        if (!d.valid) {
            d = DeviceState{};
            std::memcpy(d.mac, mac, 6);
            d.valid = true;
            return &d;
        }
    }
    return nullptr;
}

void StateMachine::feed(const uint8_t (&mac)[6], bool seen, uint32_t now_ms, int8_t rssi) {
    if (!seen) {
        return;  // 현재 seen=false는 사용하지 않음 (타임아웃으로 처리)
    }

    /**
     * RSSI 필터링: rssi != 0이면 BLE 신호이므로 임계값 체크.
     * rssi == 0이면 Classic (RSSI 없음) → 필터링 건너뜀.
     * 예: rssi=-75, threshold=-70 → -75 < -70 → 신호 약함 → 무시.
     */
    if (rssi != 0 && rssi < config_.rssi_threshold) {
        return;
    }

    DeviceState *dev = find_or_create(mac);
    if (!dev) {
        return;
    }

    bool was_detected = dev->detected;
    dev->last_seen_ms = now_ms;
    dev->detected = true;

    if (!was_detected) {
        char s[18];
        mac_to_str(mac, s, sizeof(s));
        if (rssi != 0) {
            ESP_LOGI(TAG, "%s 재실 (RSSI %d)", s, rssi);
        } else {
            ESP_LOGI(TAG, "%s 재실 (Classic)", s);
        }
    }
}

Action StateMachine::tick(uint32_t now_ms) {
    for (auto &dev : devices_) {
        if (!dev.valid) {
            continue;
        }

        // 타임아웃 체크: 오래 안 보이면 미감지 전환
        if (dev.detected && dev.last_seen_ms > 0) {
            if (now_ms - dev.last_seen_ms >= config_.presence_timeout_ms) {
                dev.detected = false;
                dev.went_undetected = true;

                char s[18];
                mac_to_str(dev.mac, s, sizeof(s));
                ESP_LOGI(TAG, "%s 퇴실 (타임아웃 %lums)", s, config_.presence_timeout_ms);
            }
        }

        // Unlock 판정
        if (dev.detected && config_.auto_unlock_enabled) {
            if (dev.last_unlock_ms == 0) {
                dev.last_unlock_ms = now_ms;
                dev.went_undetected = false;

                char s[18];
                mac_to_str(dev.mac, s, sizeof(s));
                ESP_LOGI(TAG, "%s → Unlock (최초 감지)", s);
                return Action::Unlock;
            }

            uint32_t cooldown_ms = config_.cooldown_sec * 1000;
            if (dev.went_undetected &&
                (cooldown_ms == 0 || now_ms - dev.last_unlock_ms >= cooldown_ms)) {
                dev.last_unlock_ms = now_ms;
                dev.went_undetected = false;

                char s[18];
                mac_to_str(dev.mac, s, sizeof(s));
                ESP_LOGI(TAG, "%s → Unlock (재감지, 쿨다운 경과)", s);
                return Action::Unlock;
            }
        }
    }

    cleanup_stale(now_ms);
    return Action::NoOp;
}

int StateMachine::device_count() const {
    int count = 0;
    for (const auto &d : devices_) {
        if (d.valid) {
            ++count;
        }
    }
    return count;
}

void StateMachine::update_config(AppConfig cfg) {
    config_ = cfg;
}

void StateMachine::cleanup_stale(uint32_t now_ms) {
    for (auto &d : devices_) {
        if (d.valid && !d.detected && now_ms - d.last_seen_ms >= kStaleThresholdMs) {
            char s[18];
            mac_to_str(d.mac, s, sizeof(s));
            ESP_LOGI(TAG, "%s 슬롯 정리 (24시간 미감지)", s);
            d = DeviceState{};
        }
    }
}
