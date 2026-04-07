#include <gtest/gtest.h>
#include "statemachine.h"

class StateMachineTest : public ::testing::Test {
protected:
    DeviceConfig dev_cfg{
        .version = 1,
        .rssi_threshold = -70,
        ._pad = {},
        .presence_timeout_ms = 15000,
        .enter_window_ms = 5000,
        .enter_min_count = 3,
        .alias = {},
    };
    const uint8_t mac_a[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
    const uint8_t mac_b[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};

    /**
     * 헬퍼: 윈도우 내에 enter_min_count 횟수만큼 feed를 보내어 진입 조건을 충족시킨다.
     * 1초 간격으로 관측을 보내고 마지막 feed 시각을 반환.
     * update_device_config는 호출하지 않으므로, 각 테스트에서 명시적으로 호출해야 함.
     */
    uint32_t feed_enter(StateMachine &sm, const uint8_t (&mac)[6], uint32_t start_ms, int8_t rssi = -65) {
        uint32_t t = start_ms;
        for (uint32_t i = 0; i < dev_cfg.enter_min_count; ++i) {
            sm.feed(mac, true, t, rssi);
            if (i + 1 < dev_cfg.enter_min_count) {
                t += 1000;
            }
        }
        return t;
    }
};

// 윈도우 내 3회 관측 → tick()에서 재실 전환 → Unlock
TEST_F(StateMachineTest, FirstDetection) {
    StateMachine sm;
    sm.update_device_config(mac_a, dev_cfg);
    uint32_t t = feed_enter(sm, mac_a, 1000);
    EXPECT_EQ(sm.tick(t), Action::Unlock);
}

// 관측 1회만 → tick()에서 진입 안 됨 (NoOp)
TEST_F(StateMachineTest, InsufficientObservations) {
    StateMachine sm;
    sm.update_device_config(mac_a, dev_cfg);
    sm.feed(mac_a, true, 1000, -65);
    EXPECT_EQ(sm.tick(1000), Action::NoOp);
}

// 관측 3회이지만 간격이 윈도우(5초) 초과 → 진입 안 됨
TEST_F(StateMachineTest, ObservationsOutsideWindow) {
    StateMachine sm;
    sm.update_device_config(mac_a, dev_cfg);
    // 3회 관측, 각각 3초 간격 = 총 6초 걸림
    sm.feed(mac_a, true, 1000, -65);
    sm.feed(mac_a, true, 4000, -65);
    sm.feed(mac_a, true, 7000, -65);
    // tick 시점(7000)에서: 7000-1000=6000 > 5000(윈도우) → 첫 관측은 윈도우 밖
    // 윈도우 내 관측: 4000, 7000 → 2회 < 3회
    EXPECT_EQ(sm.tick(7000), Action::NoOp);
}

// 재실 후 약한 RSSI feed → 타임아웃 리셋됨 (재실 유지)
TEST_F(StateMachineTest, StayDetectedWithWeakSignal) {
    StateMachine sm;
    sm.update_device_config(mac_a, dev_cfg);
    uint32_t t = feed_enter(sm, mac_a, 1000);
    EXPECT_EQ(sm.tick(t), Action::Unlock);

    // 10초 후 약한 신호 (RSSI -80, threshold -70 미만)로 feed
    // 이미 detected이므로 RSSI 무시, last_seen 갱신
    sm.feed(mac_a, true, t + 10000, -80);
    // 원래 마지막 feed(t) 기준이면 타임아웃(15초)이 t+15000이지만
    // 약한 신호 feed로 last_seen이 갱신되었으므로 t+10000 기준으로 리셋
    // t+20000에서 체크: t+20000 - (t+10000) = 10000 < 15000 → 아직 재실
    EXPECT_EQ(sm.tick(t + 20000), Action::NoOp);

    // t+25000에서 체크: t+25000 - (t+10000) = 15000 >= 15000 → 퇴실
    EXPECT_EQ(sm.tick(t + 25000), Action::NoOp);
    // 퇴실 확인: 다시 3회 관측하면 재진입 → Unlock
    uint32_t t2 = feed_enter(sm, mac_a, t + 26000);
    EXPECT_EQ(sm.tick(t2), Action::Unlock);
}

// 재실 후 feed 중단 → presence_timeout_ms(15초) 후 퇴실
TEST_F(StateMachineTest, DepartureTimeout) {
    StateMachine sm;
    sm.update_device_config(mac_a, dev_cfg);
    uint32_t t = feed_enter(sm, mac_a, 1000);
    EXPECT_EQ(sm.tick(t), Action::Unlock);

    // 15초 경과 → 퇴실
    EXPECT_EQ(sm.tick(t + 15000), Action::NoOp);

    // 퇴실 확인: 재관측 → Unlock
    uint32_t t2 = feed_enter(sm, mac_a, t + 16000);
    EXPECT_EQ(sm.tick(t2), Action::Unlock);
}

// 퇴실 후 다시 3회 관측 → 재진입 → Unlock
TEST_F(StateMachineTest, ReentryAfterDeparture) {
    StateMachine sm;
    sm.update_device_config(mac_a, dev_cfg);
    uint32_t t = feed_enter(sm, mac_a, 1000);
    EXPECT_EQ(sm.tick(t), Action::Unlock);

    // 퇴실 (타임아웃)
    EXPECT_EQ(sm.tick(t + 15000), Action::NoOp);

    // 재진입: 3회 관측
    uint32_t t2 = feed_enter(sm, mac_a, t + 20000);
    EXPECT_EQ(sm.tick(t2), Action::Unlock);
}

// 감지 유지 → tick()에서 NoOp
TEST_F(StateMachineTest, ContinuousPresenceReturnsNoOp) {
    StateMachine sm;
    sm.update_device_config(mac_a, dev_cfg);
    uint32_t t = feed_enter(sm, mac_a, 1000);
    EXPECT_EQ(sm.tick(t), Action::Unlock);

    // 계속 감지 중이면 NoOp
    sm.feed(mac_a, true, t + 2000);
    EXPECT_EQ(sm.tick(t + 2000), Action::NoOp);

    sm.feed(mac_a, true, t + 5000);
    EXPECT_EQ(sm.tick(t + 5000), Action::NoOp);
}

// 미등록 MAC feed → device_count() 증가 (동적 슬롯)
TEST_F(StateMachineTest, NewMacFeedIncreasesDeviceCount) {
    StateMachine sm;
    EXPECT_EQ(sm.device_count(), 0);

    sm.feed(mac_a, true, 1000, -65);
    EXPECT_EQ(sm.device_count(), 1);

    sm.feed(mac_b, true, 1000, -65);
    EXPECT_EQ(sm.device_count(), 2);

    // 같은 MAC을 다시 feed해도 카운트 변하지 않음
    sm.feed(mac_a, true, 2000, -65);
    EXPECT_EQ(sm.device_count(), 2);
}

// 슬롯 최대치 초과 feed → 무시
TEST_F(StateMachineTest, ExceedingMaxDevicesIsIgnored) {
    StateMachine sm;

    for (int i = 0; i < StateMachine::kMaxDevices; ++i) {
        uint8_t mac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, static_cast<uint8_t>(i)};
        sm.feed(mac, true, 1000, -65);
    }
    EXPECT_EQ(sm.device_count(), kMaxTrackedDevices);

