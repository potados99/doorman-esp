#include "monitor_task.h"

#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "monitor";

static SystemStats s_stats = {};

static void monitor_task(void *) {
    while (true) {
        s_stats.free_heap = esp_get_free_heap_size();
        s_stats.min_free_heap = esp_get_minimum_free_heap_size();
        s_stats.task_count = uxTaskGetNumberOfTasks();

        ESP_LOGI(TAG, "heap=%lu min=%lu tasks=%d",
                 (unsigned long)s_stats.free_heap,
                 (unsigned long)s_stats.min_free_heap,
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
        1,   // 낮은 우선순위
        nullptr,
        tskNO_AFFINITY);
    configASSERT(ok == pdPASS);

    ESP_LOGI(TAG, "Monitor task started");
}

SystemStats monitor_get_stats() {
    return s_stats;
}
