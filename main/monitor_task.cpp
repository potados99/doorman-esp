#include "monitor_task.h"

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

        ESP_LOGI(TAG, "ram=%lu/%lu psram=%lu/%lu tasks=%d",
                 (unsigned long)s_stats.internal_free,
                 (unsigned long)s_stats.internal_total,
                 (unsigned long)s_stats.spiram_free,
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
