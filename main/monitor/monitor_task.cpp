#include "monitor/monitor_task.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "monitor";

static SystemStats s_stats = {};

static void monitor_task(void *) {
    while (true) {
        s_stats.internal_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        s_stats.internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        s_stats.spiram_free    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        s_stats.spiram_total   = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        s_stats.task_count     = uxTaskGetNumberOfTasks();

        // 로그는 used/total 형식 — 프론트엔드가 보기 직관적이고 used 자체가 의미 단위.
        const unsigned long ram_used   = static_cast<unsigned long>(
            s_stats.internal_total - s_stats.internal_free);
        const unsigned long psram_used = static_cast<unsigned long>(
            s_stats.spiram_total - s_stats.spiram_free);

        ESP_LOGI(TAG, "ram=%lu/%lu psram=%lu/%lu tasks=%d",
                 ram_used,
                 (unsigned long)s_stats.internal_total,
                 psram_used,
                 (unsigned long)s_stats.spiram_total,
                 s_stats.task_count);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void monitor_task_start() {
    BaseType_t ok = xTaskCreatePinnedToCore(
        monitor_task,
        "monitor",
        2048,
        nullptr,
        1,
        nullptr,
        tskNO_AFFINITY);
    configASSERT(ok == pdPASS);

    ESP_LOGI(TAG, "Monitor task started");
}

SystemStats monitor_get_stats() {
    return s_stats;
}
