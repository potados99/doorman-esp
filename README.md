# doorman-esp

ESP32 기반 사무실 도어락 자동 열림 장치입니다.
등록된 사용자의 접근을 Bluetooth 프레즌스로 감지하고, GPIO 펄스로 도어락 리모컨 입력을 대신 트리거합니다.

현재 결론은 `Classic-only`가 아니라 `BTDM dual-mode`입니다.

- iPhone: Classic BT generic accessory 경로는 기대하기 어렵습니다. BLE로 한 번 bond를 맺고, 이후 iPhone이 내보내는 advertising 주소를 IRK로 해석하는 경로가 주력입니다.
- Android / macOS: Classic Bluetooth SPP 페어링과 bonded BR/EDR 대상 directed probe가 더 유망합니다.
- ESP32: Wi-Fi + BLE + Classic 동시 사용을 전제로 `BTDM` 모드로 가져갑니다.

현재 리포지토리는 완성품이 아니라, 최종 제품을 향해 단계적으로 구현 중입니다.

- `components/statemachine/`: BT presence → 문열림 판단 상태머신 (순수 C++, 호스트 테스트)
- `components/config/`: AppConfig 설정 구조체 + validate() (순수 C++)
- `host_test/`: GTest 기반 호스트 테스트
- `main/`: ESP-IDF 의존 진입점과 하드웨어 경계
- `docs/`: 설계 브레인스토밍과 기능 플랜 문서

## 현재 상태

README는 미래 희망사항이 아니라 현재 기준으로 적습니다.

- `main/`: WiFi 프로비저닝 (SoftAP → STA), HTTP Basic Auth, 웹 문열기, OTA 업로드
- `main/bt_manager.cpp`: BTDM dual-mode BT 매니저
  - 수동 페어링 토글 (자동 시작 없음, 타이머 없음)
  - BLE bond 후 IRK 기반 RPA resolve
  - Classic SPP pairing + bonded BR/EDR remote-name probe
  - 본딩된 기기 삭제 API (`bt_remove_bond`)
- `main/sm_task.cpp`: StateMachine을 FreeRTOS 태스크로 래핑
  - BT feed 큐 수신 → tick() → Unlock 판정
  - SM은 항상 드라이런 판정하고, SM Task에서만 실제 전달 결정
  - 3중 억제 체계: 시작 유예기간(15초) / auto_unlock OFF / 페어링 중
- `main/control_task.cpp`: GPIO 도어 제어 태스크. AutoUnlock / ManualUnlock 큐 수신 → GPIO 펄스
- `main/config_service.cpp`: AppConfig 서비스. getConfig()/setConfig() + NVS 영속 + mutex 보호
- `main/http_server.cpp`: HTTP + WebSocket 서버. 멀티 WS 클라이언트 브로드캐스트(최대 5), 빌드 정보 표시
- `components/statemachine/`: StateMachine — feed(mac, seen, now_ms, rssi) + tick(now_ms) API
  - 윈도우 기반 진입 판단 (`enter_window_ms` 내 `enter_min_count` 이상 관측 시 재실 전환)
  - RSSI 임계값 필터링 (진입 판단에만 적용, 재실 유지에는 미적용)
  - per-device 관측 타임스탬프 원형 버퍼
  - auto_unlock_enabled 토글 지원
  - 호스트 GTest 28개 통과
- `components/config/`: AppConfig 구조체 (presence_timeout_ms, rssi_threshold, enter_window_ms, enter_min_count, auto_unlock_enabled) + validate()
- `frontend/`: index.html (STA 메인 대시보드) + setup.html (SoftAP 프로비저닝)
  - Dashboard: 문열기, 페어링 수동 토글, 본딩 기기 목록/삭제
  - Settings: Enter/Exit 그룹별 튜닝 UI, auto_unlock 토글, 계정, WiFi, OTA 업로드
  - 로그 뷰어: WebSocket 실시간 스트리밍, 태그 필터, 텍스트 필터, 전체화면, 자동 스크롤 일시정지(위로 스크롤 시)
- `.github/workflows/build.yml`: 태그 기반 GitHub Actions 워크플로우. 호스트 테스트 + ESP-IDF 빌드 후 GitHub Release 생성
- `scripts/ota_upload.sh`: MAC 기반 IP 탐색 + OTA 업로드 스크립트
- `partitions.csv`: 8MB flash 기준 OTA 파티션 레이아웃

