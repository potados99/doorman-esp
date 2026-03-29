#pragma once

#include "config.h"

#include <array>
#include <cstdint>
#include <cstring>

enum class Action { Unlock, NoOp };

class StateMachine {
public:
    static const int kMaxDevices = 30;

    explicit StateMachine(AppConfig cfg);

    void feed(const uint8_t (&mac)[6], bool seen, uint32_t now_ms);
    Action tick(uint32_t now_ms);
    int device_count() const;

private:
    struct DeviceState {
        uint8_t mac[6] = {};
        bool valid = false;
        bool detected = false;
        bool went_undetected = false;
        uint32_t last_seen_ms = 0;
        uint32_t last_unlock_ms = 0;
    };

    AppConfig config_;
    std::array<DeviceState, kMaxDevices> devices_ = {};

    DeviceState *find_device(const uint8_t (&mac)[6]);
    DeviceState *find_or_create(const uint8_t (&mac)[6]);
    void cleanup_stale(uint32_t now_ms);
};
