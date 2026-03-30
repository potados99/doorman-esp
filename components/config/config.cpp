#include "config.h"

static const uint32_t kMaxCooldownSec = 3600;         // 1시간
static const uint32_t kMaxPresenceTimeoutMs = 60000;   // 1분

bool validate(const AppConfig &cfg) {
    return cfg.cooldown_sec <= kMaxCooldownSec &&
           cfg.presence_timeout_ms > 0 &&
           cfg.presence_timeout_ms <= kMaxPresenceTimeoutMs &&
           cfg.rssi_threshold >= -100 &&
           cfg.rssi_threshold <= 0;
}
