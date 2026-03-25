# doorman-esp

ESP32 기반 사무실 도어락 자동 열림 장치.
Classic Bluetooth presence detection으로 등록된 사용자의 접근을 감지하고, GPIO 펄스로 도어락 리모컨 입력을 대신 트리거하는 것을 목표로 한다.

현재 리포지토리는 완성품이 아니라, 다음 방향으로 가기 위한 초기 골격이다.

- `components/gatekeeper/`: 호스트 테스트 가능한 순수 C++ 도메인 로직 시작점
- `host_test/`: GTest 기반 호스트 테스트
- `main/`: ESP-IDF 의존 진입점과 하드웨어 경계
- `docs/`: 설계 브레인스토밍과 기능 플랜 문서

## 현재 상태

지금 구현된 것은 많지 않다. README는 미래 희망사항이 아니라 현재 기준으로 적는다.

- `main/`: SoftAP + HTTP OTA 업로드 경로는 동작하는 최소 구현이 들어가 있음
- `frontend/index.html`: OTA 업로드용 단일 페이지 구현됨
- `components/gatekeeper/`: `Gatekeeper::feed()` 골격만 존재
- `host_test/gatekeeper_test.cpp`: 최소 GTest 스캐폴딩 존재
- `partitions.csv`: 8MB flash 기준 OTA 파티션 레이아웃 정의
- `docs/plans/2026-03-25-feat-softap-ota-upload-plan.md`: 현재 구현한 SoftAP + HTTP OTA의 기준 플랜

즉, 개발용 OTA 경로는 확보했고, 이제 핵심 앱 로직과 상태머신 쪽을 채워 넣는 단계다.

## 하드웨어 가정

- 보드: ESP32-WROVER-IE 계열
- Flash: 8MB
- PSRAM: 8MB
- 도어 제어: GPIO -> 릴레이 -> 도어락 리모컨 입력 병렬 연결

현재 파티션 테이블은 8MB 기준으로 아래처럼 잡혀 있다.

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

이 프로젝트는 `ESP-IDF를 쓰되, 앱 로직은 C++17로 작성`하는 방향으로 간다.

핵심 원칙은 단순하다.

- ESP-IDF 경계는 얇고 담백하게 유지한다.
- 상태머신, 정책, 캐시, 검증 로직은 C++로 작성한다.
- 불필요한 추상화와 래퍼는 만들지 않는다.
- 테스트 가능한 순수 로직은 `components/`로 분리한다.
- 하드웨어 의존 코드는 `main/`에 둔다.

한 줄로 요약하면 이렇다.

`하드웨어 경계는 C스럽게, 도메인 로직은 C++스럽게.`

## OTA 전략

펌웨어 업데이트는 한 가지 경로로 고정하지 않는다.
이 프로젝트는 목적이 다른 두 경로를 함께 가진다.

- `로컬 수동 OTA`: SoftAP + HTTP 업로드로 `.bin` 파일을 직접 올린다. 주 용도는 로컬 개발, 초기 bring-up, 디버거 없이 빠른 반복 플래시다.
- `원격 자동 OTA`: GitHub Releases를 주기적으로 폴링해 새 버전이 있으면 자동으로 업데이트한다. 주 용도는 배포 후 운영 편의다.

두 경로는 모두 같은 OTA 파티션 구조(`ota_0` / `ota_1`)를 사용한다.
차이는 업데이트 트리거 방식이고, 플래시 쓰기와 부트 파티션 전환은 가능한 한 공통 흐름으로 가져간다.

현재 활성 플랜은 `로컬 수동 OTA`만 다룬다.
GitHub 폴링 기반 자동 OTA는 그 다음 단계다.

## C++ 컨벤션

ESP-IDF는 C 중심 생태계지만, 앱 레벨 로직까지 C로 쓸 이유는 없다.
이 프로젝트에서는 아래 기준을 기본 규칙으로 삼는다.

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

클래스는 아래 중 하나라도 만족할 때만 쓴다.

- 내부 상태를 오래 유지해야 한다
- 명확한 불변식이 있다
- 같은 데이터를 묶은 채 여러 메서드가 같이 움직여야 한다

