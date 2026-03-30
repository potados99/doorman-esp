#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * BT Manager: 듀얼모드 BT(BLE + Classic) presence 감지와 페어링을 관리한다.
 */

/** BT 스택 초기화 + presence 태스크 시작. 부팅 시 페어링 자동 시작 안 함. */
esp_err_t bt_manager_start();

/** 페어링 윈도우를 연다. 수동으로 닫을 때까지 열려있음. */
void bt_request_pairing();

/** 페어링 윈도우를 닫는다. */
void bt_stop_pairing();

/** 현재 페어링 모드인지 반환. 웹 UI 상태 표시용. */
bool bt_is_pairing();

/** 본딩된 기기 삭제 (BLE + Classic 양쪽). */
void bt_remove_bond(const uint8_t (&mac)[6]);

/**
 * 본딩된 기기의 identity address 목록을 가져온다.
 * BLE: bond_key의 identity address (IRK resolve에 사용되는 실제 주소)
 * Classic: bd_addr 그대로
 * 중복 제거 포함.
 *
 * out_macs: [max_count][6] 크기의 버퍼
 * 반환: 실제 기기 수
 */
int bt_get_bonded_devices(uint8_t (*out_macs)[6], int max_count);
