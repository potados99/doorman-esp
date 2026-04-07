#include "bt_manager.h"
#include "config_service.h"
#include "control_task.h"
#include "door_control.h"
#include "http_server.h"
#include "device_config_service.h"
#include "sm_task.h"
#include "wifi.h"

#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <nvs.h>
#include <nvs_flash.h>

static const char *TAG = "main";

/** 연속 panic reset N회 이상이면 세이프 모드 진입. */
static constexpr int kPanicThreshold = 3;

/** 정상 부팅 후 이 시간(초)이 지나면 panic 카운터를 0으로 리셋. */
static constexpr int kSafeModeResetDelaySec = 60;

static constexpr const char *kNvsNamespace = "sys";
static constexpr const char *kKeyPanicCount = "panic_cnt";

static bool s_safe_mode = false;

bool is_safe_mode() { return s_safe_mode; }

/**
 * NVS에서 연속 panic 카운터를 읽고 갱신한다.
 *
 * - 직전 리셋이 panic이면 카운터 +1
 * - 그 외(정상 리셋, 전원 사이클 등)면 카운터 0으로 리셋
 * - 카운터 >= kPanicThreshold이면 세이프 모드 플래그 세팅
 *
 * NVS open 실패 시에도 세이프 모드 없이 정상 부팅한다 (fail-open).
 */
static void check_safe_mode() {
    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "Reset reason: %d", static_cast<int>(reason));

    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }

    uint8_t count = 0;
    nvs_get_u8(handle, kKeyPanicCount, &count);  // 키 없으면 count=0 유지

    if (reason == ESP_RST_PANIC) {
        count++;
        ESP_LOGW(TAG, "Consecutive panic count: %u", count);
    } else {
        count = 0;
    }

    nvs_set_u8(handle, kKeyPanicCount, count);
    nvs_commit(handle);
    nvs_close(handle);

    if (count >= kPanicThreshold) {
        s_safe_mode = true;
        ESP_LOGE(TAG, "*** SAFE MODE *** (%u consecutive panics, threshold=%d)",
                 count, kPanicThreshold);
    }
}

/**
 * 정상 가동 확정 후 panic 카운터를 0으로 리셋하는 타이머 콜백.
 * 이 시점까지 crash 없이 살아남았으면 환경이 안정적이라고 판단.
 */
static void reset_panic_counter(void *) {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, kKeyPanicCount, 0);
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "Panic counter reset (stable for %ds)", kSafeModeResetDelaySec);
    }
}

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

    // 연속 panic 카운터 확인 → 세이프 모드 판단
    check_safe_mode();

    ESP_LOGI(TAG, "Starting Doorman...%s", s_safe_mode ? " [SAFE MODE]" : "");

    // AppConfig 서비스 초기화 (NVS에서 설정 로드)
    config_service_init();

    // per-device config 서비스 초기화 (NVS에서 설정 로드 + mutex 생성)
    device_config_service_init();

    // GPIO 설정
    door_control_init();

    // WiFi: NVS 크레덴셜 확인 → STA 시도 → 실패 시 SoftAP 폴백
    WifiMode mode = wifi_start();

    // 모드에 따라 다른 웹서버 구성 (STA에서 WS 로그 스트리밍 포함)
    start_webserver(mode);

    if (s_safe_mode) {
        ESP_LOGW(TAG, "Safe mode: BT/SM/Control tasks skipped. OTA and web UI available.");
        return;
    }

    // ── 이하 정상 모드에서만 실행 ──

    // Control 태스크: GPIO 펄스 명령을 큐로 직렬화
    control_task_start();

    // SM 태스크: StateMachine 소유, BT 이벤트 처리 → Unlock 판단
    sm_task_start();

    // BT Manager: 듀얼모드 presence 감지 + 페어링
    ESP_ERROR_CHECK(bt_manager_start());

    // 정상 부팅 후 60초 뒤 panic 카운터 리셋 (안정 확정)
    const esp_timer_create_args_t timer_args = {
        .callback = reset_panic_counter,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "panic_rst",
        .skip_unhandled_events = false,
    };
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&timer_args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, static_cast<uint64_t>(kSafeModeResetDelaySec) * 1000000);
    }

    if (mode == WifiMode::STA) {
        ESP_LOGI(TAG, "Ready (STA mode). Visit http://doorman.local");
    } else {
        ESP_LOGI(TAG, "Ready (SoftAP mode). Connect to 'Doorman-Setup' (pw: 12345678), visit http://192.168.4.1");
    }
}