그 외에는 `struct + function` 또는 순수 함수가 더 낫다.

### 4. 테스트 전략

- ESP 의존 없는 로직은 `components/` 아래 순수 C++로 둔다
- 이런 코드는 호스트에서 GTest로 검증한다
- ESP-IDF 의존 로직은 `main/`에서 얇게 유지한다

즉, 테스트 가능성은 `인터페이스 남발`이 아니라 `경계 분리`로 확보한다.

## 레이어링 원칙

이 프로젝트는 무거운 엔터프라이즈식 3계층을 하지 않는다.
대신 `handler/task -> service -> domain` 정도의 얇은 분리를 기본으로 본다.

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

핵심 원칙은 이렇다.

- 도메인은 항상 분리한다
- 서비스는 `오케스트레이션이 생길 때` 분리한다
- 태스크나 핸들러가 아직 얇으면 서비스 없이 도메인을 직접 써도 된다
- 얇은 forwarding만 하는 서비스는 만들지 않는다

예를 들어:

- HTTP API는 보통 `handler -> service -> domain/infra`
- BT scan loop는 처음엔 `task -> domain`으로 시작 가능
- `scan -> feed -> action`이 커지면 `task -> service -> domain`으로 올린다

즉, 서비스는 무조건 한 겹 더 끼우는 장식이 아니라, `유스케이스를 묶는 계층`이다.

## 디렉터리 원칙

현재 구조:

```text
components/
  gatekeeper/
    include/gatekeeper.h
    gatekeeper.cpp
    CMakeLists.txt
host_test/
  gatekeeper_test.cpp
frontend/
  index.html
main/
  main.cpp
  CMakeLists.txt
  wifi.h
  wifi.cpp
  http_server.h
  http_server.cpp
docs/
  brainstorms/
  plans/
sdkconfig.defaults
partitions.csv
```

앞으로도 기본 방향은 크게 바꾸지 않는다.

- `components/`: 호스트 테스트 가능한 순수 로직
- `main/`: handler, task, service, infra 같은 ESP-IDF 의존 코드
- `host_test/`: 컴포넌트 단위 테스트
- `frontend/`: 임베드될 웹 정적 파일
- `docs/`: 구현보다 앞서 합의가 필요한 설계/플랜 문서

## 가까운 구현 우선순위

현재 기준 우선순위는 아래와 같다.

1. 개발용 SoftAP + HTTP OTA 업로드
2. GitHub Releases 폴링 기반 자동 OTA
3. Gatekeeper 상태머신 구체화
4. Bluetooth presence scan 연결
5. GPIO 도어 트리거 연결
6. 설정 저장과 웹 UI 확장

SoftAP + OTA는 운영용 기능이 아니라 `개발 편의용`이다.
초회만 시리얼/JTAG로 올리고, 이후 반복 개발은 웹 업로드로 돌리는 흐름을 의도한다.

## 빌드와 테스트

### ESP-IDF 빌드

환경 준비가 끝난 상태라면 일반적인 IDF 흐름으로 빌드한다.

```bash
idf.py build
idf.py flash
idf.py monitor
```

### 호스트 테스트

호스트 테스트는 루트 `CMakeLists.txt`의 `HOST_TEST` 경로를 사용한다.

```bash
cmake -S . -B build-host -DHOST_TEST=ON
cmake --build build-host
ctest --test-dir build-host
```

## 문서

현재 참고할 설계 문서는 아래 두 개다.

- `docs/brainstorms/2026-03-25-doorman-architecture-brainstorm.md`
- `docs/plans/2026-03-25-feat-softap-ota-upload-plan.md`

브레인스토밍 문서는 방향을 잡는 데 쓰고, 실제 구현 전환은 플랜 문서를 기준으로 한다.

## 하지 않을 것

이 프로젝트에서는 아래 같은 방향을 기본적으로 피한다.

- Arduino 레이어 추가
- 이유 없는 대규모 추상화
- 미래를 위한 인터페이스 설계
- 문서와 코드의 상태 불일치
- 존재하지 않는 파일/구조를 README에 먼저 적어두는 것

README는 코드베이스의 현재와 기준을 설명하는 문서여야 한다.
앞으로도 이 원칙대로 유지한다.
