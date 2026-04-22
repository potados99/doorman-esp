---
title: "refactor: 모듈 레이아웃 정리 + feat: Slack 문열림 알림"
type: refactor
status: active
date: 2026-04-22
---

# 모듈 레이아웃 정리 + Slack 문열림 알림

## Overview

두 개의 연속된 작업을 하나의 플랜에 담는다. **PR은 둘로 분리**하되 설계 결정이 맞물려 있어 한 문서에서 추적한다.

**Phase 1 (refactor)** — `components/config`를 해체하고 도메인별 폴더 구조(스크리밍 아키텍처)로 재편한다. `AppConfig` 구조체를 소멸시키고, `*_service` 접미어를 걷어낸다. 기능 변경 0.

**Phase 2 (feat)** — Phase 1의 구조 위에 `main/slack/notifier.*`를 추가한다. **사용자가 HTTP로 문열림 API를 호출한 경우에만** Slack Incoming Webhook으로 비동기 알림을 쏜다 (웹 UI 버튼 클릭, 외부 `POST /api/door/open` 호출 포함). **BLE presence 기반 자동 열림(`AutoUnlock`)은 알림 대상이 아님** — 시스템이 주체인 이벤트는 보안 로그 가치가 없고, 사용자 알림 의도와 무관. 크레덴셜은 NVS 저장 + 웹 UI 입력.

**왜 한 문서인가?** Slack notifier를 먼저 짜면 낡은 `nvs_config.*` 관례를 따라가야 해서 리팩터 직후 다시 이사해야 함. 리팩터를 먼저 하면 notifier가 `main/slack/` 폴더에 자연스럽게 앉음. 두 작업의 설계 근거가 동일한 대화(`config` 해체, 도메인 응집, NVS 직접 접근 허용)에서 도출되어 분리하면 맥락이 파편화된다.

## Motivation

### 현재 구조의 문제
1. **"config"라는 이름 아래 성격이 다른 두 부류가 혼재한다.**
   - `components/config/` — 순수 타입 + validate (`AppConfig`, `DeviceConfig`)
   - `main/config_service.*` — AppConfig 런타임 캐시 (필드 1개)
   - `main/device_config_service.*` — DeviceConfig 런타임 캐시 (기기별 다중, hot path)
   - `main/nvs_config.*` — wifi/auth 크레덴셜 저장소 (캐시 없음)
   - → 이름이 같을 뿐 레이어도 기능도 전부 다름.
2. **`AppConfig`는 필드 1개짜리 허구 추상**이다. `auto_unlock_enabled` 토글 하나를 위해 구조체·service·mutex·NVS blob 레이어를 유지. YAGNI 위반.
3. **`main/`이 플랫(flat)하다.** 11개 파일이 도메인 구분 없이 한 디렉토리에 쌓여있음. 스크리밍 아키텍처 위반 — 폴더만 봐서는 이 기기가 뭘 하는지 보이지 않음.
4. **`components/config`가 statemachine에 강제 의존을 만든다.** statemachine.h는 `DeviceConfig`만 필요한데 `AppConfig`까지 함께 딸려옴.

### Slack 알림이 필요한 이유
누가 언제 문을 열었는지 실시간으로 추적하고 싶음. 현 WS 로그는 브라우저 열어둬야 보이고 휘발됨. Slack 채널 1개면 기록 + 실시간 알림 둘 다 해결.

## Technical Architecture

### 최종 디렉토리 레이아웃

```
components/
  device/                    ← 신설 (구 components/config에서 DeviceConfig만 이주)
    include/device.h         ← DeviceConfig 타입, kDeviceConfigVersion, validate_device_config() 선언
    device.cpp               ← validate 구현만. 순수. ESP-IDF/FreeRTOS 무의존.
    CMakeLists.txt           ← idf_component_register(SRCS "device.cpp" INCLUDE_DIRS "include")
  statemachine/              ← 변경: REQUIRES config → REQUIRES device
    include/statemachine.h   ← #include "device.h" 로 변경
    statemachine.cpp

main/
  main.cpp                   ← 초기화 순서에 slack_notifier_init() 추가 (wifi_start 이후)

  door/
    control.cpp/h            ← 구 main/door_control.* (이름 축약: _control 중복 제거)
    control_task.cpp/h       ← 구 main/control_task.* (그대로)
    auto_unlock.cpp/h        ← 구 main/config_service.*. AppConfig 소멸, bool 토글 전용 API.

  bt/
    manager.cpp/h            ← 구 main/bt_manager.*
    sm_task.cpp/h            ← 구 main/sm_task.*

  wifi/
    wifi.cpp/h               ← 구 main/wifi.* + nvs_config의 wifi 부분 흡수
                                (nvs_save_wifi / nvs_load_wifi 가 여기로)

  http/
    server.cpp/h             ← 구 main/http_server.*

  auth/
    auth.cpp/h               ← 구 nvs_config의 auth 부분. check_auth()도 여기로?
                                (현재 http_server.cpp 내부 함수 — 이주는 선택)

  device/
    device_config.cpp/h      ← 구 main/device_config_service.*
                                (service 접미어 제거 — ESP-IDF엔 Spring 같은 규약 없음)

  monitor/
    monitor_task.cpp/h       ← 구 main/monitor_task.*

  slack/                     ← 신설 (Phase 2)
    notifier.cpp/h           ← webhook URL NVS I/O + notifier task + 큐 + HTTPS POST

host_test/
  device_test.cpp            ← 구 config_test.cpp (이름만 변경)
  statemachine_test.cpp      ← #include 경로만 device.h로
```

