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

- **PoC → 제품**: BT 듀얼모드 PoC와 웹 인프라(2차 MVP)가 검증됨. 이제 핵심 로직(Gatekeeper)을 연결하고 운영 품질로 올릴 단계.
- **단순함 우선**: 감지 정책은 "감지되면 즉시 열기"로 시작. RSSI 튜닝은 실데이터 이후.
- **사무실 전용**: 범용 제품이 아니므로 설치 가이드, 다국어, 자동 프로비저닝 같은 건 불필요.

-----

## 핵심 결정사항

### 1. BT Presence → Gatekeeper → 문열림 흐름

```
[BT Scan Loop]
  ├─ BLE: bonded peer의 IRK로 advertising RPA resolve → Gatekeeper.feed()
  └─ Classic: bonded BR/EDR 대상 remote-name probe → Gatekeeper.feed()

[Gatekeeper]
  feed(mac, detected/not_detected) → Unlock / NoOp
  ├─ 등록된 기기가 감지됨 + 쿨다운 아님 → Unlock
  └─ 그 외 → NoOp

[Door Control]
  Unlock 이벤트 → GPIO 4 HIGH 500ms → LOW
```

- bt_presence_poc.cpp의 스캔 결과를 Gatekeeper에 feed하는 연결 코드가 핵심 구현 대상.
- Gatekeeper는 `components/`에 순수 C++로 유지 (호스트 테스트 가능).

### 2. 문열림 정책

- **초기 정책**: 등록된 기기가 감지되면 즉시 열기. 단순하게 시작.
- **쿨다운**: 두 가지 조건이 모두 충족되어야 재트리거.
  1. 해당 기기가 한번 "미감지" 상태로 전환된 적이 있을 것
  2. 마지막 열림으로부터 최소 N분(설정 가능)이 경과했을 것
- **향후 튜닝**: RSSI 임계값, 감지 지속시간 등은 실사용 데이터를 보고 추가.

### 3. 페어링 플로우

- **전원 인가 후 30초**: 페어링 윈도우. BLE + Classic 동시 discoverable/pairable.
- **30초 이후**: 페어링 종료, 스캔 모드로 전환.
- 현재 bt_presence_poc.cpp의 페어링 윈도우 패턴(30초)을 계승하되, 코드는 bt_manager.cpp에서 전면 재작성.
- 별도 웹 UI 트리거 없음. 전원 재인가 = 페어링 기회.
- **본딩 = 등록**: 페어링 윈도우에서 본딩 성공한 기기는 별도 승인 없이 자동으로 등록된 기기가 된다. 사무실 전용이므로 이 수준으로 충분.
- **기기 삭제 = bond 제거**: 웹 UI에서 기기를 삭제하면 ESP-IDF BT 스택의 bond 정보도 함께 삭제해야 한다. 안 그러면 BT 레벨에서는 여전히 bonded인데 Gatekeeper는 모르는 상태가 됨.

### 4. 기기 관리 웹 UI

**탭/페이지 분리 구조**:

| 탭 | 기능 |
|----|------|
| 대시보드 | 문열기 버튼, 시스템 상태 요약 (연결 기기 수, 최근 이벤트) |
| 기기 관리 | 등록된 BT 기기 목록 + 각 기기 상태(근처/부재, 마지막 감지 시각) + 삭제 |
| 설정 | 계정 변경, WiFi 재설정, 펌웨어 업데이트(수동 OTA + 현재 버전 표시), 도어 설정(쿨다운 시간 등) |

- 프론트엔드는 Vanilla HTML + JS 유지. 바이너리 임베드.
- SPA처럼 탭 전환은 JS로 처리 (페이지 리로드 없이).
- 기기 상태 갱신: WebSocket으로 실시간 푸시. ESPHome 스타일 실시간 로그도 WS로 함께 스트리밍.

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

```
components/
  gatekeeper/        # 상태머신 + presence cache + 쿨다운 (순수 C++, 호스트 테스트)
  config/            # AppConfig 구조체 + validate() (순수 C++)

main/
  main.cpp           # 초기화 흐름
  wifi.cpp           # STA/SoftAP (현재 구현 유지)
  http_server.cpp    # 모드별 웹서버 + API
  bt_manager.cpp     # bt_presence_poc.cpp 발전 → 프로덕션 BT 매니저
  door_control.cpp   # GPIO 펄스 (현재 구현 유지)
  nvs_config.cpp     # NVS 로드/저장 (현재 + 확장)
  ota_updater.cpp    # GitHub Releases 폴링 + OTA

host_test/
  gatekeeper_test.cpp
  config_test.cpp

frontend/
  index.html         # STA 메인 (탭 기반)
  setup.html         # SoftAP 프로비저닝 (현재 유지)

scripts/
  ota_upload.sh      # 수동 OTA 스크립트 (현재 유지)
```

- `bt_presence_poc.cpp` → `bt_manager.cpp`로 전면 재작성. PoC에서 검증된 패턴을 기반으로 깨끗하게.
- `ota_updater.cpp` 신규. GitHub API 폴링 + OTA 다운로드.
- `components/config/` 신규. AppConfig 구조체 (Gatekeeper 설정 포함).

### 8. 완성 기준

1. 동료들 폰 등록 → 출근 시 자동 문열림 안정 동작
2. 웹에서 기기 목록/상태 확인 가능
3. GitHub Releases 자동 OTA 동작
4. **위 전부 + 1달 무장애 운영**

-----

## 구현 로드맵 (순서)

| 단계 | 내용 | 선행 조건 |
|------|------|-----------|
| 3차 | AppConfig + Gatekeeper 상태머신 구체화 + 호스트 테스트 | - |
| 4차 | bt_presence_poc → bt_manager 프로덕션화, Gatekeeper 연결 | 3차 |
| 5차 | 웹 UI 탭 분리 + 기기 목록/상태 API | 4차 |
| 6차 | GitHub Releases 자동 OTA | 독립 (병렬 가능) |
| 7차 | 통합 테스트 + 실사무실 배포 + 1달 운영 | 전체 |

-----

## 해결된 질문

- **사용자 범위?** → 우리 사무실 전용. 관리자는 나.
- **페어링 플로우?** → 전원 인가 후 30초 윈도우. 별도 UI 트리거 없음.
- **기기 관리 범위?** → 목록 + 삭제 + 상태(근처/부재, 마지막 감지).
- **문열림 정책?** → 감지 즉시. 나중에 RSSI 등 튜닝.
- **쿨다운?** → 미감지 전환 + 최소 시간 병행.
- **자동 OTA?** → potados99/doorman releases 폴링, firmware.bin, 정수 버전 비교.
- **웹 UI 구성?** → 탭 분리 (대시보드, 기기관리, 설정).
- **버전 포맷?** → 단일 정수 (v1, v2, v3).
- **완성 기준?** → 자동 문열림 + 웹 관리 + 자동 OTA + 1달 무장애 운영.
- **기기 상태 갱신?** → WebSocket. ESPHome 스타일 실시간 로그도 WS로 스트리밍.
- **OTA 중 BT?** → 그냥 병행. 코어 분리(BT=Core0, WiFi=Core1) 되어있으니 시도해보고 판단.
- **BT PoC → 프로덕션?** → 전면 재작성. PoC에서 배운 것을 바탕으로 깨끗하게.
- **BLE/Classic 융합?** → 하나만 감지되면 OK. 별도 통합 로직 불필요.

-----

## 다음 단계

→ 단계별 `/workflows:plan`으로 구현 플랜 작성 (3차부터)
