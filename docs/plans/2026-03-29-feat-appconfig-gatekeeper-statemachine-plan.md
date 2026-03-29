---
title: "feat: AppConfig + StateMachine 상태머신 구체화"
type: feat
status: active
date: 2026-03-29
---

# AppConfig + StateMachine 상태머신 구체화

## Overview

최종 제품 로드맵의 **3차 단계**. BT presence → 자동 문열림의 핵심인 StateMachine(구 Gatekeeper)을 구체화하고, 설정을 관리하는 AppConfig 컴포넌트를 신설한다. 두 컴포넌트 모두 순수 C++로 `components/`에 두어 호스트 GTest로 검증한다.

## Motivation

현재 Gatekeeper는 `feed()` 시그니처만 존재하고, 내부 로직이 없다. 4차(BT 프로덕션화)에서 BT 스캔 결과를 연결하려면, 먼저 상태머신과 설정 구조체가 완성되어 있어야 한다. 호스트 테스트로 로직을 검증한 뒤에 하드웨어와 연결하는 순서가 맞다.

## Proposed Solution

### AppConfig (`components/config/`)

**두 계층으로 분리:**
- `components/config/` — AppConfig 구조체 + `validate()` (순수 C++, 호스트 테스트 가능)
- `main/config_service.cpp` — `getConfig()`/`setConfig()` + NVS 영속 + lock (ESP 의존, Phase 4)

Phase 3에서는 **구조체와 validate()만** 구현. 서비스 래퍼(lock + NVS)는 Phase 4에서.

```cpp
// components/config/include/config.h
#pragma once

#include <cstdint>

struct AppConfig {
    uint32_t cooldown_sec = 120;          // 쿨다운 시간 (초). 0이면 시간 조건 무시 (went_undetected만으로 재트리거).
    uint32_t presence_timeout_ms = 5000;  // feed() 없이 이 시간 경과하면 미감지 전환.
};

// 설정값이 허용 범위 내인지 검증. false면 무효.
bool validate(const AppConfig &cfg);
```

```cpp
// components/config/config.cpp
#include "config.h"

static const uint32_t kMaxCooldownSec = 3600;         // 1시간
static const uint32_t kMaxPresenceTimeoutMs = 60000;   // 1분

bool validate(const AppConfig &cfg) {
    return cfg.cooldown_sec <= kMaxCooldownSec &&
           cfg.presence_timeout_ms > 0 &&
           cfg.presence_timeout_ms <= kMaxPresenceTimeoutMs;
}
```

- Phase 3에서는 `cooldown_sec` + `presence_timeout_ms`만 포함. WiFi/Auth/OTA 필드는 해당 Phase에서 추가.
- `validate()`는 free function.
- **Phase 4에서** `main/config_service.cpp`가 `getConfig()`/`setConfig()` with lock + NVS 로드/저장을 제공. StateMachine은 직접 AppConfig 참조를 안 들고, 필요할 때 `getConfig()`를 호출.
- **Phase 3 호스트 테스트에서는** AppConfig를 값으로 직접 전달하여 테스트.

### StateMachine (`components/statemachine/`)

기존 `components/gatekeeper/`를 리네임. `feed()` + `tick()` 이중 API로 이벤트와 시간을 분리한다.

**핵심 설계: feed/tick 분리**

BLE는 수동 수신이라 advertisement가 안 들리면 콜백이 안 온다. "미감지"를 호출 측에서 명시적으로 넘기는 건 부자연스럽다. 대신:
- `feed(mac, now_ms)`: "이 기기가 방금 보였다" — 감지 이벤트만 기록
- `tick(now_ms)`: 주기적 호출. 타임아웃에 의한 미감지 전환 + 쿨다운 만료 확인 + Unlock 판정

`ScanResult` enum은 제거. "안 보인다"는 feed가 안 불리는 것으로 자연스럽게 표현되고, tick이 `presence_timeout_ms` 기반으로 판단한다.

