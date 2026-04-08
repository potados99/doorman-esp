---
title: sm_task 스택 오버플로우 → safe mode 무한루프 사고 (복합 버그)
date: 2026-04-08
category: runtime-errors
problem_type: stack_overflow + null_guard_missing
component: sm_task + http_server + safe_mode
chip: esp32
idf_version: v6.0
severity: critical
keywords:
  - stack overflow
  - canary watchpoint
  - safe mode
  - nullptr mutex
  - xSemaphoreTake assert
  - uxTaskGetStackHighWaterMark
  - sm_task
  - sm_get_snapshots
related_commits:
  - 69819bd  # 해결
status: resolved
---

# sm_task 스택 오버플로우 → safe mode 무한루프 사고 (복합 버그)

## 한 줄 요약

부팅 직후 BT peer 8개 동시 감지 burst가 sm_task 스택(4096)을 넘어 canary trip → panic 3회 → safe mode 진입. 그런데 safe mode에서는 `sm_task_start()`가 skip되어 `s_snapshot_mutex = nullptr`인데, http의 `sm_get_snapshots()`는 nullptr 체크 없이 `xSemaphoreTake` → assert → 또 panic. **최초 진입 원인과 무한루프 탈출 불가 원인이 서로 다른 두 버그의 연쇄**.

## 증상

- 기기가 "뻗은 것처럼" 보임 (BT 감지/자동열림 동작 안 함)
- `/api/info` 응답: `{"safe_mode": true}`
- `/api/coredump` 응답: httpd 태스크에서 `xQueueSemaphoreTake` assert
- 부팅 로그: `Consecutive panic count: 122` (한참 쌓여 있음)
- OTA는 여전히 가능 (safe mode에서도 웹서버는 살아있음)

## 실제 원인 — 두 단계 분리

### Phase 1: 최초 safe mode 진입 (진짜 root cause)

**sm_task 스택 오버플로우.** 부팅 직후 시퀀스:

```
01:53:55.290 cfg_svc: Config loaded
01:53:56.811 sm: Loaded 1 device config(s) into SM
01:53:56.816 sm: SM task started (tick interval=2000ms)
01:53:57.345 bt: BLE continuous scan started
01:53:57.361 bt: BLE bonded peers: 8        ← 8개 피어 동시 등장
01:53:57.427 bt: 5C:AD:BA:BF:26:0D rssi=-47
01:53:57.428 bt: 5C:AD:BA:BF:26:0D rssi=-47
01:53:57.428 sm: 5C:AD:BA:BF:26:0D detecting 1/3 10000
01:53:57.436 sm: 5C:AD:BA:BF:26:0D detecting 2/3 10000
01:53:57.446 sm: 5C:AD:BA:BF:26:0D detecting 3/3 10000
01:53:57.450 sm: 5C:AD:BA:BF:26:0D present
01:53:57.459 sm: 5C:AD:BA:BF:26:0D unlock
01:53:57.467 sm: Unlock suppressed (grace 29356ms remaining)
01:53:57.477 sm: 80:96:98:30:40:F2 detecting 1/2 10000
... (동일 cascade 반복)
01:53:57.508 sm: Unlock suppressed (grace 29306ms remaining)

Guru Meditation Error: Core  0 panic'ed (Unhandled debug exception).
Debug exception reason: Stack canary watchpoint triggered (sm_task)
```

한 tick 안에 feed cascade가 터지면서 **스택 4096B가 한계를 초과**. 구성 요소:
- sm_task 초기화 지역 배열: `macs[16][6] + DeviceConfig[16] ≈ 864B`
- `StateMachine sm` 지역 인스턴스 (`DeviceState[16]` 내장, `recent_obs[]` 포함 ~1KB)
- 드레인 루프에서 `sm.feed()`, `sm.tick()` 호출 → 내부 함수 call chain
- 각 함수 프레임마다 **stack canary** 오버헤드 (`CONFIG_COMPILER_STACK_CHECK_MODE_STRONG=y`)
- `ESP_LOGI` vararg + printf 포맷 임시 버퍼 (매 호출당 ~80B)
- `control_queue_send`, `app_config_get` 등 cascade

평소엔 1~2개 디바이스 feed만 처리하던 상황에선 3KB 정도로 여유 있게 통과했지만, 부팅 직후 모든 본딩 peer가 한꺼번에 감지되어 burst를 이룰 때 한계 돌파. **장기 잠복 버그**.

