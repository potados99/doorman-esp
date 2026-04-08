#pragma once

#include "config.h"

/**
 * 기기별 설정 서비스: NVS에 MAC 주소를 키로 DeviceConfig blob을 저장한다.
 *
 * NVS namespace "dev", key: MAC 12자리 대문자 hex (예: "842F57A0C4EA").
 * 최대 15개 기기 설정을 인메모리 캐시로 관리하며, FreeRTOS mutex로 보호한다.
 * NVS I/O는 백그라운드 태스크에서 비동기로 처리되어 호출자를 블로킹하지 않는다.
 */

/**
 * NVS에서 전체 기기 설정을 로드하고 mutex를 생성한다.
 * app_main()에서 nvs_flash_init() 이후, 다른 서비스 전에 호출.
 */
void device_config_service_init();

/**
 * 지정한 MAC 주소의 DeviceConfig를 반환한다.
 *
 * 캐시에 없으면 DeviceConfig 기본값을 반환한다(NVS 조회 없음).
 * 반환값은 복사본이므로 호출자가 자유롭게 사용 가능.
 */
DeviceConfig device_config_get(const uint8_t (&mac)[6]);

/**
 * 지정한 MAC 주소의 설정이 캐시에 존재하는지 확인한다.
 * NVS 조회는 수행하지 않는다.
 */
bool device_config_exists(const uint8_t (&mac)[6]);

/**
 * 지정한 MAC 주소의 DeviceConfig를 업데이트한다.
 *
 * invalid 값은 거부한다. 캐시는 mutex 안에서 즉시 업데이트되며,
 * NVS 저장은 비동기로 백그라운드에서 수행된다.
 */
void device_config_set(const uint8_t (&mac)[6], const DeviceConfig &cfg);

/**
 * 지정한 MAC 주소의 DeviceConfig를 캐시 및 NVS에서 삭제한다.
 *
 * 존재하지 않는 키 삭제는 조용히 무시한다.
 * 캐시는 즉시 삭제되며, NVS 삭제는 비동기로 백그라운드에서 수행된다.
 */
void device_config_delete(const uint8_t (&mac)[6]);

/**
 * 현재 캐시에 있는 모든 기기 설정을 복사한다.
 *
 * macs, configs 배열은 각각 max 개 이상의 공간이 있어야 한다.
 * 반환값: 실제 복사된 항목 수.
 */
int device_config_get_all(uint8_t (*macs)[6], DeviceConfig *configs, int max);

