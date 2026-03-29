#pragma once

#include "config.h"

#include <cstdint>

/**
 * SM Task: StateMachine을 소유하고 FreeRTOS 큐로 feed 이벤트를 받는다.
 *
 * BT Manager가 감지 이벤트를 sm_feed_queue_send()로 보내면,
 * 이 태스크가 큐에서 꺼내서 StateMachine.feed()를 호출한다.
 * 주기적으로 StateMachine.tick()을 호출하여 시간 기반 상태 전이를 평가하고,
 * Unlock 액션이 나오면 Control Task에 명령을 보낸다.
 *
 * StateMachine은 이 태스크만 접근 — 별도 뮤텍스 불필요.
 */

/**
 * SM 태스크를 생성하고 StateMachine을 초기화한다.
 *
 * cfg: NVS에서 로드한 앱 설정. cooldown/timeout 값이 StateMachine에 전달된다.
 * app_main()에서 config_service_init() 이후 호출.
 */
void sm_task_start(AppConfig cfg);

/**
 * BT 감지 이벤트를 SM 태스크의 피드 큐에 전송한다.
 *
 * BT Manager의 콜백(ISR이 아닌 BT 태스크 컨텍스트)에서 호출.
 * mac: 6바이트 BT MAC 주소 (BLE identity 또는 Classic BD_ADDR)
 * seen: true면 감지, false면 미감지
 * now_ms: 현재 시간(밀리초). esp_timer_get_time() / 1000으로 산출.
 */
void sm_feed_queue_send(const uint8_t (&mac)[6], bool seen, uint32_t now_ms);
