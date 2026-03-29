# Doorman 최종 제품 스펙 — 브레인스토밍

**날짜**: 2026-03-29
**상태**: 확정
**선행 문서**:
- `docs/brainstorms/2026-03-25-doorman-architecture-brainstorm.md` (아키텍처 기반)
- `docs/brainstorms/2026-03-27-mvp2-wifi-provisioning-web-brainstorm.md` (2차 MVP)

-----

## 무엇을 만드는가

ESP32-WROVER-IE 기반 사무실 도어락 자동열림 시스템의 **최종 제품**.
등록된 사용자의 Bluetooth 프레즌스를 감지하여 문을 자동으로 열고,
웹 UI로 기기를 관리하며, GitHub Releases를 통해 자동 업데이트된다.

**핵심 사용 시나리오**:
1. 관리자가 기기를 설치하고 WiFi 프로비저닝
2. 동료가 폰을 페어링 (전원 인가 후 30초 윈도우)
3. 이후 동료가 사무실 앞에 오면 자동으로 문 열림
4. 관리자가 웹에서 등록 기기 관리, 상태 모니터링
5. 새 펌웨어가 릴리즈되면 자동 업데이트

**사용자**: 우리 사무실 전용. 관리자는 나, 사용자는 동료들.

-----

## 왜 이 접근인가

- **PoC → 제품**: BT 듀얼모드 PoC와 웹 인프라(2차 MVP)가 검증됨. 이제 핵심 로직(StateMachine)을 연결하고 운영 품질로 올릴 단계.
- **단순함 우선**: 감지 정책은 "감지되면 즉시 열기"로 시작. RSSI 튜닝은 실데이터 이후.
- **사무실 전용**: 범용 제품이 아니므로 설치 가이드, 다국어, 자동 프로비저닝 같은 건 불필요.

-----

## 핵심 결정사항

### 1. BT Presence → StateMachine → 문열림 흐름

```
[BT Scan Loop]
  ├─ BLE: bonded peer의 IRK로 advertising RPA resolve → StateMachine.feed(mac, true, now_ms)
  └─ Classic: bonded BR/EDR remote-name probe
       ├─ 성공 → StateMachine.feed(mac, true, now_ms)
       └─ 실패 → StateMachine.feed(mac, false, now_ms)

[StateMachine] (구 Gatekeeper)
  feed(mac, seen, now_ms)
    — seen=true: "보였다" → last_seen 갱신
    — seen=false: "못 찾았다" → 즉시 미감지 전환
  tick(now_ms) → Unlock / NoOp
    ├─ 타임아웃(feed(true) 이후 presence_timeout_ms 경과) → 미감지 전환
    ├─ 등록된 기기가 감지 중 + 쿨다운 조건 충족 → Unlock
    └─ 그 외 → NoOp

[Door Control]
  Unlock 이벤트 → GPIO 4 HIGH 500ms → LOW
```

- **feed(seen)/tick 2중 API**: BLE는 항상 feed(true). Classic은 결과에 따라 true/false. 단일 메서드로 양쪽 경로 통합. "안 보인다"는 feed(false) 즉시 전환 또는 tick() 타임아웃으로 처리.
- StateMachine은 `components/statemachine/`에 순수 C++로 유지 (호스트 테스트 가능).

### 2. 문열림 정책

- **초기 정책**: 등록된 기기가 감지되면 즉시 열기. 단순하게 시작.
- **쿨다운**: 두 가지 조건이 모두 충족되어야 재트리거.
  1. 해당 기기가 한번 "미감지" 상태로 전환된 적이 있을 것
  2. 마지막 열림으로부터 최소 N분(설정 가능)이 경과했을 것
- **향후 튜닝**: RSSI 임계값, 감지 지속시간 등은 실사용 데이터를 보고 추가.

### 3. 페어링 플로우

- **전원 인가 후 30초**: 페어링 윈도우. BLE advertising + Classic discoverable 동시 활성화.
- **30초 이후**: 페어링 종료, 스캔 모드로 전환.
- **웹에서도 페어링 트리거 가능**: 웹 UI "페어링 시작" → HTTP → BT Task 큐 → 페어링 윈도우 ON (30초). 부팅 후 30초와 동일한 메커니즘.
- **스캔 중단 없이 동작**: BLE advertising과 scan은 BT 컨트롤러 내부에서 시분할. Classic도 discoverable과 page scan 동시 가능. 페어링 윈도우 열어도 기존 감지 루프가 멈추지 않음.
- **페어링 윈도우 종료**: BT Task 루프 안에서 경과 시간 체크 (`if pairing_active && elapsed >= 30s → stop`). 타이머 콜백 대신 BT Task 내부 if를 쓴다 — BT API 호출이 모두 같은 태스크 컨텍스트에서 일어나 스레드 안전.
- 현재 bt_presence_poc.cpp의 페어링 윈도우 패턴을 계승하되, 코드는 bt_manager.cpp에서 전면 재작성.
- **본딩 = 등록**: 페어링 윈도우에서 본딩 성공한 기기는 별도 승인 없이 자동으로 등록된 기기가 된다. 사무실 전용이므로 이 수준으로 충분.
- **기기 삭제 = bond 제거**: 웹 UI에서 기기를 삭제하면 ESP-IDF BT 스택의 bond 정보도 함께 삭제해야 한다. 안 그러면 BT 레벨에서는 여전히 bonded인데 StateMachine는 모르는 상태가 됨.

