# doorman-esp

Bluetooth Classic presence detection based automatic door lock opener for office use.

-----

## 개요

도어락을 Bluetooth Classic presence detection으로 자동 열림 구현.  
스마트폰과 최초 1회 페어링 후, 문 앞 접근 시 자동으로 릴레이를 트리거해 도어락을 오픈한다.

-----

## 하드웨어

### 메인 보드

- **ESP32-DevKitC VIE** (ESP32-WROVER-IE 기반)
    - Classic Bluetooth + BLE 듀얼 지원 (오리지널 ESP32 칩)
    - PSRAM 8MB (BT 스택 + WiFi + HTTP 서버 동시 운용 여유 확보)
    - U.FL 외장 안테나 커넥터 — 향후 안테나 외부 노출 옵션
    - Flash 16MB

### 릴레이

- **1채널 3.3V 릴레이 모듈**
    - VCC: ESP32 3V3 전원 핀 (GPIO 아님)
    - 제어: GPIO 1핀
    - 도어락 리모컨 버튼 양단에 병렬 연결

### 도어락 연동

- **실내용 리모컨 수신기 세트** 별도 구매
- 리모컨 버튼 양단에 릴레이 병렬 연결 → ESP32 GPIO로 트리거
- RF 신호 분석 불필요, 수신기는 도어락에 그대로 유지

-----

## Bluetooth 설계

### Presence Detection 방식

- **Classic Bluetooth page scan** (BLE 아님)
- 스마트폰과 최초 1회 페어링 필수
- 페어링 후 MAC 고정 → 랜덤화 문제 없음
- iOS/Android 모두 지원

### 순회 로직

- 등록된 기기 리스트를 순차 page scan
- page timeout: `esp_bt_gap_set_page_to()` 로 최솟값 튜닝 (실측 필요)
- 이론상 대당 ~14ms (0x0016 × 0.625ms), 실제는 테스트로 확인
- 항상 풀스피드 순회 (PIR 없음)

### 오탐 방지

- 새로 감지된 MAC만 트리거
- 이미 실내에 있는 기기는 presence cache로 추적 → 무시
- 일정 시간 미감지 시 cache에서 제거

-----

## 소프트웨어 스택

### 개발 환경

- **ESP-IDF v6.0** (순수 IDF, Arduino as a component 미사용)
- **C++** (전 파일 `.cpp`, `.c` 없음)
- **CLion** + ESP-IDF 플러그인
- 파일명 컨벤션: `snake_case`, 클래스명: `PascalCase`

### Arduino 미사용 이유

- HTTP 서버 → IDF 내장 `esp_http_server`
- OTA → IDF 내장 `esp_https_ota` / `esp_ota_ops`
- WebSocket → IDF 내장
- BT → 원래부터 IDF API 직접
- Arduino 레이어가 필요한 기능 없음

### 주요 sdkconfig 설정

```
# PSRAM
CONFIG_SPIRAM=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y

# Bluetooth Classic (BLE 꺼짐)
CONFIG_BT_ENABLED=y
CONFIG_BT_CLASSIC_ENABLED=y
CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y

# 코어 분리: BT → Core 0, WiFi → Core 1
CONFIG_BT_BLUEDROID_PINNED_TO_CORE_0=y
CONFIG_BTDM_CTRL_PINNED_TO_CORE_0=y
CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_1=y

# CPU/스케줄러
CONFIG_ESP32_DEFAULT_CPU_FREQ_240=y
CONFIG_FREERTOS_HZ=1000

# 플래시/파티션
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y

# 코어덤프 → Flash
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y

# 로그 타임스탬프 → 시스템 시간
CONFIG_LOG_TIMESTAMP_SOURCE_SYSTEM=y

# 스택 스매싱 보호
CONFIG_COMPILER_STACK_CHECK_MODE_STRONG=y
```

-----

## 아키텍처

### FreeRTOS 태스크 구성

|태스크              |코어    |역할                      |
|-----------------|------|------------------------|
|`bt_scan_task`   |Core 0|BT page scan + 상태머신 tick|
|`github_ota_task`|Core 1|GitHub 릴리즈 폴링 + 자동 OTA  |
|HTTP 서버          |Core 1|IDF httpd 자체 태스크 (자동)   |

- 태스크는 클래스 불필요 → 단순 C 스타일 함수로 구현
- 도메인 객체를 `arg`로 받아 조작

### 상태머신

- `DoorLockStateMachine` 클래스가 핵심 도메인 로직 담당
- `bt_scan_task` 가 루프에서 `sm.tick()` 호출
- 상태: `IDLE` → `SCANNING` → `DEVICE_FOUND` → `UNLOCKING` → `COOLDOWN`

### 인터페이스 기반 설계 (테스트 가능성)

```cpp
class IBluetoothScanner { ... };   // mock 가능
class IGPIOController { ... };     // mock 가능
```

- 호스트 GTest로 상태머신 로직 단위 테스트
- 하드웨어 의존 코드는 Unity on device

### 프로젝트 구조

```
main/
├── main.cpp                        # app_main() 진입점만
├── CMakeLists.txt
│
├── bt/
│   ├── bluetooth_scanner.h/.cpp   # IDF BT API 래핑
│   ├── presence_cache.h/.cpp
│   └── bt_scan_task.cpp           # 태스크 진입 함수
│
├── door/
│   ├── door_lock_state_machine.h/.cpp
│   └── debouncer.h/.cpp
│
├── gpio/
│   ├── gpio_controller.h/.cpp
│
├── ota/
│   ├── github_ota_checker.h/.cpp
│   └── github_ota_task.cpp
│
└── log/
    └── log_server.h/.cpp          # WebSocket 로그 스트리밍
```

-----

## 원격 관리

### 로그

- WebSocket으로 실시간 스트리밍
- 브라우저 또는 터미널 클라이언트로 접속

### OTA

- `github_ota_task` 가 분 단위로 GitHub Releases API 폴링
- 현재 펌웨어 버전과 비교, 신규 릴리즈 감지 시 자동 `esp_https_ota`
- 별도 Push 필요 없음

-----

## 설계 철학

- **실용적 C++**: OOP 강박 없이 상황에 맞는 패러다임 혼용
- **C 스타일 혼용 허용**: FreeRTOS/IDF API 직접 호출, 불필요한 래핑 금지
- **인터페이스는 테스트 가능성을 위해**: 하드웨어 의존성 역전
- **YAGNI**: 필요할 때 클래스로 올리기, 미리 추상화 금지
- **레이어 최소화**: Arduino 없이 IDF 네이티브로 군더더기 제거