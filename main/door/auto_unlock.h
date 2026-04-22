#ifndef DOORMAN_ESP_AUTO_UNLOCK_H
#define DOORMAN_ESP_AUTO_UNLOCK_H

/**
 * BT presence 기반 자동 문열림 토글.
 *
 * NVS namespace "door", key "auto"에 bool로 저장됩니다 (구 config_service와 동일 키).
 * sm_task가 Unlock 판정 시 이 값을 확인해 실제 열림 여부를 결정합니다.
 * 웹 UI의 수동 문열기(ManualUnlock)는 이 값과 무관하게 항상 허용됩니다.
 *
 * 이전의 AppConfig 구조체(필드 1개)는 이 모듈로 대체되었습니다.
 */

/**
 * NVS에서 저장된 값을 로드하고 mutex를 생성합니다.
 * app_main()에서 nvs_flash_init() 이후 호출합니다.
 */
void auto_unlock_init();

/**
 * 현재 값을 반환합니다. init 전 호출되거나 NVS 로드 실패 시 false를 반환합니다.
 */
bool auto_unlock_is_enabled();

/**
 * 값을 갱신하고 NVS에 즉시 저장합니다. init 전 호출은 조용히 무시됩니다.
 */
void auto_unlock_set(bool enabled);

#endif //DOORMAN_ESP_AUTO_UNLOCK_H