    // 초과 MAC은 무시
    uint8_t extra_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    sm.feed(extra_mac, true, 1000, -65);
    EXPECT_EQ(sm.device_count(), kMaxTrackedDevices);
}

// 복수 기기 독립성
TEST_F(StateMachineTest, MultipleDevicesAreIndependent) {
    StateMachine sm;
    sm.update_device_config(mac_a, dev_cfg);
    sm.update_device_config(mac_b, dev_cfg);

    uint32_t t = feed_enter(sm, mac_a, 1000);
    feed_enter(sm, mac_b, 1000);

    // 첫 번째 tick에서 mac_a가 Unlock
    EXPECT_EQ(sm.tick(t), Action::Unlock);
    // 두 번째 tick에서 mac_b가 Unlock
    EXPECT_EQ(sm.tick(t), Action::Unlock);

    // 이후 둘 다 감지 유지 중이므로 NoOp
    sm.feed(mac_a, true, t + 2000);
    sm.feed(mac_b, true, t + 2000);
    EXPECT_EQ(sm.tick(t + 2000), Action::NoOp);
}

// 정확히 presence_timeout_ms 경계에서 미감지 전환
TEST_F(StateMachineTest, ExactTimeoutBoundaryCausesUndetected) {
    StateMachine sm;
    sm.update_device_config(mac_a, dev_cfg);
    uint32_t t = feed_enter(sm, mac_a, 1000);
    EXPECT_EQ(sm.tick(t), Action::Unlock);

    // 정확히 presence_timeout_ms(15000) 경과 시점
    // t - last_seen = 15000 >= 15000 → 미감지 전환
    EXPECT_EQ(sm.tick(t + 15000), Action::NoOp);

    // 미감지 전환 확인: 재감지 → Unlock
    uint32_t t2 = feed_enter(sm, mac_a, t + 16000);
    EXPECT_EQ(sm.tick(t2), Action::Unlock);
}

