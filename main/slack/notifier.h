#ifndef DOORMAN_ESP_SLACK_NOTIFIER_H
#define DOORMAN_ESP_SLACK_NOTIFIER_H

#include <esp_err.h>

/**
 * Slack Incoming Webhook 기반 비동기 알림 모듈.
 *
 * 목적: 사용자가 HTTP로 문열림 API를 호출한 경우에만 채널에 한 줄 알림.
 * BLE presence 기반 자동 열림(AutoUnlock)은 알림 대상이 아님 (시스템 주체
 * 이벤트라 보안 로그 가치 없음).
 *
 * 크레덴셜 저장: NVS namespace "slack", key "url". 길이 가변, 저장 실패
 * 시 조용히 알림 비활성화 (시스템엔 영향 없음).
 *
 * 메모리 모델:
 *   - 상주 태스크 1개, 스택 8192B 내부 RAM
 *     (PSRAM 금지 룰 — docs/solutions/runtime-errors/psram-task-stack-bricks-device.md)
 *   - 단일 코어 핀 (APP_CPU_NUM) — tskNO_AFFINITY 금지
 *   - 큐 depth 3, 아이템 = 포인터(8B), body는 PSRAM heap_caps_malloc
 *   - webhook URL은 PSRAM heap에 strdup (url mutex로 보호)
 *
 * 모든 public API는 s_queue==nullptr 가드 포함 (init 전 호출 안전).
 */

/**
 * 부팅 시 1회 호출. NVS에서 URL 로드 + mutex/큐/태스크 생성.
 * wifi_start() 이후에 호출해야 URL 로드 시점과 맞습니다.
 * safe mode에선 호출 건너뜀 (notifier_send가 nullptr 가드로 자동 무시).
 */
void slack_notifier_init();

/**
 * 비동기 전송 요청. 큐가 가득 차면 drop 후 WARN 로그.
 * msg는 호출자 소유 (내부에서 PSRAM으로 복사).
 * msg==nullptr 또는 init 전 호출은 조용히 무시.
 */
void slack_notifier_send(const char *msg);

/**
 * webhook URL을 갱신하고 NVS에 저장. 재부팅 불필요.
 * url이 nullptr 또는 빈 문자열이면 알림 비활성화 (URL 해제).
 * 잘못된 URL(prefix 불일치, 길이 범위 벗어남)은 ESP_ERR_INVALID_ARG 반환.
 */
esp_err_t slack_notifier_update_url(const char *url);

#endif //DOORMAN_ESP_SLACK_NOTIFIER_H
