---
title: "feat: SoftAP WiFi + HTTP OTA bin 업로드"
type: feat
status: active
date: 2026-03-25
---

# SoftAP WiFi + HTTP OTA bin 업로드

## Overview

ESP32에서 WiFi SoftAP를 띄우고, 웹 브라우저에서 펌웨어 .bin 파일을 업로드하여 OTA 업데이트할 수 있는 개발/테스트용 최소 구현.
초회만 시리얼/JTAG로 플래시한 뒤, 이후 반복 개발은 SoftAP + HTTP OTA로 빠르게 돌리는 것을 목표로 한다.

## Proposed Solution

1. ESP32가 SoftAP 모드로 WiFi AP를 생성 (SSID: `Doorman-Setup`, PW: `12345678`)
2. HTTP 서버가 포트 80에서 동작
3. `GET /` → 임베드된 HTML 페이지 (업로드 폼) 서빙
4. `POST /api/firmware/upload` → raw binary 수신 → OTA 파티션에 스트리밍 쓰기 → 재부팅
5. 프론트엔드: JavaScript `XMLHttpRequest`로 업로드 (진행률 표시, `application/octet-stream`)

## Prerequisites

사전 수정 사항 없음. `sdkconfig.defaults`의 8MB Flash 설정과 `partitions.csv`(ota_0 + ota_1, ~7.13MB)가 정합한다.

이 플랜은 개발/테스트 환경 전용이다. 부팅 후 SoftAP와 OTA 업로드 엔드포인트를 상시 노출하며, 인증/물리 버튼 게이트/캡티브 포털은 후순위로 둔다.

**파티션 레이아웃** (factory 없음, littlefs 없음):

| 파티션 | 오프셋 | 크기 |
|--------|--------|------|
| nvs | 0x9000 | 24KB |
| otadata | 0xF000 | 8KB |
| phy_init | 0x11000 | 4KB |
| ota_0 | 0x20000 | 3MB |
| ota_1 | 0x320000 | 3MB |
| coredump | 0x620000 | 1MB |
| nvs_keys | 0x720000 | 4KB |

- 시리얼 플래시 시 ota_0에 기록
- OTA 업데이트 시 `esp_ota_get_next_update_partition(NULL)` → ota_1 → ota_0 교대

## Technical Approach

### 파일 구조

```
main/
  main.cpp             # app_main: NVS 초기화 → SoftAP → HTTP 서버
  wifi.h / wifi.cpp    # WiFi SoftAP 초기화
  http_server.h        # start_webserver() 선언
  http_server.cpp      # GET /, POST /api/firmware/upload 핸들러
  CMakeLists.txt       # SRCS, REQUIRES, EMBED_TXTFILES 업데이트
frontend/
  index.html           # 업로드 폼 + JS 진행률 표시
```

### main/CMakeLists.txt

```cmake
idf_component_register(
    SRCS "main.cpp" "wifi.cpp" "http_server.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event esp_http_server app_update nvs_flash
    EMBED_TXTFILES "../frontend/index.html"
)
```

- `app_update`: `esp_ota_ops.h` 제공 컴포넌트 (이름 불일치 주의)
- `EMBED_TXTFILES`: null-terminated로 임베드. 심볼: `_binary_index_html_start` / `_binary_index_html_end`
- `esp_driver_uart` REQUIRES 제거 (UART echo 코드 삭제)

### main/main.cpp

```cpp
#include <nvs_flash.h>
#include <esp_log.h>
#include "wifi.h"
#include "http_server.h"

static const char *TAG = "main";

extern "C" void app_main(void) {
    // NVS 초기화 (WiFi 내부에서 필요)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Starting Doorman...");
    wifi_init_softap();
    start_webserver();
    ESP_LOGI(TAG, "Ready. Connect to 'Doorman-Setup' (pw: 12345678), visit http://192.168.4.1");
}
```

### main/wifi.h / wifi.cpp

```cpp
// wifi.h
#pragma once
void wifi_init_softap();
```

