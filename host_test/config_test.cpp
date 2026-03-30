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
