# doorman-esp

ESP32 기반 사무실 도어락 자동 열림 장치입니다.
등록된 사용자의 Bluetooth 프레즌스를 감지하여 문을 자동으로 열고, 웹 UI로 기기를 관리합니다.

## 어떻게 동작하는지

Bluetooth 듀얼모드(BLE + Classic)로 본딩된 기기의 존재를 감지합니다.

- **iPhone**: BLE bond 후 확보한 IRK로 advertising의 RPA를 resolve합니다.
- **Android / macOS**: Classic SPP 페어링 후 bonded BR/EDR 주소에 remote-name probe를 보냅니다.

감지 이벤트는 StateMachine으로 전달됩니다. StateMachine은 윈도우 기반 진입 판단을 수행합니다. 설정된 시간 윈도우 내에 RSSI 조건을 충족하는 관측이 일정 횟수 이상 쌓이면 "재실"로 전환하고, Unlock 액션을 발생시킵니다. 이후 feed가 끊기면 타임아웃으로 "퇴실"로 전환됩니다.

SM Task는 StateMachine의 Unlock 판정을 받되, 3중 억제 체계로 실제 전달을 결정합니다.

1. **시작 유예기간** (15초): 재부팅 직후 기존 기기 flood 방지
2. **auto_unlock OFF**: 사용자가 명시적으로 꺼놓은 상태
3. **페어링 중**: 새 기기 bond 도중 문열림 방지

억제를 통과한 Unlock은 Control Task 큐로 전달되고, GPIO 4에 500ms HIGH 펄스를 보내 릴레이를 구동합니다.

## 웹 UI

STA 모드에서 `http://doorman.local`로 접속합니다. HTTP Basic Auth로 보호됩니다.

**Dashboard 탭**:
- 문열기 버튼 (수동)
- Auto-Unlock 토글
- 실시간 로그 뷰어 (WebSocket 스트리밍, 태그 필터, 텍스트 필터, 전체화면, 자동 스크롤 일시정지)

**Settings 탭**:
- Bonded Devices: 본딩된 기기 목록 조회 및 삭제
- Presence Tuning: Enter 파라미터 (윈도우, 카운트, RSSI 임계값), Exit 타임아웃
- Bluetooth Pairing: 수동 시작/종료 토글
- Firmware: OTA 펌웨어 업로드
- Account: 관리자 계정 변경
- WiFi: SSID/비밀번호 변경 및 재부팅

## 설치 및 사용

### 하드웨어

| 항목 | 사양 |
|------|------|
| 보드 | ESP32-WROVER-IE |
| Flash | 8 MB |
| PSRAM | 8 MB |
| 도어 제어 | GPIO 4 → 릴레이 → 도어락 리모컨 입력 병렬 연결 |

### 첫 설치

1. 시리얼로 펌웨어를 플래시합니다.
   ```bash
   idf.py flash
   ```
2. 기기가 `Doorman-Setup` SoftAP를 열고 `http://192.168.4.1`에서 WiFi 프로비저닝 페이지를 제공합니다.
3. SSID와 비밀번호를 입력하면 기기가 재부팅하여 STA 모드로 전환됩니다.
4. 이후 `http://doorman.local`에서 웹 UI에 접속합니다. 기본 계정은 `admin` / `admin`입니다.

### 이후 펌웨어 업데이트

- **웹 UI**: Settings 탭 → Firmware → `.bin` 파일 업로드
- **스크립트**: `scripts/ota_upload.sh` (MAC 기반 IP 탐색 + OTA 업로드)

### 페어링

웹 UI의 Settings 탭에서 Pairing 버튼으로 수동 시작/종료합니다. 페어링 모드에서는 BLE advertising + Classic discoverable 상태가 됩니다. 본딩 성공한 기기는 별도 승인 없이 자동 등록됩니다.

### 튜닝

Settings 탭의 Presence Tuning에서 파라미터를 조절합니다.

