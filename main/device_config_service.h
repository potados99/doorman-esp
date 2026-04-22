#ifndef DOORMAN_ESP_DEVICE_CONFIG_SERVICE_H
#define DOORMAN_ESP_DEVICE_CONFIG_SERVICE_H

#include "device.h"

/**
 * 기기별 설정 서비스: NVS에 MAC 주소를 키로 DeviceConfig blob을 저장합니다.
 *
 * NVS namespace "dev", key: MAC 12자리 대문자 hex (예: "842F57A0C4EA").
 * 본드 슬롯 상한(CONFIG_BT_SMP_MAX_BONDS)만큼의 인메모리 캐시를 mutex로
 * 보호하며, set/delete는 **동기** NVS write입니다 (mutex 보유 중 NVS I/O 수행).
 *
 * 호출 빈도가 사람 액션 기반(모달 저장 등)이라 매우 낮고, NVS write
 * latency(~10ms)는 HTTP 응답 RTT 안에 자연스럽게 흡수됩니다. 이전엔 BT
 * 콜백 컨텍스트 격리를 위해 백그라운드 큐 + cfg_nvs_task 구조였지만, BT
 * manager가 더 이상 device_config NVS를 쓰지 않게 되어 큐를 제거했습니다.
 */

/**
 * NVS에서 전체 기기 설정을 로드하고 mutex를 생성합니다.
 * app_main()에서 nvs_flash_init() 이후, 다른 서비스 전에 호출합니다.
 */
void device_config_service_init();

/**
 * 지정한 MAC 주소의 DeviceConfig를 반환합니다.
 *
 * 캐시에 없으면 DeviceConfig 기본값을 반환합니다(NVS 조회 없음).
 * 반환값은 복사본이므로 호출자가 자유롭게 사용 가능합니다.
 */
DeviceConfig device_config_get(const uint8_t (&mac)[6]);

/**
 * 지정한 MAC 주소의 설정이 캐시에 존재하는지 확인합니다.
 * NVS 조회는 수행하지 않습니다.
 */
bool device_config_exists(const uint8_t (&mac)[6]);

/**
 * 지정한 MAC 주소의 DeviceConfig를 업데이트합니다.
 *
 * invalid 값은 거부합니다. mutex는 캐시 업데이트 동안에만 짧게 잡고
 * NVS 저장은 mutex 밖에서 동기적으로 수행됩니다 — sm_task의 feed 처리
 * 같은 reader가 NVS write 동안 블록되지 않도록 합니다. 반환 시점에는
 * 캐시도 NVS도 새 값이 반영된 상태입니다.
 */
void device_config_set(const uint8_t (&mac)[6], const DeviceConfig &cfg);

/**
 * 지정한 MAC 주소의 DeviceConfig를 캐시 및 NVS에서 삭제합니다.
 *
 * 존재하지 않는 키 삭제는 조용히 무시합니다. set과 동일하게 mutex는
 * 캐시 변경 동안에만 잡고 NVS 삭제는 mutex 밖에서 수행됩니다.
 */
void device_config_delete(const uint8_t (&mac)[6]);

/**
 * 현재 캐시에 있는 모든 기기 설정을 복사합니다.
 *
 * macs, configs 배열은 각각 max 개 이상의 공간이 있어야 합니다.
 * 반환값: 실제 복사된 항목 수입니다.
 */
int device_config_get_all(uint8_t (*macs)[6], DeviceConfig *configs, int max);


#endif //DOORMAN_ESP_DEVICE_CONFIG_SERVICE_H