Canary가 트립되면 즉시 Debug exception → 태스크 컨텍스트로 panic handler 진입 → 재부팅.

### Phase 2: safe mode 무한루프 (이 버그가 없었으면 자가 복구됐을 것)

Phase 1이 3회 반복되면 `check_safe_mode()`가 `s_safe_mode = true` 설정 → `sm_task_start()`, `control_task_start()`, `bt_manager_start()` 등이 전부 skip된 채 `app_main()` 리턴.

그런데 `start_webserver()`는 safe mode 이전에 호출되어 여전히 살아있음. `/api/devices` 핸들러 `devices_handler()`가 `sm_get_snapshots()`를 호출하는데:

```cpp
// main/sm_task.cpp (수정 전)
int sm_get_snapshots(DeviceState *out, int max) {
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);  // ← nullptr!
    int n = ...;
    memcpy(out, s_snapshots, n * sizeof(DeviceState));
    xSemaphoreGive(s_snapshot_mutex);
    return n;
}
```

`s_snapshot_mutex`는 `sm_task_start()` 안에서 `xSemaphoreCreateMutex()`로 생성되는데, safe mode에서는 이 경로가 skip됨 → **영원히 nullptr**. `xSemaphoreTake(nullptr, ...)` → `xQueueSemaphoreTake`의 `configASSERT(pxQueue)` → panic.

그리고 프론트엔드가 `/api/devices`를 자동 폴링 (기본 1~2초 간격) → 매 폴링마다 panic → 재부팅 → safe mode → 다시 panic. **무한루프**. 사고 당시 카운터가 **122**까지 쌓여 있었음 (uint8이라 255에서 오버플로우할 뻔).

부가 증상: 같은 레이스는 **normal 부팅 경로에도 이론적으로 존재**. `main.cpp`:
- L121: `start_webserver(mode)`  — http 라우트 등록 완료
- L134: `sm_task_start()`        — mutex 여기서 생성

L121 ~ L134 사이에 `/api/devices` 요청이 들어오면 normal 모드 부팅에서도 같은 panic. 프론트엔드의 자동 폴링 + 빠른 WiFi 연결이 이 윈도우를 자주 적중. 그래서 Phase 1이 우연히 안정적이었더라도 이 레이스로 safe mode 진입할 수 있음.

## 진단 과정 — 막다른 길과 돌파구

### 막다른 길 1: "옛날 카운트 아닐까?"
Panic count 122를 보자마자 의심한 첫 가설: "이전 PSRAM brick 사고(b1b80d4) 기간에 쌓인 옛날 카운트인데, check_safe_mode()가 non-panic reboot에서 리셋을 안 하나?"

코드 확인 결과 `check_safe_mode()`는 명시적으로 리셋함:
```cpp
if (reason == ESP_RST_PANIC) count++;
else count = 0;
```

**기각**. 지금도 panic이 실제로 발생하고 있는 것임.

### 막다른 길 2: "Reset reason 4가 ESP_RST_SW 아닌가?"
초기 로그 `Reset reason: 4` 를 보고 SW reset으로 오해. 실제로는 ESP-IDF `esp_reset_reason_t`에서 **4 = ESP_RST_PANIC**. (0~3이 UNKNOWN/POWERON/EXT/SW, 4부터 PANIC). 이 오독이 "진짜 뭔가 panic 중이다"를 깨닫는 걸 지연시켰음.

### 돌파구: `/api/coredump` + `addr2line`
Coredump API로 exc_task, pc, backtrace 받아서 `xtensa-esp32-elf-addr2line -pfiaC -e build/doorman.elf`로 resolve:

```
0x400976f0: panic_abort
0x400976b5: esp_system_abort
0x40096426: __assert_func
0x401ff90a: xQueueSemaphoreTake   ← assert 실패 지점
0x400e7730: sm_get_snapshots at main/sm_task.cpp:235
0x400e48bc: devices_handler at main/http_server.cpp:571
```

이걸로 **Phase 2** (nullptr mutex)가 바로 드러남. 하지만 이건 safe mode에서 무한루프만 설명하지, **최초 진입**은 설명 안 됨. Phase 1을 놓칠 뻔했음.

