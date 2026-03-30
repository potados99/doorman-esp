#pragma once

#include <cstdint>

/**
 * 앱 전역 설정.
 *
 * StateMachine이 문열림 판단에 사용하는 파라미터를 담는다.
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
     * feed() 이후 이 시간 안에 다음 feed()가 안 오면 미감지로 전환.
     * BLE advertising 간격 + probe 실패율을 고려해서 넉넉하게.
     */
    uint32_t presence_timeout_ms = 15000;

    /**
     * BLE RSSI 임계값 (dBm).
     * 이 값보다 신호가 약하면 (더 작은 음수면) 무시한다.
     * 예: -70이면 RSSI가 -70 이상일 때만 재실로 인정.
     * Classic은 RSSI를 제공하지 않으므로 이 필터링을 건너뜀 (rssi=0으로 호출).
     */
    int8_t rssi_threshold = -70;

    /**
     * BT 자동 문열림 활성화 여부.
     * false이면 BT 기기가 감지되어도 자동으로 문을 열지 않는다.
     * 웹 UI의 수동 문열기(ManualUnlock)는 이 값과 무관하게 항상 가능.
     */
    bool auto_unlock_enabled = false;
};

/**
 * AppConfig 값이 허용 범위 내인지 검증.
 */
bool validate(const AppConfig &cfg);
