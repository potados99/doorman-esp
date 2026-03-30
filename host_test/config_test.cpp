#include <gtest/gtest.h>
#include "config.h"

TEST(ConfigTest, DefaultValuesAreValid) {
    AppConfig cfg;
    EXPECT_TRUE(validate(cfg));
}

TEST(ConfigTest, PresenceTimeoutZeroIsRejected) {
    AppConfig cfg;
    cfg.presence_timeout_ms = 0;
    EXPECT_FALSE(validate(cfg));
}

TEST(ConfigTest, PresenceTimeoutExceedingMaxIsRejected) {
    AppConfig cfg;
    cfg.presence_timeout_ms = 60001;
    EXPECT_FALSE(validate(cfg));
}

// 경계값 테스트: presence_timeout_ms=1 (최소 허용)
TEST(ConfigTest, PresenceTimeoutMinBoundaryIsValid) {
    AppConfig cfg;
    cfg.presence_timeout_ms = 1;
    EXPECT_TRUE(validate(cfg));
}

// 경계값 테스트: presence_timeout_ms=60000 (최대 허용)
TEST(ConfigTest, PresenceTimeoutMaxBoundaryIsValid) {
    AppConfig cfg;
    cfg.presence_timeout_ms = 60000;
    EXPECT_TRUE(validate(cfg));
}

// enter_window_ms 범위 테스트
TEST(ConfigTest, EnterWindowBelowMinIsRejected) {
    AppConfig cfg;
    cfg.enter_window_ms = 999;
    EXPECT_FALSE(validate(cfg));
}

TEST(ConfigTest, EnterWindowAboveMaxIsRejected) {
    AppConfig cfg;
    cfg.enter_window_ms = 30001;
    EXPECT_FALSE(validate(cfg));
}

TEST(ConfigTest, EnterWindowMinBoundaryIsValid) {
    AppConfig cfg;
    cfg.enter_window_ms = 1000;
    EXPECT_TRUE(validate(cfg));
}

TEST(ConfigTest, EnterWindowMaxBoundaryIsValid) {
    AppConfig cfg;
    cfg.enter_window_ms = 30000;
    EXPECT_TRUE(validate(cfg));
}

// enter_min_count 범위 테스트
TEST(ConfigTest, EnterMinCountZeroIsRejected) {
    AppConfig cfg;
    cfg.enter_min_count = 0;
    EXPECT_FALSE(validate(cfg));
}

TEST(ConfigTest, EnterMinCountAboveMaxIsRejected) {
    AppConfig cfg;
    cfg.enter_min_count = 11;
    EXPECT_FALSE(validate(cfg));
}

TEST(ConfigTest, EnterMinCountMinBoundaryIsValid) {
    AppConfig cfg;
    cfg.enter_min_count = 1;
    EXPECT_TRUE(validate(cfg));
}

TEST(ConfigTest, EnterMinCountMaxBoundaryIsValid) {
    AppConfig cfg;
    cfg.enter_min_count = 10;
    EXPECT_TRUE(validate(cfg));
}
