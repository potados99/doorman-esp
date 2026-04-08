#pragma once

#include <cstdint>

/**
 * 앱 전역 설정.
 *
 * 감지 파라미터(rssi, timeout 등)는 DeviceConfig으로 이전됩니다.
 * AppConfig에는 앱 수준 토글만 남습니다.
 */
struct AppConfig {
    /**
     * BT 자동 문열림 활성화 여부.
     * false이면 BT 기기가 감지되어도 자동으로 문을 열지 않습니다.
     * 웹 UI의 수동 문열기(ManualUnlock)는 이 값과 무관하게 항상 가능합니다.
     */
    bool auto_unlock_enabled = false;
};

/**
 * 기기별 설정.
 *
 * NVS에 blob 형태로 저장되며, MAC 주소를 키로 사용합니다.
 * 구조체 레이아웃이 바뀌면 version을 올리고 마이그레이션 처리가 필요합니다.
 */
static constexpr uint8_t kDeviceConfigVersion = 1;

struct DeviceConfig {
    uint8_t  version = kDeviceConfigVersion; // NVS blob 버전
    int8_t   rssi_threshold = -70;
    uint8_t  _pad[2] = {};              // 명시적 패딩 (Xtensa 4-byte 정렬)
    uint32_t presence_timeout_ms = 15000;
    uint32_t enter_window_ms = 5000;
    uint32_t enter_min_count = 3;
    char     alias[32] = {};            // 별명 (UTF-8, null-terminated)
};
static_assert(sizeof(DeviceConfig) == 48, "DeviceConfig layout changed — update NVS version");

/**
 * DeviceConfig 값이 허용 범위 내인지 검증합니다.
 *
 * alias는 알파벳·한글·숫자·공백·하이픈·언더바와
 * 실기기 이름에 흔한 일부 문장부호(()+' .)를 허용합니다.
 * JSON 문자열을 깨는 문자("  \ )와 제어문자는 거부합니다. 빈 문자열은 허용합니다.
 */
bool validate_device_config(const DeviceConfig &cfg);
