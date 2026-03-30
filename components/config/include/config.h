#pragma once

#include <cstdint>

/**
 * 앱 전역 설정.
 *
 * StateMachine이 문열림 판단에 사용하는 파라미터를 담는다.
 */
struct AppConfig {
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
     * RSSI 필터링은 진입 판단에만 적용되며, 이미 재실인 기기는 RSSI 무관하게 유지.
     */
    int8_t rssi_threshold = -70;

    /**
     * 진입 판단 윈도우 (밀리초).
     * 이 시간 내에 enter_min_count 이상의 RSSI 충족 관측이 있으면 재실로 전환.
     */
    uint32_t enter_window_ms = 5000;

    /**
     * 진입 판단 윈도우 내 최소 관측 수.
     * enter_window_ms 이내에 이 횟수 이상 관측되어야 재실로 인정.
     */
    uint32_t enter_min_count = 3;

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
