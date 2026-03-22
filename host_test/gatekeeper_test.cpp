#include <gtest/gtest.h>
#include "gatekeeper.h"

class GatekeeperTest : public ::testing::Test {
protected:
    Gatekeeper gatekeeper;
    const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
};

TEST_F(GatekeeperTest, InitialFeedDetectedReturnsNoOpForNow) {
    auto event = gatekeeper.feed(mac, ScanResult::Detected);
    EXPECT_EQ(event, GatekeeperEvent::NoOp);
}

TEST_F(GatekeeperTest, FeedNotDetectedReturnsNoOp) {
    auto event = gatekeeper.feed(mac, ScanResult::NotDetected);
    EXPECT_EQ(event, GatekeeperEvent::NoOp);
}
