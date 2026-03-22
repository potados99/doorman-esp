#include "gatekeeper.h"

GatekeeperEvent Gatekeeper::feed(const uint8_t (&mac)[6], ScanResult result) {
    // TODO: 상태머신 로직 구현
    return GatekeeperEvent::NoOp;
}
