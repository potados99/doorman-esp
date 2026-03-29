#pragma once

#include <cstdint>

/**
 * 앱 전역 설정.
 *
 * StateMachine이 문열림 판단에 사용하는 파라미터를 담는다.
 * Phase 3에서는 값 복사로 전달. Phase 4에서 getConfig()/setConfig() 서비스로 전환.
 * 필드는 현재 Phase에서 필요한 것만 포함한다 (YAGNI).
 */
struct AppConfig {
    /**
     * 쿨다운 시간 (초).
     * 한번 Unlock 후, 같은 기기가 미감지→재감지되더라도 이 시간이 지나야 다시 Unlock.
     * 0이면 시간 조건 무시 — went_undetected만으로 재트리거.
     */
    uint32_t cooldown_sec = 120;

    /**
     * Presence 타임아웃 (밀리초).
     * feed(true) 이후 이 시간 안에 다음 feed(true)가 안 오면 미감지로 전환.
     * BLE는 advertising 수신 간격에 의존하므로 너무 짧으면 false negative 발생.
     */
    uint32_t presence_timeout_ms = 5000;
};

/**
 * AppConfig 값이 허용 범위 내인지 검증.
 * cooldown_sec: 0~3600 (0은 시간 조건 비활성, 최대 1시간)
 * presence_timeout_ms: 1~60000 (0은 거부 — 타임아웃 없이는 미감지 전환 불가)
 */
bool validate(const AppConfig &cfg);