**삭제 대상:**
- `components/config/` (디렉토리 통째로)
- `main/config_service.cpp/h`
- `main/device_config_service.cpp/h` (이주)
- `main/nvs_config.cpp/h` (해체 후 wifi/auth/slack로 흡수)
- `main/door_control.cpp/h`, `main/bt_manager.cpp/h`, `main/wifi.cpp/h`, `main/http_server.cpp/h`, `main/sm_task.cpp/h`, `main/control_task.cpp/h`, `main/monitor_task.cpp/h` (전부 도메인 폴더로 이동)

### 데이터 흐름 (Phase 2 완료 후)

```
BT Manager ──SmMsg──▶ sm_task ──AutoUnlock──▶ control_task ──▶ door.control (GPIO)
                                                                (알림 없음)

HTTP /api/door/open ──▶ slack_notifier_send("🚪 문열림 요청")
                   └──▶ control_queue_send(ManualUnlock) ──▶ control_task ──▶ GPIO

HTTP /api/slack/update ──▶ slack_notifier_update_url() ──▶ NVS 저장 + 캐시 교체
```

**Slack 알림 발사 지점은 `main/http/server.cpp`의 `door_open_handler`** — `check_auth()` 통과 후, `control_queue_send()` 직전/직후 한 줄. 이유:
- 알림 목적은 "**누가 API를 쳤다**"는 보안 이벤트 기록. 시스템 자율 동작(BLE auto)은 제외.
- `control_task`에서 쏘면 manual/auto 브랜치 분기 필요 + `AutoUnlock`도 함께 흘러감 → 범위 위반.
- HTTP 핸들러에서 바로 쏘면 **인증된 API 호출 = 1 알림** 매핑이 간단·정확.
- `ControlCommand` enum 확장 필요 없음 (BLE vs Classic, manual/auto 구분 불필요).

**주의**: manual 요청이 `control_queue_send`로 들어간 뒤 pulse가 실제로 나가는지 여부는 **체크하지 않음**. 현재 `ManualUnlock`은 pairing gate/grace 체크 없이 무조건 `door_trigger_pulse()` 호출하므로 API 호출 = 실제 열림이 1:1. 이 전제가 깨지면(예: 향후 manual에도 gate 추가) 그때 재검토.

### Slack notifier 메모리 모델

```
notifier_task 상주 1개 (스택 8192B, 내부 RAM — PSRAM 금지 룰)
  └─ s_queue (depth 3, item = SlackMsg{ char *body_psram; size_t len; }, 8B * 3 = 24B)
     └─ producer: heap_caps_malloc(len+1, MALLOC_CAP_SPIRAM) → 큐 send
        │  실패 시: heap_caps_free (소유권 규칙: send 성공 시 소유권 이전)
        └─ consumer: HTTPS POST → heap_caps_free

s_webhook_url (static char *, PSRAM heap_caps_malloc으로 init 시 strdup)
s_webhook_mutex (URL 업데이트 vs 읽기 race 방지, init/update/read에서만 짧게)
```

**PSRAM 명시 할당 도입의 정당성:**
- `sdkconfig.defaults:54` `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024` — 1024B 이상만 자동 PSRAM
- Slack 메시지 body는 통상 수십~수백 바이트라 자동 PSRAM 경계 미만
- `heap_caps_malloc(MALLOC_CAP_SPIRAM)` 패턴을 **프로젝트 최초 도입** (현재 명시 사용처 0)
- 내부 RAM은 monitor_task 기준 **190KB 여유** 확보 상태라 강제는 아님. 안전 마진 확보용.

### sdkconfig 변경 (Phase 2)

| 항목 | 현재 | 변경 후 | 이유 |
|---|---|---|---|
| `CONFIG_MBEDTLS_DYNAMIC_BUFFER` | (미설정) | `=y` | SSL RX/TX 16KB 버퍼를 handshake 시에만 할당·해제. idle 메모리 수십 KB 회수. **필수.** |
| `CONFIG_MBEDTLS_DYNAMIC_FREE_PEER_CERT` | (미설정) | `=y` | handshake 후 peer cert heap 반환 |
| `CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA` | (미설정) | `=y` | handshake 후 config/CA chain heap 반환 |
| `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN` | `4096` | `2048` | Slack 요청 body 수백 바이트 → 2KB로 충분, 2KB 절감 |
| `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` | `16384` | (유지) | Slack 응답은 짧지만 TLS 프레임 크기는 서버가 정함. 보수적으로 유지. |
| `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL` | `=y` | (해제) | FULL은 flash 과다. `_CMN`으로 교체. |
| `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN` | (미설정) | `=y` | Mozilla top ~41개. DigiCert Global Root 포함 → `hooks.slack.com` 커버. flash 수백 KB 절감. |

