#include "statemachine.h"

static const uint32_t kStaleThresholdMs = 86400000;  // 24시간

StateMachine::StateMachine(AppConfig cfg) : config_(cfg) {}

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
    return nullptr;  // 슬롯 풀
}

void StateMachine::feed(const uint8_t (&mac)[6], bool seen, uint32_t now_ms) {
    DeviceState *dev = find_or_create(mac);
    if (!dev) {
        return;  // 슬롯 풀 (30개 초과)
    }

    if (seen) {
        dev->last_seen_ms = now_ms;
        dev->detected = true;
    } else {
        dev->detected = false;
        dev->went_undetected = true;
    }
}

Action StateMachine::tick(uint32_t now_ms) {
    for (auto &dev : devices_) {
        if (!dev.valid) {
            continue;
        }

        // 1. 타임아웃 체크: 오래 안 보이면 미감지 전환
        if (dev.detected && dev.last_seen_ms > 0) {
            if (now_ms - dev.last_seen_ms >= config_.presence_timeout_ms) {
                dev.detected = false;
                dev.went_undetected = true;
            }
        }

        // 2. Unlock 판정: 감지 중이고 쿨다운 조건 충족 시
        if (dev.detected) {
            if (dev.last_unlock_ms == 0) {
                // 최초 감지 -> 무조건 Unlock
                dev.last_unlock_ms = now_ms;
                dev.went_undetected = false;
                return Action::Unlock;
            }

            uint32_t cooldown_ms = config_.cooldown_sec * 1000;
            if (dev.went_undetected &&
                (cooldown_ms == 0 || now_ms - dev.last_unlock_ms >= cooldown_ms)) {
                dev.last_unlock_ms = now_ms;
                dev.went_undetected = false;
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

void StateMachine::cleanup_stale(uint32_t now_ms) {
    for (auto &d : devices_) {
        if (d.valid && !d.detected && now_ms - d.last_seen_ms >= kStaleThresholdMs) {
            d = DeviceState{};
        }
    }
}
