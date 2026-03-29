#include <gtest/gtest.h>
#include "statemachine.h"

class StateMachineTest : public ::testing::Test {
protected:
    AppConfig cfg;
    const uint8_t mac_a[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
    const uint8_t mac_b[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};
};

// 최초 feed(true) -> tick()에서 Unlock
TEST_F(StateMachineTest, FirstFeedSeenThenTickReturnsUnlock) {
    StateMachine sm(cfg);
    sm.feed(mac_a, true, 1000);
    EXPECT_EQ(sm.tick(1000), Action::Unlock);
}

// 감지 유지(쿨다운 내) -> tick()에서 NoOp
TEST_F(StateMachineTest, ContinuousPresenceWithinCooldownReturnsNoOp) {
    StateMachine sm(cfg);
    sm.feed(mac_a, true, 1000);
    EXPECT_EQ(sm.tick(1000), Action::Unlock);

    // 쿨다운(120초=120000ms) 내에 계속 감지
    sm.feed(mac_a, true, 2000);
    EXPECT_EQ(sm.tick(2000), Action::NoOp);

    sm.feed(mac_a, true, 50000);
    EXPECT_EQ(sm.tick(50000), Action::NoOp);
}

// presence_timeout 경과 -> tick()에서 미감지 전환
TEST_F(StateMachineTest, TimeoutCausesUndetected) {
    StateMachine sm(cfg);
    sm.feed(mac_a, true, 1000);
    EXPECT_EQ(sm.tick(1000), Action::Unlock);

    // presence_timeout_ms(5000) 경과 후 tick -> 미감지 전환, 하지만 쿨다운 미충족이므로 NoOp
    EXPECT_EQ(sm.tick(6001), Action::NoOp);

    // 재감지 but 쿨다운 미경과 -> NoOp
    sm.feed(mac_a, true, 7000);
    EXPECT_EQ(sm.tick(7000), Action::NoOp);
}

// 미감지 후 재감지 + 쿨다운 경과 -> tick()에서 Unlock
TEST_F(StateMachineTest, RedetectionAfterCooldownReturnsUnlock) {
    StateMachine sm(cfg);
    sm.feed(mac_a, true, 1000);
    EXPECT_EQ(sm.tick(1000), Action::Unlock);

    // 타임아웃으로 미감지 전환
    EXPECT_EQ(sm.tick(6001), Action::NoOp);

    // 쿨다운(120초=120000ms) 경과 후 재감지
    sm.feed(mac_a, true, 121001);
    EXPECT_EQ(sm.tick(121001), Action::Unlock);
}

// 미감지 후 재감지 + 쿨다운 미경과 -> tick()에서 NoOp
TEST_F(StateMachineTest, RedetectionBeforeCooldownReturnsNoOp) {
    StateMachine sm(cfg);
    sm.feed(mac_a, true, 1000);
    EXPECT_EQ(sm.tick(1000), Action::Unlock);

    // 타임아웃으로 미감지 전환
    EXPECT_EQ(sm.tick(6001), Action::NoOp);

    // 쿨다운 미경과 상태에서 재감지
    sm.feed(mac_a, true, 10000);
    EXPECT_EQ(sm.tick(10000), Action::NoOp);
}

// cooldown_sec=0 -> went_undetected만으로 즉시 재트리거
TEST_F(StateMachineTest, ZeroCooldownRetriggersImmediately) {
    cfg.cooldown_sec = 0;
    StateMachine sm(cfg);

    sm.feed(mac_a, true, 1000);
    EXPECT_EQ(sm.tick(1000), Action::Unlock);

    // 타임아웃으로 미감지 전환
    EXPECT_EQ(sm.tick(6001), Action::NoOp);

    // 즉시 재감지 -> cooldown=0이므로 바로 Unlock
    sm.feed(mac_a, true, 6002);
    EXPECT_EQ(sm.tick(6002), Action::Unlock);
}

// feed(false) -> 즉시 미감지 전환
TEST_F(StateMachineTest, FeedNotSeenCausesImmediateUndetected) {
    StateMachine sm(cfg);
    sm.feed(mac_a, true, 1000);
    EXPECT_EQ(sm.tick(1000), Action::Unlock);

    // feed(false)로 즉시 미감지
    sm.feed(mac_a, false, 2000);

    // 쿨다운 미경과 -> NoOp
    EXPECT_EQ(sm.tick(2000), Action::NoOp);
}

// feed(false) 후 feed(true) + 쿨다운 경과 -> Unlock
TEST_F(StateMachineTest, FeedNotSeenThenRedetectionAfterCooldownReturnsUnlock) {
    StateMachine sm(cfg);
    sm.feed(mac_a, true, 1000);
    EXPECT_EQ(sm.tick(1000), Action::Unlock);

    // feed(false)로 즉시 미감지
    sm.feed(mac_a, false, 2000);

    // 쿨다운 경과 후 재감지
    sm.feed(mac_a, true, 121001);
    EXPECT_EQ(sm.tick(121001), Action::Unlock);
}

// 미등록 MAC feed -> device_count() 증가 (동적 슬롯)
TEST_F(StateMachineTest, NewMacFeedIncreasesDeviceCount) {
    StateMachine sm(cfg);
    EXPECT_EQ(sm.device_count(), 0);

    sm.feed(mac_a, true, 1000);
    EXPECT_EQ(sm.device_count(), 1);

    sm.feed(mac_b, true, 1000);
    EXPECT_EQ(sm.device_count(), 2);

    // 같은 MAC을 다시 feed해도 카운트 변하지 않음
    sm.feed(mac_a, true, 2000);
    EXPECT_EQ(sm.device_count(), 2);
}

// 슬롯 30개 초과 feed -> 무시
TEST_F(StateMachineTest, ExceedingMaxDevicesIsIgnored) {
    StateMachine sm(cfg);

    // 30개 슬롯 채우기
    for (int i = 0; i < StateMachine::kMaxDevices; ++i) {
        uint8_t mac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, static_cast<uint8_t>(i)};
        sm.feed(mac, true, 1000);
    }
    EXPECT_EQ(sm.device_count(), 30);