**주의:** `sdkconfig.defaults`에는 현재 mbedtls 설정이 **전혀 없음**. 위 변경은 반드시 `sdkconfig.defaults`에도 명시해야 재생성 시 유실되지 않음.

## Implementation Phases

### Phase 1: 모듈 레이아웃 리팩터 (PR 1)

**목표:** 기능 변경 0. 컴파일·링크·host_test·실제 부팅 전부 기존과 동일 동작. 파일 위치·이름·include 경로만 이동.

#### 1-1. `components/device` 신설

- `components/device/include/device.h` 작성:
  ```cpp
  // AppConfig 구조체는 여기 없음 — 소멸 예정
  static constexpr uint8_t kDeviceConfigVersion = 1;
  struct DeviceConfig { ... };  // 기존 그대로
  static_assert(sizeof(DeviceConfig) == 48, ...);
  bool validate_device_config(const DeviceConfig &cfg);
  ```
- `components/device/device.cpp` — 기존 `components/config/config.cpp`에서 `validate_device_config` + UTF-8 헬퍼만 이주
- `components/device/CMakeLists.txt` — `idf_component_register(SRCS "device.cpp" INCLUDE_DIRS "include")`
- `components/config/` 디렉토리 완전 삭제

#### 1-2. `AppConfig` 소멸 & `door/auto_unlock` 분리

- `main/door/auto_unlock.h`:
  ```cpp
  void auto_unlock_init();               // NVS에서 bool 로드 + mutex 생성
  bool auto_unlock_is_enabled();         // 스냅샷 (nullptr 가드 필수)
  void auto_unlock_set(bool enabled);    // NVS 저장 + 캐시 갱신
  ```
- NVS namespace: 기존 `"door"` 그대로 유지 (마이그레이션 불필요, key `"auto"` 동일)
- 호출처 치환:
  - `main/http_server.cpp` 3곳 → `main/http/server.cpp`로 이동하면서 같이 수정
    - `AppConfig cfg = app_config_get(); ... cfg.auto_unlock_enabled` → `auto_unlock_is_enabled()`
  - `main/sm_task.cpp` 1곳 → `main/bt/sm_task.cpp` 이동 + 같은 치환
  - `main/main.cpp`에서 `config_service_init()` → `auto_unlock_init()`
- `main/config_service.cpp/h` 삭제

#### 1-3. `device_config_service` → `device/device_config` 이주

- `main/device/device_config.cpp/h` = 기존 `device_config_service.cpp/h` 내용 그대로
- 파일 상단 `#include "config.h"` → `#include "device.h"`
- 함수명 변경 없음 (`device_config_get`, `device_config_set`, …) — 이름은 이미 충분히 구체적
- NVS namespace `"dev"` 유지

#### 1-4. 나머지 도메인 폴더 이동

기계적 이동 + include 경로만 조정:

| 이전 | 이후 |
|---|---|
| `main/door_control.cpp/h` | `main/door/control.cpp/h` |
| `main/control_task.cpp/h` | `main/door/control_task.cpp/h` |
| `main/bt_manager.cpp/h` | `main/bt/manager.cpp/h` |
| `main/sm_task.cpp/h` | `main/bt/sm_task.cpp/h` |
| `main/wifi.cpp/h` | `main/wifi/wifi.cpp/h` |
| `main/http_server.cpp/h` | `main/http/server.cpp/h` |
| `main/monitor_task.cpp/h` | `main/monitor/monitor_task.cpp/h` |

**`nvs_config.*` 해체:**
- `nvs_load_wifi` / `nvs_save_wifi` → `main/wifi/wifi.cpp` 하단에 흡수 (wifi 도메인이 자기 크레덴셜 소유)
- `nvs_load_auth` / `nvs_save_auth` → `main/auth/auth.cpp` (신규 파일, 기존 `check_auth()`도 `http/server.cpp`에서 이주 검토 — 선택)
- `WifiConfig` / `AuthConfig` 타입은 각자 파일로 이동
- `main/nvs_config.cpp/h` 삭제

#### 1-5. `main/CMakeLists.txt` 재구성

```cmake
idf_component_register(
    SRCS
        "main.cpp"
        "door/control.cpp" "door/control_task.cpp" "door/auto_unlock.cpp"
        "bt/manager.cpp" "bt/sm_task.cpp"
        "wifi/wifi.cpp"
        "http/server.cpp"
        "auth/auth.cpp"
        "device/device_config.cpp"
        "monitor/monitor_task.cpp"
    INCLUDE_DIRS
        "." "door" "bt" "wifi" "http" "auth" "device" "monitor"
    REQUIRES
        app_update esp_driver_gpio esp_event esp_http_server esp_netif
        esp_ringbuf bt mbedtls esp_wifi nvs_flash espcoredump
        statemachine device                                    # ← config → device
    EMBED_TXTFILES
        "../frontend/index.html" "../frontend/setup.html"
)
```