개발 인프라(WiFi, OTA 업로드, 웹 제어), BT 매니저, 핵심 상태머신, 태스크 구조(SM/Control), 멀티 클라이언트 WebSocket 로그 스트리밍, 설정 튜닝 UI(Enter/Exit 그룹, auto_unlock 토글), 태그 기반 빌드/릴리즈 워크플로우가 확보되었습니다. 다음 단계는 GitHub Releases 기반 자동 OTA와 실환경 운영입니다.

## 하드웨어 가정

- 보드: ESP32-WROVER-IE 계열
- Flash: 8MB
- PSRAM: 8MB
- 도어 제어: GPIO -> 릴레이 -> 도어락 리모컨 입력 병렬 연결

현재 파티션 테이블은 8MB 기준으로 아래처럼 잡혀 있습니다.

| Partition | Offset | Size |
| --- | --- | --- |
| `nvs` | `0x9000` | `24KB` |
| `otadata` | `0xF000` | `8KB` |
| `phy_init` | `0x11000` | `4KB` |
| `ota_0` | `0x20000` | `3MB` |
| `ota_1` | `0x320000` | `3MB` |
| `coredump` | `0x620000` | `1MB` |
| `nvs_keys` | `0x720000` | `4KB` |

## 아키텍처 원칙

이 프로젝트는 `ESP-IDF를 쓰되, 앱 로직은 C++17로 작성`하는 방향으로 갑니다.

핵심 원칙은 단순합니다.

- ESP-IDF 경계는 얇고 담백하게 유지합니다.
- 상태머신, 정책, 캐시, 검증 로직은 C++로 작성합니다.
- 불필요한 추상화와 래퍼는 만들지 않습니다.
- 테스트 가능한 순수 로직은 `components/`로 분리합니다.
- 하드웨어 의존 코드는 `main/`에 둡니다.

한 줄로 요약하면 이렇습니다.

`하드웨어 경계는 C스럽게, 도메인 로직은 C++스럽게.`

## OTA 전략

펌웨어 업데이트는 한 가지 경로로 고정하지 않습니다.
이 프로젝트는 목적이 다른 두 경로를 함께 가집니다.

- `로컬 수동 OTA`: SoftAP + HTTP 업로드로 `.bin` 파일을 직접 올립니다. 주 용도는 로컬 개발, 초기 bring-up, 디버거 없이 빠른 반복 플래시입니다.
- `원격 자동 OTA`: GitHub Releases를 주기적으로 폴링해 새 버전이 있으면 자동으로 업데이트합니다. 주 용도는 배포 후 운영 편의입니다.

두 경로는 모두 같은 OTA 파티션 구조(`ota_0` / `ota_1`)를 사용합니다.
차이는 업데이트 트리거 방식이고, 플래시 쓰기와 부트 파티션 전환은 가능한 한 공통 흐름으로 가져갑니다.

현재 활성 플랜은 `로컬 수동 OTA`만 다룹니다.
GitHub 폴링 기반 자동 OTA는 그 다음 단계입니다.

## Bluetooth 전략

현재 bring-up 결과 기준 전략은 아래와 같습니다.

- iPhone은 generic Classic BT accessory를 Settings에서 잘 노출하지 않습니다.
- iPhone 대응은 `BLE pairing -> bond 저장 -> IRK 확보 -> 이후 advertising의 RPA resolve` 경로가 맞습니다.
- Android와 macOS는 Classic SPP pairing이 더 현실적이고, bonded 기기에 대해 BR/EDR directed probe를 계속 시도하는 방식이 유효합니다.
- 그래서 펌웨어는 `BLE + Classic`을 둘 다 켜는 `BTDM` dual-mode로 갑니다.

현재 동작은 다음과 같습니다.

- 페어링 모드 (수동 토글):
  - BLE는 Heart Rate/Battery/Device Information 기기로 advertising
  - Classic은 SPP 서버로 pairable/discoverable
- 일반 모드:
  - BLE는 continuous scan으로 iPhone advertising을 수신하고 IRK로 resolve
  - Classic은 bonded BR/EDR 주소에 `esp_bt_gap_read_remote_name()` probe

즉, 최종 presence 판단은 단일 무선기술에 올인하지 않고 플랫폼별로 다른 path를 사용합니다.

## Bring-up에서 확인한 것

