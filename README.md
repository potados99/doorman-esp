# doorman-esp

ESP32 기반 사무실 도어락 자동 열림 장치입니다.
등록된 사용자의 Bluetooth 프레즌스를 감지하여 문을 자동으로 열고, 웹 UI로 기기를 관리합니다.

## 어떻게 동작하는지

Bluetooth 듀얼모드(BLE + Classic)로 본딩된 기기의 존재를 감지합니다.

- **iPhone**: BLE bond 후 확보한 IRK로 advertising의 RPA를 resolve합니다.
- **Android / macOS**: Classic SPP 페어링 후 bonded BR/EDR 주소에 remote-name probe를 보냅니다.

감지 이벤트는 StateMachine으로 전달됩니다. StateMachine은 윈도우 기반 진입 판단을 수행합니다. 설정된 시간 윈도우 내에 RSSI 조건을 충족하는 관측이 일정 횟수 이상 쌓이면 "재실"로 전환하고, Unlock 액션을 발생시킵니다. 이후 feed가 끊기면 타임아웃으로 "퇴실"로 전환됩니다.

판정 파라미터는 **기기별로 따로** 설정됩니다 (`DeviceConfig`). 같은 사람의 기기라도 카드별로 RSSI 임계값/윈도우/타임아웃을 조절할 수 있습니다.

SM Task는 StateMachine의 Unlock 판정을 받되, 3중 억제 체계로 실제 전달을 결정합니다.

1. **시작 유예기간** (30초): 재부팅 직후 기존 기기 flood 방지
2. **auto_unlock OFF**: 사용자가 명시적으로 꺼놓은 상태
3. **페어링 중**: 새 기기 bond 도중 문열림 방지

### 페어링 중 SM 가상 시계

페어링 윈도우가 열리면 BLE 스캔이 일시 중단됩니다. 스캔이 멈춘 동안 SM에 feed가 안 들어오면 모든 기기가 timeout으로 퇴실 판정될 위험이 있습니다. SM Task는 **가상 시계**(`virtual_now_ms`)를 유지하여 페어링 모드 동안 시간 진행을 멈춥니다. 페어링이 끝나면 그대로 이어서 흐릅니다 — SM 입장에서 페어링 30초는 "0초"처럼 보입니다.

### BLE RPA resolve 최적화

BLE 환경에서는 주변의 모든 기기(에어팟, 아이워치, BLE 마우스 등)가 advertising을 보내고, 매 광고마다 본딩된 기기인지 식별해야 합니다. iPhone은 RPA(Resolvable Private Address)를 사용하므로 식별에 IRK 기반 AES 연산이 필요합니다.

모든 광고에 대해 bonded peer 수만큼 AES resolve를 시도하면 Bluedroid 태스크가 밀려 등록된 기기의 감지가 느려집니다. 이를 2단계 캐시로 해결합니다.

1. **Positive 패스트패스** (`last_adv_addr`): 매칭 성공 시 현재 RPA를 기억. 이후 같은 RPA 광고는 memcmp로 즉시 매칭 (AES 0회).
2. **Negative 캐시** (32슬롯, 순환): AES resolve 실패한 주소를 기억. 같은 주소가 다시 오면 즉시 스킵 (AES 0회).

RPA는 ~15분마다 바뀌므로 캐시는 자연스럽게 갱신됩니다. 결과적으로 **첫 1회만 AES를 타고, 이후 ~15분간은 memcmp만으로 처리**됩니다.

스캔 slow path와 페어링 직후 identity 확보 경로(`auth_cmpl`)는 동일한 매칭 함수(`match_peer_by_addr_slow`)를 공유합니다 — "주소 → 본드된 peer 인덱스" 매핑이 한 곳에서만 정의됩니다.

### 페어링 직후 identity 확보

`ESP_GAP_BLE_AUTH_CMPL_EVT`의 `bd_addr`은 연결 순간의 주소(RPA일 수 있음)입니다. 페어링 모달과 NVS에 RPA를 그대로 박으면 이후 스캔 feed가 사용하는 identity 주소와 mismatch가 생깁니다. 그래서 콜백 진입 즉시 `refresh_ble_bond_cache()`로 IRK + static_addr를 읽고, identity로 변환한 뒤 로그/식별에 사용합니다.

이 콜백은 **첫 페어링과 재연결 둘 다에서 발사**됩니다 (`smp_proc_pairing_cmpl`이 저장된 디바이스 record에서 auth_mode를 복원하므로 비트로 구분 불가). 첫 페어링만 골라내기 위해 `bt_is_pairing()` 플래그(사용자가 명시적으로 페어링 모달을 연 상태)를 게이트로 사용합니다 — 이 게이트가 프론트엔드 페어링 모달 UX와도 정확히 일치합니다.

### 세이프 모드

연속 3회 이상 panic reset이 발생하면 세이프 모드로 진입합니다. WiFi와 HTTP 서버(OTA 포함)만 올리고 BT/SM/Control 태스크는 시작하지 않습니다.