**INCLUDE_DIRS에 각 서브폴더 포함**시키면 기존 `#include "wifi.h"` 같은 flat include도 그대로 작동 — 일부러 path-qualified include(`#include "wifi/wifi.h"`)로 바꿀지는 **별도 정책 결정**. 이 플랜에서는 일단 flat 유지(최소 변경).

#### 1-6. 루트 `CMakeLists.txt` host_test 섹션

`add_library(config …)` / `target_link_libraries(statemachine config)` → `device`로 교체. `add_executable(host_test_executable …)`에 `device_test.cpp` 추가.

#### 1-7. host_test 업데이트

- `host_test/config_test.cpp` → `host_test/device_test.cpp` (이름 변경)
- 내부 `#include "config.h"` → `#include "device.h"`
- `host_test/statemachine_test.cpp` include 경로만 조정

#### 1-8. 검증

- `idf.py build` 통과
- host_test(`cmake -DHOST_TEST=1 ...`) 통과
- 실기기 flash 후:
  - 부팅 정상 (`main.cpp` 초기화 순서 유지)
  - `/api/info`, `/api/stats`, `/api/auto-unlock/status`, `/api/devices` 응답 변경 없음
  - 문 열기 수동 1회, BLE auto 1회 각각 성공
  - monitor heap/task count 변화 없음 (± task 이름/수 동일)

### Phase 2: Slack Notifier (PR 2)

**목표:** 문이 **실제로 열린 직후** Slack 채널에 한 줄 알림.

#### 2-1. `sdkconfig.defaults` 업데이트 먼저

위 "sdkconfig 변경" 표대로 **정확히 sdkconfig.defaults에 추가** (sdkconfig만 바꾸면 재생성 시 유실). 이후 `idf.py reconfigure` 또는 sdkconfig 수동 동기화. 커밋 단위는 설정만 따로.

#### 2-2. `main/slack/notifier.h` API

```cpp
// 부팅 시 1회. wifi_start() 이후에 호출해야 URL 로드 시점과 맞음.
void slack_notifier_init();

// 비동기 전송 요청. 큐가 가득 차면 drop 후 WARN 로그.
// msg는 호출자 소유 (내부에서 복사해 PSRAM으로 이동).
// nullptr 가드 필수 — init 전 호출 무시.
void slack_notifier_send(const char *msg);

// 웹 UI에서 호출. NVS 저장 + 메모리 캐시 atomic swap. 재부팅 불필요.
// url == nullptr 또는 빈 문자열이면 알림 비활성화 (URL 해제).
esp_err_t slack_notifier_update_url(const char *url);

// 현재 URL 설정 여부만 반환 (UI 표시용, URL 문자열 노출하지 않음).
bool slack_notifier_is_configured();
```

#### 2-3. `main/slack/notifier.cpp` 구현 요점

- **상태**:
  ```cpp
  static QueueHandle_t s_queue = nullptr;          // 내부 RAM OK (24B)
  static TaskHandle_t  s_task  = nullptr;
  static char         *s_webhook_url = nullptr;    // PSRAM heap_caps_malloc
  static SemaphoreHandle_t s_url_mutex = nullptr;

  struct SlackMsg { char *body; size_t len; };     // body는 PSRAM heap (DMA 경로 아님 — 아래 R6 참조)
  ```
- **init 순서**: mutex 생성 → NVS에서 URL 로드 (PSRAM strdup) → 큐 생성 → 태스크 생성
- **태스크 생성 — 내부 RAM 스택 + 단일 코어 핀 필수**:
  ```cpp
  xTaskCreatePinnedToCore(
      notifier_task,
      "slack_notif",
      8192,               // 내부 RAM 스택. PSRAM 금지 룰 (2026-04-08 brick 사고).
      nullptr,
      4,                  // monitor(1)보다 높고 sm(5)보다 낮게
      &s_task,
      APP_CPU_NUM);       // tskNO_AFFINITY 금지 (psram-task-stack-bricks-device.md §3)
  ```
  - `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)` 사용 **절대 금지**.
  - mbedtls·lwIP가 이 태스크 컨텍스트에서 돌 것이고, 이들은 DMA·flash·IRAM ISR 경로에 닿음 → 스택이 PSRAM이면 재현 불규칙한 panic. 규칙은 "닿을 수 있으면 내부 RAM".
- **태스크**:
  ```cpp
  static void notifier_task(void *) {
      SlackMsg item;
      for (;;) {
          if (xQueueReceive(s_queue, &item, portMAX_DELAY) == pdTRUE) {
              post_to_slack(item.body, item.len);  // URL nullptr이면 내부에서 skip
              heap_caps_free(item.body);
          }
      }
  }
  ```