```cpp
// components/statemachine/include/statemachine.h
#pragma once

#include "config.h"

#include <array>
#include <cstdint>

enum class Action { Unlock, NoOp };

class StateMachine {
public:
    static const int kMaxDevices = 30;

    explicit StateMachine(AppConfig cfg);  // 값 복사. Phase 4에서 getConfig() 패턴으로 전환 가능.

    // BT/BLE 스캔 결과 기록. now_ms는 부팅 이후 경과 밀리초.
    //   seen=true:  감지됨 (BLE adv 수신, Classic probe 성공). last_seen 갱신.
    //   seen=false: 미감지 (Classic probe 실패). 즉시 미감지 전환.
    // BLE는 항상 seen=true로 호출. 타임아웃은 tick()이 처리.
    void feed(const uint8_t (&mac)[6], bool seen, uint32_t now_ms);

    // 주기적 호출. 시간 기반 상태 전이를 평가하고, Unlock이 필요하면 반환.
    // 내부적으로: 타임아웃(feed(true) 이후 presence_timeout_ms 경과) → 미감지 전환,
    //            쿨다운 만료 + 재감지 → Unlock.
    Action tick(uint32_t now_ms);

    // 현재 추적 중인 기기 수.
    int device_count() const;

private:
    struct DeviceState {
        uint8_t mac[6] = {};
        bool valid = false;
        bool detected = false;           // 현재 감지 상태
        bool went_undetected = false;    // 마지막 Unlock 이후 미감지를 거쳤는가
        uint32_t last_seen_ms = 0;       // 마지막 feed() 시각
        uint32_t last_unlock_ms = 0;     // 마지막 Unlock 발행 시각 (0 = 아직 없음)
    };

    AppConfig config_;
    std::array<DeviceState, kMaxDevices> devices_ = {};

    DeviceState *find_device(const uint8_t (&mac)[6]);
};
```

**feed() 로직:**

```
feed(mac, seen, now_ms):
  device = find_or_create(mac)  // 없으면 빈 슬롯에 동적 생성
  if (!device) → return          // 슬롯 풀 (30개 초과)

  if (seen):
    device->last_seen_ms = now_ms
    device->detected = true
  else:
    // Classic probe 실패 등: 즉시 미감지 전환
    device->detected = false
    device->went_undetected = true
```

- add_device/remove_device 없음. BT 스택이 bond를 관리하고, bt_manager가 bonded peer만 feed.
- SM은 feed 오는 MAC에 대해 DeviceState를 **동적 생성**. 별도 등록 절차 불필요.
- 오래 미감지된 슬롯은 tick()에서 정리 (예: 24시간 이상 미감지 → 슬롯 해제).

**tick() 로직:**

```
tick(now_ms):
  for each device in devices_ where valid:

    // 1. 타임아웃 체크: 오래 안 보이면 미감지 전환
    if (device.detected && device.last_seen_ms > 0):
      if (now_ms - device.last_seen_ms >= config_.presence_timeout_ms):
        device.detected = false
        device.went_undetected = true

    // 2. Unlock 판정: 감지 중이고 쿨다운 조건 충족 시
    if (device.detected):
      if (device.last_unlock_ms == 0):
        // 최초 감지 → 무조건 Unlock
        device.last_unlock_ms = now_ms
        device.went_undetected = false
        return Unlock

      cooldown_ms = config_.cooldown_sec * 1000
      if (device.went_undetected && (cooldown_ms == 0 || now_ms - device.last_unlock_ms >= cooldown_ms)):
        device.last_unlock_ms = now_ms
        device.went_undetected = false
        return Unlock

  return NoOp
```

tick()은 한 번에 최대 하나의 Unlock만 반환. 호출 주기가 충분히 빈번하면(1~3초) 복수 기기 동시 감지도 다음 tick에서 처리.

**설계 결정 근거:**
- **feed(seen)/tick 2중 API**: feed(true)는 "보였다", feed(false)는 "못 찾았다"(즉시 미감지). tick()은 타임아웃 기반 미감지 전환. BLE는 항상 feed(true), Classic은 결과에 따라 true/false. 단일 메서드로 양쪽 경로 통합.
- **ScanResult enum 제거**: bool seen 파라미터가 더 직관적. BLE/Classic 호출 패턴 차이를 자연스럽게 수용.
- **Action enum**: Gatekeeper 접두사 제거. 단순하게 `Unlock` / `NoOp`.
- `std::array<DeviceState, 30>`: ESP32 BT bond 슬롯 제한(BLE 15 + Classic 15)이 사실상 상한. 동적 할당 불필요.
- MAC은 `uint8_t[6]` 유지: ESP-IDF BT API와 일치. 변환 비용 제거.
### 태스크 설계 (Phase 4에서 구현, Phase 3에서 인터페이스 준비)