// SM은 항상 Unlock을 판정한다 (드라이런).
// auto_unlock 억제는 SM Task에서 수행. SM 자체는 조건 없이 Unlock 반환.
TEST_F(StateMachineTest, AlwaysReturnsUnlock) {
    StateMachine sm;
    sm.update_device_config(mac_a, dev_cfg);

    uint32_t t = feed_enter(sm, mac_a, 1000);
    EXPECT_EQ(sm.tick(t), Action::Unlock);
}

// 오래된 슬롯 정리
TEST_F(StateMachineTest, StaleDevicesAreCleaned) {
    StateMachine sm;
    sm.update_device_config(mac_a, dev_cfg);

    uint32_t t = feed_enter(sm, mac_a, 1000);
    EXPECT_EQ(sm.tick(t), Action::Unlock);
    EXPECT_EQ(sm.device_count(), 1);

    // 타임아웃으로 미감지 전환
    EXPECT_EQ(sm.tick(t + 15001), Action::NoOp);

    // 24시간(86400000ms) 이상 경과 → 슬롯 정리
    EXPECT_EQ(sm.tick(t + 86400001), Action::NoOp);
    EXPECT_EQ(sm.device_count(), 0);
}

// RSSI가 threshold 미만이면 미감지 상태에서 진입 판단에 반영 안 함
TEST_F(StateMachineTest, WeakRssiIgnoredForEntry) {
    StateMachine sm;
    sm.update_device_config(mac_a, dev_cfg);

    // 약한 RSSI(-80)로 3회 관측 → 진입 안 됨
    sm.feed(mac_a, true, 1000, -80);
    sm.feed(mac_a, true, 2000, -80);
    sm.feed(mac_a, true, 3000, -80);
    EXPECT_EQ(sm.tick(3000), Action::NoOp);

    // 강한 RSSI(-65)로 3회 관측 → 진입
    sm.feed(mac_a, true, 4000, -65);
    sm.feed(mac_a, true, 5000, -65);
    sm.feed(mac_a, true, 6000, -65);
    EXPECT_EQ(sm.tick(6000), Action::Unlock);
}

// Classic(rssi=0)은 RSSI 필터링 건너뜀
TEST_F(StateMachineTest, ClassicBypassesRssiFilter) {
    StateMachine sm;
    sm.update_device_config(mac_a, dev_cfg);

    // Classic (rssi=0)으로 3회 관측 → 진입
    sm.feed(mac_a, true, 1000, 0);
    sm.feed(mac_a, true, 2000, 0);
    sm.feed(mac_a, true, 3000, 0);
    EXPECT_EQ(sm.tick(3000), Action::Unlock);
}

// ── 신규 테스트 ──────────────────────────────────────────────────────────────

