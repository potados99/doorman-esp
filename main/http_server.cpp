#include "http_server.h"
#include "bt_manager.h"
#include "control_task.h"
#include "door_control.h"
#include "nvs_config.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstring>

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

static bool upload_in_progress = false;

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
    int received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) {
        return -1;
    }
    buf[received] = '\0';
    return received;
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
            int fd = s_ws_fd;
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
                    s_ws_fd = -1;
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

    if (upload_in_progress) {
        response_status = "409 Conflict";
        response_message = "Upload already in progress";
        goto cleanup;
    }
    upload_in_progress = true;
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

    upload_in_progress = false;
    upload_claimed = false;

    send_text(req, "200 OK", "OK");
    schedule_restart();
    return ESP_OK;

cleanup:
    if (ota_session_open) esp_ota_abort(ota_handle);
    if (upload_claimed) upload_in_progress = false;
    send_text(req, response_status, response_message);
    return result == ESP_OK ? ESP_FAIL : result;
}

/** 페어링 시작 핸들러. 웹 UI에서 호출하여 30초 페어링 윈도우를 연다. */
static esp_err_t pairing_start_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    bt_request_pairing();
    return send_text(req, "200 OK", "Pairing started (30s window)");
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
    config.max_uri_handlers = 12;

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
            {"/",                      HTTP_GET,  index_page_handler,   false},
            {"/api/door/open",         HTTP_POST, door_open_handler,    false},
            {"/api/firmware/upload",   HTTP_POST, ota_upload_handler,   false},
            {"/api/auth/update",       HTTP_POST, auth_update_handler,  false},
            {"/api/wifi/update",       HTTP_POST, wifi_update_handler,  false},
            {"/api/pairing/start",     HTTP_POST, pairing_start_handler, false},
            {"/ws",                    HTTP_GET,  ws_handler,           true},
        };
        ok = register_routes(server, sta_routes, 7);

        /* STA 모드에서만 로그 스트리밍 활성화 */
        s_server = server;
        init_log_streaming();
    }

    if (!ok) return nullptr;

    ESP_LOGI(TAG, "HTTP server started (mode=%s, port=%d)",
             mode == WifiMode::SoftAP ? "SoftAP" : "STA", config.server_port);
    return server;
}
