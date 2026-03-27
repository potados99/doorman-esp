#include "bt_presence_poc.h"
#include "door_control.h"
#include "http_server.h"
#include "wifi.h"

#include <esp_log.h>
#include <nvs_flash.h>

static const char *TAG = "main";

extern "C" void app_main(void) {
    // NVS — WiFi driver와 앱 설정 모두 사용
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Starting Doorman...");

    door_control_init();

    // WiFi: NVS 크레덴셜 확인 → STA 시도 → 실패 시 SoftAP 폴백
    WifiMode mode = wifi_start();

    // 모드에 따라 다른 웹서버 구성
    start_webserver(mode);

    // BT presence PoC (WiFi 모드와 무관하게 동작)
    ESP_ERROR_CHECK(bt_presence_poc_start());

    if (mode == WifiMode::STA) {
        ESP_LOGI(TAG, "Ready (STA mode). Visit http://doorman.local");
    } else {
        ESP_LOGI(TAG, "Ready (SoftAP mode). Connect to 'Doorman-Setup' (pw: 12345678), visit http://192.168.4.1");
    }
}
