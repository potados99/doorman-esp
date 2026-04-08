#pragma once

#include "config.h"

/**
 * AppConfig 서비스: NVS에서 로드한 앱 설정을 스레드 안전하게 관리합니다.
 *
 * 여러 태스크에서 설정을 읽을 수 있고(SM Task 시작 시, HTTP 핸들러 등),
 * 웹 UI에서 설정을 변경할 수도 있으므로 FreeRTOS mutex로 보호합니다.
 *
 * NVS namespace "door"에 auto_unlock 등을 저장합니다.
 * 초기 값이 NVS에 없으면 AppConfig의 컴파일 타임 기본값을 사용합니다.
 */

/**
 * NVS에서 AppConfig를 로드하고 mutex를 생성합니다.
 * app_main()에서 nvs_flash_init() 이후, 다른 서비스 전에 호출합니다.
 */
void config_service_init();

/**
 * 현재 AppConfig의 스냅샷을 반환합니다.
 *
 * mutex lock → memcpy → unlock이므로 호출 비용이 거의 없습니다.
 * 반환된 AppConfig는 복사본이므로 호출자가 자유롭게 사용 가능합니다.
 */
AppConfig app_config_get();

/**
 * AppConfig를 업데이트하고 NVS에 즉시 저장합니다.
 *
 * NVS 저장이 포함되므로 빈번하게 호출하면 flash wear에 주의합니다.
 */
void app_config_set(const AppConfig &cfg);