```cpp
// wifi.cpp
#include "wifi.h"
#include <cstring>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>

static const char *TAG = "wifi";

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        auto *event = static_cast<wifi_event_ap_staconnected_t *>(event_data);
        ESP_LOGI(TAG, "station " MACSTR " joined, AID=%d", MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        auto *event = static_cast<wifi_event_ap_stadisconnected_t *>(event_data);
        ESP_LOGI(TAG, "station " MACSTR " left, AID=%d", MAC2STR(event->mac), event->aid);
    }
}

void wifi_init_softap() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));

    wifi_config_t wifi_config = {};
    std::strcpy(reinterpret_cast<char *>(wifi_config.ap.ssid), "Doorman-Setup");
    wifi_config.ap.ssid_len = std::strlen("Doorman-Setup");
    std::strcpy(reinterpret_cast<char *>(wifi_config.ap.password), "12345678");
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));  // v6.0: WIFI_IF_AP
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started. SSID: Doorman-Setup, Password: 12345678");
}
```

**v6.0 주의**: `ESP_IF_WIFI_AP` 제거됨 → `WIFI_IF_AP` 사용.

### main/http_server.h / http_server.cpp

```cpp
// http_server.h
#pragma once
#include <esp_http_server.h>
httpd_handle_t start_webserver();
```

```cpp
// http_server.cpp
#include "http_server.h"
#include <algorithm>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

static const char *TAG = "httpd";

// EMBED_TXTFILES로 임베드된 HTML
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

// ── 동시 업로드 방지 ──
static bool upload_in_progress = false;

// ── GET / ──
static esp_err_t index_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

// ── POST /api/firmware/upload ──
static esp_err_t ota_upload_handler(httpd_req_t *req) {
    esp_err_t err = ESP_FAIL;
    esp_ota_handle_t ota_handle = 0;
    bool ota_session_open = false;
    int response_status = HTTPD_500_INTERNAL_SERVER_ERROR;
    const char *response_message = "Upload failed";

    // 동시 업로드 차단
    if (upload_in_progress) {
        httpd_resp_send_err(req, HTTPD_409_CONFLICT, "Upload already in progress");
        return ESP_FAIL;
    }
    upload_in_progress = true;

    // Content-Length 검증
    if (req->content_len <= 0) {
        response_status = HTTPD_400_BAD_REQUEST;
        response_message = "No content";
        goto cleanup;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(nullptr);
    if (!update_partition) {
        response_message = "No OTA partition";
        goto cleanup;
    }

    // 파티션 크기 초과 검증
    if (req->content_len > update_partition->size) {
        response_status = HTTPD_400_BAD_REQUEST;
        response_message = "Firmware too large";
        goto cleanup;
    }

    ESP_LOGI(TAG, "OTA: partition=%s, size=%d bytes", update_partition->label, req->content_len);

    // content_len을 전달하여 필요 영역만 erase (OTA_SIZE_UNKNOWN보다 빠름)
    err = esp_ota_begin(update_partition, req->content_len, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        response_message = "OTA begin failed";
        goto cleanup;
    }
    ota_session_open = true;

    // 4KB 버퍼로 스트리밍 수신 + 쓰기 (flash sector 정렬)
    char buf[4096];
    int remaining = req->content_len;

    while (remaining > 0) {
        const int received = httpd_req_recv(req, buf, std::min(remaining, static_cast<int>(sizeof(buf))));

        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;  // 타임아웃은 재시도
        }
        if (received <= 0) {
            ESP_LOGE(TAG, "httpd_req_recv failed: %d", received);
            response_message = "Receive failed";
            goto cleanup;
        }

        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            response_message = "Flash write failed";
            goto cleanup;
        }

        remaining -= received;
    }

    // 이미지 검증 + 부트 파티션 전환
    err = esp_ota_end(ota_handle);
    ota_session_open = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        response_status = HTTPD_400_BAD_REQUEST;
        response_message = "Invalid firmware image";
        goto cleanup;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        response_message = "Set boot partition failed";
        goto cleanup;
    }

    upload_in_progress = false;
    ESP_LOGI(TAG, "OTA success! Rebooting...");
    httpd_resp_sendstr(req, "OK");

    // 응답 전송 후 재부팅
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK;  // 도달하지 않음

cleanup:
    if (ota_session_open) {
        esp_ota_abort(ota_handle);
    }
    upload_in_progress = false;
    httpd_resp_send_err(req, response_status, response_message);
    return ESP_FAIL;
}

httpd_handle_t start_webserver() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;        // OTA 핸들러용 스택 확대 (기본 4096 부족)
    config.recv_wait_timeout = 30;   // 대용량 업로드 대비 타임아웃 확대
    config.lru_purge_enable = true;  // 소켓 부족 시 LRU 정리

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return nullptr;
    }

    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
    };
    httpd_register_uri_handler(server, &index_uri);

    const httpd_uri_t ota_uri = {
        .uri = "/api/firmware/upload",
        .method = HTTP_POST,
        .handler = ota_upload_handler,
    };
    httpd_register_uri_handler(server, &ota_uri);

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
    return server;
}
```