### 핵심 단서: 시리얼로 직접 부팅 캡쳐
`/api/reboot`로 trigger + `pyserial`로 115200 캡쳐 → 부팅 로그 전체 확보. 거기서 **다른 종류의 패닉**이 발견됨:

```
Guru Meditation Error: Core 0 panic'ed (Unhandled debug exception).
Debug exception reason: Stack canary watchpoint triggered (sm_task)
```

`addr2line`로 backtrace resolve했더라면 sm_task 내부 함수 chain이 보였을 것. 여기서 Phase 1 정체 확정.

**교훈: 코어덤프는 "마지막" 하나만 저장한다.** Safe mode에서의 nullptr panic이 계속 덮어쓰고 있어서 진짜 원인인 stack overflow의 코어덤프는 이미 사라진 상태였음. **시리얼 모니터를 띄워놓고 실제 재현을 캡쳐하는 것**이 결정적이었음.

## 해결 (커밋 69819bd)

### Fix 1 — sm_task 스택 상향

```cpp
// main/sm_task.cpp
BaseType_t ok = xTaskCreatePinnedToCore(
    sm_task, "sm_task",
    6144,   // ← 4096에서 상향. 2KB 여유 확보.
    nullptr, 5, nullptr, tskNO_AFFINITY);
```

4096 → 6144. 증분 2KB는 최근 RAM 최적화(커밋 6b017fc)로 확보한 여유에서 감수.

### Fix 2 — sm_get_snapshots nullptr 가드

```cpp
// main/sm_task.cpp
int sm_get_snapshots(DeviceState *out, int max) {
    // safe mode에서는 sm_task_start가 호출되지 않아 mutex가 nullptr.
    // HTTP devices_handler가 이 경로를 계속 치므로, null 체크 없이 take하면
    // assert로 panic → 재부팅 → safe mode → 다시 panic 무한루프.
    if (s_snapshot_mutex == nullptr) {
        return 0;
    }
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    int n = s_snapshot_count < max ? s_snapshot_count : max;
    memcpy(out, s_snapshots, n * sizeof(DeviceState));
    xSemaphoreGive(s_snapshot_mutex);
    return n;
}
```

이 한 줄 가드가 **"부팅 race로 인한 최초 진입"과 "safe mode 무한루프" 양쪽을 동시에 해결**. 다른 queue-based API(`control_queue_send`, `sm_feed_queue_send`, `sm_remove_device_queue_send`)는 이미 `if (s_queue == nullptr) return;` 가드가 있었음. `sm_get_snapshots`만 혼자 빠져있던 장기 잠복 버그.

### 복구 시퀀스
- 새 펌웨어는 여전히 safe mode 상태로 시작하지만, 더 이상 panic 안 함.
- normal mode로 진입하는 건 panic count 리셋 후인데, safe mode에서는 `reset_panic_counter` 타이머가 설정되지 않음.
- 하지만 OTA는 safe mode에서도 가능. OTA 재부팅 시 `esp_restart()` → `ESP_RST_SW` → `check_safe_mode()`가 count=0으로 리셋 → 다음 부팅부터 normal mode.
- Normal mode 부팅 후 60초 생존 시 `reset_panic_counter` 타이머가 다시 count=0으로 commit.

## 왜 지금 터졌나 — 잠복의 이유

- sm_task 스택 4096은 꽤 오래 유지된 값. 평소엔 1~3개 BT peer 환경에서 3KB 미만으로 운영됐음.
- 사고 당시는 **본딩된 BLE peer 8개** 상태. 부팅 직후 전원 켜진 근처 기기 대부분이 순식간에 감지되어 feed cascade.
- 각 feed마다 `sm: ... detecting N/M`, `present`, `unlock`, `Unlock suppressed (grace ...)` 로그 4~5줄 → `ESP_LOGI` 포맷 버퍼가 스택에 누적.
- StateMachine 내부에서 peer 발견 시 `update_device_config` → `device_config_get` → DeviceConfig 복사(48B) → 여러 call frame.
- 이 모든 게 한 `xQueueReceive` 드레인 루프(최대 16개 메시지/iteration) 안에서 처리됨 → **스택 peak이 한 순간에 몰림**.

이 조합은 "본딩 기기 수 × BLE advertising 밀도 × 부팅 타이밍"이라는 environmental 조건이 맞아야 재현. 그래서 개발·QA 환경(peer 1~2개)에선 안 나왔고, 실제 설치 환경(peer 8개)에서 폭발.

