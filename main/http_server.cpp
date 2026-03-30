#include "http_server.h"
#include "bt_manager.h"
#include "config_service.h"
#include "control_task.h"
#include "door_control.h"
#include "nvs_config.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstring>

#include <esp_gap_ble_api.h>
#include <esp_gap_bt_api.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>

static const char *TAG = "httpd";

// ── Embedded HTML ──

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char setup_html_start[] asm("_binary_setup_html_start");

// ── Helpers ──

static std::atomic<bool> upload_in_progress{false};

static esp_err_t send_text(httpd_req_t *req, const char *status, const char *msg) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

static void delayed_restart(void *) {
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void schedule_restart() {
    xTaskCreate(delayed_restart, "restart", 2048, nullptr, 5, nullptr);
}

// ── Basic Auth ──

static bool check_auth(httpd_req_t *req) {
    char hdr[256] = {};
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK ||
        std::strncmp(hdr, "Basic ", 6) != 0) {
        goto unauthorized;
    }

    {
        unsigned char decoded[128];
        size_t decoded_len = 0;
        if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                                  reinterpret_cast<const unsigned char *>(hdr + 6),
                                  std::strlen(hdr + 6)) != 0) {
            goto unauthorized;
        }
        decoded[decoded_len] = 0;

        char *colon = std::strchr(reinterpret_cast<char *>(decoded), ':');
        if (!colon) {
            goto unauthorized;
        }
        *colon = '\0';

        const char *user = reinterpret_cast<const char *>(decoded);
        const char *pass = colon + 1;

        AuthConfig auth = nvs_load_auth();
        if (std::strcmp(user, auth.username) == 0 && std::strcmp(pass, auth.password) == 0) {
            return true;
        }
    }

unauthorized:
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Doorman\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Unauthorized");
    return false;
}

// URL decode in-place: %20 -> ' ', + -> ' '
static void url_decode(char *str) {
    char *src = str;
    char *dst = str;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], '\0'};
            *dst++ = static_cast<char>(strtol(hex, nullptr, 16));
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// Read POST body into a stack buffer. Returns length or -1.
static int read_body(httpd_req_t *req, char *buf, size_t buf_size) {
    if (req->content_len <= 0 || req->content_len >= static_cast<int>(buf_size)) {
        return -1;
    }

    int total = 0;
    while (total < req->content_len) {
        int received = httpd_req_recv(req, buf + total, req->content_len - total);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            return -1;
        }
        total += received;
    }

    buf[total] = '\0';
    return total;
}

// ── WebSocket 로그 스트리밍 ──

/**
 * Ring Buffer: esp_log → WS 브리지.
 *
 * custom_vprintf가 모든 로그를 시리얼 + Ring Buffer에 동시 출력한다.
 * WS sender 태스크가 Ring Buffer에서 읽어서 연결된 WS 클라이언트에 전송.
 * 8KB면 약 50~100줄의 로그를 버퍼링 가능.
 */
static RingbufHandle_t s_log_ringbuf = nullptr;
static vprintf_like_t s_original_vprintf = nullptr;
static httpd_handle_t s_server = nullptr;
static std::atomic<int> s_ws_fd{-1};  /* 현재 연결된 WS 클라이언트 fd. -1이면 미연결. */

/**
 * 커스텀 vprintf: 시리얼 출력 + Ring Buffer 복사.
 *
 * esp_log_set_vprintf()로 등록되어 ESP_LOGx 매크로 호출 시 실행된다.
 * Ring Buffer에 넣을 때 ISR 컨텍스트가 아니므로 xRingbufferSend 사용.
 * Ring Buffer가 가득 차면 조용히 드랍 (WS 클라이언트가 없거나 느릴 때).
 */