Phase 3에서는 StateMachine과 AppConfig의 순수 로직만 호스트 테스트로 검증한다.
Phase 4에서 아래 태스크/큐 구조로 래핑한다. Phase 3의 API 설계는 이 구조를 전제한다.

```
[BT Task] ──feed queue──> [SM Task] ──unlock queue──> [Control Task] ──> GPIO
[HTTP Task] ─────────────────────────manual queue────> [Control Task]
[Any Task] ──get/set──> [AppConfig Service] (lock)
```

- **SM Task가 StateMachine 인스턴스의 유일한 소유자.** 외부에서는 큐로만 접근.
- 기기 등록/삭제 이벤트 없음. BT 스택이 bond를 관리하고, bt_manager가 bonded peer만 feed.

**왜 3태스크인가 — 오버엔지니어링이 아닌 이유:**

| 태스크 | 타이밍 특성 | 블로킹 | 분리 근거 |
|--------|-----------|--------|-----------|
| BT Task | BT 스택 주도, 이벤트 기반 | Non-blocking | BT 스택이 태스크를 소유. 합칠 수 없음 |
| SM Task | 주기적 tick(1~3초) + 큐 이벤트 | Non-blocking | SM 인스턴스 단독 소유. BT 타이밍에 종속 안 됨 |
| Control Task | 명령 대기, 이벤트 기반 | **Blocking (0.5초)** | SM에 합치면 0.5초간 feed/tick 멈춤 |

ESP-IDF 가이드의 "타이밍이 비슷한 태스크는 묶어라"에 대해: BT는 스택 소유이므로 합칠 수 없고, SM과 Control은 블로킹 특성이 다르므로 묶으면 안 된다. 8MB PSRAM 환경에서 태스크 스택(4~8KB) 오버헤드는 무시 가능.

**SM Task 입력 큐:**

메시지 타입은 Feed 하나뿐: `{mac, seen, now_ms}`.

SM Task 루프: 큐 대기(타임아웃 = tick 주기) → 메시지 있으면 feed() → 타임아웃이면 tick() → Unlock 반환 시 Control 큐에 전송.

**Control Task 입력 큐:**

| 메시지 | 소스 |
|--------|------|
| AutoUnlock | SM Task |
| ManualUnlock | HTTP 서버 |

Control Task 루프: 큐 대기 → 명령 수신 → GPIO HIGH 0.5초 → GPIO LOW → 다음 대기. 동기 처리, 한 번에 하나.

**AppConfig 서비스:**
- `getConfig()`: 복사본 반환 (lock 내에서 memcpy)
- `setConfig()`: NVS 저장 + 내부 값 갱신 (lock 내)
- SM Task는 tick()마다 `getConfig()`로 최신 설정 참조.

**실시간 로그 → WebSocket 스트리밍 (Phase 5):**

기기 상태를 별도 API로 빼지 않는다. SM Task가 찍는 ESP_LOG가 곧 상태 정보.

```
[Any Task] → ESP_LOGI() → esp_log_set_vprintf(custom_vprintf)
    ├─ UART (시리얼)
    └─ Ring Buffer (~8KB, circular) → WS Handler → 브라우저
```

- `esp_log_set_vprintf()`로 전체 ESP_LOG 후킹. 필터링 없이 전체 전송.
- FreeRTOS Ring Buffer (thread-safe). WS 클라이언트 없으면 순환 덮어씀.
- SM Task는 상태 변경 시 의미 있는 로그를 ESP_LOGI로 출력 (감지/미감지/Unlock/쿨다운 등).
- **Phase 3에서 SM 로직 구현 시 적절한 ESP_LOGI 호출을 포함**해야 Phase 5 WS 스트리밍과 자연스럽게 연결됨.

Phase 3 StateMachine에 넣을 로그 포인트:
- feed(true) → 새 기기 첫 감지, 또는 미감지 후 재감지
- feed(false) → 즉시 미감지 전환
- tick() → 타임아웃에 의한 미감지 전환
- tick() → Unlock 판정
- tick() → 연속 감지 N초 요약 (1~5초 간격, 스팸 방지)
- cleanup_stale() → 오래된 슬롯 정리

### NVS 연동 (Phase 4에서 `main/config_service.cpp`로)

Phase 3에서는 NVS 연동을 하지 않는다. AppConfig는 기본값으로 생성하여 테스트.
Phase 4에서 `config_service.cpp`가 NVS namespace `door`, keys `cooldown`/`timeout`으로 로드/저장 + lock 제공.

