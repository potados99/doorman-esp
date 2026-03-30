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

    DeviceState *dev = find_or_create(mac);
    if (!dev) {
        return;
    }

    /**
     * 이미 재실 → RSSI 상관없이 last_seen만 갱신 (재실 유지).
     * 약한 신호라도 살아있으면 타임아웃을 리셋한다.
     */
    if (dev->detected) {
        dev->last_seen_ms = now_ms;
        return;
    }

    /**
     * 미감지 상태 → RSSI 필터링 적용 (진입 판단).
     * rssi != 0이면 BLE 신호이므로 임계값 체크.
     * rssi == 0이면 Classic (RSSI 없음) → 필터링 건너뜀.
     * 예: rssi=-75, threshold=-70 → -75 < -70 → 신호 약함 → 무시.
     */
    if (rssi != 0 && rssi < config_.rssi_threshold) {
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
        if (now_ms - dev->recent_obs[i] <= config_.enter_window_ms) {
            ++count;
        }
    }
    if (count > 0) {
        char s[18];
        mac_to_str(mac, s, sizeof(s));
        ESP_LOGI(TAG, "%s %lums내 %d/%lu건 (RSSI %d)",
                 s, (unsigned long)config_.enter_window_ms,
                 count, (unsigned long)config_.enter_min_count, rssi);
    }
}

Action StateMachine::tick(uint32_t now_ms) {
    for (auto &dev : devices_) {
        if (!dev.valid) {
            continue;
        }

        // 1. 퇴실 판단: 오래 안 보이면 미감지 전환
        if (dev.detected && dev.last_seen_ms > 0) {
            if (now_ms - dev.last_seen_ms >= config_.presence_timeout_ms) {
                dev.detected = false;
                dev.went_undetected = true;

                char s[18];
                mac_to_str(dev.mac, s, sizeof(s));
                ESP_LOGI(TAG, "%s 퇴실 (타임아웃 %lums)", s,
                         (unsigned long)config_.presence_timeout_ms);
            }
        }

        // 2. 진입 판단: 윈도우 내 관측 수 카운트
        if (!dev.detected) {
            int count = 0;
            for (int i = 0; i < dev.obs_count && i < DeviceState::kMaxRecentObs; ++i) {
                if (now_ms - dev.recent_obs[i] <= config_.enter_window_ms) {
                    ++count;
                }
            }
            if (count >= static_cast<int>(config_.enter_min_count)) {
                dev.detected = true;

                char s[18];
                mac_to_str(dev.mac, s, sizeof(s));
                if (dev.last_rssi != 0) {
                    ESP_LOGI(TAG, "%s 재실 (관측 %d회/%lums, RSSI %d)", s,
                             count, (unsigned long)config_.enter_window_ms, dev.last_rssi);
                } else {
                    ESP_LOGI(TAG, "%s 재실 (관측 %d회/%lums, Classic)", s,
                             count, (unsigned long)config_.enter_window_ms);
                }
            }
        }

        // 3. Unlock 판정
        if (dev.detected && config_.auto_unlock_enabled) {
            if (dev.last_unlock_ms == 0) {
                // 최초 감지 → Unlock
                dev.last_unlock_ms = now_ms;
                dev.went_undetected = false;

                char s[18];
                mac_to_str(dev.mac, s, sizeof(s));
                ESP_LOGI(TAG, "%s → Unlock (최초 감지)", s);
                return Action::Unlock;
            }

            if (dev.went_undetected) {
                // 퇴실 후 재감지 → Unlock
                dev.last_unlock_ms = now_ms;
                dev.went_undetected = false;

                char s[18];
                mac_to_str(dev.mac, s, sizeof(s));
                ESP_LOGI(TAG, "%s → Unlock (재감지)", s);
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
    bool was_disabled = !config_.auto_unlock_enabled;
    bool now_enabled = cfg.auto_unlock_enabled;
    config_ = cfg;

    // auto_unlock이 꺼졌다가 켜지면, 현재 detected인 기기들을
    // "이미 한번 처리한 상태"로 마킹. 안 그러면 일괄 Unlock 폭주.
    if (was_disabled && now_enabled) {
        for (auto &dev : devices_) {
            if (dev.valid && dev.detected) {
                if (dev.last_unlock_ms == 0) {
                    // 최초 감지 상태인 기기 — 이미 있는 것으로 간주
                    // 다음에 퇴실 후 재감지될 때 Unlock 발행
                    dev.last_unlock_ms = 1;  // 0이 아닌 값으로 설정 (최초 감지 조건 회피)
                    dev.went_undetected = false;
                }
            }
        }
    }
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
