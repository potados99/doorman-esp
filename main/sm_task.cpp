#include "sm_task.h"
#include "config_service.h"
#include "control_task.h"
#include "statemachine.h"

#include <cstring>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

static const char *TAG = "sm";

/**
 * 큐로 전달되는 피드 메시지.
 * BT Manager → SM Task로 감지 이벤트를 넘기기 위한 POD 구조체.
 */
struct FeedMsg {
    uint8_t mac[6];
    bool seen;
    uint32_t now_ms;
    int8_t rssi;  // BLE RSSI (dBm). 0이면 Classic (RSSI 필터링 건너뜀).
};

/**
 * 큐 깊이 16: BLE 스캔은 짧은 시간에 여러 advertising을 수신할 수 있으므로
 * 충분한 버퍼를 확보. 큐가 차면 오래된 이벤트는 드랍되지만,
 * BLE는 곧 다시 advertising을 보내므로 실질적 영향 없다.
 */
static constexpr int kQueueDepth = 16;

/**
 * tick 주기 (밀리초).
 * xQueueReceive 타임아웃으로 사용하여,
 * 피드 메시지가 없어도 주기적으로 tick()을 호출한다.
 * 2초면 presence timeout 판단에 충분한 해상도를 제공한다.
 */
static constexpr int kTickIntervalMs = 2000;

static QueueHandle_t s_queue = nullptr;

static char *mac_to_str(const uint8_t *mac, char *buf, size_t size) {
    snprintf(buf, size, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

/**
 * 시작 후 유예기간 (밀리초).
 * 재부팅 직후 이미 근처에 있는 기기들이 "최초 감지 → Unlock"으로
 * 문을 여는 걸 방지한다. 유예기간 동안 SM은 정상 동작(감지 추적)하되
 * Unlock만 Control Task에 안 보낸다. SM 내부적으로 last_unlock_ms가
 * 세팅되므로, 유예 후에는 이미 "처리 완료" 상태.
 */
static constexpr uint32_t kStartupGraceMs = 15000;

static void sm_task(void *arg) {
    auto *cfg = static_cast<AppConfig *>(arg);
    StateMachine sm(*cfg);
    delete cfg;

    uint32_t start_ms = (uint32_t)(esp_timer_get_time() / 1000);

    ESP_LOGI(TAG, "StateMachine initialized (timeout=%lums, grace=%lums)",
             (unsigned long)sm.config().presence_timeout_ms,
             (unsigned long)kStartupGraceMs);

    FeedMsg msg;
    while (true) {
        BaseType_t got = xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(kTickIntervalMs));

        sm.update_config(app_config_get());

        if (got == pdTRUE) {
            sm.feed(msg.mac, msg.seen, msg.now_ms, msg.rssi);
        }

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        Action action = sm.tick(now_ms);

        /**
         * Unlock 억제: SM은 항상 정상 판정(드라이런). 여기서만 실제 전달 여부 결정.
         * - 유예기간: 재부팅 직후 기존 기기 flood 방지
         * - auto_unlock OFF: 사용자가 명시적으로 꺼놓은 상태
         */
        if (action == Action::Unlock) {
            AppConfig current_cfg = app_config_get();
            bool in_grace = (now_ms - start_ms) < kStartupGraceMs;

            if (in_grace) {
                ESP_LOGI(TAG, "Unlock suppressed (grace %lums remaining)",
                         (unsigned long)(kStartupGraceMs - (now_ms - start_ms)));
            } else if (!current_cfg.auto_unlock_enabled) {
                ESP_LOGI(TAG, "Unlock suppressed (auto_unlock OFF)");
            } else {
                control_queue_send(ControlCommand::AutoUnlock);
            }
        }
    }
}

void sm_task_start(AppConfig cfg) {
    s_queue = xQueueCreate(kQueueDepth, sizeof(FeedMsg));
    configASSERT(s_queue);

    /**
     * cfg를 힙에 복사하여 태스크에 전달.
     * 태스크 시작 후 sm_task() 내부에서 StateMachine 생성에 사용하고 해제.
     * 스택 변수를 포인터로 넘기면 호출자 리턴 시 dangling 되므로 힙 복사가 필수.
     */
    auto *cfg_copy = new AppConfig(cfg);

    BaseType_t ok = xTaskCreatePinnedToCore(
        sm_task,
        "sm_task",
        4096,
        cfg_copy,
        5,
        nullptr,
        tskNO_AFFINITY);
    configASSERT(ok == pdPASS);

    ESP_LOGI(TAG, "SM task started (tick interval=%dms)", kTickIntervalMs);
}

void sm_feed_queue_send(const uint8_t (&mac)[6], bool seen, uint32_t now_ms, int8_t rssi) {
    if (s_queue == nullptr) {
        ESP_LOGE(TAG, "SM feed queue not initialized");
        return;
    }

    FeedMsg msg = {};
    std::memcpy(msg.mac, mac, 6);
    msg.seen = seen;
    msg.now_ms = now_ms;
    msg.rssi = rssi;

    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "SM feed queue full — event dropped");
    }
}