static int custom_vprintf(const char *fmt, va_list args) {
    /* Ring Buffer용 복사를 먼저 수행 (args 소비 전) */
    if (s_log_ringbuf != nullptr) {
        char buf[256];
        va_list args_copy;
        va_copy(args_copy, args);
        int len = vsnprintf(buf, sizeof(buf), fmt, args_copy);
        va_end(args_copy);

        if (len > 0) {
            size_t send_len = (len < static_cast<int>(sizeof(buf))) ? len : sizeof(buf) - 1;
            /* 0 timeout: 공간 없으면 즉시 드랍 */
            xRingbufferSend(s_log_ringbuf, buf, send_len, 0);
        }
    }

    /* 시리얼 출력 (원본 args 소비) */
    return s_original_vprintf(fmt, args);
}

/**
 * WS sender 태스크: Ring Buffer에서 로그를 읽어 WS 클라이언트에 전송.
 *
 * 100ms 주기로 Ring Buffer를 폴링하여 데이터가 있으면 WS frame으로 보낸다.
 * httpd_ws_send_frame_async()를 사용하여 httpd 태스크와 동기화.
 * WS 연결이 없으면 Ring Buffer 데이터를 소비만 하고 버린다.
 */
static void ws_sender_task(void *) {
    while (true) {
        size_t item_size = 0;
        void *item = xRingbufferReceive(s_log_ringbuf, &item_size, pdMS_TO_TICKS(100));

        if (item != nullptr && item_size > 0) {
            int fd = s_ws_fd.load();
            if (fd >= 0 && s_server != nullptr) {
                httpd_ws_frame_t pkt = {};
                pkt.type = HTTPD_WS_TYPE_TEXT;
                pkt.payload = static_cast<uint8_t *>(item);
                pkt.len = item_size;
                pkt.final = true;

                esp_err_t err = httpd_ws_send_frame_async(s_server, fd, &pkt);
                if (err != ESP_OK) {
                    /**
                     * 전송 실패 시 WS 연결이 끊어진 것으로 간주.
                     * 다음 WS 연결 시 s_ws_fd가 새로 설정된다.
                     */
                    s_ws_fd.store(-1);
                }
            }
            vRingbufferReturnItem(s_log_ringbuf, item);
        }
    }
}

// ── SoftAP Handlers ──

static esp_err_t setup_page_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, setup_html_start, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t wifi_setup_handler(httpd_req_t *req) {
    char body[256];
    if (read_body(req, body, sizeof(body)) < 0) {
        return send_text(req, "400 Bad Request", "Invalid request");
    }

    char ssid[33] = {};
    char pass[65] = {};
    if (httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) != ESP_OK || ssid[0] == '\0') {
        return send_text(req, "400 Bad Request", "SSID is required");
    }
    httpd_query_key_value(body, "pass", pass, sizeof(pass)); // password can be empty
    url_decode(ssid);
    url_decode(pass);

    nvs_save_wifi(ssid, pass);
    send_text(req, "200 OK", "OK");
    schedule_restart();
    return ESP_OK;
}

// ── STA Handlers ──

static esp_err_t index_page_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t door_open_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    /**
     * 이전: door_trigger_pulse() 직접 호출
     * 이후: Control Task 큐를 통해 간접 호출하여 SM Task 명령과 직렬화
     */
    control_queue_send(ControlCommand::ManualUnlock);
    return send_text(req, "200 OK", "OK");
}

static esp_err_t auth_update_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    char body[256];
    if (read_body(req, body, sizeof(body)) < 0) {
        return send_text(req, "400 Bad Request", "Invalid request");
    }

    char user[32] = {};
    char pass[64] = {};
    if (httpd_query_key_value(body, "user", user, sizeof(user)) != ESP_OK || user[0] == '\0') {
        return send_text(req, "400 Bad Request", "Username is required");
    }
    if (httpd_query_key_value(body, "pass", pass, sizeof(pass)) != ESP_OK || pass[0] == '\0') {
        return send_text(req, "400 Bad Request", "Password is required");
    }
    url_decode(user);
    url_decode(pass);

    nvs_save_auth(user, pass);
    return send_text(req, "200 OK", "OK");
}