- 이 프로젝트 기준으로 iPhone은 generic Classic BT accessory를 pairing 대상으로 기대하기 어렵습니다. iPhone presence는 BLE advertising 수신과 IRK 기반 RPA resolve 쪽이 주 경로입니다.
- macOS는 generic Classic BT 장치를 `Other Device` 등으로 더 잘 노출합니다.
- Android는 Classic SPP와 bonded MAC 기반 재접근 흐름이 공식 문서와도 더 잘 맞습니다.
- BLE pair 이후 장치 펌웨어는 bonded peer의 IRK와 identity address를 볼 수 있습니다. 앱 API가 숨기는 것과는 다른 층위입니다.
- ESP32는 `BTDM`으로 BLE와 Classic을 동시에 운용할 수 있으므로, iPhone용 BLE path와 Android/macOS용 Classic path를 한 펌웨어에서 같이 가져갈 수 있습니다.

## C++ 컨벤션

ESP-IDF는 C 중심 생태계지만, 앱 레벨 로직까지 C로 쓸 이유는 없습니다.
이 프로젝트에서는 아래 기준을 기본 규칙으로 삼습니다.

### 1. 어디까지 C++를 쓰는가

적극적으로 C++를 쓰는 영역:

- 상태머신
- 설정 구조체와 검증
- presence cache
- 순수 판단 로직
- 호스트 테스트 대상 코드

담백한 C 스타일로 두는 영역:

- `app_main()`
- FreeRTOS task entry 함수
- ESP-IDF event callback
- HTTP handler entry
- NVS, WiFi, BT, GPIO 같은 하드웨어/프레임워크 경계

### 2. 어떤 C++를 선호하는가

선호:

- `enum class`
- 작은 `struct`
- 명시적인 생성자 주입
- 값 타입 중심 설계
- `std::array`, `std::optional` 같은 가벼운 표준 라이브러리
- 클래스와 free function의 혼용

지양:

- 모든 것을 객체로 만드는 설계
- 얇은 IDF API까지 전부 래핑하는 계층
- 테스트를 위해 미리 인터페이스를 남발하는 것
- 상속/가상함수 중심 설계
- 예외나 RTTI에 기대는 설계
- 핫패스에서 과도한 동적 할당과 큰 문자열 조작

### 3. 클래스 사용 기준

클래스는 아래 중 하나라도 만족할 때만 씁니다.

- 내부 상태를 오래 유지해야 합니다
- 명확한 불변식이 있습니다
- 같은 데이터를 묶은 채 여러 메서드가 같이 움직여야 합니다

그 외에는 `struct + function` 또는 순수 함수가 더 낫습니다.

### 4. 테스트 전략

- ESP 의존 없는 로직은 `components/` 아래 순수 C++로 둡니다
- 이런 코드는 호스트에서 GTest로 검증합니다
- ESP-IDF 의존 로직은 `main/`에서 얇게 유지합니다

즉, 테스트 가능성은 `인터페이스 남발`이 아니라 `경계 분리`로 확보합니다.

## 레이어링 원칙

이 프로젝트는 무거운 엔터프라이즈식 3계층을 하지 않습니다.
대신 `handler/task -> service -> domain` 정도의 얇은 분리를 기본으로 봅니다.

- `components/`
  - 순수 도메인 객체
  - 상태머신
  - validator
  - value type
  - 호스트 테스트 가능한 로직

- `service`
  - 유스케이스 orchestration
  - 도메인 객체 조합
  - 필요하면 NVS, WiFi, OTA, GPIO 같은 infra 호출 조율

- `handler` / `task`
  - HTTP 요청 진입점
  - FreeRTOS task entry
  - 이벤트 수신과 주기 실행
  - 가능하면 얇게 유지

핵심 원칙은 이렇습니다.

- 도메인은 항상 분리합니다
- 서비스는 `오케스트레이션이 생길 때` 분리합니다
- 태스크나 핸들러가 아직 얇으면 서비스 없이 도메인을 직접 써도 됩니다
- 얇은 forwarding만 하는 서비스는 만들지 않습니다

예를 들어:

- HTTP API는 보통 `handler -> service -> domain/infra`
- BT scan loop는 처음엔 `task -> domain`으로 시작 가능합니다
- `scan -> feed -> action`이 커지면 `task -> service -> domain`으로 올립니다

즉, 서비스는 무조건 한 겹 더 끼우는 장식이 아니라, `유스케이스를 묶는 계층`입니다.

## 디렉터리 원칙

현재 구조:

