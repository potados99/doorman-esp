#include <gtest/gtest.h>
#include "device.h"

// ── DeviceConfig 테스트 ───────────────────────────────────────────────────────

TEST(DeviceConfigTest, DefaultValuesAreValid) {
    DeviceConfig cfg;
    EXPECT_TRUE(validate_device_config(cfg));
}

// rssi_threshold 경계값
TEST(DeviceConfigTest, RssiThresholdMinBoundaryIsValid) {
    DeviceConfig cfg;
    cfg.rssi_threshold = -100;
    EXPECT_TRUE(validate_device_config(cfg));
}

TEST(DeviceConfigTest, RssiThresholdMaxBoundaryIsValid) {
    DeviceConfig cfg;
    cfg.rssi_threshold = 0;
    EXPECT_TRUE(validate_device_config(cfg));
}

TEST(DeviceConfigTest, RssiThresholdBelowMinIsRejected) {
    DeviceConfig cfg;
    cfg.rssi_threshold = -101;
    EXPECT_FALSE(validate_device_config(cfg));
}

TEST(DeviceConfigTest, RssiThresholdAboveMaxIsRejected) {
    DeviceConfig cfg;
    cfg.rssi_threshold = 1;
    EXPECT_FALSE(validate_device_config(cfg));
}

// presence_timeout_ms 경계값
TEST(DeviceConfigTest, PresenceTimeoutMinBoundaryIsValid) {
    DeviceConfig cfg;
    cfg.presence_timeout_ms = 1;
    EXPECT_TRUE(validate_device_config(cfg));
}

TEST(DeviceConfigTest, PresenceTimeoutMaxBoundaryIsValid) {
    DeviceConfig cfg;
    cfg.presence_timeout_ms = 60000;
    EXPECT_TRUE(validate_device_config(cfg));
}

TEST(DeviceConfigTest, PresenceTimeoutZeroIsRejected) {
    DeviceConfig cfg;
    cfg.presence_timeout_ms = 0;
    EXPECT_FALSE(validate_device_config(cfg));
}

TEST(DeviceConfigTest, PresenceTimeoutAboveMaxIsRejected) {
    DeviceConfig cfg;
    cfg.presence_timeout_ms = 60001;
    EXPECT_FALSE(validate_device_config(cfg));
}

// enter_window_ms 경계값
TEST(DeviceConfigTest, EnterWindowMinBoundaryIsValid) {
    DeviceConfig cfg;
    cfg.enter_window_ms = 1000;
    EXPECT_TRUE(validate_device_config(cfg));
}

TEST(DeviceConfigTest, EnterWindowMaxBoundaryIsValid) {
    DeviceConfig cfg;
    cfg.enter_window_ms = 30000;
    EXPECT_TRUE(validate_device_config(cfg));
}

TEST(DeviceConfigTest, EnterWindowBelowMinIsRejected) {
    DeviceConfig cfg;
    cfg.enter_window_ms = 999;
    EXPECT_FALSE(validate_device_config(cfg));
}

TEST(DeviceConfigTest, EnterWindowAboveMaxIsRejected) {
    DeviceConfig cfg;
    cfg.enter_window_ms = 30001;
    EXPECT_FALSE(validate_device_config(cfg));
}

// enter_min_count 경계값
TEST(DeviceConfigTest, EnterMinCountMinBoundaryIsValid) {
    DeviceConfig cfg;
    cfg.enter_min_count = 1;
    EXPECT_TRUE(validate_device_config(cfg));
}

TEST(DeviceConfigTest, EnterMinCountMaxBoundaryIsValid) {
    DeviceConfig cfg;
    cfg.enter_min_count = 10;
    EXPECT_TRUE(validate_device_config(cfg));
}

TEST(DeviceConfigTest, EnterMinCountZeroIsRejected) {
    DeviceConfig cfg;
    cfg.enter_min_count = 0;
    EXPECT_FALSE(validate_device_config(cfg));
}

TEST(DeviceConfigTest, EnterMinCountAboveMaxIsRejected) {
    DeviceConfig cfg;
    cfg.enter_min_count = 11;
    EXPECT_FALSE(validate_device_config(cfg));
}

// alias 검증
TEST(DeviceConfigTest, AliasEmptyStringIsAllowed) {
    DeviceConfig cfg;
    // alias 기본값은 {} (null로 채워짐) — 빈 문자열
    EXPECT_TRUE(validate_device_config(cfg));
}

TEST(DeviceConfigTest, AliasAlphanumericIsAllowed) {
    DeviceConfig cfg;
    strncpy(cfg.alias, "Device01", sizeof(cfg.alias) - 1);
    EXPECT_TRUE(validate_device_config(cfg));
}

TEST(DeviceConfigTest, AliasWithSpaceHyphenUnderscoreIsAllowed) {
    DeviceConfig cfg;
    strncpy(cfg.alias, "My-Device_1", sizeof(cfg.alias) - 1);
    EXPECT_TRUE(validate_device_config(cfg));
}

TEST(DeviceConfigTest, AliasWithCommonDeviceNamePunctuationIsAllowed) {
    DeviceConfig cfg;
    strncpy(cfg.alias, "John's AirPods (2)+", sizeof(cfg.alias) - 1);
    EXPECT_TRUE(validate_device_config(cfg));
}

TEST(DeviceConfigTest, AliasWithDoubleQuoteIsRejected) {
    DeviceConfig cfg;
    strncpy(cfg.alias, "Bad\"Name", sizeof(cfg.alias) - 1);
    EXPECT_FALSE(validate_device_config(cfg));
}

TEST(DeviceConfigTest, AliasWithBackslashIsRejected) {
    DeviceConfig cfg;
    strncpy(cfg.alias, "Bad\\Name", sizeof(cfg.alias) - 1);
    EXPECT_FALSE(validate_device_config(cfg));
}

TEST(DeviceConfigTest, AliasWithLessThanIsRejected) {
    DeviceConfig cfg;
    strncpy(cfg.alias, "Bad<Name", sizeof(cfg.alias) - 1);
    EXPECT_FALSE(validate_device_config(cfg));
}

TEST(DeviceConfigTest, AliasWithGreaterThanIsRejected) {
    DeviceConfig cfg;
    strncpy(cfg.alias, "Bad>Name", sizeof(cfg.alias) - 1);
    EXPECT_FALSE(validate_device_config(cfg));
}

TEST(DeviceConfigTest, VersionMismatchIsRejected) {
    DeviceConfig cfg;
    cfg.version = 99;
    EXPECT_FALSE(validate_device_config(cfg));
}

TEST(DeviceConfigTest, AliasWithKoreanIsAllowed) {
    DeviceConfig cfg;
    // "현관" — UTF-8: EC 98 84 EA B4 80
    const char *korean = "\xEC\x98\x84\xEA\xB4\x80";
    strncpy(cfg.alias, korean, sizeof(cfg.alias) - 1);
    EXPECT_TRUE(validate_device_config(cfg));
}