- NVS `cfg/panic_cnt` 카운터로 연속 panic 횟수 추적
- 정상 부팅 후 60초 경과 시 카운터 자동 리셋
- 웹 UI에 세이프 모드 배너 표시, BT 관련 기능 비활성화
- 펌웨어 업데이트(OTA 가능) 또는 시리얼 강제 플래시로 복구

억제를 통과한 Unlock은 Control Task 큐로 전달되고, GPIO 4에 펄스를 보내 릴레이를 구동합니다.

## 웹 UI

STA 모드에서 `http://doorman.local`로 접속합니다. HTTP Basic Auth로 보호됩니다.

**메인 탭**:
- 문열기 버튼 (수동)
- Auto-Unlock 토글
- 본드 기기 카드 목록 (기기당 카드 1개, 실시간 RSSI/감지 상태/마지막 감지 시각 표시)
- 카드 클릭 시 모달: alias, RSSI 임계값, 진입 윈도우, 진입 최소 카운트, 퇴실 타임아웃 편집 + 기기 삭제

**관리 탭** (Danger Zone):
- 펌웨어 업데이트 (`.bin` 파일 OTA)
- 계정 변경 (사용자명/비밀번호)
- WiFi 변경 (SSID/비밀번호)
- 기기 재부팅

**페어링 모달**:
- 헤더의 + 버튼으로 연다. 페어링 모드 진입 → BLE advertising + Classic discoverable. 모달을 닫으면 자동으로 종료. 새 본딩이 완료되면 모달에 연결 알림.

**로그 오버레이**:
- 헤더의 클립보드 아이콘으로 연다. WebSocket 실시간 로그 스트리밍, 태그 필터, 텍스트 필터, 자동 스크롤 일시정지.

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
   . $IDF_PATH/export.sh
   idf.py flash
   ```
2. 기기가 `Doorman-Setup` SoftAP를 열고 `http://192.168.4.1`에서 WiFi 프로비저닝 페이지를 제공합니다.
3. SSID와 비밀번호를 입력하면 기기가 재부팅하여 STA 모드로 전환됩니다.
4. 이후 `http://doorman.local`(또는 할당된 IP)에서 웹 UI에 접속합니다. 기본 계정은 `admin` / `admin`입니다.

### 이후 펌웨어 업데이트

- **웹 UI**: 관리 탭 → 펌웨어 업데이트 → `.bin` 파일 업로드
- **스크립트**: `scripts/ota_upload.sh <target> <firmware.bin> <user> <pass>`
  - `<target>`은 MAC 주소(`c0:5d:89:df:1f:f0`), `host:port`, 또는 IP 모두 가능
  - MAC를 주면 mDNS/ARP로 IP를 탐색합니다

### 페어링

웹 UI 헤더의 **+ 기기** 버튼으로 페어링 모달을 엽니다. 페어링 모드에서는 BLE advertising + Classic discoverable 상태가 됩니다. 본딩 성공한 기기는 별도 승인 없이 자동 등록되고, 모달에 연결 알림이 표시됩니다. 모달을 닫으면 페어링 모드가 종료됩니다.

### 튜닝

기기 카드 클릭 시 열리는 모달에서 기기별로 조절합니다. 기본값:

| 파라미터 | 설명 | 기본값 |
|----------|------|--------|
| RSSI 임계값 | 진입 시 RSSI 하한 (dBm) | -75 |
| 진입 윈도우 | 진입 판단 윈도우 (ms) | 10000 |
| 진입 최소 카운트 | 윈도우 내 최소 관측 수 | 3 |
| 퇴실 타임아웃 | feed 없이 이 시간 경과 시 퇴실 (ms) | 40000 |

기기별로 다른 값을 사용할 수 있습니다 — 예를 들어 거리감 둔감한 기기에는 RSSI 임계를 -85로 낮추거나, 빠른 진입 인식이 필요한 기기는 최소 카운트를 2로 줄이는 식입니다.

## 개발

### 빌드

ESP-IDF v6.0 환경에서 빌드합니다.

```bash
. $IDF_PATH/export.sh
idf.py build
```

### 호스트 테스트

`components/config`와 `components/statemachine`은 ESP-IDF에 의존하지 않는 순수 C++이므로 호스트에서 GTest로 검증합니다.

```bash
cmake -S . -B build_host_test -DHOST_TEST=ON
cmake --build build_host_test
cd build_host_test && ./config_test && ./statemachine_test
```

### OTA 스크립트

```bash
scripts/ota_upload.sh <target> build/doorman.bin <user> <pass>
```

`<target>`은 MAC, `host:port`, 또는 IP. MAC을 주면 ARP/mDNS 탐색을 거칩니다.

### 버전

빌드 시 git revision count가 `PROJECT_VER`로 자동 주입됩니다. 웹 UI에 `v134 · Apr 8 2026 23:13:40` 형태로 빌드 버전과 컴파일 시각이 표시됩니다.

## 아키텍처 요약

FreeRTOS 태스크 5개 + httpd 1개로 구성됩니다.

