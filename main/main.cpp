#include "gatekeeper.h"

static Gatekeeper gatekeeper;

extern "C" void app_main(void)
{
    const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    auto event = gatekeeper.feed(mac, ScanResult::Detected);
    (void)event;
}
