#include <gtest/gtest.h>
#include "config.h"

TEST(ConfigTest, DefaultValuesAreValid) {
    AppConfig cfg;
    EXPECT_TRUE(validate(cfg));
}

TEST(ConfigTest, CooldownSecExceedingMaxIsRejected) {
    AppConfig cfg;
    cfg.cooldown_sec = 3601;
    EXPECT_FALSE(validate(cfg));
}

TEST(ConfigTest, CooldownSecZeroIsAllowed) {
    AppConfig cfg;
    cfg.cooldown_sec = 0;
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