- **BT Task** (`presence_task`): BLE/Classic 스캔. 매칭된 본드의 identity 주소로 SM Task feed 큐에 push.
- **SM Task** (`sm_task`): StateMachine 구동. tick + feed drain. Unlock 판정 시 Control Task 큐로 전달. 가상 시계로 페어링 중 시간 정지.
- **Control Task** (`control_task`): 큐로 받은 unlock 명령을 GPIO 펄스로 직렬화. 수동 unlock(HTTP)도 같은 큐를 거침.
- **Monitor Task** (`monitor_task`): 1초마다 heap/태스크 상태 로그.
- **HTTP Task** (`httpd`): REST API + WebSocket 로그 스트리밍 + OTA. 모달 저장/페어링/재부팅 등 사용자 명령 처리.

```
[BT Task] ──feed queue──▸ [SM Task] ──unlock queue──▸ [Control Task] → GPIO
[HTTP Task] ──────────────────────unlock queue──▸ [Control Task]
[Any Task] ──ESP_LOG──▸ Ring Buffer ──▸ [WS Handler] ──▸ Browser
```

`AppConfig`(`auto_unlock_enabled`)와 `DeviceConfig[]`(per-device tuning)는 별도 태스크가 아닌 mutex로 보호되는 in-memory 캐시입니다. NVS write는 사용자 액션(HTTP) 시점에만 발생하며, **호출자(httpd)가 직접 동기로 수행**합니다 — cache 보호 mutex는 짧게 잡고 NVS write는 mutex 밖에서 호출하여 sm_task의 feed 처리가 NVS I/O에 블록되지 않게 격리합니다.

### 메모리 관리

내부 RAM 압박 해소를 위해 다음 자원을 PSRAM으로 라우팅합니다.

- BT 컨트롤러 풀 (`CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST=y`)
- WiFi/lwIP 동적 할당 (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`)
- 1KB 이상 일반 alloc (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024`)
- `EXT_RAM_BSS_ATTR` 표시한 정적 캐시 (`s_snapshots`, `s_entries`)

**중요**: 핵심 태스크(`sm_task`/`control_task`/`monitor_task`/`bt_*`/`httpd_*`) 스택은 영구히 내부 RAM만 사용합니다. 과거 PSRAM 스택 시도가 device-bricked 사고를 유발했습니다 (`docs/solutions/runtime-errors/psram-task-stack-bricks-device.md` 참조).

### 슬롯 상한 단일 진실원

본드 슬롯 상한은 ESP-IDF의 `CONFIG_BT_SMP_MAX_BONDS`(BLE+Classic 합산)가 결정합니다. `kMaxBleBondedDevices`, `kMaxClassicBondedDevices`, `kMaxEntries`(device config 캐시)가 모두 이 값에서 유도되며, `kMaxTrackedDevices`는 host_test 빌드 호환성을 위해 literal로 두되 `static_assert`로 일치를 강제합니다.

## 프로젝트 구조

```
doorman-esp/
├── .github/workflows/
│   └── build.yml                # CI: 호스트 테스트 + ESP-IDF 빌드
├── components/
│   ├── config/
│   │   ├── include/config.h     # AppConfig + DeviceConfig + validate()
│   │   ├── config.cpp           # alias UTF-8 well-formed 검증 포함
│   │   └── CMakeLists.txt
│   └── statemachine/
│       ├── include/statemachine.h  # StateMachine (feed/tick API)
│       ├── statemachine.cpp
│       └── CMakeLists.txt
├── frontend/
│   ├── index.html               # STA 메인 (메인 + 관리 탭, 모달, 로그 오버레이)
│   └── setup.html               # SoftAP WiFi 프로비저닝
├── host_test/
│   ├── statemachine_test.cpp    # StateMachine GTest
│   └── config_test.cpp          # DeviceConfig validate GTest (alias 검증 포함)
├── main/
│   ├── main.cpp                 # app_main() 부팅 흐름 + safe mode 판정
│   ├── bt_manager.h / .cpp      # BT 듀얼모드 presence + 페어링
│   ├── sm_task.h / .cpp         # StateMachine 태스크 (가상 시계 포함)
│   ├── control_task.h / .cpp    # GPIO 도어 제어 태스크
│   ├── monitor_task.h / .cpp    # heap/task 상태 주기 로그
│   ├── http_server.h / .cpp     # HTTP + WebSocket 서버 + query_and_decode 헬퍼
│   ├── config_service.h / .cpp  # AppConfig get/set + NVS + mutex
│   ├── device_config_service.h / .cpp  # per-device config 캐시 + 동기 NVS
│   ├── door_control.h / .cpp    # GPIO 펄스 제어
│   ├── nvs_config.h / .cpp      # NVS 읽기/쓰기 (WiFi, Auth)
│   └── wifi.h / .cpp            # STA/SoftAP 전환
├── scripts/
│   └── ota_upload.sh            # MAC/host/IP 기반 OTA 업로드
├── docs/
│   ├── plans/                   # 구현 플랜
│   └── solutions/               # 사고 분석 + 안전 룰 (PSRAM 스택, sm 스택 overflow 등)
├── partitions.csv               # 8MB OTA 파티션 레이아웃
├── sdkconfig.defaults           # BTDM, PSRAM, 코어 분리 등
└── CMakeLists.txt
```
