#pragma once

#include <cstdint>

struct AppConfig {
    uint32_t cooldown_sec = 120;          // 쿨다운 시간 (초). 0이면 시간 조건 무시 (went_undetected만으로 재트리거).
    uint32_t presence_timeout_ms = 5000;  // feed() 없이 이 시간 경과하면 미감지 전환.
};

// 설정값이 허용 범위 내인지 검증. false면 무효.
bool validate(const AppConfig &cfg);