**교훈: 스택 사이즈 결정은 "평소 사용량"이 아니라 "최대 burst 사용량"으로.**

## 안전 룰 (재발 방지)

### Task 스택 관리
1. **모든 task 생성 직후 `uxTaskGetStackHighWaterMark()`를 monitor_task에서 주기 로그**. 실제 peak 사용량을 관측 가능하게 만들 것. 현재 monitor_task는 heap 상태만 로그함 → 확장 필요.
2. **스택 여유 규칙: peak ≤ stack_size × 0.6**. 60% 이하에서 운영. 40% 이상 여유 없으면 burst 대비 부족.
3. 부팅 직후 high-fanout 태스크(sm_task, bt_manager callback thread 등)는 **특별 주의**. 평균이 아닌 burst로 측정.

### Nullptr 가드 일관성
1. **"Task의 s_queue/s_mutex를 사용하는 모든 public API는 nullptr 가드 필수"** — 이건 코드 리뷰 체크리스트에 추가.
2. safe mode 경로가 있는 프로젝트는 특히 중요. task가 안 떠있는 상태에서도 http 핸들러가 그 task의 공유 상태에 접근할 수 있기 때문.
3. `configASSERT`로 nullptr를 잡는 건 **crash → 재부팅 → 같은 crash 반복**의 무한루프 발판이 됨. 방어적 early return이 낫음.

### Safe mode 설계
1. Safe mode에서 돌아가는 http 핸들러는 "어떤 공유 상태가 없어도" 작동해야 함.
2. `is_safe_mode()` 체크를 handler 진입점에서 명시적으로 할 수 있는 경로도 고려. 하지만 근본적으론 **API 레이어가 자체적으로 nullptr safe한 게 견고**.
3. Safe mode에서도 `reset_panic_counter` 타이머를 **OTA 성공 후 일정 시간 생존 조건으로** 돌리는 것도 고려. 현재는 normal mode 전용.

### 진단 도구
1. 개발·배포 시 **시리얼 로그 캡쳐 파이프라인**을 항상 준비. coredump 한 장만으론 원인의 일부만 보일 수 있음.
2. `/api/coredump` + `addr2line` 조합은 필수. 이 프로젝트엔 이미 API가 있어서 큰 도움 됐음 (커밋 a6bfecf).
3. 재현이 환경 조건에 의존할 때는 **production 환경에서 시리얼을 물리적으로 뽑아 캡쳐**하는 게 가장 확실.

## 관련 자료

- 부모 사고 문서: [PSRAM 태스크 스택 사고](psram-task-stack-bricks-device.md)
  - 이 문서의 "backup 버그"가 이번 사고를 유발한 건 아니지만, 두 사고 모두 **"스택 관련 잠복 버그가 safe mode를 어떻게 고장내는가"**라는 공통 주제.
  - 이번 사고의 nullptr 가드 누락은 PSRAM 사고 직후의 RAM 재구성(6b017fc, EXT_RAM_BSS_ATTR)과 **무관**. 원래부터 있던 잠복 버그였음.
- `CONFIG_COMPILER_STACK_CHECK_MODE_STRONG=y` — stack canary 활성화. 이게 없었으면 스택 오버플로우가 silent corruption으로 나타나 훨씬 더 찾기 어려웠을 것. **절대 끄지 말 것.**
- ESP-IDF `uxTaskGetStackHighWaterMark()` — 태스크별 스택 최소 여유 바이트 수. 자원 튜닝의 gold standard.
- ESP-IDF `esp_reset_reason_t`: `ESP_RST_PANIC = 4` (0부터 UNKNOWN/POWERON/EXT/SW/PANIC 순서). 숫자만 보고 오독하지 말 것.
- 해결 커밋: `69819bd`
- 관련 코드: `main/sm_task.cpp:188-199` (stack size), `main/sm_task.cpp:234-247` (nullptr guard)

## 핵심 교훈 (한 줄)

> **최초 진입 원인과 탈출 불가 원인이 다른 버그일 수 있다.** 현재 관측되는 crash(Phase 2)를 고치기 전에, 그것이 **진짜 처음 발생한 원인(Phase 1)인지 검증**해야 한다. 코어덤프는 마지막 하나만 남으므로 시리얼 캡쳐가 결정적. 그리고 스택 사이즈는 "평균"이 아닌 "burst peak"로 정할 것.
