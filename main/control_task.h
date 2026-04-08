#pragma once

/**
 * Control Task: GPIO 펄스 명령을 FreeRTOS 큐로 직렬화합니다.
 *
 * 여러 소스(SM Task의 자동 잠금해제, HTTP의 수동 잠금해제)에서
 * 동시에 문열기를 요청할 수 있으므로, 큐를 통해 순차 실행을 보장합니다.
 * 명령의 종류(AutoUnlock vs ManualUnlock)는 로깅 구분용이며,
 * 실제 GPIO 동작은 동일합니다.
 */

enum class ControlCommand {
    /** StateMachine이 BT presence 기반으로 판단한 자동 잠금해제입니다. */
    AutoUnlock,
    /** 웹 UI에서 사용자가 직접 트리거한 수동 잠금해제입니다. */
    ManualUnlock,
};

/**
 * Control 태스크를 생성하고 명령 큐를 초기화합니다.
 * app_main()에서 door_control_init() 이후 한 번 호출합니다.
 */
void control_task_start();

/**
 * 명령 큐에 문열기 요청을 보냅니다.
 *
 * ISR이 아닌 일반 태스크 컨텍스트에서 호출합니다.
 * 큐가 가득 차면 (이미 여러 요청이 대기 중) 로그 경고 후 무시합니다.
 * SM Task, HTTP 핸들러 등 여러 곳에서 호출 가능합니다.
 */
void control_queue_send(ControlCommand cmd);
