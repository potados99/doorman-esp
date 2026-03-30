#include "config.h"

static const uint32_t kMaxPresenceTimeoutMs = 60000;   // 1분

bool validate(const AppConfig &cfg) {
    return cfg.presence_timeout_ms > 0 &&
           cfg.presence_timeout_ms <= kMaxPresenceTimeoutMs &&
           cfg.rssi_threshold >= -100 &&
           cfg.rssi_threshold <= 0 &&
           cfg.enter_window_ms >= 1000 &&
           cfg.enter_window_ms <= 30000 &&
           cfg.enter_min_count >= 1 &&
           cfg.enter_min_count <= 10;
}
