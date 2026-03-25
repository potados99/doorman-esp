# ESP32 도어락 자동열림 시스템 — 아키텍처 브레인스토밍

**날짜**: 2026-03-25
**상태**: 확정

-----

## 무엇을 만드는가

ESP32-WROVER-IE 기반 사무실 도어락 자동열림 시스템.
등록된 사용자의 접근을 Bluetooth 프레즌스로 감지하여 도어락을 자동 개방한다.

핵심 요소:
- iPhone은 BLE advertising + IRK resolve로 presence 감지
- Android / macOS는 Classic SPP pairing + bonded BR/EDR directed probe로 presence 감지
- GPIO 펄스로 솔리티 도어락 J1 포트 제어
- 웹 UI로 페어링, 제어, 모니터링, OTA 지원
- SoftAP WiFi 프로비저닝
- OTA 2경로: 로컬 `.bin` 업로드 + GitHub Releases 폴링 자동 업데이트

-----

## 왜 이 접근인가

- **iPhone 대응**: generic Classic BT accessory 경로는 이 프로젝트 기준으로 실효성이 낮았다. 대신 BLE로 한 번 bond를 맺고, 이후 iPhone이 뿌리는 advertising 주소를 IRK로 resolve하는 경로가 현실적이다.
- **Android / macOS 대응**: Classic SPP pairing과 bonded BR/EDR 대상 directed probe가 여전히 유망하다.
- **ESP32 BTDM dual-mode**: ESP32는 BLE와 Classic을 동시에 운용할 수 있으므로, 플랫폼별로 다른 무선 경로를 한 펌웨어에 같이 올릴 수 있다.
- **ESP-IDF 네이티브**: Arduino 레이어 불필요. IDF 내장 기능(httpd, OTA, BT)으로 충분
- **웹 UI 바이너리 임베드**: SPIFFS/LittleFS 불필요, OTA 시 UI도 함께 업데이트
- **AppConfig 전역 설정 객체**: NVS 영속 + 런타임 휘발을 하나의 구조체로 단순화
- **OTA 2경로 병행**: 개발용 수동 업로드와 운영용 자동 업데이트 요구를 분리

-----

## 핵심 결정사항

### 1. 코드 구조: 컴포넌트 분리 + main 플랫

ESP 의존 없는 순수 로직은 `components/`로 분리하여 호스트 GTest 대상으로 삼는다.
하드웨어 의존 코드는 `main/`에 플랫하게 배치 (서브디렉토리 없이 시작, 필요 시 분리).

```
components/
  gatekeeper/          # 상태머신 + presence cache (호스트 테스트 가능)
    include/gatekeeper.h
    gatekeeper.cpp
    CMakeLists.txt
  config/              # AppConfig 구조체 + 기본값 + validate()
    include/config.h
    config.cpp
    CMakeLists.txt
main/
  main.cpp             # app_main, 초기화 흐름
  http_server.cpp      # HTTP 핸들러 + WebSocket
  bt_presence_poc.cpp  # BLE + Classic dual-mode bring-up / presence PoC
  door_control.cpp     # GPIO 펄스 출력
  wifi.cpp             # STA/SoftAP 전환
  ota.cpp              # OTA 처리
  nvs_config.cpp       # NVS 로드/저장 (ESP 의존 부분)
host_test/
  gatekeeper_test.cpp
  config_test.cpp
frontend/
  index.html
  app.js
```

**이유**: 호스트 테스트 가능성이 핵심. `feed()` 패턴 덕분에 IBluetoothScanner 같은 인터페이스 없이도 상태머신을 완전히 테스트 가능.

### 2. Gatekeeper 이름 및 API 유지

- 클래스명: `Gatekeeper` (문을 열지 말지 판단하는 역할을 잘 표현)
- API: `feed(mac, ScanResult) → Event` (외부에서 스캔 결과 주입, 액션 반환)
- Presence cache는 Gatekeeper 내부 private 멤버로 포함
- config는 생성자에서 `const AppConfig&`로 주입

```cpp
class Gatekeeper {
public:
    explicit Gatekeeper(const AppConfig& cfg);
    Event feed(const uint8_t (&mac)[6], ScanResult result);
private:
    const AppConfig& config_;
    State state_;
    // presence cache 등
};
```

### 3. AppConfig 컴포넌트

