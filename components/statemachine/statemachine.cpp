#include "statemachine.h"

#ifdef ESP_PLATFORM
#include <esp_log.h>
static const char *TAG = "sm";
#else
#define ESP_LOGI(tag, fmt, ...)
#endif

#include <cstdio>

static const uint32_t kStaleThresholdMs = 86400000;  // 24시간

StateMachine::StateMachine() = default;

void StateMachine::mac_to_str(const uint8_t *mac, char *buf, size_t buf_size) {
    snprintf(buf, buf_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

DeviceState *StateMachine::find_device(const uint8_t (&mac)[6]) {
    for (auto &d : devices_) {
        if (d.valid && std::memcmp(d.mac, mac, 6) == 0) {
            return &d;
        }
    }
    return nullptr;
}

DeviceState *StateMachine::find_or_create(const uint8_t (&mac)[6]) {
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
        return;  // 현재 seen=false는 사용하지 않습니다 (타임아웃으로 처리)
    }

    DeviceState *dev = find_or_create(mac);
    if (!dev) {
        return;
    }

    /**
     * 이미 재실 → RSSI 상관없이 last_seen만 갱신 (재실 유지).
     * 약한 신호라도 살아있으면 타임아웃을 리셋합니다.
     */
    if (dev->detected) {
        dev->last_seen_ms = now_ms;
        return;
    }

    /**
     * 미감지 상태 → RSSI 필터링 적용 (진입 판단).
     * rssi != 0이면 BLE 신호이므로 임계값을 체크합니다.
     * rssi == 0이면 Classic (RSSI 없음) → 필터링을 건너뜁니다.
     * 예: rssi=-75, threshold=-70 → -75 < -70 → 신호 약함 → 무시합니다.
     */
    if (rssi != 0 && rssi < dev->dev_config.rssi_threshold) {
        return;
    }

    /** RSSI OK → 관측 기록 (원형 버퍼에 타임스탬프 저장) */
    dev->last_seen_ms = now_ms;
    dev->last_rssi = rssi;
    dev->recent_obs[dev->obs_idx] = now_ms;
    dev->obs_idx = (dev->obs_idx + 1) % DeviceState::kMaxRecentObs;
    if (dev->obs_count < DeviceState::kMaxRecentObs) {
        dev->obs_count++;
    }

    /** 진입 진행 상황 로그: 윈도우 내 관측 수 표시 */
    int count = 0;
    for (int i = 0; i < dev->obs_count && i < DeviceState::kMaxRecentObs; ++i) {
        if (now_ms - dev->recent_obs[i] <= dev->dev_config.enter_window_ms) {
            ++count;
        }
    }
    if (count > 0) {
        char s[18];
        mac_to_str(mac, s, sizeof(s));
        ESP_LOGI(TAG, "%s detecting %d/%lu %lu",
                 s, count, (unsigned long)dev->dev_config.enter_min_count,
                 (unsigned long)dev->dev_config.enter_window_ms);
    }
}

Action StateMachine::tick(uint32_t now_ms) {
    for (auto &dev : devices_) {
        if (!dev.valid) {
            continue;
        }

        // 1. 퇴실 판단: 오래 안 보이면 미감지 전환
        if (dev.detected && dev.last_seen_ms > 0) {
            if (now_ms - dev.last_seen_ms >= dev.dev_config.presence_timeout_ms) {
                dev.detected = false;
                dev.went_undetected = true;

                char s[18];
                mac_to_str(dev.mac, s, sizeof(s));
                ESP_LOGI(TAG, "%s absent %lu", s,
                         (unsigned long)dev.dev_config.presence_timeout_ms);
            }
        }

        // 2. 진입 판단: 윈도우 내 관측 수 카운트
        if (!dev.detected) {
            int count = 0;
            for (int i = 0; i < dev.obs_count && i < DeviceState::kMaxRecentObs; ++i) {
                if (now_ms - dev.recent_obs[i] <= dev.dev_config.enter_window_ms) {
                    ++count;
                }
            }
            if (count >= static_cast<int>(dev.dev_config.enter_min_count)) {
                dev.detected = true;

                char s[18];
                mac_to_str(dev.mac, s, sizeof(s));
                ESP_LOGI(TAG, "%s present", s);
            }
        }

        // 3. Unlock 판정 (auto_unlock 여부와 무관하게 항상 판정. 억제는 SM Task에서.)
        if (dev.detected) {
            if (dev.last_unlock_ms == 0) {
                // 최초 감지 → Unlock
                dev.last_unlock_ms = now_ms;
                dev.went_undetected = false;

                char s[18];
                mac_to_str(dev.mac, s, sizeof(s));
                ESP_LOGI(TAG, "%s unlock", s);
                return Action::Unlock;
            }

            if (dev.went_undetected) {
                // 퇴실 후 재감지 → Unlock
                dev.last_unlock_ms = now_ms;
                dev.went_undetected = false;

                char s[18];
                mac_to_str(dev.mac, s, sizeof(s));
                ESP_LOGI(TAG, "%s unlock", s);
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

void StateMachine::update_device_config(const uint8_t (&mac)[6], const DeviceConfig &cfg) {
    /**
     * 슬롯이 이미 있으면 dev_config만 교체합니다.
     * 없으면 새 슬롯을 만들어 config를 적용합니다.
     * 이렇게 해야 sm_task가 시작 시 NVS config를 push할 때
     * 슬롯이 미리 준비되어, 이후 feed()가 동일한 슬롯을 사용하게 됩니다.
     */
    DeviceState *dev = find_or_create(mac);
    if (dev) {
        dev->dev_config = cfg;
    }
}

void StateMachine::remove_device(const uint8_t (&mac)[6]) {
    DeviceState *dev = find_device(mac);
    if (dev) {
        char s[18];
        mac_to_str(mac, s, sizeof(s));
        ESP_LOGI(TAG, "%s 슬롯 제거 (remove_device)", s);
        *dev = DeviceState{};
    }
}

int StateMachine::dump_states(DeviceState *out, int max) const {
    int count = 0;
    for (const auto &d : devices_) {
        if (count >= max) {
            break;
        }
        if (d.valid) {
            out[count++] = d;
        }
    }
    return count;
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