- **POST 로직** — `esp_http_client_config_t`:
  - `.url = s_webhook_url`
  - `.crt_bundle_attach = esp_crt_bundle_attach`
  - `.timeout_ms = 5000` (TLS handshake + Slack 응답 충분)
  - `.method = HTTP_METHOD_POST`
  - 매 호출 `esp_http_client_init → set_header Content-Type: application/json → set_post_field(json) → perform → cleanup` (keepalive 재사용 X — 호출 빈도 낮음)
  - URL 읽을 때 mutex 짧게 잡고 로컬 char 포인터 로컬 복사 (URL이 교체 중 free될 레이스 방지). 실제로는 mutex 안에서 **strdup으로 요청 직전 스냅샷** 떠서 POST 중엔 URL 해제에 영향 없게.
  - 응답은 확인만 (200 아니면 WARN 로그), body 파싱 안 함
- **JSON 직렬화**: `snprintf(buf, sizeof(buf), "{\"text\":\"%s\"}", escaped_msg)`. 메시지에 `"`, `\`, 제어문자 이스케이프 필요 — 짧은 helper 하나. cJSON 도입은 과함 (프로젝트 내 선례 없음, `per-device-config-ui` 플랜에서도 snprintf 선호).
- **메시지 최대 길이 256B** (producer가 `heap_caps_malloc(len+1, SPIRAM)`; 256 초과는 잘림)

#### 2-4. `main/slack/webhook_nvs.cpp` 내부 헬퍼

- NVS namespace `"slack"` (기존 `"net"`, `"auth"`, `"door"`, `"dev"` 와 충돌 없음)
- Key `"url"` — `nvs_set_str` / `nvs_get_str`
- URL 유효성: prefix `https://hooks.slack.com/services/` 체크 + 길이 32~256

#### 2-5. 호출 지점

`main/http/server.cpp`의 `door_open_handler` 한 곳에서만:
```cpp
static esp_err_t door_open_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    slack_notifier_send("🚪 문열림 요청");          // ← 추가 (인증 통과한 API 호출만)
    control_queue_send(ControlCommand::ManualUnlock);
    return send_text(req, "200 OK", "OK");
}
```

- `check_auth()` 통과 **이후**에 호출 → 미인증 요청은 알림 안 남김 (스팸 방지)
- `control_task`와 `bt/sm_task`는 **건드리지 않음** — `AutoUnlock` 경로는 알림 없음
- 메시지에 시각(HH:MM:SS)을 붙일지는 초안에선 생략 — `sntp` 필요. Slack 자체 타임스탬프로 충분.

**(선택 고민)** 요청자(IP, 세션) 구분을 메시지에 넣을지: 현재 basic auth라 사용자 하나뿐이고 IP는 내부망이라 의미 약함. 초안 스코프 밖.

#### 2-6. HTTP 엔드포인트

- `POST /api/slack/update` — body `url=<webhook>` urlencoded. prefix/길이 검증 후 `slack_notifier_update_url()`. `send_text("200 OK")` 후 **재부팅 안 함** (auth와 동일 패턴).
- `GET /api/slack/status` — `{"configured": true|false}` JSON. URL 값은 절대 노출 금지 (wifi pass 패턴과 동일).
- 두 핸들러 모두 `check_auth()` 통과 필수.

#### 2-7. 프론트엔드 설정 UI

`frontend/index.html` (또는 setup.html) 설정 섹션에:
- webhook URL 텍스트 입력 (현재 값 **비노출**, placeholder "https://hooks.slack.com/services/…")
- 저장 버튼 → `POST /api/slack/update`
- 상태 뱃지: `/api/slack/status` 조회 후 "설정됨" / "미설정"

#### 2-8. 검증 (⚠️ 순서 엄수 — OTA 방어선)

**첫 번째 플래시는 반드시 시리얼**. 2026-04-08 brick 사고의 교훈: TLS/mbedtls/lwIP 경로가 이 프로젝트 최초라 예측 못한 panic 시 OTA로는 복구 불가. 시리얼 검증 후에야 OTA 허용.

1. **시리얼 빌드 + 플래시** (OTA 아님)
2. 실기기에서 Slack 채널 준비 + webhook URL 발급
3. 웹 UI에서 URL 저장 → 상태 "설정됨" 표시
4. **단일 알림 smoke test**: 웹 UI 수동 문열기 1회 → Slack 채널 "🚪 문열림 요청" 5초 내 수신
5. **BLE 자동열기 시 알림 없음 확인** (의도적 제외 검증)
6. **Stack high water mark 실측** (문서 §"예방" 4번 항목):
   ```cpp
   // notifier_task 내부 또는 monitor_task에 추가
   UBaseType_t hwm = uxTaskGetStackHighWaterMark(s_task);
   ESP_LOGI(TAG, "slack_notif stack hwm=%u bytes free", hwm * sizeof(StackType_t));
   ```
   - 목표: 첫 handshake 이후 **2KB 이상 여유**. 2KB 미만이면 스택 12KB로 증설.
7. **Burst stress test**: `curl`로 `/api/door/open` 20회 연속 호출 (5초 간격 2회 + 동시 10회)
   - 큐 drop 로그 정상 (큐 depth 3 초과 시 WARN)
   - 시스템 panic 없음
   - PSRAM body DMA 경로 안전성 검증 (R6)
