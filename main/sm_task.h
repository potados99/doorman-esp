#pragma once

#include "config.h"
#include "statemachine.h"

#include <cstdint>

/**
 * SM Task: StateMachine을 소유하고 FreeRTOS 큐로 메시지를 받는다.
 *
 * BT Manager가 감지 이벤트를 sm_feed_queue_send()로 보내면,
 * 이 태스크가 큐에서 꺼내서 StateMachine.feed()를 호출한다.
 * 주기적으로 StateMachine.tick()을 호출하여 시간 기반 상태 전이를 평가하고,
 * Unlock 액션이 나오면 Control Task에 명령을 보낸다.
 *
 * StateMachine은 이 태스크만 접근 — 별도 뮤텍스 불필요.
 * 스냅샷(s_snapshots)은 s_snapshot_mutex로 보호하여 외부에서 안전하게 읽을 수 있다.
 */

void sm_task_start(AppConfig cfg);

/**
 * BT 감지 이벤트를 SM 태스크의 피드 큐에 전송한다.
 *
 * mac: 6바이트 BT MAC 주소 (BLE identity 또는 Classic BD_ADDR)
 * seen: true면 감지 (현재 false는 사용 안 함)
 * now_ms: 현재 시간(밀리초)
 * rssi: BLE RSSI (dBm). 0이면 RSSI 필터링 건너뜀 (Classic용).
 */
void sm_feed_queue_send(const uint8_t (&mac)[6], bool seen, uint32_t now_ms, int8_t rssi = 0);

/**
 * 기기 슬롯 제거 메시지를 SM 태스크 큐에 전송한다.
 *
 * SM 태스크가 StateMachine.remove_device()를 호출하여 슬롯을 해제한다.
 */
void sm_remove_device_queue_send(const uint8_t (&mac)[6]);

/**
 * 기기별 config 생성 메시지를 SM 태스크 큐에 전송한다.
 *
 * SM 태스크가 device_config_set()을 호출하여 NVS에 저장하고,
 * 다음 루프에서 device_config_changed() 감지 → SM에 반영된다.
 */
void sm_create_config_queue_send(const uint8_t (&mac)[6], const char *alias);

/**
 * 현재 SM이 추적 중인 기기 상태 스냅샷을 복사한다.
 *
 * out에 DeviceState 배열을 복사하며, 반환값은 실제 복사된 항목 수.
 * mutex로 보호되므로 SM Task 외부에서 안전하게 호출 가능.
 */
int sm_get_snapshots(DeviceState *out, int max);
