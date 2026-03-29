#include "sm_task.h"
#include "control_task.h"
#include "statemachine.h"

#include <cstring>

#include <esp_log.h>
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

static void sm_task(void *arg) {
    auto *cfg = static_cast<AppConfig *>(arg);
    StateMachine sm(*cfg);
    delete cfg;

    ESP_LOGI(TAG, "StateMachine initialized (cooldown=%lus, timeout=%lums)",
             (unsigned long)sm.config().cooldown_sec,
             (unsigned long)sm.config().presence_timeout_ms);

    FeedMsg msg;
    while (true) {
        /**
         * 타임아웃 = tick 주기.
         * 메시지가 있으면 feed() 처리, 타임아웃이면 그냥 tick()으로 진행.
         * 이 패턴으로 별도 타이머 없이 주기적 tick을 보장한다.
         */
        BaseType_t got = xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(kTickIntervalMs));

        if (got == pdTRUE) {
            char mac_str[18];
            mac_to_str(msg.mac, mac_str, sizeof(mac_str));
            ESP_LOGI(TAG, "Feed: %s seen=%s", mac_str, msg.seen ? "yes" : "no");

            uint8_t mac_ref[6];
            std::memcpy(mac_ref, msg.mac, 6);
            sm.feed(reinterpret_cast<const uint8_t(&)[6]>(*mac_ref), msg.seen, msg.now_ms);
        }

        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        Action action = sm.tick(now_ms);

        if (action == Action::Unlock) {
            ESP_LOGI(TAG, "Unlock! Sending command to control task");
            control_queue_send(ControlCommand::AutoUnlock);
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

void sm_feed_queue_send(const uint8_t (&mac)[6], bool seen, uint32_t now_ms) {
    if (s_queue == nullptr) {
        ESP_LOGE(TAG, "SM feed queue not initialized");
        return;
    }

    FeedMsg msg = {};
    std::memcpy(msg.mac, mac, 6);
    msg.seen = seen;
    msg.now_ms = now_ms;

    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "SM feed queue full — event dropped");
    }
}
