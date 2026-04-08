#pragma once

/**
 * 시스템 모니터 태스크: 주기적으로 힙/태스크 상태를 로그로 출력한다.
 *
 * 1초마다 ESP_LOGI로 free heap과 태스크 수를 찍는다.
 * 웹 UI에서는 /api/stats로 최신 값을 조회할 수 있다.
 */
void monitor_task_start();

/**
 * 최신 시스템 통계를 반환한다.
 * monitor task가 주기적으로 갱신하는 값을 읽는다.
 */
struct SystemStats {
    uint32_t free_heap;
    uint32_t min_free_heap;
    int task_count;
};

SystemStats monitor_get_stats();