8. **내부 RAM dip 관찰 (monitor WS 로그)**:
   - POST 전/중/후 `ram=` 변화 기록
   - TLS handshake 피크가 내부 RAM에서 **< 30KB dip** 목표
   - `psram=` 변화로 handshake heap이 실제로 PSRAM에 떨어지는지 확인
9. **WiFi 끊긴 상태에서 문 여러 번 열기** → 큐 drop 로그 확인, 시스템 영향 없음
10. **잘못된 URL 저장 시도** → 400 반환, NVS 불변
11. **URL 비우기** → 알림 중단, 시스템 영향 없음
12. **30분 이상 idle + 간헐적 문열기 stress** 통과 후에만 **OTA 채널에 올림**

## Edge Cases & Error Handling

| 케이스 | 동작 | 비고 |
|---|---|---|
| Slack 미설정 상태에서 문 열림 | queue send → notifier가 URL nullptr 확인 후 조용히 drop | WARN 로그 1회 (첫 요청에만) |
| webhook URL 업데이트 중 동시 문 열림 | url mutex 아래 스냅샷 strdup 후 mutex 해제하고 POST | 이미 큐에 들어간 메시지는 예전 URL로도 OK (짧은 창) |
| WiFi 끊김 | esp_http_client가 DNS/connect 실패 → 5초 타임아웃 → 다음 메시지 처리 | 재시도 없음 (fire-and-forget) |
| Slack 다운 / 5xx | 에러 로그만, 재시도 X | 문열림 기록 1건 손실 허용 (이메일 없는 세상 가정) |
| 큐 포화 (3개) | 신규 send가 drop + WARN | door 동작엔 영향 없음, producer가 PSRAM heap 회수 |
| 큰 URL (길이 초과) | update_url에서 400 반환, NVS 안 건드림 | prefix + 길이 32~256 검증 |
| safe mode 부팅 | main.cpp에서 safe mode일 땐 notifier_init 건너뜀 | notifier_send는 nullptr 가드로 무시 |
| OTA 중 알림 | 큐 drop 허용 (OTA가 우선) | heap 경합 피하려면 OTA 핸들러 진입 시 queue 비우기? → 초안 X, YAGNI |
| 부팅 직후 wifi 연결 전 문 열림 | notifier 태스크는 떠 있으나 POST 실패 → 로그만 | monitor_task가 먼저 관찰 |
| notifier task 스택 오버플로우 | TLS handshake peak > 8KB 시 panic | `uxTaskGetStackHighWaterMark()` 초기 관찰 후 12KB까지 증설 가능. Phase 2 검증 항목 |
| webhook URL NVS 읽기 실패 (미설정 또는 손상) | `s_webhook_url = nullptr` 로 두고 그냥 시작 | init 실패해도 시스템은 살아있어야 함 |

## Acceptance Criteria

### Phase 1 (Functional — 변경 없음 확인)
- [ ] 빌드 통과 (`idf.py build`, warning 증가 0)
- [ ] host_test 통과 (validate_device_config + statemachine 전체)
- [ ] 실기기 부팅 정상 (safe mode 진입 X)
- [ ] `/api/info`, `/api/stats` 응답 byte-identical
- [ ] 수동 문열기 정상 동작
- [ ] BLE 자동 문열기 정상 동작
- [ ] `/api/auto-unlock/toggle` 후 재부팅 없이 다음 이벤트에 반영 (기존 동작 유지)
- [ ] 기기 등록/삭제 (`/api/devices/*`) 정상
- [ ] monitor task 출력 포맷 동일, task 수 동일

### Phase 1 (Technical)
- [ ] `components/config/` 디렉토리 삭제됨
- [ ] `main/config_service.*`, `main/device_config_service.*`, `main/nvs_config.*`, `main/door_control.*`, `main/bt_manager.*`, `main/wifi.*`, `main/http_server.*`, `main/sm_task.*`, `main/control_task.*`, `main/monitor_task.*` 전부 삭제됨
- [ ] 새 폴더(`main/door`, `main/bt`, `main/wifi`, `main/http`, `main/auth`, `main/device`, `main/monitor`) 존재
- [ ] `grep -r "AppConfig" main components host_test` 결과 0건
- [ ] `grep -r "#include \"config.h\"" main components host_test` 결과 0건
- [ ] `components/device/device.cpp` ESP-IDF 헤더 의존 0 (순수 C++)
- [ ] `main/door/auto_unlock.*` 의 `auto_unlock_is_enabled()` public API 첫 줄에 nullptr 가드

### Phase 2 (Functional)
- [ ] `POST /api/slack/update`가 유효한 webhook URL 저장
- [ ] `GET /api/slack/status`가 `{"configured": bool}` 반환 (URL 값 비노출)
- [ ] 웹 UI 문열기 버튼 클릭 시 Slack 채널에 "🚪 문열림 요청" 5초 이내 수신
- [ ] 외부에서 `curl -u ... POST /api/door/open` 호출 시도 알림 수신 (미인증은 알림 X)
- [ ] **BLE presence 기반 자동 열림 시 Slack 알림 없음** (AutoUnlock 경로는 무시)
- [ ] URL 업데이트가 재부팅 없이 다음 메시지에 반영됨
- [ ] URL 빈 문자열 저장 → 알림 비활성화됨 (POST 시도 X)
- [ ] WiFi 끊긴 상태에서도 문 열림 자체는 정상 (알림은 drop)

