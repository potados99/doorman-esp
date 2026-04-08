#include "config.h"

#include <cstring>

/**
 * alias 문자열의 개별 바이트가 허용된 문자인지 확인합니다.
 *
 * ASCII 범위(0x00~0x7F):
 *   - 영문 대소문자, 숫자, 공백, 하이픈(-), 언더바(_)
 *   - 실기기 이름에 흔한 (), +, ', . 도 허용합니다
 *   - JSON 문자열을 깨는 ", \ 와 제어문자는 거부합니다
 *
 * 0x80 이상 바이트: UTF-8 한글 등 멀티바이트 시퀀스를 허용합니다
 *   (추가적인 UTF-8 시퀀스 유효성 검증은 하지 않습니다)
 */
static bool is_alias_byte_allowed(unsigned char c) {
    if (c >= 0x80) {
        // UTF-8 멀티바이트 (한글 등)
        return true;
    }
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
        return true;
    }
    if (c >= '0' && c <= '9') {
        return true;
    }
    if (c == ' ' || c == '-' || c == '_') {
        return true;
    }
    if (c == '(' || c == ')' || c == '+' || c == '\'' || c == '.') {
        return true;
    }
    return false;
}

/**
 * UTF-8 시퀀스가 well-formed인지 확인합니다.
 * - 0xxxxxxx               : 1바이트 (ASCII)
 * - 110xxxxx 10xxxxxx      : 2바이트
 * - 1110xxxx 10xxxxxx*2    : 3바이트 (한글 포함)
 * - 11110xxx 10xxxxxx*3    : 4바이트
 * 시퀀스 도중 null 종료가 오면 깨진 것으로 간주합니다.
 */
static bool is_alias_utf8_well_formed(const char *s, size_t max_len) {
    size_t i = 0;
    while (i < max_len && s[i] != '\0') {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t need;
        if ((c & 0x80) == 0x00) {
            need = 0;  // ASCII
        } else if ((c & 0xE0) == 0xC0) {
            need = 1;
        } else if ((c & 0xF0) == 0xE0) {
            need = 2;
        } else if ((c & 0xF8) == 0xF0) {
            need = 3;
        } else {
            return false;  // 잘못된 lead byte (10xxxxxx 단독 등)
        }

        // 연속 바이트 검증
        for (size_t k = 1; k <= need; ++k) {
            if (i + k >= max_len) {
                return false;
            }
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if (cc == '\0') {
                return false;
            }
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
        }
        i += 1 + need;
    }
    return true;
}

bool validate_device_config(const DeviceConfig &cfg) {
    if (cfg.version != kDeviceConfigVersion) {
        return false;
    }
    if (cfg.rssi_threshold < -100 || cfg.rssi_threshold > 0) {
        return false;
    }
    if (cfg.presence_timeout_ms < 1 || cfg.presence_timeout_ms > 60000) {
        return false;
    }
    if (cfg.enter_window_ms < 1000 || cfg.enter_window_ms > 30000) {
        return false;
    }
    if (cfg.enter_min_count < 1 || cfg.enter_min_count > 10) {
        return false;
    }

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
    if (!null_found) {
        return false;  // null-terminator 없음
    }

    // UTF-8 시퀀스 무결성 확인 (멀티바이트 경계가 깨지지 않았는지)
    if (!is_alias_utf8_well_formed(cfg.alias, sizeof(cfg.alias))) {
        return false;
    }

    return true;
}
