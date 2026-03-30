#include <gtest/gtest.h>
#include "statemachine.h"

class StateMachineTest : public ::testing::Test {
protected:
    AppConfig cfg{
        .presence_timeout_ms = 15000,     // 퇴실 타임아웃
        .rssi_threshold = -70,
        .enter_window_ms = 5000,          // 진입 판단 윈도우
        .enter_min_count = 3,             // 윈도우 내 최소 관측 수
        .auto_unlock_enabled = true,
    };
    const uint8_t mac_a[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
    const uint8_t mac_b[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};

    /**
     * 헬퍼: 윈도우 내에 enter_min_count 횟수만큼 feed를 보내어 진입 조건을 충족시킨다.
     * 1초 간격으로 관측을 보내고 마지막 feed 시각을 반환.
     */
    uint32_t feed_enter(StateMachine &sm, const uint8_t (&mac)[6], uint32_t start_ms, int8_t rssi = -65) {
        uint32_t t = start_ms;
        for (uint32_t i = 0; i < cfg.enter_min_count; ++i) {
            sm.feed(mac, true, t, rssi);
            if (i + 1 < cfg.enter_min_count) {
                t += 1000;
            }
        }
        return t;
    }
};

// 윈도우 내 3회 관측 → tick()에서 재실 전환 → Unlock
TEST_F(StateMachineTest, FirstDetection) {
    StateMachine sm(cfg);
    uint32_t t = feed_enter(sm, mac_a, 1000);
    EXPECT_EQ(sm.tick(t), Action::Unlock);
}

// 관측 1회만 → tick()에서 진입 안 됨 (NoOp)
TEST_F(StateMachineTest, InsufficientObservations) {
    StateMachine sm(cfg);
    sm.feed(mac_a, true, 1000, -65);
    EXPECT_EQ(sm.tick(1000), Action::NoOp);
}

// 관측 3회이지만 간격이 윈도우(5초) 초과 → 진입 안 됨
TEST_F(StateMachineTest, ObservationsOutsideWindow) {
    StateMachine sm(cfg);
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
    StateMachine sm(cfg);
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
    StateMachine sm(cfg);
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
    StateMachine sm(cfg);
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
    StateMachine sm(cfg);
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
    StateMachine sm(cfg);
    EXPECT_EQ(sm.device_count(), 0);

    sm.feed(mac_a, true, 1000, -65);
    EXPECT_EQ(sm.device_count(), 1);

    sm.feed(mac_b, true, 1000, -65);
    EXPECT_EQ(sm.device_count(), 2);

    // 같은 MAC을 다시 feed해도 카운트 변하지 않음
    sm.feed(mac_a, true, 2000, -65);
    EXPECT_EQ(sm.device_count(), 2);
}

// 슬롯 30개 초과 feed → 무시
TEST_F(StateMachineTest, ExceedingMaxDevicesIsIgnored) {
    StateMachine sm(cfg);

    // 30개 슬롯 채우기
    for (int i = 0; i < StateMachine::kMaxDevices; ++i) {
        uint8_t mac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, static_cast<uint8_t>(i)};
        sm.feed(mac, true, 1000, -65);
    }
    EXPECT_EQ(sm.device_count(), 30);

    // 31번째 MAC은 무시
    uint8_t extra_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    sm.feed(extra_mac, true, 1000, -65);
    EXPECT_EQ(sm.device_count(), 30);
}

// 복수 기기 독립성
TEST_F(StateMachineTest, MultipleDevicesAreIndependent) {
    StateMachine sm(cfg);

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
    StateMachine sm(cfg);
    uint32_t t = feed_enter(sm, mac_a, 1000);
    EXPECT_EQ(sm.tick(t), Action::Unlock);

    // 정확히 presence_timeout_ms(15000) 경과 시점
    // t - last_seen = 15000 >= 15000 → 미감지 전환
    EXPECT_EQ(sm.tick(t + 15000), Action::NoOp);

    // 미감지 전환 확인: 재감지 → Unlock
    uint32_t t2 = feed_enter(sm, mac_a, t + 16000);
    EXPECT_EQ(sm.tick(t2), Action::Unlock);
}

// auto_unlock_enabled=false면 감지해도 Unlock 안 함
TEST_F(StateMachineTest, AutoUnlockDisabledSuppressesUnlock) {
    cfg.auto_unlock_enabled = false;
    StateMachine sm(cfg);

    uint32_t t = feed_enter(sm, mac_a, 1000);
    EXPECT_EQ(sm.tick(t), Action::NoOp);

    // 타임아웃 경과 후 재감지 — 여전히 NoOp
    EXPECT_EQ(sm.tick(t + 15001), Action::NoOp);
    uint32_t t2 = feed_enter(sm, mac_a, t + 17000);
    EXPECT_EQ(sm.tick(t2), Action::NoOp);
}

// auto_unlock_enabled=false에서 true로 변경 시, 이미 detected인 기기는 일괄 Unlock 안 함
TEST_F(StateMachineTest, AutoUnlockReenabledDoesNotCauseFlood) {
    cfg.auto_unlock_enabled = false;
    StateMachine sm(cfg);

    // 비활성 상태에서 감지 — Unlock 안 함
    uint32_t t = feed_enter(sm, mac_a, 1000);
    EXPECT_EQ(sm.tick(t), Action::NoOp);

    // 다시 활성화 → 이미 detected인 기기는 "이미 처리됨"으로 마킹됨
    cfg.auto_unlock_enabled = true;
    sm.update_config(cfg);

    // 이미 감지 중인 기기에 대해 Unlock이 발생하지 않아야 함 (일괄 Unlock 방지)
    EXPECT_EQ(sm.tick(t + 1), Action::NoOp);

    // 퇴실 후 재감지하면 Unlock
    EXPECT_EQ(sm.tick(t + 15001), Action::NoOp);  // 타임아웃으로 미감지 전환
    uint32_t t2 = feed_enter(sm, mac_a, t + 17000);
    EXPECT_EQ(sm.tick(t2), Action::Unlock);
}

// 오래된 슬롯 정리
TEST_F(StateMachineTest, StaleDevicesAreCleaned) {
    StateMachine sm(cfg);

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
    StateMachine sm(cfg);

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
    StateMachine sm(cfg);

    // Classic (rssi=0)으로 3회 관측 → 진입
    sm.feed(mac_a, true, 1000, 0);
    sm.feed(mac_a, true, 2000, 0);
    sm.feed(mac_a, true, 3000, 0);
    EXPECT_EQ(sm.tick(3000), Action::Unlock);
}