- `components/config/`: 구조체 정의 + 기본값 + validate() (순수 C++)
- `main/nvs_config.cpp`: NVS 로드/저장 로직 (ESP 의존)
- Gatekeeper 컴포넌트가 config 컴포넌트를 REQUIRES

### 4. 파티션 테이블: 현행 유지

현재 파티션 레이아웃(ota_0 + ota_1 + coredump + nvs_keys, ~7.13MB/8MB) 그대로 유지.
factory 파티션 없음, littlefs 없음. 웹 파일은 바이너리 임베드로 해결.

### 5. OTA는 2경로로 간다

- **로컬 수동 OTA**: SoftAP + HTTP 업로드로 브라우저에서 `.bin` 파일을 직접 올린다.
- **원격 자동 OTA**: GitHub Releases를 주기적으로 폴링해 새 릴리즈가 있으면 자동 업데이트한다.
- 두 경로 모두 동일한 OTA 파티션(`ota_0`, `ota_1`)을 사용한다.
- 현재 활성 플랜은 로컬 수동 OTA만 다룬다. GitHub 폴링 자동 OTA는 다음 단계 플랜으로 진행한다.

### 6. MVP 우선순위

**1단계 (최우선):**
- SoftAP WiFi 프로비저닝
- HTTP 서버
- OTA bin 업로드 (웹 UI에 업로드 버튼)

**2단계:**
- BLE + Classic dual-mode presence bring-up 안정화

**3단계:**
- GitHub Releases 폴링 기반 자동 OTA

**4단계:**
- BT presence와 문열림 핵심 로직 연결

**5단계:**
- 풀 웹 UI (페어링, 모니터링, 설정)

### 7. 프론트엔드

- Vanilla HTML + JS (2개 파일: index.html + app.js)
- Pico CSS는 CDN (사용자 브라우저에서 로드)
- 바이너리에 EMBED_FILES로 임베드
- 개발 시 localhost 라이브서버로 핫리로드, ESP API는 CORS 열어서 직접 호출

### 8. 보안: MVP에서는 무방비

HTTP API 인증은 후순위. 사무실 내부 네트워크 신뢰 기반으로 시작.

-----

## 해결된 질문

- **등록 기기 수 상한**: 제한 없이 시작. 순회 시간이 문제되면 그때 제한 추가.
- **iPhone presence 경로**: generic Classic BT accessory가 아니라 BLE bond + IRK + advertising RPA resolve 쪽으로 간다.
- **Android / macOS presence 경로**: Classic SPP pairing + bonded BR/EDR probe를 유지한다.
- **ESP32 무선 스택 방향**: BLE 또는 Classic 하나만 고르지 않고 `BTDM dual-mode`로 같이 운용한다.
- **BLE pair 이후 식별 정보**: 장치 펌웨어는 bonded peer의 IRK와 identity address를 얻을 수 있고, 이를 통해 iPhone advertising의 RPA를 해석할 수 있다.

## 미해결 질문 (실기기 테스트 필요)

1. **BLE detect 품질**: iPhone advertising 빈도와 실제 문 앞 감지 안정성이 충분한지?
2. **Classic probe 품질**: Android 기종별로 bonded BR/EDR probe 성공률과 응답 지연이 어떤지?
3. **융합 정책**: BLE와 Classic 결과를 Gatekeeper에서 어떻게 합성할지?
4. **RSSI 임계값**: 문 앞 vs 실내 구분에 어떤 값이 적절한지?
5. **솔리티 도어락 J1 포트 사양**: active-high 확인 필요, 풀다운 저항값 검증
6. **Door pulse duration**: 도어락이 인식하는 최소 펄스 폭

-----

## 참고: 새 설계 문서 vs 기존 README

| 항목 | 기존 README | 새 설계 (이 브레인스토밍 기준) |
|------|------------|--------------------------|
| 설계 기준 | 폐기 | 새 문서가 정본 |
| 코드 구조 | main/ 서브디렉토리 | components/ 분리 + main/ 플랫 |
| 인터페이스 | IBluetoothScanner 등 | 불필요 (feed() 패턴으로 해결) |
| Bluetooth | Classic-only 가정 | iPhone BLE + Android/macOS Classic의 BTDM dual-mode |
| OTA | 자동 폴링 단일 | 수동 업로드 + GitHub 폴링 자동 업데이트 |
| 웹 UI | 최소 (로그만) | 풀 기능 (4대 핵심) |
| 프로비저닝 | 미언급 | SoftAP + HTTP 폼 |
