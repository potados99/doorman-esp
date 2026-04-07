#include "sm_task.h"
#include "bt_manager.h"
#include "config_service.h"
#include "control_task.h"
#include "device_config_service.h"

#include <cstring>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static const char *TAG = "sm";

/**
 * 큐 메시지 타입.
 */
enum class SmMsgType { Feed, RemoveDevice, CreateConfig };

/**
 * 큐로 전달되는 메시지.
 * Feed: BT Manager → SM Task 감지 이벤트.
 * RemoveDevice: 슬롯 제거 요청.
 * CreateConfig: 기기별 config 생성 요청 (NVS 저장 → SM 반영).
 */
struct SmMsg {
    SmMsgType type;
    union {
        struct { uint8_t mac[6]; bool seen; uint32_t now_ms; int8_t rssi; } feed;
        struct { uint8_t mac[6]; } remove;
        struct { uint8_t mac[6]; char alias[32]; } create_config;
    };
};
static_assert(sizeof(SmMsg) <= 48, "SmMsg too large for queue");

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

/** 스냅샷 보호 mutex 및 버퍼. */
static SemaphoreHandle_t s_snapshot_mutex = nullptr;
static DeviceState       s_snapshots[kMaxTrackedDevices] = {};
static int               s_snapshot_count = 0;

/**
 * 시작 후 유예기간 (밀리초).
 * 재부팅 직후 이미 근처에 있는 기기들이 "최초 감지 → Unlock"으로
 * 문을 여는 걸 방지한다. 유예기간 동안 SM은 정상 동작(감지 추적)하되
 * Unlock만 Control Task에 안 보낸다. SM 내부적으로 last_unlock_ms가
 * 세팅되므로, 유예 후에는 이미 "처리 완료" 상태.
 */
static constexpr uint32_t kStartupGraceMs = 30000;

static void sm_task(void *) {
    StateMachine sm;

    uint32_t start_ms = (uint32_t)(esp_timer_get_time() / 1000);

    ESP_LOGI(TAG, "StateMachine initialized (grace=%lums)", (unsigned long)kStartupGraceMs);

    // ── 시작 시퀀스: NVS 기기별 config 전부 로드 → SM에 push ──
    {
        uint8_t macs[kMaxTrackedDevices][6];
        DeviceConfig cfgs[kMaxTrackedDevices];
        int n = device_config_get_all(macs, cfgs, kMaxTrackedDevices);
        for (int i = 0; i < n; i++) {
            sm.update_device_config(reinterpret_cast<const uint8_t(&)[6]>(macs[i]), cfgs[i]);
        }
        ESP_LOGI(TAG, "Loaded %d device config(s) into SM", n);
    }

    SmMsg msg;
    while (true) {
        // 큐 drain: tick당 최대 16개 dequeue
        int drained = 0;
        while (xQueueReceive(s_queue, &msg, drained == 0 ? pdMS_TO_TICKS(kTickIntervalMs) : 0) == pdTRUE) {
            switch (msg.type) {
            case SmMsgType::Feed:
                sm.update_device_config(msg.feed.mac, device_config_get(msg.feed.mac));
                sm.feed(msg.feed.mac, msg.feed.seen, msg.feed.now_ms, msg.feed.rssi);
                break;
            case SmMsgType::RemoveDevice:
                sm.remove_device(msg.remove.mac);
                break;
            case SmMsgType::CreateConfig: {
                uint8_t mac[6];
                std::memcpy(mac, msg.create_config.mac, 6);
                DeviceConfig dev_cfg = device_config_get(mac);

                if (!device_config_exists(mac)) {
                    // 최초 페어링이면 기본 설정 + alias를 저장한다.
                    snprintf(dev_cfg.alias, sizeof(dev_cfg.alias), "%s", msg.create_config.alias);
                    device_config_set(mac, dev_cfg);
                    break;
                }

                // 이미 존재하는 config는 threshold를 보존하고 alias가 비어있을 때만 채운다.
                if (dev_cfg.alias[0] == '\0' && msg.create_config.alias[0] != '\0') {
                    snprintf(dev_cfg.alias, sizeof(dev_cfg.alias), "%s", msg.create_config.alias);
                    device_config_set(mac, dev_cfg);
                }
                break;
            }
            }
            if (++drained >= 16) break;
        }

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        Action action = sm.tick(now_ms);

        // 스냅샷 갱신: 전역 배열에 직접 dump
        xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
        s_snapshot_count = sm.dump_states(s_snapshots, kMaxTrackedDevices);
        xSemaphoreGive(s_snapshot_mutex);

        /**
         * Unlock 억제 — SM은 항상 드라이런 판정. 여기서만 실제 전달 결정.
         * 1. 유예기간: 재부팅 직후 기존 기기 flood 방지
         * 2. auto_unlock OFF: 사용자가 명시적으로 꺼놓은 상태
         * 3. 페어링 중: 새 기기 bond 중에 문 열리면 안 됨
         */
        if (action == Action::Unlock) {
            AppConfig current_cfg = app_config_get();
            bool in_grace = (now_ms - start_ms) < kStartupGraceMs;

            if (in_grace) {
                ESP_LOGI(TAG, "Unlock suppressed (grace %lums remaining)",
                         (unsigned long)(kStartupGraceMs - (now_ms - start_ms)));
            } else if (!current_cfg.auto_unlock_enabled) {
                ESP_LOGI(TAG, "Unlock suppressed (auto_unlock OFF)");
            } else if (bt_is_pairing()) {
                ESP_LOGI(TAG, "Unlock suppressed (pairing mode)");
            } else {
                control_queue_send(ControlCommand::AutoUnlock);
            }
        }
    }
}