### 4. 웹 UI + 실시간 로그 스트리밍

**탭/페이지 분리 구조**:

| 탭 | 기능 |
|----|------|
| 대시보드 | 문열기 버튼, 실시간 로그 스트리밍 |
| 설정 | 계정 변경, WiFi 재설정, 펌웨어 업데이트(수동 OTA + 현재 버전 표시), 도어 설정(쿨다운 시간 등) |

- 프론트엔드는 Vanilla HTML + JS 유지. 바이너리 임베드.
- SPA처럼 탭 전환은 JS로 처리 (페이지 리로드 없이).

**기기 상태 = 로그로 해결. 별도 상태 API 없음.**

기기 목록/상태를 전용 API로 빼는 대신, StateMachine(SM Task)이 찍는 로그 자체가 기기 상태 정보를 담는다. 브라우저는 로그 스트림을 읽어서 현재 상황을 파악한다.

SM Task 로그 예시:
```
[sm] 기기 AA:BB:CC 감지됨 (첫 감지 → Unlock)
[sm] 기기 AA:BB:CC 연속 감지 45초
[sm] 기기 AA:BB:CC 미감지 전환 (타임아웃)
[sm] 기기 AA:BB:CC 재감지 → Unlock
[door] 문 열림 (GPIO 펄스)
```

**로그 → WebSocket 스트리밍 구조:**

```
[Any Task] → ESP_LOGI() → esp_log_set_vprintf(custom_vprintf)
    ├─ UART (시리얼, 항상)
    └─ Ring Buffer (circular, ~8KB)
            ↓
[WS Handler] → Ring Buffer 읽기 → WebSocket send → 브라우저
```

- `esp_log_set_vprintf()`로 전체 ESP_LOG 출력을 후킹.
- 필터링 없이 전체 전송. 부팅 시에만 잠깐 지저분하고, 이후엔 우리가 찍은 로그만 나옴.
- FreeRTOS `xRingbufferCreate()`로 thread-safe ring buffer. 별도 lock 불필요.
- WS 클라이언트 없으면 ring buffer가 그냥 순환 덮어씀.
- WS 연결 시 현재 버퍼 내용부터 전송 시작.

### 5. GitHub Releases 자동 OTA

- **소스**: `potados99/doorman` 레포의 최신 릴리즈
- **자산**: 릴리즈 내 `firmware.bin` 파일
- **폴링 주기**: N시간마다 (설정 가능, 기본 6시간 등)
- **버전 비교**: 릴리즈 태그명(단일 정수, e.g. `3`, `4`) vs 펌웨어 빌드 시 주입된 버전
- **동작**: 새 버전이면 다운로드 → OTA 파티션에 쓰기 → 검증 → 재부팅
- **에러 처리**: 릴리즈 없음, firmware.bin 없음, 다운로드 실패 → 무시. 다음 폴링에 재시도.
- **버전 포맷**: 단일 정수 (v1, v2, v3). 비교는 정수 크기 비교.
- **빌드 시 버전 주입**: `esp_app_desc`로 빌드 타임에 바이너리에 포함. 런타임에 `esp_app_get_description()`으로 읽음. NVS 별도 저장 불필요.

### 6. NVS 확장

기존 2차 MVP의 `net/`, `auth/` 네임스페이스에 추가:

| Namespace | Key | 용도 | 기본값 |
|-----------|-----|------|--------|
| `door` | `cooldown` | 쿨다운 시간 (초) | 120 |
| `ota` | `poll_h` | OTA 폴링 간격 (시간) | 6 |

BT bond 정보는 ESP-IDF BT 스택이 자체 NVS 네임스페이스에 관리.

### 7. 코드 아키텍처 (1차 브레인스토밍 계승 + 확장)

**태스크/큐 아키텍처:**

```
[BT Task] ──feed queue──> [SM Task] ──unlock queue──> [Control Task] → GPIO
[HTTP Task] ──────────────────────────manual queue──> [Control Task]
[HTTP Task] ──get/set──> [AppConfig Service] (lock)
[Any Task] ──ESP_LOGI()──> Ring Buffer ──> [WS Handler] ──> 브라우저
```