static esp_err_t wifi_update_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    char body[256];
    if (read_body(req, body, sizeof(body)) < 0) {
        return send_text(req, "400 Bad Request", "Invalid request");
    }

    char ssid[33] = {};
    char pass[65] = {};
    if (httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) != ESP_OK || ssid[0] == '\0') {
        return send_text(req, "400 Bad Request", "SSID is required");
    }
    httpd_query_key_value(body, "pass", pass, sizeof(pass));
    url_decode(ssid);
    url_decode(pass);

    nvs_save_wifi(ssid, pass);
    send_text(req, "200 OK", "OK");
    schedule_restart();
    return ESP_OK;
}

static esp_err_t ota_upload_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    esp_err_t result = ESP_FAIL;
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = nullptr;
    bool ota_session_open = false;
    bool upload_claimed = false;
    int remaining = 0;
    const char *response_status = "500 Internal Server Error";
    const char *response_message = "Upload failed";

    bool expected = false;
    if (!upload_in_progress.compare_exchange_strong(expected, true)) {
        response_status = "409 Conflict";
        response_message = "Upload already in progress";
        goto cleanup;
    }
    upload_claimed = true;

    {
        char content_type[64] = {};
        if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
            std::strcmp(content_type, "application/octet-stream") != 0) {
            response_status = "415 Unsupported Media Type";
            response_message = "Expected application/octet-stream";
            goto cleanup;
        }
    }

    if (req->content_len <= 0) {
        response_status = "400 Bad Request";
        response_message = "No content";
        goto cleanup;
    }

    update_partition = esp_ota_get_next_update_partition(nullptr);
    if (!update_partition) {
        response_message = "No OTA partition";
        goto cleanup;
    }

    if (req->content_len > static_cast<int>(update_partition->size)) {
        response_status = "400 Bad Request";
        response_message = "Firmware too large";
        goto cleanup;
    }

    ESP_LOGI(TAG, "OTA target=%s size=%d", update_partition->label, req->content_len);

    result = esp_ota_begin(update_partition, req->content_len, &ota_handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(result));
        response_message = "OTA begin failed";
        goto cleanup;
    }
    ota_session_open = true;

    {
        char buf[4096];
        remaining = req->content_len;
        while (remaining > 0) {
            int to_read = std::min(remaining, static_cast<int>(sizeof(buf)));
            int received = httpd_req_recv(req, buf, to_read);
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            if (received <= 0) {
                ESP_LOGE(TAG, "recv failed: %d", received);
                response_message = "Receive failed";
                goto cleanup;
            }
            result = esp_ota_write(ota_handle, buf, received);
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(result));
                response_message = "Flash write failed";
                goto cleanup;
            }
            remaining -= received;
        }
    }

    result = esp_ota_end(ota_handle);
    ota_session_open = false;
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(result));
        response_status = "400 Bad Request";
        response_message = "Invalid firmware image";
        goto cleanup;
    }

    result = esp_ota_set_boot_partition(update_partition);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(result));
        response_message = "Set boot partition failed";
        goto cleanup;
    }

    upload_in_progress.store(false);
    upload_claimed = false;

    send_text(req, "200 OK", "OK");
    schedule_restart();
    return ESP_OK;

cleanup:
    if (ota_session_open) esp_ota_abort(ota_handle);
    if (upload_claimed) upload_in_progress.store(false);
    send_text(req, response_status, response_message);
    return result == ESP_OK ? ESP_FAIL : result;
}

/** 페어링 토글. 꺼져있으면 켜고, 켜져있으면 끈다. */
static esp_err_t pairing_toggle_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    if (bt_is_pairing()) {
        bt_stop_pairing();
        return send_text(req, "200 OK", "off");
    }
    bt_request_pairing();
    return send_text(req, "200 OK", "on");
}

