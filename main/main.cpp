#include "bt_manager.h"
#include "config_service.h"
#include "control_task.h"
#include "door_control.h"
#include "http_server.h"
#include "sm_task.h"
#include "wifi.h"

#include <esp_log.h>
#include <esp_ota_ops.h>
#include <nvs_flash.h>

static const char *TAG = "main";

extern "C" void app_main(void) {
    // NVS — WiFi driver와 앱 설정 모두 사용
    esp_err_t ret = nvs_flash_init();
    ESP_LOGI(TAG, "nvs_flash_init() = %s (%d)", esp_err_to_name(ret), ret);
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS erase triggered by: %s", esp_err_to_name(ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /**
     * OTA 롤백 방지: 이 펌웨어가 정상임을 bootloader에 확정한다.
     * 이걸 안 부르면 전원 사이클 시 bootloader가 이전 파티션으로 롤백한다.
     */
    esp_ota_mark_app_valid_cancel_rollback();

    ESP_LOGI(TAG, "Starting Doorman...");

    // AppConfig 서비스 초기화 (NVS에서 설정 로드)
    config_service_init();

    // GPIO 설정
    door_control_init();

    // WiFi: NVS 크레덴셜 확인 → STA 시도 → 실패 시 SoftAP 폴백
    WifiMode mode = wifi_start();

    // 모드에 따라 다른 웹서버 구성 (STA에서 WS 로그 스트리밍 포함)
    start_webserver(mode);

    // Control 태스크: GPIO 펄스 명령을 큐로 직렬화
    control_task_start();

    // SM 태스크: StateMachine 소유, BT 이벤트 처리 → Unlock 판단
    AppConfig cfg = app_config_get();
    sm_task_start(cfg);

    // BT Manager: 듀얼모드 presence 감지 + 페어링 (부팅 시 30초 윈도우)
    ESP_ERROR_CHECK(bt_manager_start());

    if (mode == WifiMode::STA) {
        ESP_LOGI(TAG, "Ready (STA mode). Visit http://doorman.local");
    } else {
        ESP_LOGI(TAG, "Ready (SoftAP mode). Connect to 'Doorman-Setup' (pw: 12345678), visit http://192.168.4.1");
    }
}
