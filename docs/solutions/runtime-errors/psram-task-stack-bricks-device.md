---
title: 핵심 태스크 스택을 PSRAM에 두면 OTA 후 부팅 실패·디바이스 사망
date: 2026-04-08
category: runtime-errors
problem_type: memory_layout
component: freertos_task_creation
chip: esp32
idf_version: v6.0
severity: critical
keywords:
  - psram
  - spiram
  - task stack
  - xTaskCreatePinnedToCoreWithCaps
  - MALLOC_CAP_SPIRAM
  - cache disabled
  - dma
  - bricked
related_commits:
  - b1b80d4  # 원인 (도입)
  - 312d85f  # 해결 (revert)
status: resolved
---

# 핵심 태스크 스택을 PSRAM에 두면 OTA 후 부팅 실패·디바이스 사망

## 한 줄 요약

ESP32에서 NVS·드라이버 콜백·이벤트가 얽힌 핵심 태스크의 스택을 `MALLOC_CAP_SPIRAM`으로 옮기면, 캐시 비활성화 윈도우 또는 DMA 경로에서 즉시/지연 크래시가 발생하고, OTA로는 원인 추적조차 못 한 채 device-bricked까지 갈 수 있다.

## 증상

- 빌드/플래시는 성공.
- 부팅 직후 또는 일정 동작 후 무한 리부트 또는 완전 정지.
- coredump도 못 남기는 경우 있음 (스택이 PSRAM이라 dump 경로 자체가 죽음).
- OTA로 후속 fix 시도 시 증상 변동 → 결국 시리얼 강제 복구 필요.
- 시리얼에서 보면 `rst:0x1 (POWERON_RESET), boot:0x3 (DOWNLOAD_BOOT)` 상태(GPIO0 hold)로 떨어져 있는 경우 있음.

## 원인 (도입 커밋: b1b80d4)

내부 RAM 절약(`22K → 33K free`)을 위해 다음 태스크들의 스택을 PSRAM으로 이동:

```cpp
// main/control_task.cpp, sm_task.cpp, monitor_task.cpp, device_config_service.cpp
BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
    cfg_nvs_task,
    "cfg_nvs",
    2048,
    nullptr,
    3,
    nullptr,
    tskNO_AFFINITY,
    MALLOC_CAP_SPIRAM);  // ← 위험
```

## 왜 죽었는가 — PSRAM 스택의 진짜 지뢰

표면적으로 IDF는 `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)` 또는 `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`을 통해 PSRAM 스택을 "지원"한다. 그러나 다음 조건을 모두 만족할 때만 안전하다:

1. **DMA-free**: 그 태스크 코드가 스택에 잡은 버퍼를 DMA로 넘기지 않음.
2. **캐시-safe**: 인터럽트 컨텍스트나 IRAM-only 콜백이 그 태스크에 침범하지 않음.
3. **단일 코어 핀**: `tskNO_AFFINITY` 회피, `PRO_CPU_NUM`/`APP_CPU_NUM`로 못 박음.
4. **검증된 순수 워커**: 드라이버/이벤트 콜백이 진입하지 않는 비즈니스-로직-전용.

이번 케이스에서 깨진 가정:

### 1. DMA 호환성 (가장 흔한 원인)

PSRAM 주소는 GDMA 등 DMA 컨트롤러가 직접 read/write 못 한다(또는 ESP32 원본은 아예 불가, S3/C3는 일부 가능). 스택에 잡은 임시 버퍼를 SPI/I2S/Wi-Fi/HTTP 드라이버에 그대로 넘기는 코드 경로(예: `nvs_set_str`이 내부적으로 flash drv로 전달, ESP-NETIF, mbedtls 일부)에서 즉시 abort.

`cfg_nvs_task`는 이 위험의 정점이다 — flash 쓰기 경로 전체가 그 태스크 스택을 거쳐가는데, 스택이 PSRAM이면 path 어딘가에서 DMA 또는 IRAM-only 경로가 PSRAM 주소를 만나 사망.

### 2. 캐시 disable 윈도우 + IRAM ISR

`spi_flash_disable_interrupts_caches_and_other_cpu()` 또는 NVS commit, OTA write 중에는 양 코어 캐시가 잠깐 꺼진다. 그 동안 스케줄러는 멈추므로 "내가 NVS 쓰는 태스크가 자기 PSRAM 스택 못 읽는다" 같은 직관적 시나리오는 발생 안 한다.

진짜 위험은 이것이다:

- Wi-Fi/BT/타이머/GPIO ISR은 **level-1 IRAM 인터럽트**라 캐시 꺼진 상태에서도 발사된다.
- 인터럽트 진입 시 컨텍스트 저장은 **인터럽트 당한 태스크의 스택**으로 간다.
- 그 스택이 PSRAM이면 → 캐시 꺼져 있어 push 못함 → CPU exception → reset.

이 시나리오는 **인터럽트 빈도와 NVS 쓰기 빈도가 겹쳤을 때 비결정적으로 터진다.** OTA로 후속 fix를 시도해도 증상 재현이 들쭉날쭉했던 이유.

### 3. `tskNO_AFFINITY` 조합

PSRAM 스택 + `tskNO_AFFINITY`는 IDF 버전에 따라 비공식 조합. 코어 마이그레이션 시점의 컨텍스트 save/restore가 race를 일으킬 수 있다. b1b80d4는 이 조합을 그대로 썼다.

### 4. 부팅 경로 자체 침범

`monitor_task`/`sm_task`는 init 직후 즉시 돌면서 ESP-IDF 이벤트 루프, NVS 로드, BT init 등과 동시에 동작한다. 그 와중에 위 1~3 어떤 것이라도 한 번이라도 맞으면 첫 부팅에서 사망 → "device bricked처럼 보임".

