#ifndef DOORMAN_ESP_MONITOR_TASK_H
#define DOORMAN_ESP_MONITOR_TASK_H

#include <cstdint>

/**
 * 시스템 모니터 태스크: 주기적으로 힙/태스크 상태를 로그로 출력합니다.
 *
 * 1초마다 ESP_LOGI로 free heap과 태스크 수를 찍습니다.
 * 웹 UI에서는 /api/stats로 최신 값을 조회할 수 있습니다.
 */
void monitor_task_start();

/**
 * 최신 시스템 통계를 반환합니다.
 * monitor task가 주기적으로 갱신하는 값을 읽습니다.
 */
struct SystemStats {
    uint32_t internal_free;
    uint32_t internal_total;
    uint32_t spiram_free;
    uint32_t spiram_total;
    int task_count;
};

SystemStats monitor_get_stats();

#endif //DOORMAN_ESP_MONITOR_TASK_H
