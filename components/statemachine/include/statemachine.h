#pragma once

#include "config.h"

#include <array>
#include <cstdint>
#include <cstring>

/**
 * 최대 추적 가능 기기 수. SM, device_config_service, httpd에서 공유.
 *
 * 실제 슬롯 상한은 ESP-IDF `CONFIG_BT_SMP_MAX_BONDS`(BLE+Classic 합산)가 결정합니다.
 * 이 헤더는 host_test 빌드(pc-side gtest)에서도 include되므로 sdkconfig에 직접
 * 의존하지 않고 **literal**로 유지합니다. 대신 타깃 빌드(main/sm_task.cpp)에서
 * `static_assert(kMaxTrackedDevices == CONFIG_BT_SMP_MAX_BONDS, ...)`로 두 값의
 * 일치를 컴파일 시점에 강제합니다.
 */
static constexpr int kMaxTrackedDevices = 15;

/**
 * StateMachine이 tick()에서 반환하는 액션.
 * Unlock이면 문을 열어야 하고, NoOp이면 아무것도 하지 않습니다.
 */
enum class Action { Unlock, NoOp };

/**
 * per-device 상태. feed()가 새 MAC을 만나면 빈 슬롯에 동적으로 생성됩니다.
 * 24시간 이상 미감지되면 tick()에서 정리됩니다(슬롯 해제).
 */
struct DeviceState {
    uint8_t mac[6] = {};
    bool valid = false;
    bool detected = false;
    bool went_undetected = false;
    uint32_t last_seen_ms = 0;        /**< 아무 feed(true) 시각 (RSSI 무관, 재실 유지용) */
    uint32_t last_unlock_ms = 0;
    int8_t last_rssi = 0;             /**< 마지막 관측의 RSSI (로그용) */

    /** 진입 판단용: RSSI >= threshold인 관측 타임스탬프 원형 버퍼 */
    static constexpr int kMaxRecentObs = 10;
    uint32_t recent_obs[kMaxRecentObs] = {};
    int obs_idx = 0;
    int obs_count = 0;  /**< 전체 기록된 관측 수 (kMaxRecentObs까지만) */

    DeviceConfig dev_config;  /**< 기기별 설정 (update_device_config()로 갱신) */
};

/**
 * BT presence 기반 문열림 판단 상태머신.
 *
 * 외부에서 feed()로 BT 감지 이벤트를 넣고, tick()을 주기적으로 호출하면
 * 내부적으로 per-device 상태(감지 여부, 타임아웃)를 관리하고
 * 문을 열어야 하는 시점에 Unlock을 반환합니다.
 *
 * 기기 등록/삭제 API는 없습니다. BT 스택이 bond를 관리하고,
 * bt_manager가 bonded peer만 feed하므로 SM은 들어오는 MAC에 대해
 * 동적으로 슬롯을 생성합니다.
 *
 * 이 클래스는 순수 C++이며 ESP-IDF에 의존하지 않습니다.
 * SM Task가 유일한 소유자 — 외부에서는 큐로만 접근합니다.
 */
class StateMachine {
public:
    static constexpr int kMaxDevices = kMaxTrackedDevices;

    StateMachine();

    /**
     * BT/BLE 스캔 결과를 기록합니다.
     *
     * seen=true:  기기가 감지됨. BLE adv 수신 또는 Classic probe 성공.
     * seen=false: 현재 사용하지 않습니다 (probe 실패는 타임아웃으로 처리).
     *
     * rssi: BLE 신호 세기 (dBm). 0이면 RSSI 필터링을 건너뜁니다 (Classic용).
     *       dev_config.rssi_threshold보다 약하면 seen=true여도 무시합니다.
     *
     * now_ms를 명시적으로 받는 이유: 호스트 테스트에서 시간을 완전히 제어하기 위함입니다.
     */
    void feed(const uint8_t (&mac)[6], bool seen, uint32_t now_ms, int8_t rssi = 0);

    /**
     * 주기적으로 호출하여 시간 기반 상태 전이를 평가합니다.
     *
     * 1. 타임아웃 체크: feed(true) 이후 presence_timeout_ms 경과 → 미감지 전환
     * 2. Unlock 판정: 최초 감지 또는 퇴실 후 재감지 시 Unlock 반환
     *
     * 한 번에 최대 하나의 Unlock만 반환합니다.
     * 복수 기기가 동시에 조건을 만족해도 다음 tick에서 순차 처리합니다.
     * 1~3초 주기로 호출하면 실사용에 문제없습니다.
     */
    [[nodiscard]] Action tick(uint32_t now_ms);

    /** 현재 추적 중인(valid 슬롯이 있는) 기기 수. */
    [[nodiscard]] int device_count() const;

    /**
     * 특정 MAC 기기의 기기별 설정을 갱신합니다.
     * 슬롯이 없으면 생성합니다 — feed() 호출 전에 설정해두어도,
     * feed()가 슬롯을 만든 후 update_device_config()가 덮어쓰므로 순서는 무관합니다.
     * 슬롯이 이미 존재하면 dev_config만 교체합니다.
     */
    void update_device_config(const uint8_t (&mac)[6], const DeviceConfig &cfg);

    /**
     * 특정 MAC 기기의 슬롯을 삭제합니다.
     * 존재하지 않으면 무시합니다.
     */
    void remove_device(const uint8_t (&mac)[6]);

    /**
     * 현재 valid한 슬롯을 out 배열에 복사합니다.
     * 반환값: 실제 복사된 항목 수 (max를 초과하지 않음).
     */
    int dump_states(DeviceState *out, int max) const;

private:
    std::array<DeviceState, kMaxDevices> devices_ = {};

    /** MAC으로 기존 슬롯을 검색합니다. 없으면 nullptr를 반환합니다. */
    DeviceState *find_device(const uint8_t (&mac)[6]);

    /** MAC으로 기존 슬롯을 검색하고, 없으면 빈 슬롯에 새로 생성합니다. 슬롯 풀이면 nullptr를 반환합니다. */
    DeviceState *find_or_create(const uint8_t (&mac)[6]);

    /** 24시간 이상 미감지된 슬롯을 해제하여 재활용 가능하게 합니다. */
    void cleanup_stale(uint32_t now_ms);

    /** MAC을 "AA:BB:CC:DD:EE:FF" 형태로 변환. 로그용. */
    static void mac_to_str(const uint8_t *mac, char *buf, size_t buf_size);
};