```text
.github/
  workflows/
    build.yml
components/
  statemachine/
    include/statemachine.h
    statemachine.cpp
    CMakeLists.txt
  config/
    include/config.h
    config.cpp
    CMakeLists.txt
host_test/
  statemachine_test.cpp
  config_test.cpp
frontend/
  index.html
  setup.html
main/
  main.cpp
  wifi.h / wifi.cpp
  http_server.h / http_server.cpp
  bt_manager.h / bt_manager.cpp
  sm_task.h / sm_task.cpp
  control_task.h / control_task.cpp
  config_service.h / config_service.cpp
  door_control.h / door_control.cpp
  nvs_config.h / nvs_config.cpp
  idf_component.yml
  CMakeLists.txt
scripts/
  ota_upload.sh
docs/
  brainstorms/
  plans/
sdkconfig.defaults
partitions.csv
```

앞으로도 기본 방향은 크게 바꾸지 않습니다.

- `components/`: 호스트 테스트 가능한 순수 로직
- `main/`: handler, task, service, infra 같은 ESP-IDF 의존 코드
- `host_test/`: 컴포넌트 단위 테스트
- `frontend/`: 임베드될 웹 정적 파일
- `docs/`: 구현보다 앞서 합의가 필요한 설계/플랜 문서

## 가까운 구현 우선순위

현재 기준 우선순위는 아래와 같습니다.

1. ~~SoftAP + OTA~~ ✅
2. ~~BT presence PoC~~ ✅
3. ~~WiFi 프로비저닝 + 웹 제어 + GPIO~~ ✅
4. ~~AppConfig + StateMachine 상태머신~~ ✅
5. ~~bt_presence_poc → bt_manager 프로덕션화 + StateMachine 연결~~ ✅
6. ~~웹 UI 탭 분리 + WebSocket 로그 스트리밍~~ ✅
7. ~~태그 기반 GitHub Actions 호스트 테스트 + 빌드/릴리즈~~ ✅
8. ~~설정 UI 확장 (Enter/Exit 그룹별 튜닝, auto_unlock 토글, 빌드 정보 표시)~~ ✅
9. GitHub Releases 폴링 기반 자동 OTA
10. 통합 테스트 + 실사무실 배포 + 1달 운영

SoftAP + OTA는 운영용 기능이 아니라 `개발 편의용`입니다.
초회만 시리얼/JTAG로 올리고, 이후 반복 개발은 웹 업로드로 돌리는 흐름을 의도합니다.

## 빌드와 테스트

### ESP-IDF 빌드

환경 준비가 끝난 상태라면 일반적인 IDF 흐름으로 빌드합니다.

```bash
idf.py build
idf.py flash
idf.py monitor
```

### 호스트 테스트

호스트 테스트는 루트 `CMakeLists.txt`의 `HOST_TEST` 경로를 사용합니다.

```bash
cmake -S . -B build-host -DHOST_TEST=ON
cmake --build build-host
ctest --test-dir build-host
```

## 문서

설계 문서는 `docs/` 아래에 있습니다.

- `docs/brainstorms/2026-03-25-doorman-architecture-brainstorm.md` — 초기 아키텍처 방향
- `docs/brainstorms/2026-03-27-mvp2-wifi-provisioning-web-brainstorm.md` — 2차 MVP
- `docs/brainstorms/2026-03-29-final-product-spec-brainstorm.md` — 최종 제품 스펙 (정본)
- `docs/plans/2026-03-25-feat-softap-ota-upload-plan.md` — SoftAP + OTA 업로드 플랜
- `docs/plans/2026-03-29-feat-appconfig-gatekeeper-statemachine-plan.md` — 3차 구현 플랜
- `.github/workflows/build.yml` — 태그 기반 빌드/릴리즈 워크플로우 (ESP-IDF 빌드)

브레인스토밍 문서는 방향을 잡는 데 쓰고, 실제 구현 전환은 플랜 문서를 기준으로 합니다.

## 하지 않을 것

이 프로젝트에서는 아래 같은 방향을 기본적으로 피합니다.

- Arduino 레이어 추가
- 이유 없는 대규모 추상화
- 미래를 위한 인터페이스 설계
- 문서와 코드의 상태 불일치
- 존재하지 않는 파일/구조를 README에 먼저 적어두는 것

README는 코드베이스의 현재와 기준을 설명하는 문서여야 합니다.
앞으로도 이 원칙대로 유지합니다.