    // 31번째 MAC은 무시
    uint8_t extra_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    sm.feed(extra_mac, true, 1000);
    EXPECT_EQ(sm.device_count(), 30);
}

// 복수 기기 독립성
TEST_F(StateMachineTest, MultipleDevicesAreIndependent) {
    StateMachine sm(cfg);

    sm.feed(mac_a, true, 1000);
    sm.feed(mac_b, true, 1000);

    // 첫 번째 tick에서 mac_a가 Unlock
    EXPECT_EQ(sm.tick(1000), Action::Unlock);
    // 두 번째 tick에서 mac_b가 Unlock
    EXPECT_EQ(sm.tick(1000), Action::Unlock);

    // 이후 둘 다 쿨다운 내이므로 NoOp
    sm.feed(mac_a, true, 2000);
    sm.feed(mac_b, true, 2000);
    EXPECT_EQ(sm.tick(2000), Action::NoOp);
}

// 정확히 presence_timeout_ms 경계에서 미감지 전환
TEST_F(StateMachineTest, ExactTimeoutBoundaryCausesUndetected) {
    StateMachine sm(cfg);
    sm.feed(mac_a, true, 1000);
    EXPECT_EQ(sm.tick(1000), Action::Unlock);

    // 정확히 presence_timeout_ms(5000) 경과 시점 tick(6000)
    // 6000 - 1000 = 5000 >= 5000 → 미감지 전환
    // 쿨다운 미충족이므로 NoOp 반환, 하지만 내부적으로 미감지 상태
    EXPECT_EQ(sm.tick(6000), Action::NoOp);

    // 미감지 전환 확인: 쿨다운(120s=120000ms) 경과 후 재감지 → Unlock
    sm.feed(mac_a, true, 121001);
    EXPECT_EQ(sm.tick(121001), Action::Unlock);
}

// 정확히 cooldown 경계(120000ms)에서 Unlock
TEST_F(StateMachineTest, ExactCooldownBoundaryReturnsUnlock) {
    StateMachine sm(cfg);
    sm.feed(mac_a, true, 1000);
    EXPECT_EQ(sm.tick(1000), Action::Unlock);

    // 타임아웃으로 미감지 전환
    EXPECT_EQ(sm.tick(6001), Action::NoOp);

    // 정확히 cooldown_sec(120) * 1000 = 120000ms 경과 후 재감지
    sm.feed(mac_a, true, 121000);
    EXPECT_EQ(sm.tick(121000), Action::Unlock);
}

// 오래된 슬롯 정리
TEST_F(StateMachineTest, StaleDevicesAreCleaned) {
    StateMachine sm(cfg);

    sm.feed(mac_a, true, 1000);
    EXPECT_EQ(sm.tick(1000), Action::Unlock);
    EXPECT_EQ(sm.device_count(), 1);

    // 타임아웃으로 미감지 전환
    EXPECT_EQ(sm.tick(6001), Action::NoOp);

    // 24시간(86400000ms) 이상 경과 -> 슬롯 정리
    EXPECT_EQ(sm.tick(86401001), Action::NoOp);
    EXPECT_EQ(sm.device_count(), 0);
}