## 진단이 어려웠던 이유 (compound 가치 ↑)

- **coredump 미생성**: espcoredump도 task stack을 traverse하는데 PSRAM이 죽은 상황에선 그 경로 자체가 fault.
- **OTA로는 디버깅 불가**: OTA write 자체가 캐시 disable + flash write 콤보라 새로운 PSRAM 스택 빌드도 똑같은 path에서 죽는다. 즉 "fix 시도용 OTA"가 또 brick.
- **로그 부재**: 부팅 직후 사망이라 시리얼 로그도 거의 안 남음.
- **간헐성**: ISR + cache disable race이라 동일 빌드가 어떤 부팅에선 살고 어떤 부팅에선 죽는다.

## 해결 (커밋: 312d85f)

```bash
git revert b1b80d4
```

`sm/control/monitor/cfg_nvs` 4개 태스크 모두 `xTaskCreatePinnedToCore`(내부 RAM 스택)로 원복.

내부 RAM 회수 효과(11KB)는 잃지만, 그건 **다른 방법**으로 회수해야 한다 (아래 "대안" 참조).

복구는 시리얼 + esptool로 강제 플래시:

```bash
# 기기를 download mode(GPIO0 hold + reset)로 진입시킨 뒤
python -m esptool --chip esp32 -p /dev/cu.usbserial-XXXX -b 115200 \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-size 8MB --flash-freq 40m \
  0x1000 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0xf000 build/ota_data_initial.bin \
  0x20000 build/doorman.bin
```

## 안전 룰 (앞으로 PSRAM 스택을 고려할 때)

| 항목 | PSRAM 스택 OK | PSRAM 스택 금지 |
|---|---|---|
| 태스크 종류 | 순수 계산 워커, 큰 임시 데이터 보유 | 드라이버 콜백, 이벤트 핸들러, NVS/flash 접근 |
| DMA 사용 | 없음 | SPI/I2S/Wi-Fi/HTTP/mbedtls 등 어디든 |
| ISR 진입 | 거의 없음 | 빈번한 인터럽트 환경 |
| 코어 핀 | 단일 코어 명시 | `tskNO_AFFINITY` |
| 검증 | 장시간 stress 테스트 통과 | 즉시 production 투입 |

**doorman-esp 룰**: `sm_task`, `control_task`, `monitor_task`, `cfg_nvs_task`, `bt_*`, `httpd_*` 는 **영구적으로 내부 RAM 스택만 사용한다.**

PSRAM 스택을 정말 써야 한다면:
1. 새 워커 태스크를 만들고 그 안에서만 격리된 계산을 한다.
2. NVS/flash/DMA 호출 코드가 그 태스크 컨텍스트에 절대 진입 못 하게 한다.
3. 단일 코어에 핀한다.
4. minimum hours 단위 stress test 후 OTA 채널에 올린다 (ota는 brick 위험 + 디버깅 불가).

## 대안 — 내부 RAM 회수의 안전한 길

PSRAM 스택을 못 쓰면 11KB는 어떻게 회수할까:

1. **태스크 스택 사이즈 다이어트**: `uxTaskGetStackHighWaterMark()`로 실제 사용량 측정 후 여유분만 남기고 줄임.
2. **BT 메모리 PSRAM 이동** (이미 적용됨, 커밋 1504a10): BT 컨트롤러 풀을 PSRAM으로 — 이건 안전한 합법 패턴.
3. **`.bss` 정적 버퍼 점검**: 큰 전역 배열을 `EXT_RAM_BSS_ATTR` 또는 lazy alloc.
4. **mbedtls/lwIP 메모리 PSRAM 풀로 라우팅**: `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` 등.
5. **사용하지 않는 컴포넌트 비활성화**: `sdkconfig`에서 사용 안 하는 드라이버/스택 끄기.

## 예방 (재발 방지)

1. **PR 리뷰 체크리스트**: `xTaskCreatePinnedToCoreWithCaps` 또는 `MALLOC_CAP_SPIRAM` + stack 키워드가 diff에 있으면 자동 경고.
2. **OTA 방어선**: 시스템 핵심 태스크 변경은 OTA로 보내기 전에 시리얼 빌드 → 최소 N시간 부팅/재부팅/NVS write stress 통과 후에만 OTA.
3. **이중 partition + rollback**: OTA 파티션 두 개 + ESP-IDF의 `esp_ota_mark_app_invalid_rollback_and_reboot()` 활용해서 첫 부팅 self-test 실패 시 자동 롤백 (이 프로젝트엔 아직 없음 → TODO).
4. **stack high water mark 모니터링**: `monitor_task`가 주기적으로 모든 태스크의 high water mark를 ws 로그로 흘려서, 스택 다이어트 근거를 누적.

## 관련 자료

- ESP-IDF docs: [Task creation with custom memory caps](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/freertos_idf.html#_CPPv432xTaskCreatePinnedToCoreWithCaps)
- ESP-IDF docs: [PSRAM with FreeRTOS](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/external-ram.html#external-ram-allocator)
- 관련 이 프로젝트의 안전한 PSRAM 활용 사례:
  - `1504a10` BT 메모리를 PSRAM으로 (안전 ✅)
  - `e6d35e1` PSRAM 설정 안정화 + RAM/PSRAM 분리 표시
- 본 사고의 trigger 커밋: `b1b80d4`
- 본 사고의 해결 커밋: `312d85f`

## 핵심 교훈 (한 줄)

> "PSRAM stack이 지원된다"는 것과 "내 태스크에 안전하다"는 완전히 다른 명제다. **flash/NVS/DMA/ISR이 닿는 모든 태스크의 스택은 영구히 내부 RAM에 둔다.**