// 기기별 RSSI threshold: mac_a(-60), mac_b(-80). rssi=-65 feed → mac_a만 진입
TEST_F(StateMachineTest, PerDeviceRssiThreshold) {
    StateMachine sm;

    DeviceConfig cfg_a = dev_cfg;
    cfg_a.rssi_threshold = -60;   // 엄격: -65 < -60 → 무시

    DeviceConfig cfg_b = dev_cfg;
    cfg_b.rssi_threshold = -80;   // 느슨: -65 >= -80 → 통과

    sm.update_device_config(mac_a, cfg_a);
    sm.update_device_config(mac_b, cfg_b);

    // rssi=-65로 양쪽 모두 3회 feed
    for (int i = 0; i < 3; ++i) {
        sm.feed(mac_a, true, 1000 + i * 1000, -65);
        sm.feed(mac_b, true, 1000 + i * 1000, -65);
    }

    // mac_a: threshold=-60이므로 -65는 통과 안 함 → NoOp
    // mac_b: threshold=-80이므로 -65는 통과 → Unlock
    Action first  = sm.tick(3000);
    Action second = sm.tick(3000);

    // mac_a는 진입 못했으므로 두 tick 중 하나만 Unlock (mac_b만)
    EXPECT_TRUE((first == Action::Unlock && second == Action::NoOp) ||
                (first == Action::NoOp  && second == Action::NoOp));
    // 정확히 하나만 Unlock이어야 함 (mac_b)
    int unlock_count = (first == Action::Unlock ? 1 : 0) + (second == Action::Unlock ? 1 : 0);
    EXPECT_EQ(unlock_count, 1);
}

// 기기별 타임아웃: mac_a=5000ms, mac_b=20000ms. 10초 경과 → mac_a만 퇴실
TEST_F(StateMachineTest, PerDeviceTimeout) {
    StateMachine sm;

    DeviceConfig cfg_a = dev_cfg;
    cfg_a.presence_timeout_ms = 5000;    // 짧은 타임아웃

    DeviceConfig cfg_b = dev_cfg;
    cfg_b.presence_timeout_ms = 20000;   // 긴 타임아웃

    sm.update_device_config(mac_a, cfg_a);
    sm.update_device_config(mac_b, cfg_b);

    // 양쪽 모두 진입
    uint32_t t = feed_enter(sm, mac_a, 1000);
    feed_enter(sm, mac_b, 1000);

    EXPECT_EQ(sm.tick(t), Action::Unlock);   // mac_a 또는 mac_b Unlock
    EXPECT_EQ(sm.tick(t), Action::Unlock);   // 나머지 하나 Unlock

    // 10초 경과: mac_a(5000ms timeout) → 퇴실, mac_b(20000ms) → 아직 재실
    uint32_t t2 = t + 10000;
    EXPECT_EQ(sm.tick(t2), Action::NoOp);   // 퇴실 전환만, Unlock 없음

    // mac_a 재진입 → Unlock (퇴실 후 재감지)
    uint32_t t3 = feed_enter(sm, mac_a, t2 + 1000);
    EXPECT_EQ(sm.tick(t3), Action::Unlock);

    // mac_b는 아직 재실 중 → NoOp
    sm.feed(mac_b, true, t3 + 1000);
    EXPECT_EQ(sm.tick(t3 + 1000), Action::NoOp);
}

// 기기별 진입 윈도우: mac_a=2000ms(짧음), mac_b=8000ms(긺). 같은 관측으로 차이 검증
TEST_F(StateMachineTest, PerDeviceEnterWindow) {
    StateMachine sm;

    DeviceConfig cfg_a = dev_cfg;
    cfg_a.enter_window_ms = 2000;   // 2초 윈도우 — 1초 간격 3회 = 2초 → 통과
    cfg_a.enter_min_count = 3;

    DeviceConfig cfg_b = dev_cfg;
    cfg_b.enter_window_ms = 1500;   // 1.5초 윈도우 — 1초 간격 3회 = 2초 → 초과
    cfg_b.enter_min_count = 3;

    sm.update_device_config(mac_a, cfg_a);
    sm.update_device_config(mac_b, cfg_b);

    // 1초 간격으로 3회 feed (총 2초)
    sm.feed(mac_a, true, 1000, -65);
    sm.feed(mac_b, true, 1000, -65);
    sm.feed(mac_a, true, 2000, -65);
    sm.feed(mac_b, true, 2000, -65);
    sm.feed(mac_a, true, 3000, -65);
    sm.feed(mac_b, true, 3000, -65);

    // tick(3000): mac_a(window=2000) → 3000-1000=2000 <= 2000 → 3회 모두 유효 → 진입
    //             mac_b(window=1500) → 3000-1000=2000 > 1500 → 첫 관측 제외 → 2회 < 3회
    Action first  = sm.tick(3000);
    Action second = sm.tick(3000);

    int unlock_count = (first == Action::Unlock ? 1 : 0) + (second == Action::Unlock ? 1 : 0);
    EXPECT_EQ(unlock_count, 1);  // mac_a만 진입
}