| 파라미터 | 설명 | 기본값 |
|----------|------|--------|
| Enter Window | 진입 판단 윈도우 (ms) | 5000 |
| Enter Min Count | 윈도우 내 최소 관측 수 | 3 |
| RSSI Threshold | 진입 시 RSSI 하한 (dBm) | -70 |
| Exit Timeout | feed 없이 이 시간 경과 시 퇴실 (ms) | 15000 |

## 개발

### 빌드

ESP-IDF v6.0 환경에서 빌드합니다.

```bash
idf.py build
```

### 호스트 테스트

StateMachine과 AppConfig는 순수 C++이므로 호스트에서 GTest로 테스트합니다.

```bash
cmake -S . -B build-host -DHOST_TEST=ON
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

### OTA 스크립트

```bash
scripts/ota_upload.sh
```

MAC 주소 기반으로 네트워크에서 기기를 탐색하고 펌웨어를 업로드합니다.

### 버전

`v*` 태그를 push하면 GitHub Actions가 호스트 테스트 → ESP-IDF 빌드 → GitHub Release를 수행합니다. 태그에서 `v` 접두사를 제거한 정수를 `PROJECT_VER`로 주입합니다. 로컬 빌드에서는 git revision count가 자동 주입됩니다.

## 아키텍처 요약

FreeRTOS 태스크 3개와 큐 3개, Ring Buffer 1개로 구성됩니다. BT Task가 BLE/Classic 스캔 결과를 feed 큐로 SM Task에 전달하고, SM Task가 StateMachine을 구동하여 Unlock 판정 시 unlock 큐로 Control Task에 전달합니다. HTTP Task는 웹 API를 제공하며, 수동 문열기 명령을 Control Task에 직접 보냅니다. 모든 ESP_LOG 출력은 Ring Buffer를 거쳐 WebSocket으로 브라우저에 스트리밍됩니다. AppConfig Service는 태스크가 아니라 mutex로 보호되는 get/set 함수이며, NVS에 영속합니다.

```
[BT Task] ──feed queue──▸ [SM Task] ──unlock queue──▸ [Control Task] → GPIO
[HTTP Task] ──────────────────────manual queue──▸ [Control Task]
[Any Task] ──ESP_LOG──▸ Ring Buffer ──▸ [WS Handler] ──▸ Browser
```

## 프로젝트 구조

```
doorman-esp/
├── .github/workflows/
│   └── build.yml                # 태그 기반 빌드/릴리즈 워크플로우
├── components/
│   ├── config/
│   │   ├── include/config.h     # AppConfig 구조체 + validate()
│   │   ├── config.cpp
│   │   └── CMakeLists.txt
│   └── statemachine/
│       ├── include/statemachine.h  # StateMachine (feed/tick API)
│       ├── statemachine.cpp
│       └── CMakeLists.txt
├── frontend/
│   ├── index.html               # STA 메인 (Dashboard + Settings 탭)
│   └── setup.html               # SoftAP WiFi 프로비저닝
├── host_test/
│   ├── statemachine_test.cpp    # StateMachine GTest
│   └── config_test.cpp          # AppConfig GTest
├── main/
│   ├── main.cpp                 # app_main() 부팅 흐름
│   ├── bt_manager.h / .cpp      # BT 듀얼모드 presence + 페어링
│   ├── sm_task.h / .cpp         # StateMachine 태스크 (feed 큐 → tick → unlock 큐)
│   ├── control_task.h / .cpp    # GPIO 도어 제어 태스크
│   ├── http_server.h / .cpp     # HTTP + WebSocket 서버
│   ├── config_service.h / .cpp  # AppConfig get/set + NVS + mutex
│   ├── door_control.h / .cpp    # GPIO 펄스 제어
│   ├── nvs_config.h / .cpp      # NVS 읽기/쓰기 (WiFi, Auth)
│   └── wifi.h / .cpp            # STA/SoftAP 전환
├── scripts/
│   └── ota_upload.sh            # MAC 기반 OTA 업로드
├── docs/
│   ├── brainstorms/             # 설계 브레인스토밍
│   └── plans/                   # 구현 플랜
├── partitions.csv               # 8MB OTA 파티션 레이아웃
├── sdkconfig.defaults           # BTDM, PSRAM, 코어 분리 등
└── CMakeLists.txt
```