**핵심 설계 결정**:
- `application/octet-stream`으로 raw binary 수신 (multipart 파싱 불필요)
- `esp_ota_begin()`에 `req->content_len` 전달 → 필요 영역만 erase (속도 개선)
- 단일 `cleanup:` 경로로 에러 처리 통일 (`goto cleanup` 스타일)
- `esp_ota_abort()`는 `esp_ota_begin()` 성공 후 `esp_ota_end()` 이전 실패에서만 호출
- `upload_in_progress` 플래그 — 동시 업로드 차단 (HTTP 409)
- 4KB 버퍼 — flash sector 정렬, 8KB 스택 안에서 안전
- `recv_wait_timeout = 30` — SoftAP 경유 3MB 업로드에 충분한 여유

### frontend/index.html

```html
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Doorman - Firmware Update</title>
    <style>
        body { font-family: system-ui, sans-serif; max-width: 480px; margin: 40px auto; padding: 0 20px; }
        h1 { font-size: 1.4em; }
        button { background: #0066cc; color: #fff; border: none; padding: 10px 20px;
                 border-radius: 6px; cursor: pointer; font-size: 1em; }
        button:disabled { background: #999; cursor: not-allowed; }
        #status { margin-top: 12px; padding: 10px; border-radius: 6px; display: none; }
        .ok  { background: #d4edda; color: #155724; }
        .err { background: #f8d7da; color: #721c24; }
        .run { background: #cce5ff; color: #004085; }
        progress { width: 100%; height: 20px; margin-top: 10px; display: none; }
    </style>
</head>
<body>
    <h1>Doorman</h1>
    <p>Firmware Update</p>
    <input type="file" id="file" accept=".bin"><br><br>
    <button id="btn" onclick="upload()">Upload &amp; Reboot</button>
    <progress id="bar" value="0" max="100"></progress>
    <div id="status"></div>
<script>
function upload() {
    var f = document.getElementById('file').files[0];
    if (!f) return show('Select a .bin file first.', 'err');

    var btn = document.getElementById('btn');
    var bar = document.getElementById('bar');
    btn.disabled = true;
    btn.textContent = 'Uploading...';
    bar.style.display = 'block';
    show('Uploading ' + (f.size / 1024).toFixed(0) + ' KB...', 'run');

    var xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/firmware/upload');
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');

    xhr.upload.onprogress = function(e) {
        if (e.lengthComputable) bar.value = Math.round(e.loaded / e.total * 100);
    };
    xhr.onload = function() {
        if (xhr.status === 200) {
            show('Upload complete! Rebooting... Reconnect to WiFi in ~10s.', 'ok');
        } else {
            show('Failed: ' + xhr.responseText, 'err');
            btn.disabled = false;
            btn.textContent = 'Upload & Reboot';
        }
    };
    xhr.onerror = function() {
        show('Connection lost. Device may be rebooting.', 'err');
    };
    xhr.send(f);
}
function show(msg, cls) {
    var s = document.getElementById('status');
    s.textContent = msg;
    s.className = cls;
    s.style.display = 'block';
}
</script>
</body>
</html>
```

**UX 포인트**:
- `XMLHttpRequest.upload.onprogress`로 진행률 표시 (서버 추가 엔드포인트 불필요)
- 재부팅 안내 ("Reconnect to WiFi in ~10s")
- 에러 시 버튼 재활성화