/** 페어링 현재 상태 조회. */
static esp_err_t pairing_status_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    return send_text(req, "200 OK", bt_is_pairing() ? "on" : "off");
}

/** BT 자동 문열림 토글. 현재 상태를 반전시키고 NVS에 저장한다. */
static esp_err_t auto_unlock_toggle_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    AppConfig cfg = app_config_get();
    cfg.auto_unlock_enabled = !cfg.auto_unlock_enabled;
    app_config_set(cfg);

    const char *state = cfg.auto_unlock_enabled ? "enabled" : "disabled";
    ESP_LOGI(TAG, "Auto-unlock %s", state);
    return send_text(req, "200 OK", state);
}

/** BT 자동 문열림 현재 상태 조회. */
static esp_err_t auto_unlock_status_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    AppConfig cfg = app_config_get();
    return send_text(req, "200 OK", cfg.auto_unlock_enabled ? "enabled" : "disabled");
}

/** 튜닝 파라미터 조회: rssi_threshold, presence_timeout_ms, enter_window_ms, enter_min_count */
static esp_err_t tuning_get_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    AppConfig cfg = app_config_get();
    char buf[96];
    snprintf(buf, sizeof(buf), "%d,%lu,%lu,%lu",
             cfg.rssi_threshold,
             (unsigned long)cfg.presence_timeout_ms,
             (unsigned long)cfg.enter_window_ms,
             (unsigned long)cfg.enter_min_count);
    return send_text(req, "200 OK", buf);
}

/** 튜닝 파라미터 변경: rssi=N&timeout=N&enter_window=N&enter_count=N (form-urlencoded) */
static esp_err_t tuning_set_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    char body[256];
    if (read_body(req, body, sizeof(body)) < 0) {
        return send_text(req, "400 Bad Request", "Invalid request");
    }

    AppConfig cfg = app_config_get();

    char val[16] = {};
    if (httpd_query_key_value(body, "rssi", val, sizeof(val)) == ESP_OK) {
        cfg.rssi_threshold = (int8_t)atoi(val);
    }
    if (httpd_query_key_value(body, "timeout", val, sizeof(val)) == ESP_OK) {
        cfg.presence_timeout_ms = (uint32_t)atoi(val);
    }
    if (httpd_query_key_value(body, "enter_window", val, sizeof(val)) == ESP_OK) {
        cfg.enter_window_ms = (uint32_t)atoi(val);
    }
    if (httpd_query_key_value(body, "enter_count", val, sizeof(val)) == ESP_OK) {
        cfg.enter_min_count = (uint32_t)atoi(val);
    }

    if (!validate(cfg)) {
        return send_text(req, "400 Bad Request", "Invalid values");
    }

    app_config_set(cfg);

    char buf[96];
    snprintf(buf, sizeof(buf), "%d,%lu,%lu,%lu",
             cfg.rssi_threshold,
             (unsigned long)cfg.presence_timeout_ms,
             (unsigned long)cfg.enter_window_ms,
             (unsigned long)cfg.enter_min_count);
    ESP_LOGI(TAG, "Tuning updated: rssi=%d timeout=%lums enter_window=%lums enter_count=%lu",
             cfg.rssi_threshold,
             (unsigned long)cfg.presence_timeout_ms,
             (unsigned long)cfg.enter_window_ms,
             (unsigned long)cfg.enter_min_count);
    return send_text(req, "200 OK", buf);
}