### Phase 2 (Technical)
- [ ] notifier_task는 `xTaskCreatePinnedToCore` (Caps 버전 **금지**), 단일 코어 핀 (`APP_CPU_NUM` 등), 스택 8192B **내부 RAM**
- [ ] notifier_task 코드 상단에 PSRAM stack 금지 근거 주석 + `docs/solutions/runtime-errors/psram-task-stack-bricks-device.md` 링크
- [ ] SlackMsg body는 `heap_caps_malloc(MALLOC_CAP_SPIRAM)` 로 할당 (소유권 이전 규칙 준수)
- [ ] `s_webhook_url`은 PSRAM
- [ ] `slack_notifier_send()`, `slack_notifier_is_configured()` 공개 API 첫 줄에 `if (s_queue == nullptr) return;` nullptr 가드
- [ ] sdkconfig.defaults에 mbedtls DYNAMIC_BUFFER 3개 + CMN bundle + OUT_CONTENT_LEN 변경 **명시**
- [ ] TLS handshake 중 내부 RAM dip을 monitor WS 로그로 측정 (< 30KB 목표)
- [ ] **notifier_task 스택 high water mark 로그**: 첫 handshake 후 2KB 이상 여유 확인, 초기 배포 동안 주기 로그
- [ ] Burst stress test (20회 연속 + 동시 10회) 통과 후에만 OTA 배포
- [ ] `/api/slack/*` 두 엔드포인트 `check_auth()` 통과 필요
- [ ] webhook URL 검증: prefix `https://hooks.slack.com/services/` + 길이 32~256
- [ ] **첫 번째 기기 반영은 시리얼 플래시만** (OTA 금지), stress test 후 OTA 해금

## Dependencies & Risks

### Dependencies
- Phase 2는 Phase 1 완료 후에만 진행 (merge 순서 강제)
- Slack 워크스페이스에 Incoming Webhook 앱 승인 필요 (워크스페이스 관리자)

**OTA와 certificate bundle은 무관함**: 현 OTA는 `scripts/ota_upload.sh:101`에서 확인되듯 `http://<host>/api/firmware/upload`로 **push** (ESP32가 HTTP 서버 역할). TLS는 리버스 프록시 ↔ curl 구간에서만 발생하고 ESP32는 plain HTTP로 수신. mbedtls bundle은 ESP32가 **outbound TLS 클라이언트**로 동작할 때만 쓰이는데, 이 프로젝트에서 outbound TLS는 **Slack이 최초** — 그러므로 bundle 변경이 영향 주는 경로는 `hooks.slack.com` 하나뿐. 다른 기능(OTA, WiFi, 웹 UI, BLE) 전부 영향 없음.

### Risks
- **R1**: Phase 1에서 include 경로 이동 중 오타로 빌드 실패 — 영향 낮음 (빌드가 즉시 잡음), 복구 비용 낮음.
- **R2**: Phase 1 리팩터 후 초기화 순서 미묘한 변경으로 부팅 실패 — `main.cpp` 순서 보존 재확인 + 실기기 테스트 필수.
- **R3**: Phase 2 TLS handshake 스택 peak가 8KB 초과 → notifier_task canary panic → safe mode — 2026-04-08 사고 재현 위험. `uxTaskGetStackHighWaterMark()` 첫 배포에 반드시 로그.
- **R4**: `heap_caps_malloc(MALLOC_CAP_SPIRAM)` 프로젝트 최초 도입 — PSRAM heap 단편화 가능성. 메시지 drop 시 명시적 `heap_caps_free` 누락되면 누수. 코드리뷰 초점.
- **R5**: webhook URL 유출 위험 — `/api/slack/status`에 URL 값 반환 금지, frontend에서도 저장 후 값 재표시 금지 (wifi pass 패턴). 로그에도 URL 찍지 않기 (prefix만).
- **R6 (신규, 문서 psram-task-stack-bricks-device.md에서 도출)**: **PSRAM body가 DMA 경로에 직접 진입할 위험**.
  - `esp_http_client_set_post_field(h, body, len)`이 body 포인터를 **그대로 TLS/소켓 레이어까지 들고 가는지** 확실치 않음 (일반적으로 mbedtls는 내부 SSL 버퍼로 복사해 암호화, lwIP는 내부 pbuf로 복사 → 실제 DMA 단계에선 PSRAM 주소가 사라짐. 하지만 ESP-IDF 버전별로 최적화 경로가 달라질 수 있음).
  - 검증 방법: 첫 배포에서 스트레스(문열기 burst 20회) 돌려보고 panic 없으면 OK. 불안하면 body를 `heap_caps_malloc(MALLOC_CAP_INTERNAL)` 또는 notifier_task의 스택 버퍼로 복사해서 전달하는 보수적 경로로 전환.
