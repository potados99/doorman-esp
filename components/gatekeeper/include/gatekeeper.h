#pragma once

#include <cstdint>
#include <array>

enum class ScanResult {
    Detected,
    NotDetected,
};

enum class GatekeeperEvent {
    Unlock,
    NoOp,
};

class Gatekeeper {
public:
    GatekeeperEvent feed(const uint8_t (&mac)[6], ScanResult result);

private:
};