### 호스트 테스트 빌드 업데이트 (CMakeLists.txt 루트)

main의 새 파일들(wifi.cpp, http_server.cpp)은 ESP-IDF API에 의존하므로 호스트 테스트 대상이 아니다.
기존 gatekeeper 호스트 테스트는 변경 없이 유지된다. 루트 CMakeLists.txt 수정 불필요.

## Acceptance Criteria

- [ ] `Doorman-Setup` WiFi AP가 뜨고, 폰/노트북에서 연결 가능
- [ ] `http://192.168.4.1`에서 업로드 폼이 표시됨
- [ ] .bin 파일 업로드 시 진행률 바가 동작
- [ ] 업로드 완료 후 ESP32가 새 펌웨어로 재부팅
- [ ] 재부팅 후 다시 SoftAP + 웹서버가 뜸 (반복 OTA 가능)
- [ ] 잘못된 파일 업로드 시 에러 메시지 표시, 기존 펌웨어 유지
- [ ] 동시 업로드 시도 시 두 번째 요청에 409 응답

## Implementation Phases

### Phase 1: 인프라 (sdkconfig + CMake)

1. `sdkconfig.defaults`와 문서의 flash 크기 표기를 8MB로 통일
2. 필요 시 `sdkconfig` 삭제 후 `idf.py reconfigure`
3. `frontend/` 디렉토리 생성 + `index.html` 작성
4. `main/CMakeLists.txt` 업데이트 (SRCS, REQUIRES, EMBED_TXTFILES)

### Phase 2: WiFi SoftAP

1. `main/wifi.h` + `main/wifi.cpp` 작성
2. `main/main.cpp`에서 UART echo 제거, `wifi_init_softap()` 호출
3. 빌드 + 플래시 → SoftAP 연결 확인

### Phase 3: HTTP 서버 + OTA

1. `main/http_server.h` + `main/http_server.cpp` 작성
2. `main/main.cpp`에 `start_webserver()` 호출 추가
3. 빌드 + 플래시 → 웹 페이지 접속 확인 → OTA 업로드 테스트

## Dependencies & Risks

| 리스크 | 영향 | 대응 |
|--------|------|------|
| Flash 쓰기 중 BT 간섭 | BT 컨트롤러 타이밍 영향 가능 | MVP에서 BT 미사용이므로 무관. 향후 OTA 중 BT 일시정지 추가 |
| Task watchdog 트리거 | OTA 루프가 길면 WDT 발동 | HTTP 서버 태스크는 기본 WDT 미등록. 문제 시 `config.task_caps` 조정 |
| EMBED_TXTFILES 심볼명 불일치 | 컴파일 에러 | `nm build/doorman.elf \| grep binary` 로 실제 심볼 확인 |
| SoftAP 대역폭 | 3MB 업로드 시 12-24초 소요 | 진행률 표시로 UX 커버 |

## 향후 개선 (이 플랜 범위 밖)

- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` + `esp_ota_mark_app_valid_cancel_rollback()` — OTA 롤백 안전망
- WiFi STA 모드 전환 (SoftAP 프로비저닝 → NVS 저장 → STA 연결)
- Captive portal (DNS 리다이렉트)
- GitHub Releases OTA
- HTTP API 인증
- 펌웨어 버전 표시 (`esp_app_get_description()`)

## References

- [ESP-IDF OTA API](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/ota.html)
- [ESP-IDF HTTP Server](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/esp_http_server.html)
- [ESP-IDF SoftAP 예제](https://github.com/espressif/esp-idf/tree/master/examples/wifi/getting_started/softAP)
- [Jeija/esp32-softap-ota](https://github.com/Jeija/esp32-softap-ota) — 최소 SoftAP OTA 레퍼런스
- [ESP-IDF v6.0 WiFi Breaking Changes](https://docs.espressif.com/projects/esp-idf/en/release-v6.0/esp32/migration-guides/release-6.x/6.0/wifi.html) — `WIFI_IF_AP` 사용 필수
- [EMBED_FILES 심볼 네이밍](https://github.com/espressif/esp-idf/issues/4927)
- 브레인스토밍: `docs/brainstorms/2026-03-25-doorman-architecture-brainstorm.md`