## Technical Considerations

- **HOST_TEST 이중 관리**: `components/statemachine/CMakeLists.txt`(ESP-IDF용)과 루트 `CMakeLists.txt` HOST_TEST 경로(순수 CMake) 양쪽 모두 업데이트해야 함.
- **AppConfig 수명**: Phase 3에서는 값 복사이므로 수명 문제 없음. Phase 4에서 `getConfig()` 서비스로 전환 시에도 복사본 반환이므로 댕글링 없음.
- **시간 단위**: StateMachine 내부는 밀리초(uint32_t). 호출 측(main/)에서 FreeRTOS tick을 ms로 변환하여 전달.
- **기존 gatekeeper 디렉토리 리네임**: `components/gatekeeper/` → `components/statemachine/`. 헤더, 소스, CMakeLists, 테스트 파일 모두 변경.
- **코드 주석 스타일**: 모든 public 메서드에 `/** */` doc 주석. **why를 설명하는 주석을 풍부하게.** factbox-esp 스타일 — 메서드의 존재 이유, 파라미터 선택 근거, 사이드이펙트 등. 코드만 봐서는 알기 어려운 맥락을 주석으로 남긴다.

## Phase 4 Forward-Looking

Phase 3에서 직접 구현하지는 않지만, Phase 4 설계에 영향을 주는 결정사항:

- **페어링 윈도우**: 부팅 후 30초 자동 + 웹 UI에서 트리거 가능. 스캔 중단 없이 BLE advertising / Classic discoverable 켜고, BT Task 루프 내부 if로 타이머 관리 (타이머 콜백 대신 — BT API 호출이 같은 태스크에서 일어나 스레드 안전).
- **BT bond 삭제**: 웹 UI 기기 삭제 → HTTP → BT Task 큐 → `esp_ble_remove_bond_device()` / `esp_bt_gap_remove_bond_device()`. BT Task에서만 BT API 호출.

## Acceptance Criteria

### 기능

- [ ] `AppConfig` 구조체에 `cooldown_sec`(기본 120) + `presence_timeout_ms`(기본 5000)
- [ ] `validate()` free function: 범위 검증
- [ ] `StateMachine(AppConfig)` 생성자 (값 복사)
- [ ] `feed(mac, seen, now_ms)`: seen=true 감지 기록, seen=false 즉시 미감지 전환
- [ ] `tick(now_ms)`: 타임아웃 전환 + Unlock 판정
- [ ] feed()에서 새 MAC 동적 슬롯 생성 (add_device/remove_device 없음)
- [ ] `device_count()`: 현재 추적 중인 기기 수
- [ ] 최초 감지 → tick()에서 Unlock
- [ ] 감지 유지 (쿨다운 내) → NoOp
- [ ] 타임아웃 → 미감지 전환 → 재감지 + 쿨다운 경과 → Unlock
- [ ] 타임아웃 → 미감지 전환 → 재감지 + 쿨다운 미경과 → NoOp
- [ ] 미등록 MAC feed → 무시
- [ ] 복수 기기 독립 (기기 A 쿨다운이 기기 B에 영향 없음)
- [ ] ESP-IDF 빌드 시 statemachine, config 컴포넌트 정상 링크

### 테스트

- [ ] 호스트 GTest: 위 기능 시나리오 전부 커버
- [ ] `config_test.cpp`: 기본값 유효, 범위 초과 거부, presence_timeout 0 거부
- [ ] `statemachine_test.cpp`: cooldown_sec=0일 때 went_undetected만으로 즉시 재트리거
- [ ] `statemachine_test.cpp`: 새 MAC feed → 동적 슬롯 생성
- [ ] `statemachine_test.cpp`: 슬롯 30개 초과 시 feed 무시
- [ ] `statemachine_test.cpp`: 오래 미감지된 슬롯 tick()에서 정리
- [ ] `statemachine_test.cpp`: presence_timeout 경과 → detected가 false로 전환
- [ ] `statemachine_test.cpp`: feed(false) → 즉시 미감지 전환 (타임아웃 무관)
- [ ] `statemachine_test.cpp`: feed(false) 후 feed(true) + 쿨다운 경과 → tick()에서 Unlock
- [ ] ESP-IDF 빌드 성공 (`idf.py build`)
- [ ] 호스트 테스트 빌드 및 통과 (`cmake -DHOST_TEST=ON && ctest`)