void sm_task_start() {
    s_queue = xQueueCreate(kQueueDepth, sizeof(SmMsg));
    configASSERT(s_queue);

    s_snapshot_mutex = xSemaphoreCreateMutex();
    configASSERT(s_snapshot_mutex);

    BaseType_t ok = xTaskCreatePinnedToCore(
        sm_task,
        "sm_task",
        4096,
        nullptr,
        5,
        nullptr,
        tskNO_AFFINITY);
    configASSERT(ok == pdPASS);

    ESP_LOGI(TAG, "SM task started (tick interval=%dms)", kTickIntervalMs);
}

void sm_feed_queue_send(const uint8_t (&mac)[6], bool seen, uint32_t now_ms, int8_t rssi) {
    if (s_queue == nullptr) {
        ESP_LOGE(TAG, "SM queue not initialized");
        return;
    }

    SmMsg msg = {};
    msg.type = SmMsgType::Feed;
    std::memcpy(msg.feed.mac, mac, 6);
    msg.feed.seen = seen;
    msg.feed.now_ms = now_ms;
    msg.feed.rssi = rssi;

    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "SM queue full — feed event dropped");
    }
}

void sm_remove_device_queue_send(const uint8_t (&mac)[6]) {
    if (s_queue == nullptr) {
        ESP_LOGE(TAG, "SM queue not initialized");
        return;
    }

    SmMsg msg = {};
    msg.type = SmMsgType::RemoveDevice;
    std::memcpy(msg.remove.mac, mac, 6);

    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "SM queue full — remove_device event dropped");
    }
}

void sm_create_config_queue_send(const uint8_t (&mac)[6], const char *alias) {
    if (s_queue == nullptr) {
        ESP_LOGE(TAG, "SM queue not initialized");
        return;
    }

    SmMsg msg = {};
    msg.type = SmMsgType::CreateConfig;
    std::memcpy(msg.create_config.mac, mac, 6);
    snprintf(msg.create_config.alias, sizeof(msg.create_config.alias), "%s", alias);

    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "SM queue full — create_config event dropped");
    }
}

int sm_get_snapshots(DeviceState *out, int max) {
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    int n = s_snapshot_count < max ? s_snapshot_count : max;
    memcpy(out, s_snapshots, n * sizeof(DeviceState));
    xSemaphoreGive(s_snapshot_mutex);
    return n;
}