/** 본딩된 기기 목록 조회. BLE+Classic 양쪽에서 조회 후 중복 MAC 제거. */
static esp_err_t devices_list_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    /* MAC 주소 수집용 버퍼 (BLE 최대 15 + Classic 최대 15) */
    uint8_t macs[30][6] = {};
    int mac_count = 0;

    /* BLE 본딩 목록 조회 */
    {
        int dev_num = 15;
        esp_ble_bond_dev_t dev_list[15] = {};
        esp_err_t err = esp_ble_get_bond_device_list(&dev_num, dev_list);
        if (err == ESP_OK) {
            for (int i = 0; i < dev_num && mac_count < 30; ++i) {
                std::memcpy(macs[mac_count++], dev_list[i].bd_addr, 6);
            }
        }
    }

    /* Classic 본딩 목록 조회 */
    {
        int dev_num = 15;
        esp_bd_addr_t dev_list[15] = {};
        esp_err_t err = esp_bt_gap_get_bond_device_list(&dev_num, dev_list);
        if (err == ESP_OK) {
            for (int i = 0; i < dev_num && mac_count < 30; ++i) {
                /* 중복 MAC 제거 */
                bool dup = false;
                for (int j = 0; j < mac_count; ++j) {
                    if (std::memcmp(macs[j], dev_list[i], 6) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) {
                    std::memcpy(macs[mac_count++], dev_list[i], 6);
                }
            }
        }
    }

    /* 응답 생성: 줄바꿈 구분 MAC 목록 */
    httpd_resp_set_type(req, "text/plain");
    char line[20];
    for (int i = 0; i < mac_count; ++i) {
        snprintf(line, sizeof(line), "%02X:%02X:%02X:%02X:%02X:%02X\n",
                      macs[i][0], macs[i][1], macs[i][2],
                      macs[i][3], macs[i][4], macs[i][5]);
        httpd_resp_sendstr_chunk(req, line);
    }
    return httpd_resp_sendstr_chunk(req, nullptr);
}

/** 본딩된 기기 삭제. body: mac=AA:BB:CC:DD:EE:FF */
static esp_err_t devices_delete_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    char body[64];
    if (read_body(req, body, sizeof(body)) < 0) {
        return send_text(req, "400 Bad Request", "Invalid request");
    }

    char mac_str[24] = {};
    if (httpd_query_key_value(body, "mac", mac_str, sizeof(mac_str)) != ESP_OK || mac_str[0] == '\0') {
        return send_text(req, "400 Bad Request", "mac parameter is required");
    }
    url_decode(mac_str);

    /* MAC 문자열 파싱 (AA:BB:CC:DD:EE:FF) */
    uint8_t mac[6] = {};
    unsigned int m[6] = {};
    if (sscanf(mac_str, "%02X:%02X:%02X:%02X:%02X:%02X",
                    &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
        return send_text(req, "400 Bad Request", "Invalid MAC format");
    }
    for (int i = 0; i < 6; ++i) mac[i] = static_cast<uint8_t>(m[i]);

    bt_remove_bond(reinterpret_cast<const uint8_t(&)[6]>(*mac));
    return send_text(req, "200 OK", "OK");
}

/**
 * WebSocket 핸들러.
 *
 * 연결 시: s_ws_fd에 소켓 fd를 저장하여 ws_sender_task가 전송할 수 있게 한다.
 * 데이터 수신: 클라이언트 → 서버 메시지는 무시 (로그 스트리밍은 서버 → 클라이언트 단방향).
 * 연결 종료: httpd가 내부적으로 처리. s_ws_fd는 send 실패 시 -1로 리셋.
 */
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        /**
         * WS 핸드셰이크에서는 인증을 하지 않는다.
         * 브라우저의 new WebSocket()은 커스텀 헤더(Authorization)를 보낼 수 없기 때문.
         * 이미 index.html에 Basic Auth로 접속한 상태이고, WS는 로그 읽기 전용이므로 허용.
         */
        /* WebSocket 핸드셰이크 완료 시점 */
        s_ws_fd.store(httpd_req_to_sockfd(req));
        ESP_LOGI(TAG, "WebSocket client connected (fd=%d)", s_ws_fd.load());
        return ESP_OK;
    }

    /* 클라이언트에서 데이터가 오면 읽어서 버린다 (단방향 스트리밍) */
    httpd_ws_frame_t pkt = {};
    pkt.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    /* payload가 있다면 무시 */
    if (pkt.len > 0) {
        uint8_t *buf = static_cast<uint8_t *>(malloc(pkt.len));
        if (buf) {
            pkt.payload = buf;
            httpd_ws_recv_frame(req, &pkt, pkt.len);
            free(buf);
        }
    }
    return ESP_OK;
}