- **R7 (신규)**: **notifier가 TLS+mbedtls+lwIP 경로를 처음 도는 태스크**. 문서는 "시스템 핵심 태스크 변경은 시리얼 빌드 + stress 테스트 후에만 OTA" 권고. Slack notifier가 핵심은 아니지만 **TLS 경로가 이 프로젝트 최초 도입**이라 동일한 방어선 적용 — Phase 2의 **첫 번째 기기 반영은 반드시 시리얼 플래시**로, OTA는 30분 이상 stress test 통과 후에만.

## Out of Scope

- **BLE presence 기반 자동 열림(`AutoUnlock`) 알림** — 시스템 주체 이벤트라 보안 알림 가치 없음, 의도적 제외
- Slack chat.postMessage(봇 토큰 방식) — 지금 요구엔 과함
- 메시지 포맷팅 `blocks` 사용 — plain text로 충분
- 알림 재시도 / 큐 영속화 — fire-and-forget
- 요청자 정보(IP/세션/사용자) 메시지 포함 — basic auth 단일 사용자 + 내부망 → 의미 약함
- 문 닫힘 이벤트 알림 — 하드웨어가 문 상태를 모름 (문 열기만 트리거, 닫힘 감지 센서 없음)
- 복수 채널 / 복수 webhook — 지금 1개면 충분
- HTTP basic auth 자체 (현재 구조 유지, `main/auth/` 분리는 선택)

## Migration / Rollback

**Phase 1 롤백**: 파일 이동만이라 `git revert` 한 방. NVS 데이터 마이그레이션 없음 (namespace/key 모두 유지).
**Phase 2 롤백**: `git revert` + sdkconfig 원복. Slack 관련 NVS 키(`slack/url`)는 남지만 notifier가 없으면 아무 영향 없음 — 차기 부팅에서 새 URL 입력받거나 그대로 무시.

## References

### Internal (현재 코드)
- `components/config/include/config.h:12-47` — 해체 대상 `AppConfig`, `DeviceConfig`
- `main/config_service.*` — AppConfig 서비스 (소멸)
- `main/device_config_service.cpp:38-39` — `EXT_RAM_BSS_ATTR` PSRAM BSS 선례
- `main/nvs_config.cpp:10-69` — wifi/auth 저장 패턴 (도메인 폴더로 흡수)
- `main/http_server.cpp:324-335` — **Slack 알림 발사 지점** (door_open_handler, check_auth 통과 직후)
- `main/http_server.cpp:50-52` — `schedule_restart()` 패턴
- `main/http_server.cpp:955-971` — route table (slack 2개 추가 위치)
- `main/sm_task.cpp:166-189` — AutoUnlock 결정 흐름 (알림 대상 아님, 참고용)
- `main/control_task.cpp:18-38` — 알림 통합 대상 아님 (manual/auto 공통 통로, 건드리지 않음)
- `main/monitor_task.cpp` — heap 관찰 패턴 (알림 TLS peak 측정에 활용)
- `main/main.cpp:86-160` — 초기화 순서 (slack_notifier_init 삽입 지점)
- `CMakeLists.txt:37-61` — host_test 링크 (device 컴포넌트 추가)
- `main/CMakeLists.txt:1-32` — SRCS/INCLUDE_DIRS 전면 재구성
- `sdkconfig:3332-3395` — mbedtls 섹션
- `sdkconfig.defaults:48-65` — PSRAM 정책 주석
- `host_test/config_test.cpp` → `device_test.cpp` (rename)

### Institutional Learnings (docs/solutions + MEMORY.md + 과거 커밋)
- **`docs/solutions/runtime-errors/psram-task-stack-bricks-device.md`** — PSRAM task stack의 진짜 지뢰(DMA·캐시-disable·IRAM ISR·tskNO_AFFINITY). notifier_task 설계의 근거 문서. 이 플랜의 R6/R7/§2-8 검증 순서 전체가 이 문서에서 도출됨. (commit b1b80d4 → 312d85f revert)
- **`docs/solutions/runtime-errors/sm-task-stack-overflow-cascade.md`** — 태스크 스택은 burst peak 기준 60% 룰, public API nullptr 가드. notifier도 동일 원칙.
- **단일 파일 JS IIFE 관례** — 프론트엔드 설정 UI 추가 시 기존 IIFE 모듈 경계 유지.

### External
- [ESP-IDF esp_http_client](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/esp_http_client.html)
- [ESP-IDF mbedtls reduce memory](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/mbedtls.html) — DYNAMIC_BUFFER, DEFAULT_FREE_*
- [Slack Incoming Webhooks](https://api.slack.com/messaging/webhooks)
- [mbedtls memory footprint](https://mbed-tls.readthedocs.io/en/latest/kb/how-to/reduce-polarssl-memory-and-storage-footprint/)

### Prior Plans
- `docs/plans/2026-03-29-feat-appconfig-gatekeeper-statemachine-plan.md` — AppConfig 도입 경위 (이 플랜에서 소멸시킴)
- `docs/plans/2026-04-07-feat-per-device-config-card-ui-plan.md` — 스타일 참조, snprintf 선호 근거
