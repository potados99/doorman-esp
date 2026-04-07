#include "config.h"

#include <cstring>

/**
 * alias 문자열의 개별 바이트가 허용된 문자인지 확인한다.
 *
 * ASCII 범위(0x00~0x7F):
 *   - 영문 대소문자, 숫자, 공백, 하이픈(-), 언더바(_)
 *   - 실기기 이름에 흔한 (), +, ', . 도 허용
 *   - JSON 문자열을 깨는 ", \ 와 제어문자는 거부
 *
 * 0x80 이상 바이트: UTF-8 한글 등 멀티바이트 시퀀스 허용
 *   (추가적인 UTF-8 시퀀스 유효성 검증은 하지 않음)
 */
static bool is_alias_byte_allowed(unsigned char c) {
    if (c >= 0x80) {
        // UTF-8 멀티바이트 (한글 등)
        return true;
    }
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return true;
    if (c >= '0' && c <= '9') return true;
    if (c == ' ' || c == '-' || c == '_') return true;
    if (c == '(' || c == ')' || c == '+' || c == '\'' || c == '.') return true;
    return false;
}

bool validate_device_config(const DeviceConfig &cfg) {
    if (cfg.version != kDeviceConfigVersion) return false;
    if (cfg.rssi_threshold < -100 || cfg.rssi_threshold > 0) return false;
    if (cfg.presence_timeout_ms < 1 || cfg.presence_timeout_ms > 60000) return false;
    if (cfg.enter_window_ms < 1000 || cfg.enter_window_ms > 30000) return false;
    if (cfg.enter_min_count < 1 || cfg.enter_min_count > 10) return false;

    // alias null-termination 확인 및 문자 검증
    bool null_found = false;
    for (size_t i = 0; i < sizeof(cfg.alias); ++i) {
        if (cfg.alias[i] == '\0') {
            null_found = true;
            break;
        }
        if (!is_alias_byte_allowed(static_cast<unsigned char>(cfg.alias[i]))) {
            return false;
        }
    }
    if (!null_found) return false;  // null-terminator 없음

    return true;
}