// 감지 중 config 변경 시 다음 판단에 새 config 적용
TEST_F(StateMachineTest, UpdateDeviceConfigMidSession) {
    StateMachine sm;
    sm.update_device_config(mac_a, dev_cfg);   // timeout=15000

    // mac_a 진입
    uint32_t t = feed_enter(sm, mac_a, 1000);
    EXPECT_EQ(sm.tick(t), Action::Unlock);

    // config 변경: timeout을 3000ms로 단축
    DeviceConfig new_cfg = dev_cfg;
    new_cfg.presence_timeout_ms = 3000;
    sm.update_device_config(mac_a, new_cfg);

    // 5초 경과: 새 timeout(3000ms) 기준으로 퇴실
    EXPECT_EQ(sm.tick(t + 5000), Action::NoOp);

    // 퇴실 확인: 재진입 → Unlock
    uint32_t t2 = feed_enter(sm, mac_a, t + 6000);
    EXPECT_EQ(sm.tick(t2), Action::Unlock);
}

// remove_device 후 device_count 감소, 해당 MAC feed 시 새 슬롯 생성
TEST_F(StateMachineTest, RemoveDevice) {
    StateMachine sm;
    sm.update_device_config(mac_a, dev_cfg);
    sm.update_device_config(mac_b, dev_cfg);

    sm.feed(mac_a, true, 1000, -65);
    sm.feed(mac_b, true, 1000, -65);
    EXPECT_EQ(sm.device_count(), 2);

    // mac_a 슬롯 제거
    sm.remove_device(mac_a);
    EXPECT_EQ(sm.device_count(), 1);

    // mac_a를 다시 feed → 새 슬롯 생성
    sm.feed(mac_a, true, 2000, -65);
    EXPECT_EQ(sm.device_count(), 2);
}

// dump_states가 valid 슬롯만 복사하고 MAC/detected 등 올바른지 검증
TEST_F(StateMachineTest, DumpStates) {
    StateMachine sm;
    sm.update_device_config(mac_a, dev_cfg);
    sm.update_device_config(mac_b, dev_cfg);

    // mac_a 진입
    uint32_t t = feed_enter(sm, mac_a, 1000);
    EXPECT_EQ(sm.tick(t), Action::Unlock);

    // mac_b는 1회만 feed (미진입)
    sm.feed(mac_b, true, 1000, -65);

    DeviceState out[kMaxTrackedDevices] = {};
    int n = sm.dump_states(out, kMaxTrackedDevices);
    EXPECT_EQ(n, 2);

    // mac_a 슬롯 확인
    bool found_a = false, found_b = false;
    for (int i = 0; i < n; ++i) {
        if (std::memcmp(out[i].mac, mac_a, 6) == 0) {
            EXPECT_TRUE(out[i].detected);
            found_a = true;
        } else if (std::memcmp(out[i].mac, mac_b, 6) == 0) {
            EXPECT_FALSE(out[i].detected);
            found_b = true;
        }
    }
    EXPECT_TRUE(found_a);
    EXPECT_TRUE(found_b);
}

// update_device_config 없이 feed → 기본 DeviceConfig(기본 생성자)로 판단
TEST_F(StateMachineTest, DefaultConfigForNewDevice) {
    StateMachine sm;
    // update_device_config 호출 안 함 → DeviceConfig 기본값 사용
    // 기본값: rssi_threshold=-70, enter_min_count=3, enter_window_ms=5000

    // rssi=-65 (-65 >= -70) 로 3회 feed → 진입 가능해야 함
    sm.feed(mac_a, true, 1000, -65);
    sm.feed(mac_a, true, 2000, -65);
    sm.feed(mac_a, true, 3000, -65);

    EXPECT_EQ(sm.tick(3000), Action::Unlock);
}
