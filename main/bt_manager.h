#pragma once

#include <esp_err.h>

/**
 * BT Manager: 듀얼모드 BT(BLE + Classic) presence 감지와 페어링을 관리한다.
 *
 * bt_presence_poc.cpp에서 검증된 BT 스택 초기화, 콜백 구조, 스캔 로직을
 * 프로덕션 품질로 재작성한 것이다.
 *
 * 핵심 변경점 (PoC 대비):
 * - 감지 이벤트를 SM Task 피드 큐로 전송 (sm_feed_queue_send)
 * - 페어링을 외부에서 트리거 가능 (bt_request_pairing → 큐 기반)
 * - presence_task가 타이머 기반 페어링 윈도우 관리
 *
 * BT 콜백은 Bluedroid 내부 태스크에서 호출되므로,
 * 상태 변경은 portMUX spinlock으로 보호한다.
 */

/**
 * BT 스택을 초기화하고 듀얼모드 presence 태스크를 시작한다.
 *
 * 초기화 순서 (PoC에서 검증된 것과 동일):
 * 1. BT controller init + enable (BTDM)
 * 2. Bluedroid init + enable
 * 3. GAP/SPP/GATTS 콜백 등록
 * 4. SPP 서버 + GATT 서비스 시작
 * 5. 보안 파라미터 설정
 * 6. bond 캐시 로드
 * 7. presence 태스크 생성 (Core 0 고정)
 *
 * 부팅 시 30초 페어링 윈도우가 자동으로 열린다.
 *
 * app_main()에서 WiFi, webserver, SM task 이후 마지막에 호출.
 */
esp_err_t bt_manager_start();

/**
 * 페어링 윈도우를 열도록 요청한다 (30초).
 *
 * HTTP 핸들러에서 호출. 큐를 통해 BT 태스크에 전달되므로
 * BT API 호출은 항상 BT 태스크 컨텍스트에서 실행된다.
 * 이미 페어링 중이면 무시.
 */
void bt_request_pairing();

/**
 * 본딩된 기기를 삭제한다 (BLE + Classic 양쪽).
 *
 * HTTP 핸들러에서 호출. 큐를 통해 BT 태스크에 전달되므로
 * BT API 호출은 항상 BT 태스크 컨텍스트에서 실행된다.
 * 없는 쪽은 에러를 무시한다.
 */
void bt_remove_bond(const uint8_t (&mac)[6]);