## Implementation Phases

### Phase A: AppConfig 컴포넌트

1. `components/config/` 디렉토리 생성
   - `include/config.h`: AppConfig 구조체 + validate() 선언
   - `config.cpp`: validate() 구현
   - `CMakeLists.txt`: `idf_component_register()`
2. `host_test/config_test.cpp` 작성
3. 루트 `CMakeLists.txt` HOST_TEST 경로에 config 라이브러리 추가
4. 호스트 테스트 통과 확인

### Phase B: StateMachine (gatekeeper → statemachine 리네임 + 구현)

1. `components/gatekeeper/` → `components/statemachine/` 리네임
   - `include/statemachine.h`: StateMachine 클래스, feed()/tick() API, DeviceState
   - `statemachine.cpp`: 상태머신 + 쿨다운 + 타임아웃 로직
   - `CMakeLists.txt`: 컴포넌트명 변경 + `REQUIRES config`
2. `host_test/gatekeeper_test.cpp` → `host_test/statemachine_test.cpp` 리네임 + 전체 시나리오 테스트
3. 루트 `CMakeLists.txt` HOST_TEST 경로 업데이트 (gatekeeper → statemachine, config 링크)
4. 호스트 테스트 통과 확인

### Phase C: ESP-IDF 빌드 확인

1. `main/CMakeLists.txt`: REQUIRES에 statemachine, config 추가
2. ESP-IDF 빌드 성공 확인 (NVS 연동, 태스크 래핑은 Phase 4)

## 파일 변경 목록

### 신규
| 파일 | 역할 |
|------|------|
| `components/config/include/config.h` | AppConfig 구조체 + validate() |
| `components/config/config.cpp` | validate() 구현 |
| `components/config/CMakeLists.txt` | ESP-IDF 컴포넌트 등록 |
| `host_test/config_test.cpp` | AppConfig 호스트 테스트 |

### 리네임
| 기존 | 신규 |
|------|------|
| `components/gatekeeper/` | `components/statemachine/` |
| `components/gatekeeper/include/gatekeeper.h` | `components/statemachine/include/statemachine.h` |
| `components/gatekeeper/gatekeeper.cpp` | `components/statemachine/statemachine.cpp` |
| `host_test/gatekeeper_test.cpp` | `host_test/statemachine_test.cpp` |

### 수정
| 파일 | 변경 |
|------|------|
| `components/statemachine/include/statemachine.h` | StateMachine 클래스, feed()/tick(), DeviceState |
| `components/statemachine/statemachine.cpp` | feed/tick 로직 구현 |
| `components/statemachine/CMakeLists.txt` | 컴포넌트명 + REQUIRES config |
| `host_test/statemachine_test.cpp` | 전체 시나리오 테스트 |
| `CMakeLists.txt` (루트) | HOST_TEST에 config + statemachine 라이브러리 |
| `main/CMakeLists.txt` | REQUIRES statemachine, config 추가 |

### 현행화 (리네임 반영)
| 파일 | 변경 |
|------|------|
| `README.md` | Gatekeeper → StateMachine 용어 교체 |

## Dependencies & Risks

| 리스크 | 대응 |
|--------|------|
| 기존 gatekeeper 참조가 곳곳에 남아있을 수 있음 | 리네임 후 grep으로 잔여 참조 확인 |
| tick() 호출 주기에 따라 감지 반응 지연 | 1~3초 주기면 실사용에 무리 없음. 향후 조절 가능 |
| AppConfig 확장 시 validate() 규칙 증가 | Phase별로 필요한 필드만 추가, YAGNI |
| HOST_TEST / ESP-IDF CMakeLists 동기화 | 신규 컴포넌트 추가 시 양쪽 체크리스트 확인 |

## References

- 브레인스토밍: `docs/brainstorms/2026-03-29-final-product-spec-brainstorm.md`
- 아키텍처 기반: `docs/brainstorms/2026-03-25-doorman-architecture-brainstorm.md`
- 현재 골격: `components/statemachine/include/statemachine.h`
- 현재 호스트 테스트: `host_test/statemachine_test.cpp`
- NVS 패턴: `main/nvs_config.h`, `main/nvs_config.cpp`
- 스타일 레퍼런스: `~/Projects/factbox-esp`
- GitHub Actions 워크플로우: `.github/workflows/build.yml`
