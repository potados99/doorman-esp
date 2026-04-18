#ifndef DOORMAN_ESP_WIFI_H
#define DOORMAN_ESP_WIFI_H

enum class WifiMode {
    SoftAP,
    STA,
};

// Reads WiFi credentials from NVS.
// If found → tries STA (10s timeout), falls back to SoftAP on failure.
// If not found → starts SoftAP directly.
// In STA mode, registers mDNS as "doorman.local".
WifiMode wifi_start();

#endif //DOORMAN_ESP_WIFI_H