// ── Registration helpers ──

struct Route {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
    bool is_websocket;
};

static bool register_routes(httpd_handle_t server, const Route *routes, size_t count) {
    for (size_t i = 0; i < count; i++) {
        httpd_uri_t desc = {};
        desc.uri = routes[i].uri;
        desc.method = routes[i].method;
        desc.handler = routes[i].handler;
        desc.is_websocket = routes[i].is_websocket;
        if (httpd_register_uri_handler(server, &desc) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register %s", routes[i].uri);
            httpd_stop(server);
            return false;
        }
    }
    return true;
}

// ── Log streaming 초기화 ──

/**
 * Ring Buffer + WS sender 태스크를 생성하고 esp_log를 후킹한다.
 * STA 모드에서만 호출 — SoftAP에서는 로그 스트리밍 불필요.
 */
static void init_log_streaming() {
    s_log_ringbuf = xRingbufferCreate(8192, RINGBUF_TYPE_NOSPLIT);
    if (s_log_ringbuf == nullptr) {
        ESP_LOGE(TAG, "Failed to create log ring buffer");
        return;
    }

    s_original_vprintf = esp_log_set_vprintf(custom_vprintf);

    xTaskCreatePinnedToCore(
        ws_sender_task,
        "ws_log",
        3072,
        nullptr,
        3,  /* 로그 전송은 낮은 우선순위 */
        nullptr,
        tskNO_AFFINITY);

    ESP_LOGI(TAG, "WebSocket log streaming initialized");
}

// ── Public API ──

httpd_handle_t start_webserver(WifiMode mode) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.recv_wait_timeout = 30;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 20;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return nullptr;
    }

    bool ok = false;

    if (mode == WifiMode::SoftAP) {
        static const Route softap_routes[] = {
            {"/",               HTTP_GET,  setup_page_handler, false},
            {"/api/wifi/setup", HTTP_POST, wifi_setup_handler, false},
        };
        ok = register_routes(server, softap_routes, 2);
    } else {
        static const Route sta_routes[] = {
            {"/",                          HTTP_GET,  index_page_handler,          false},
            {"/api/door/open",             HTTP_POST, door_open_handler,           false},
            {"/api/firmware/upload",       HTTP_POST, ota_upload_handler,          false},
            {"/api/auth/update",           HTTP_POST, auth_update_handler,         false},
            {"/api/wifi/update",           HTTP_POST, wifi_update_handler,         false},
            {"/api/pairing/toggle",        HTTP_POST, pairing_toggle_handler,      false},
            {"/api/pairing/status",        HTTP_GET,  pairing_status_handler,      false},
            {"/api/auto-unlock/toggle",    HTTP_POST, auto_unlock_toggle_handler,  false},
            {"/api/auto-unlock/status",    HTTP_GET,  auto_unlock_status_handler,  false},
            {"/api/tuning",                HTTP_GET,  tuning_get_handler,          false},
            {"/api/tuning",                HTTP_POST, tuning_set_handler,          false},
            {"/api/devices",               HTTP_GET,  devices_list_handler,        false},
            {"/api/devices/delete",        HTTP_POST, devices_delete_handler,      false},
            {"/ws",                        HTTP_GET,  ws_handler,                  true},
        };
        ok = register_routes(server, sta_routes, 14);
        if (ok) {
            /* STA 모드에서만 로그 스트리밍 활성화 */
            s_server = server;
            init_log_streaming();
        }
    }

    if (!ok) return nullptr;

    ESP_LOGI(TAG, "HTTP server started (mode=%s, port=%d)",
             mode == WifiMode::SoftAP ? "SoftAP" : "STA", config.server_port);
    return server;
}