- **StateMachine Task**: 전용 태스크에서 구동. SM 인스턴스의 유일한 소유자. 입력은 큐로만 받음 (BT 결과 feed만). 기기 등록/삭제 이벤트 없음 — BT 스택이 bond 관리, bt_manager가 bonded peer만 feed. SM은 feed 오는 MAC에 대해 동적으로 상태 추적.
- **Control Task**: 문 제어 단일 지점. StateMachine(자동)과 HTTP(수동) 모두 큐로 명령 수신. 한 번에 하나씩 동기 처리 (0.5초 GPIO 블로킹).
- **AppConfig Service**: `getConfig()`/`setConfig()` with lock. 여러 태스크에서 읽기/쓰기.

**파일 구조:**

```
components/
  statemachine/      # 상태머신 + presence cache + 쿨다운 (순수 C++, 호스트 테스트)
  config/            # AppConfig 구조체 + validate() (순수 C++)

main/
  main.cpp           # 초기화 흐름
  wifi.cpp           # STA/SoftAP (현재 구현 유지)
  http_server.cpp    # 모드별 웹서버 + API + WebSocket
  bt_manager.cpp     # BT 스캔 태스크 (PoC 전면 재작성)
  sm_task.cpp        # StateMachine 구동 태스크 + 입력 큐
  control_task.cpp   # 문 제어 태스크 + 명령 큐
  config_service.cpp # AppConfig get/set + NVS 영속 + lock
  ota_updater.cpp    # GitHub Releases 폴링 + OTA

host_test/
  statemachine_test.cpp
  config_test.cpp

frontend/
  index.html         # STA 메인 (탭 기반)
  setup.html         # SoftAP 프로비저닝 (현재 유지)

scripts/
  ota_upload.sh      # 수동 OTA 스크립트 (현재 유지)
```

- `bt_presence_poc.cpp` → `bt_manager.cpp`로 전면 재작성.
- `sm_task.cpp` 신규. StateMachine을 태스크로 래핑 + 큐 수신.
- `control_task.cpp` 신규. door_control의 GPIO 호출을 큐 기반으로 래핑.
- `config_service.cpp` 신규. AppConfig get/set + NVS + lock.
- `components/config/` — AppConfig 구조체 + validate() (순수 C++, 호스트 테스트용).
- `ota_updater.cpp` 신규. GitHub API 폴링 + OTA 다운로드.

### 8. 완성 기준

1. 동료들 폰 등록 → 출근 시 자동 문열림 안정 동작
2. 웹에서 기기 목록/상태 확인 가능
3. GitHub Releases 자동 OTA 동작
4. **위 전부 + 1달 무장애 운영**

-----

## 구현 로드맵 (순서)

| 단계 | 내용 | 선행 조건 |
|------|------|-----------|
| 3차 | AppConfig + StateMachine 상태머신 구체화 + 호스트 테스트 | - |
| 4차 | bt_presence_poc → bt_manager 프로덕션화, StateMachine 연결 | 3차 |
| 5차 | 웹 UI 탭 분리 + 기기 목록/상태 API | 4차 |
| 6차 | GitHub Releases 자동 OTA | 독립 (병렬 가능) |
| 7차 | 통합 테스트 + 실사무실 배포 + 1달 운영 | 전체 |

-----

## 해결된 질문

- **사용자 범위?** → 우리 사무실 전용. 관리자는 나.
- **페어링 플로우?** → 전원 인가 후 30초 윈도우 + 웹에서도 트리거 가능. 스캔 중단 없이 동작. 페어링 타이머는 BT Task 루프 내부 if로 처리 (타이머 콜백 대신 — BT API가 같은 태스크 컨텍스트에서 호출되어 스레드 안전).
- **기기 상태 = 로그로 해결** → 별도 상태 API 없음. SM이 찍는 ESP_LOG를 WS로 스트리밍. 브라우저가 로그 스트림에서 상태 파악.
- **문열림 정책?** → 감지 즉시. 나중에 RSSI 등 튜닝.
- **쿨다운?** → 미감지 전환 + 최소 시간 병행.
- **자동 OTA?** → potados99/doorman releases 폴링, firmware.bin, 정수 버전 비교.
- **웹 UI 구성?** → 탭 분리 (대시보드, 기기관리, 설정).
- **버전 포맷?** → 단일 정수 (v1, v2, v3).
- **완성 기준?** → 자동 문열림 + 웹 관리 + 자동 OTA + 1달 무장애 운영.
- **기기 상태 갱신?** → 전체 ESP_LOG를 Ring Buffer → WS 스트리밍. 별도 상태 API 없음.
- **코드 주석 스타일?** → 메소드마다 `/** */` doc 주석. why를 설명하는 주석 풍부하게. factbox-esp 스타일.
- **OTA 중 BT?** → 그냥 병행. 코어 분리(BT=Core0, WiFi=Core1) 되어있으니 시도해보고 판단.
- **BT PoC → 프로덕션?** → 전면 재작성. PoC에서 배운 것을 바탕으로 깨끗하게.
- **BLE/Classic 융합?** → 하나만 감지되면 OK. 별도 통합 로직 불필요.

-----

## 다음 단계

→ 단계별 `/workflows:plan`으로 구현 플랜 작성 (3차부터)
