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

// NVS namespace "net"에 WiFi 크레덴셜을 저장합니다. 저장만 하고 재연결은
// 호출자가 재부팅 등으로 유도합니다 (wifi 드라이버 재시작 비용 회피).
void wifi_save_credentials(const char *ssid, const char *password);

#endif //DOORMAN_ESP_WIFI_H
