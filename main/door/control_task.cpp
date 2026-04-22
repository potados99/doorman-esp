#include "door/control_task.h"
#include "door/control.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

static const char *TAG = "ctrl";

/**
 * 큐 깊이 4: 동시에 여러 기기가 감지되어도 최대 4개까지 버퍼링합니다.
 * 실사용에서 동시에 4개 이상의 unlock 요청이 겹칠 가능성은 극히 낮습니다.
 */
static constexpr int kQueueDepth = 4;
static QueueHandle_t s_queue = nullptr;

static void control_task(void *) {
    ControlCommand cmd;
    while (true) {
        /**
         * portMAX_DELAY로 무한 대기합니다.
         * 큐에 메시지가 올 때만 깨어나서 GPIO 펄스를 실행합니다.
         */
        if (xQueueReceive(s_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            const char *reason = (cmd == ControlCommand::AutoUnlock)
                                     ? "auto (BT presence)"
                                     : "manual (web UI)";

            ESP_LOGI(TAG, "Door unlock triggered: %s", reason);

            if (!door_trigger_pulse()) {
                ESP_LOGW(TAG, "Door pulse skipped — already in progress");
            }
            /* door_trigger_pulse()가 blocking이므로 추가 딜레이 불필요 */
        }
    }
}

void control_task_start() {
    s_queue = xQueueCreate(kQueueDepth, sizeof(ControlCommand));
    configASSERT(s_queue);

    BaseType_t ok = xTaskCreatePinnedToCore(
        control_task,
        "control",
        2048,
        nullptr,
        5,
        nullptr,
        tskNO_AFFINITY);
    configASSERT(ok == pdPASS);

    ESP_LOGI(TAG, "Control task started (queue depth=%d)", kQueueDepth);
}

void control_queue_send(ControlCommand cmd) {
    if (s_queue == nullptr) {
        ESP_LOGE(TAG, "Control queue not initialized");
        return;
    }

    if (xQueueSend(s_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Control queue full — command dropped");
    }
}
