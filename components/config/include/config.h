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

/**
 * 기기별 설정.
 *
 * NVS에 blob 형태로 저장되며, MAC 주소를 키로 사용한다.
 * 구조체 레이아웃이 바뀌면 version을 올리고 마이그레이션 처리 필요.
 */
struct DeviceConfig {
    uint8_t  version = 1;               // NVS blob 버전
    int8_t   rssi_threshold = -70;
    uint8_t  _pad[2] = {};              // 명시적 패딩 (Xtensa 4-byte 정렬)
    uint32_t presence_timeout_ms = 15000;
    uint32_t enter_window_ms = 5000;
    uint32_t enter_min_count = 3;
    char     alias[32] = {};            // 별명 (UTF-8, null-terminated)
};
static_assert(sizeof(DeviceConfig) == 48, "DeviceConfig layout changed — update NVS version");

/**
 * DeviceConfig 값이 허용 범위 내인지 검증.
 *
 * alias는 알파벳·한글·숫자·공백·하이픈·언더바만 허용.
 * JSON/HTML 위험 문자("  \  <  >) 거부. 빈 문자열은 허용.
 */
bool validate_device_config(const DeviceConfig &cfg);
