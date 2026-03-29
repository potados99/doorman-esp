#include "door_control.h"

#include <atomic>

#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "door";
static const gpio_num_t DOOR_GPIO = GPIO_NUM_4;
static const int PULSE_MS = 500;

static std::atomic<bool> pulse_in_progress{false};

void door_control_init() {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << DOOR_GPIO;
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io));

    gpio_set_level(DOOR_GPIO, 0);
    ESP_LOGI(TAG, "Door control ready on GPIO %d", DOOR_GPIO);
}

bool door_trigger_pulse() {
    if (pulse_in_progress.exchange(true)) {
        return false;
    }
    gpio_set_level(DOOR_GPIO, 1);
    ESP_LOGI(TAG, "GPIO %d HIGH", DOOR_GPIO);
    vTaskDelay(pdMS_TO_TICKS(PULSE_MS));
    gpio_set_level(DOOR_GPIO, 0);
    ESP_LOGI(TAG, "GPIO %d LOW — pulse complete", DOOR_GPIO);
    pulse_in_progress.store(false);
    return true;
}
