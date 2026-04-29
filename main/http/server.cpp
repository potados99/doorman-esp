#include "http/server.h"
#include "auth/auth.h"
#include "bt/manager.h"
#include "bt/sm_task.h"
#include "device/device_config.h"
#include "door/auto_unlock.h"
#include "door/control.h"
#include "door/control_task.h"
#include "monitor/monitor_task.h"
#include "slack/notifier.h"
#include "wifi/wifi.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstring>

#include <esp_gap_ble_api.h>
#include <esp_gap_bt_api.h>
#include <esp_log.h>
#include <esp_core_dump.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <freertos/semphr.h>
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

// ── Audit log (sousveillance) ──
//
// 감시·관리 행위(🔍)와 인증 실패(⚠️)를 Slack으로 기록합니다. 철학: 서비스
// 이용(문열기)은 기록 안 하고, 들여다보려는 사람이 자신의 접근을 자동으로
// 남긴다. 상세는 docs/plans/2026-04-23-feat-audit-log-sousveillance-plan.md.
//
// IP/UA가 본질적 식별 정보라 Slack 메시지에 포함. username은 오타 노출
// 방지로 절대 포함 안 함. 이벤트 시각은 Slack 자체 타임스탬프에 의존
// (SNTP 미사용이라 epoch가 uptime 기반 → 본문에 박으면 1970-01-01 렌더).

/**
 * 프록시 뒤에서 클라이언트 IP를 추출합니다.
 *
 * 1순위 X-Real-IP: 리버스 프록시가 trusted proxy 모드에서 항상 덮어쓰므로
 * 외부 HTTPS 경로에선 스푸핑 면역. 2순위 XFF 마지막 엔트리: append 체인의
 * 첫 엔트리는 공격자 조작 가능, 마지막은 리버스 프록시가 관찰한 실제 remote에
 * 가까움. socket peer 조회는 리버스 프록시 loopback으로 고정되어 무가치라 생략.
 */
static void get_client_ip(httpd_req_t *req, char *out, size_t out_size) {
    if (out_size == 0) {
        return;
    }
    out[0] = '\0';

    if (httpd_req_get_hdr_value_str(req, "X-Real-IP", out, out_size) == ESP_OK
        && out[0] != '\0') {
        return;
    }

    char xff[128];
    if (httpd_req_get_hdr_value_str(req, "X-Forwarded-For", xff, sizeof(xff)) == ESP_OK) {
        const char *last = std::strrchr(xff, ',');
        const char *start = last ? last + 1 : xff;
        while (*start == ' ') {
            start++;
        }
        size_t n = std::strlen(start);
        if (n >= out_size) {
            n = out_size - 1;
        }
        std::memcpy(out, start, n);
        out[n] = '\0';
    }
}

/**
 * 인가된 특권 행사(🔍) 1건을 Slack으로 보냅니다. handler 본문에서 반드시
 * check_auth 통과 **후**에 호출하세요 (인증 실패 요청에 알림이 나가면
 * audit의 의미가 뒤집힙니다).
 *
 * 지역 버퍼 ~460B를 스택에 잡습니다. slack_notifier_send가 리턴하면 전부
 * 해제되므로 후속 handler 로직(OTA multipart 루프 등)과 순차 점유만 겹침.
 */
static void audit_log(httpd_req_t *req, const char *label) {
    char ip[46] = {};  // INET6 최대 — 프록시 경로라 실용상 IPv4만.
    get_client_ip(req, ip, sizeof(ip));

    char ua[160] = {};
    httpd_req_get_hdr_value_str(req, "User-Agent", ua, sizeof(ua));

    char msg[256];
    snprintf(msg, sizeof(msg), "🔍 %s\n• IP: %s\n• UA: %s",
             label,
             ip[0] ? ip : "unknown",
             ua[0] ? ua : "unknown");

    slack_notifier_send(msg);
    ESP_LOGI(TAG, "audit: %s from %s", label, ip[0] ? ip : "unknown");
}

// ── 401 rate limiter ──
//
// 가장 최근 실패 IP 1명 + 전역 60초 카운터 조합. 이전 설계안(8-slot LRU)은
// 공격자가 9개 IP를 회전시키면 evict로 침묵시킬 수 있어 분산 공격에
// 무력화됐습니다. 단일 entry로 "사람 오타 vs 공격자" 구분하고, 분산 공격은
// 전역 카운터로 잡습니다.
//
// critical section은 비교·카운터 갱신만. slack_notifier_send는 mutex 해제
// 후 호출하여 contention 길어지지 않게.

static struct {
    char     ip[46];
    uint32_t window_start_ms;
    uint32_t count;
    bool     alerted;
} s_latest_fail = {};

static struct {
    uint32_t window_start_ms;
    uint32_t count;
    bool     alerted;
} s_global_fail = {};

static SemaphoreHandle_t s_fail_mutex = nullptr;

static constexpr uint32_t kFailWindowMs       = 10000;   // 단일 IP 창
static constexpr uint32_t kFailGlobalWindowMs = 60000;   // 전역 창
static constexpr uint32_t kFailThreshold      = 3;       // 같은 IP 3회
static constexpr uint32_t kFailGlobalThresh   = 10;      // 전역 10회/분
static constexpr uint32_t kFailCooldownMs     = 60000;   // 알림 재발송 방지

static void audit_401(httpd_req_t *req) {
    if (s_fail_mutex == nullptr) {
        return;  // init 전 호출 가드 (safe mode 포함)
    }

    char ip[46] = {};
    get_client_ip(req, ip, sizeof(ip));
    if (ip[0] == '\0') {
        std::strcpy(ip, "unknown");
    }

    char ua[160] = {};
    httpd_req_get_hdr_value_str(req, "User-Agent", ua, sizeof(ua));

    /* DEBUG: 매 401마다 path/UA/IP 상태 한 줄 — 정체불명 401 추적용. 진단 끝나면 제거. */
    ESP_LOGI(TAG, "401 path=%s ua_present=%d ip_present=%d",
             req->uri,
             ua[0] != '\0' ? 1 : 0,
             ip[0] != '\0' && std::strcmp(ip, "unknown") != 0 ? 1 : 0);

    uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);

    bool alert_ip = false;
    bool alert_global = false;
    uint32_t ip_count_snap = 0;
    uint32_t ip_elapsed_ms = 0;

    xSemaphoreTake(s_fail_mutex, portMAX_DELAY);

    if (std::strcmp(s_latest_fail.ip, ip) != 0) {
        /* 새 IP: 엔트리 덮어쓰기 (단일 slot LRU). */
        std::strncpy(s_latest_fail.ip, ip, sizeof(s_latest_fail.ip) - 1);
        s_latest_fail.ip[sizeof(s_latest_fail.ip) - 1] = '\0';
        s_latest_fail.window_start_ms = now_ms;
        s_latest_fail.count = 1;
        s_latest_fail.alerted = false;
    } else if (now_ms - s_latest_fail.window_start_ms > kFailWindowMs + kFailCooldownMs) {
        /* 창 + 쿨다운 모두 만료 → 완전 리셋. */
        s_latest_fail.window_start_ms = now_ms;
        s_latest_fail.count = 1;
        s_latest_fail.alerted = false;
    } else {
        s_latest_fail.count++;
    }

    if (s_latest_fail.count == kFailThreshold && !s_latest_fail.alerted) {
        s_latest_fail.alerted = true;
        alert_ip = true;
        ip_count_snap = s_latest_fail.count;
        ip_elapsed_ms = now_ms - s_latest_fail.window_start_ms;
    }

    if (now_ms - s_global_fail.window_start_ms > kFailGlobalWindowMs) {
        s_global_fail.window_start_ms = now_ms;
        s_global_fail.count = 1;
        s_global_fail.alerted = false;
    } else {
        s_global_fail.count++;
    }

    if (s_global_fail.count == kFailGlobalThresh && !s_global_fail.alerted) {
        s_global_fail.alerted = true;
        alert_global = true;
    }

    xSemaphoreGive(s_fail_mutex);

    if (alert_ip) {
        char msg[384];
        snprintf(msg, sizeof(msg),
                 "⚠️ 연속 인증 실패 %u회 (최근 %u초)\n• IP: %s\n• UA: %s",
                 static_cast<unsigned>(ip_count_snap),
                 static_cast<unsigned>(ip_elapsed_ms / 1000),
                 ip,
                 ua[0] ? ua : "unknown");
        slack_notifier_send(msg);
        ESP_LOGW(TAG, "audit_401: repeated %u from %s (path=%s)",
                 static_cast<unsigned>(ip_count_snap), ip, req->uri);
    }
    if (alert_global) {
        slack_notifier_send("⚠️ 전역 인증 실패 10회/분 (분산 공격 의심)");
        ESP_LOGW(TAG, "audit_401: global burst");
    }
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

        AuthConfig auth = auth_load();
        if (std::strcmp(user, auth.username) == 0 && std::strcmp(pass, auth.password) == 0) {
            return true;
        }
    }

unauthorized:
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Doorman\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Unauthorized");
    /* 응답 완료 **후** audit. timing side channel로 audit 경로가 외부에
     * 관측되지 않게. slack_notifier_send는 비동기 큐잉이라 지연 없음. */
    audit_401(req);
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

/**
 * httpd_query_key_value + url_decode 래퍼.
 *
 * 반복되는 URL 인코딩 버퍼 부족 버그(alias, mac, ssid, pass, user 등 — 과거
 * 커밋 0ca7143, eb77a8b 참조)를 원천 차단하기 위한 템플릿 헬퍼입니다.
 *
 * 동작:
 * 1. out 버퍼 크기(N)로부터 필요한 임시 버퍼 크기를 컴파일 시점에 계산합니다.
 *    URL 인코딩은 1바이트를 %XX로 3바이트로 부풀리므로 (N-1)*3+1 = 3N-2 이면
 *    out을 꽉 채우는 모든 합법 입력을 받을 수 있고, 여유 2바이트를 더해
 *    N*3로 간결하게 잡습니다.
 * 2. httpd_query_key_value로 인코딩된 값을 임시 버퍼에 받습니다.
 * 3. url_decode로 제자리 디코딩.
 * 4. 디코딩 결과 길이를 재검증한 뒤 out에 복사하고 null 종료.
 *
 * 반환값:
 * - Ok       : out에 디코딩 값이 복사되고 null 종료됨.
 * - NotFound : body에 해당 key가 없음 (optional 필드 분기용). out 미변경.
 * - TooLong  : 인코딩된 값이 버퍼 초과, 또는 디코딩 후 out 크기 초과.
 *              out 미변경.
 *
 * 사용 예:
 *   char mac_str[24] = {};
 *   if (query_and_decode(body, "mac", mac_str) != QueryResult::Ok) {
 *       return send_text(req, "400 Bad Request", "mac parameter is required");
 *   }
 */
enum class QueryResult { Ok, NotFound, TooLong };

template <size_t N>
static QueryResult query_and_decode(const char *body, const char *key,
                                     char (&out)[N]) {
    static_assert(N >= 2, "out buffer must hold at least 1 char + null");

    // 인코딩 확장 계수 3 + 약간의 여유. 컴파일 시 상수라 스택 크기도 고정.
    char tmp[N * 3] = {};
    esp_err_t err = httpd_query_key_value(body, key, tmp, sizeof(tmp));
    if (err == ESP_ERR_NOT_FOUND) {
        return QueryResult::NotFound;
    }
    if (err != ESP_OK) {
        return QueryResult::TooLong;  // TRUNC 포함
    }

    url_decode(tmp);
    size_t len = strnlen(tmp, sizeof(tmp));
    if (len >= N) {
        return QueryResult::TooLong;
    }

    memcpy(out, tmp, len);
    out[len] = '\0';
    return QueryResult::Ok;
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
 * Ring Buffer: esp_log → WS 브리지입니다.
 *
 * custom_vprintf가 모든 로그를 시리얼 + Ring Buffer에 동시 출력합니다.
 * WS sender 태스크가 Ring Buffer에서 읽어서 연결된 WS 클라이언트에 전송합니다.
 * 8KB면 약 50~100줄의 로그를 버퍼링 가능합니다.
 */
static RingbufHandle_t s_log_ringbuf = nullptr;
static vprintf_like_t s_original_vprintf = nullptr;
static httpd_handle_t s_server = nullptr;

/** WS 인증 토큰입니다. 부팅마다 esp_random()으로 생성합니다. RAM에만 저장됩니다. */
static char s_ws_token[17] = {};
/** 동시 WS 클라이언트 최대 수입니다. httpd 소켓 풀(기본 7)에서 API용을 빼면 4~5가 현실적입니다. */
static constexpr int kMaxWsClients = 5;
static int s_ws_fds[kMaxWsClients] = {-1, -1, -1, -1, -1};
static portMUX_TYPE s_ws_lock = portMUX_INITIALIZER_UNLOCKED;

/**
 * 커스텀 vprintf: 시리얼 출력 + Ring Buffer 복사입니다.
 *
 * esp_log_set_vprintf()로 등록되어 ESP_LOGx 매크로 호출 시 실행됩니다.
 * Ring Buffer에 넣을 때 ISR 컨텍스트가 아니므로 xRingbufferSend를 사용합니다.
 * Ring Buffer가 가득 차면 조용히 드랍합니다 (WS 클라이언트가 없거나 느릴 때).
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
 * WS sender 태스크: Ring Buffer에서 로그를 읽어 WS 클라이언트에 전송합니다.
 *
 * 100ms 주기로 Ring Buffer를 폴링하여 데이터가 있으면 WS frame으로 보냅니다.
 * httpd_ws_send_frame_async()를 사용하여 httpd 태스크와 동기화합니다.
 * WS 연결이 없으면 Ring Buffer 데이터를 소비만 하고 버립니다.
 */
static void ws_sender_task(void *) {
    while (true) {
        size_t item_size = 0;
        void *item = xRingbufferReceive(s_log_ringbuf, &item_size, pdMS_TO_TICKS(100));

        if (item != nullptr && item_size > 0) {
            if (s_server != nullptr) {
                httpd_ws_frame_t pkt = {};
                pkt.type = HTTPD_WS_TYPE_TEXT;
                pkt.payload = static_cast<uint8_t *>(item);
                pkt.len = item_size;
                pkt.final = true;

                /** 모든 연결된 WS 클라이언트에 브로드캐스트. */
                taskENTER_CRITICAL(&s_ws_lock);
                int fds[kMaxWsClients];
                std::memcpy(fds, s_ws_fds, sizeof(fds));
                taskEXIT_CRITICAL(&s_ws_lock);

                for (int i = 0; i < kMaxWsClients; ++i) {
                    if (fds[i] < 0) {
                        continue;
                    }
                    esp_err_t err = httpd_ws_send_frame_async(s_server, fds[i], &pkt);
                    if (err != ESP_OK) {
                        taskENTER_CRITICAL(&s_ws_lock);
                        s_ws_fds[i] = -1;
                        taskEXIT_CRITICAL(&s_ws_lock);
                    }
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
    // SSID/pass 인코딩 값이 들어올 수 있으므로 body는 넉넉히 512.
    char body[512];
    if (read_body(req, body, sizeof(body)) < 0) {
        return send_text(req, "400 Bad Request", "Invalid request");
    }

    char ssid[33] = {};  // WPA SSID max 32 + null
    char pass[64] = {};  // WPA password max 63 + null
    QueryResult qr_ssid = query_and_decode(body, "ssid", ssid);
    if (qr_ssid != QueryResult::Ok || ssid[0] == '\0') {
        return send_text(req, "400 Bad Request", "SSID is required");
    }
    QueryResult qr_pass = query_and_decode(body, "pass", pass);
    if (qr_pass == QueryResult::TooLong) {
        return send_text(req, "400 Bad Request", "password too long");
    }
    // NotFound 또는 Ok 모두 허용 (오픈 네트워크용 빈 비번).

    wifi_save_credentials(ssid, pass);
    send_text(req, "200 OK", "OK");
    schedule_restart();
    return ESP_OK;
}

// ── STA Handlers ──

static esp_err_t index_page_handler(httpd_req_t *req) {
    if (!check_auth(req)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t door_open_handler(httpd_req_t *req) {
    if (!check_auth(req)) {
        return ESP_OK;
    }

    /**
     * 문열기는 서비스 이용이라 알림·audit 대상 아님. 호출마다 근태성 로그를
     * 쌓지 않기 위한 의식적 결정입니다. 관리 특권 행사만 audit_log(🔍)로
     * 남기고, 비인가 시도는 check_auth의 audit_401(⚠️)로 별도 처리.
     * 상세 철학은 docs/brainstorms/2026-04-23-audit-log-sousveillance-brainstorm.md.
     *
     * 이전: door_trigger_pulse() 직접 호출
     * 이후: Control Task 큐를 통해 간접 호출하여 SM Task 명령과 직렬화
     */
    control_queue_send(ControlCommand::ManualUnlock);
    return send_text(req, "200 OK", "OK");
}

static esp_err_t auth_update_handler(httpd_req_t *req) {
    if (!check_auth(req)) {
        return ESP_OK;
    }
    audit_log(req, "로그인 계정 변경 요청");

    char body[512];
    if (read_body(req, body, sizeof(body)) < 0) {
        return send_text(req, "400 Bad Request", "Invalid request");
    }

    char user[32] = {};  // raw max 31 + null
    char pass[64] = {};  // raw max 63 + null
    if (query_and_decode(body, "user", user) != QueryResult::Ok || user[0] == '\0') {
        return send_text(req, "400 Bad Request", "Username is required");
    }
    if (query_and_decode(body, "pass", pass) != QueryResult::Ok || pass[0] == '\0') {
        return send_text(req, "400 Bad Request", "Password is required");
    }

    auth_save(user, pass);
    return send_text(req, "200 OK", "OK");
}

static esp_err_t wifi_update_handler(httpd_req_t *req) {
    if (!check_auth(req)) {
        return ESP_OK;
    }
    audit_log(req, "WiFi 설정 변경 요청");

    char body[512];
    if (read_body(req, body, sizeof(body)) < 0) {
        return send_text(req, "400 Bad Request", "Invalid request");
    }

    char ssid[33] = {};
    char pass[64] = {};
    if (query_and_decode(body, "ssid", ssid) != QueryResult::Ok || ssid[0] == '\0') {
        return send_text(req, "400 Bad Request", "SSID is required");
    }
    if (query_and_decode(body, "pass", pass) == QueryResult::TooLong) {
        return send_text(req, "400 Bad Request", "password too long");
    }

    wifi_save_credentials(ssid, pass);
    send_text(req, "200 OK", "OK");
    schedule_restart();
    return ESP_OK;
}

static esp_err_t ota_upload_handler(httpd_req_t *req) {
    if (!check_auth(req)) {
        return ESP_OK;
    }
    audit_log(req, "펌웨어 업로드 요청");

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
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
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
    if (ota_session_open) {
        esp_ota_abort(ota_handle);
    }
    if (upload_claimed) {
        upload_in_progress.store(false);
    }
    send_text(req, response_status, response_message);
    return result == ESP_OK ? ESP_FAIL : result;
}

/** 페어링 토글입니다. 꺼져있으면 켜고, 켜져있으면 끕니다. */
static esp_err_t pairing_toggle_handler(httpd_req_t *req) {
    if (!check_auth(req)) {
        return ESP_OK;
    }
    if (bt_is_pairing()) {
        bt_stop_pairing();
        return send_text(req, "200 OK", "off");
    }
    bt_request_pairing();
    return send_text(req, "200 OK", "on");
}

/** 페어링 현재 상태를 조회합니다. */
static esp_err_t pairing_status_handler(httpd_req_t *req) {
    if (!check_auth(req)) {
        return ESP_OK;
    }
    return send_text(req, "200 OK", bt_is_pairing() ? "on" : "off");
}

/** 시스템 정보를 조회합니다. 빌드 버전 + 런타임 상태(세이프 모드 등) JSON 반환합니다. */
extern bool is_safe_mode();

static esp_err_t info_handler(httpd_req_t *req) {
    if (!check_auth(req)) {
        return ESP_OK;
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"version\":\"v%s\",\"date\":\"%s %s\",\"safe_mode\":%s}",
             desc->version, desc->date, desc->time,
             is_safe_mode() ? "true" : "false");
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/** 시스템 통계 조회. 힙 + 태스크 수. */
static esp_err_t stats_handler(httpd_req_t *req) {
    if (!check_auth(req)) {
        return ESP_OK;
    }

    SystemStats stats = monitor_get_stats();
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"internal_free\":%lu,\"internal_total\":%lu,"
             "\"spiram_free\":%lu,\"spiram_total\":%lu,\"task_count\":%d}",
             (unsigned long)stats.internal_free,
             (unsigned long)stats.internal_total,
             (unsigned long)stats.spiram_free,
             (unsigned long)stats.spiram_total,
             stats.task_count);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/**
 * Core dump 요약 조회.
 * flash에 저장된 core dump가 있으면 JSON으로 반환합니다.
 */
static esp_err_t coredump_handler(httpd_req_t *req) {
    if (!check_auth(req)) {
        return ESP_OK;
    }
    audit_log(req, "크래시 덤프 다운로드 요청");

    esp_core_dump_summary_t summary = {};
    esp_err_t err = esp_core_dump_get_summary(&summary);
    if (err != ESP_OK) {
        return send_text(req, "200 OK", "{\"available\":false}");
    }

    char buf[512];
    char backtrace[256] = {};
    int pos = 0;
    for (int i = 0; i < summary.exc_bt_info.depth && i < 16; ++i) {
        pos += snprintf(backtrace + pos, sizeof(backtrace) - pos,
                        "%s0x%08lx", i > 0 ? "," : "",
                        (unsigned long)summary.exc_bt_info.bt[i]);
        if (pos >= (int)sizeof(backtrace) - 12) break;
    }

    snprintf(buf, sizeof(buf),
             "{\"available\":true,"
             "\"task\":\"%s\","
             "\"exc_cause\":%lu,"
             "\"pc\":\"0x%08lx\","
             "\"backtrace\":[%s]}",
             summary.exc_task,
             (unsigned long)summary.ex_info.exc_cause,
             (unsigned long)summary.exc_pc,
             backtrace);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/** WS 인증 토큰 발급. Basic Auth 뒤에 있으므로 인증된 사용자만 획득 가능. */
static esp_err_t ws_token_handler(httpd_req_t *req) {
    if (!check_auth(req)) {
        return ESP_OK;
    }
    return send_text(req, "200 OK", s_ws_token);
}

/**
 * 기기 재부팅.
 *
 * audit 대상 아님 — 재부팅 "요청"을 기록하려 해도 vTaskDelay(500) 안에
 * Slack HTTPS 왕복이 못 끝나 메시지가 소실되기 일쑤. 대신 부팅 직후
 * main.cpp에서 💬 부팅 완료 (reason) 로 결과를 확실히 알림. reason이
 * "소프트 재부팅"이면 여기서 호출된 결과, "패닉 — 크래시"면 자동 복구.
 */
static esp_err_t reboot_handler(httpd_req_t *req) {
    if (!check_auth(req)) {
        return ESP_OK;
    }

    send_text(req, "200 OK", "Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/** BT 자동 문열림 토글. 현재 상태를 반전시키고 NVS에 저장합니다. */
static esp_err_t auto_unlock_toggle_handler(httpd_req_t *req) {
    if (!check_auth(req)) {
        return ESP_OK;
    }

    bool new_state = !auto_unlock_is_enabled();
    auto_unlock_set(new_state);

    const char *state = new_state ? "enabled" : "disabled";
    /* auto_unlock 모듈이 NVS 저장 시 로그를 찍으므로 여기서는 생략 */
    return send_text(req, "200 OK", state);
}

/** BT 자동 문열림 현재 상태 조회. */
static esp_err_t auto_unlock_status_handler(httpd_req_t *req) {
    if (!check_auth(req)) {
        return ESP_OK;
    }

    return send_text(req, "200 OK", auto_unlock_is_enabled() ? "enabled" : "disabled");
}

/** Slack webhook URL 설정. 빈 문자열이면 알림 비활성화. 재부팅 불필요. */
static esp_err_t slack_update_handler(httpd_req_t *req) {
    if (!check_auth(req)) {
        return ESP_OK;
    }
    audit_log(req, "Slack 웹훅 변경 요청");

    char body[512];
    if (read_body(req, body, sizeof(body)) < 0) {
        return send_text(req, "400 Bad Request", "Invalid request");
    }

    char url[256] = {};
    QueryResult qr = query_and_decode(body, "url", url);
    if (qr == QueryResult::TooLong) {
        return send_text(req, "400 Bad Request", "URL too long");
    }
    /* NotFound 또는 빈 값은 비활성화 요청으로 해석 */

    esp_err_t err = slack_notifier_update_url(url[0] ? url : nullptr);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_text(req, "400 Bad Request", "Invalid webhook URL");
    }
    if (err != ESP_OK) {
        return send_text(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    return send_text(req, "200 OK", "OK");
}

/**
 * 본딩된 기기 목록 + 기기별 config + SM 스냅샷을 JSON으로 반환합니다.
 * snprintf + chunked 전송, 힙 할당 없습니다.
 */
static DeviceState *find_snapshot(DeviceState *snaps, int count, const uint8_t *mac) {
    for (int i = 0; i < count; i++) {
        if (memcmp(snaps[i].mac, mac, 6) == 0) {
            return &snaps[i];
        }
    }
    return nullptr;
}

static esp_err_t devices_handler(httpd_req_t *req) {
    if (!check_auth(req)) {
        return ESP_OK;
    }

    uint8_t macs[kMaxTrackedDevices][6];
    int bond_count = bt_get_bonded_devices(macs, kMaxTrackedDevices);
    DeviceState snapshots[kMaxTrackedDevices];
    int snap_count = sm_get_snapshots(snapshots, kMaxTrackedDevices);

    httpd_resp_set_type(req, "application/json");

    char buf[256];
    snprintf(buf, sizeof(buf), "{\"auto_unlock\":%s,\"devices\":[",
             auto_unlock_is_enabled() ? "true" : "false");
    httpd_resp_sendstr_chunk(req, buf);

    for (int i = 0; i < bond_count; i++) {
        DeviceConfig cfg = device_config_get(macs[i]);
        DeviceState *snap = find_snapshot(snapshots, snap_count, macs[i]);

        snprintf(buf, sizeof(buf),
            "%s{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
            "\"alias\":\"%s\",\"detected\":%s,\"rssi\":%d,"
            "\"last_seen_ms\":%lu,\"config\":{"
            "\"rssi_threshold\":%d,\"presence_timeout_ms\":%lu,"
            "\"enter_window_ms\":%lu,\"enter_min_count\":%lu}}",
            i > 0 ? "," : "",
            macs[i][0], macs[i][1], macs[i][2],
            macs[i][3], macs[i][4], macs[i][5],
            cfg.alias,
            snap ? (snap->detected ? "true" : "false") : "false",
            snap ? snap->last_rssi : 0,
            snap ? (unsigned long)snap->last_seen_ms : 0UL,
            cfg.rssi_threshold,
            (unsigned long)cfg.presence_timeout_ms,
            (unsigned long)cfg.enter_window_ms,
            (unsigned long)cfg.enter_min_count);
        httpd_resp_sendstr_chunk(req, buf);
    }

    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, nullptr);
    return ESP_OK;
}

/** 본딩된 기기 삭제. body: mac=AA:BB:CC:DD:EE:FF */
static esp_err_t devices_delete_handler(httpd_req_t *req) {
    if (!check_auth(req)) {
        return ESP_OK;
    }

    char body[64];
    if (read_body(req, body, sizeof(body)) < 0) {
        return send_text(req, "400 Bad Request", "Invalid request");
    }

    char mac_str[24] = {};  // raw max "AA:BB:CC:DD:EE:FF" = 17 + null
    if (query_and_decode(body, "mac", mac_str) != QueryResult::Ok || mac_str[0] == '\0') {
        return send_text(req, "400 Bad Request", "mac parameter is required");
    }

    /* MAC 문자열 파싱 (AA:BB:CC:DD:EE:FF) */
    uint8_t mac[6] = {};
    unsigned int m[6] = {};
    if (sscanf(mac_str, "%02X:%02X:%02X:%02X:%02X:%02X",
                    &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
        return send_text(req, "400 Bad Request", "Invalid MAC format");
    }
    for (int i = 0; i < 6; ++i) {
        mac[i] = static_cast<uint8_t>(m[i]);
    }

    bt_remove_bond(mac);
    device_config_delete(mac);
    sm_remove_device_queue_send(mac);
    return send_text(req, "200 OK", "OK");
}

/** 기기별 설정 저장. body: mac=AA:BB:CC:DD:EE:FF&alias=...&rssi=N&timeout=N&enter_window=N&enter_count=N */
static esp_err_t devices_config_handler(httpd_req_t *req) {
    if (!check_auth(req)) {
        return ESP_OK;
    }

    char body[256];
    if (read_body(req, body, sizeof(body)) < 0) {
        return send_text(req, "400 Bad Request", "Invalid request");
    }

    char mac_str[24] = {};  // raw max "AA:BB:CC:DD:EE:FF" = 17 + null
    if (query_and_decode(body, "mac", mac_str) != QueryResult::Ok || mac_str[0] == '\0') {
        return send_text(req, "400 Bad Request", "mac parameter is required");
    }

    uint8_t mac[6] = {};
    unsigned int m[6] = {};
    if (sscanf(mac_str, "%02X:%02X:%02X:%02X:%02X:%02X",
               &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
        return send_text(req, "400 Bad Request", "Invalid MAC format");
    }
    for (int i = 0; i < 6; ++i) {
        mac[i] = static_cast<uint8_t>(m[i]);
    }

    // 현재 config를 기반으로 부분 업데이트
    DeviceConfig cfg = device_config_get(mac);

    char val[64] = {};
    // alias는 optional — 미포함이면 기존 값 유지, 길이 초과면 400.
    // query_and_decode 내부에서 UTF-8 경계 무결성은 validate_device_config가
    // 이후 config.cpp:is_alias_utf8_well_formed로 최종 검증합니다.
    switch (query_and_decode(body, "alias", cfg.alias)) {
        case QueryResult::TooLong:
            return send_text(req, "400 Bad Request", "alias too long (max 31 bytes)");
        case QueryResult::NotFound:  // 기존 값 유지
        case QueryResult::Ok:
            break;
    }
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

    if (!validate_device_config(cfg)) {
        return send_text(req, "400 Bad Request", "Invalid values");
    }

    device_config_set(mac, cfg);
    return send_text(req, "200 OK", "OK");
}

/**
 * WebSocket 핸들러. 최대 kMaxWsClients명 동시 접속.
 * 연결 시: 빈 슬롯에 fd를 추가합니다. ws_sender_task가 전체 브로드캐스트.
 * 연결 종료: send 실패 시 ws_sender_task가 슬롯을 정리합니다.
 */
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        /**
         * WS 핸드셰이크 시 query param으로 토큰을 검증합니다.
         * 브라우저의 new WebSocket()은 커스텀 헤더를 보낼 수 없으므로
         * Basic Auth 대신 토큰으로 인증합니다.
         */
        char query[64] = {};
        char token[24] = {};
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
            httpd_query_key_value(query, "token", token, sizeof(token)) != ESP_OK ||
            std::strcmp(token, s_ws_token) != 0) {
            ESP_LOGW(TAG, "WS rejected — invalid token");
            httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid token");
            return ESP_FAIL;
        }
        audit_log(req, "로그 스트리밍 요청");

        int fd = httpd_req_to_sockfd(req);
        bool added = false;

        taskENTER_CRITICAL(&s_ws_lock);
        /* 같은 fd가 이미 등록돼있으면 중복 방지 */
        for (int i = 0; i < kMaxWsClients; ++i) {
            if (s_ws_fds[i] == fd) {
                added = true;
                break;
            }
        }
        if (!added) for (int i = 0; i < kMaxWsClients; ++i) {
            if (s_ws_fds[i] < 0) {
                s_ws_fds[i] = fd;
                added = true;
                break;
            }
        }
        taskEXIT_CRITICAL(&s_ws_lock);

        if (added) {
            ESP_LOGI(TAG, "WS client connected (fd=%d)", fd);
        } else {
            ESP_LOGW(TAG, "WS client rejected — max %d reached", kMaxWsClients);
        }
        return ESP_OK;
    }

    /* 클라이언트에서 데이터가 오면 읽어서 버립니다 (단방향 스트리밍) */
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
 * Ring Buffer + WS sender 태스크를 생성하고 esp_log를 후킹합니다.
 * STA 모드에서만 호출합니다 — SoftAP에서는 로그 스트리밍이 불필요합니다.
 */
static void init_log_streaming() {
    /* WS 인증 토큰 생성. 부팅마다 랜덤. */
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    snprintf(s_ws_token, sizeof(s_ws_token), "%08lx%08lx", (unsigned long)r1, (unsigned long)r2);
    ESP_LOGI(TAG, "WS token generated");

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
    if (s_fail_mutex == nullptr) {
        /* 우선순위 상속 mutex — httpd 단일 task 가정이라 contention 거의 0.
         * 실패 시(내부 RAM 부족) audit_401이 nullptr 가드로 조용히 무시. */
        s_fail_mutex = xSemaphoreCreateMutex();
        if (s_fail_mutex == nullptr) {
            ESP_LOGW(TAG, "audit_401 mutex alloc failed — rate limiter disabled");
        }
    }

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
        ok = register_routes(server, softap_routes,
                             sizeof(softap_routes) / sizeof(softap_routes[0]));
    } else {
        static const Route sta_routes[] = {
            {"/",                          HTTP_GET,  index_page_handler,          false},
            {"/api/door/open",             HTTP_POST, door_open_handler,           false},
            {"/api/firmware/upload",       HTTP_POST, ota_upload_handler,          false},
            {"/api/auth/update",           HTTP_POST, auth_update_handler,         false},
            {"/api/wifi/update",           HTTP_POST, wifi_update_handler,         false},
            {"/api/pairing/toggle",        HTTP_POST, pairing_toggle_handler,      false},
            {"/api/pairing/status",        HTTP_GET,  pairing_status_handler,      false},
            {"/api/info",                   HTTP_GET,  info_handler,                false},
            {"/api/stats",                 HTTP_GET,  stats_handler,               false},
            {"/api/ws-token",              HTTP_GET,  ws_token_handler,            false},
            {"/api/reboot",                HTTP_POST, reboot_handler,              false},
            {"/api/auto-unlock/toggle",    HTTP_POST, auto_unlock_toggle_handler,  false},
            {"/api/auto-unlock/status",    HTTP_GET,  auto_unlock_status_handler,  false},
            {"/api/slack/update",          HTTP_POST, slack_update_handler,        false},
            {"/api/devices",               HTTP_GET,  devices_handler,             false},
            {"/api/devices/config",        HTTP_POST, devices_config_handler,      false},
            {"/api/devices/delete",        HTTP_POST, devices_delete_handler,      false},
            {"/api/coredump",              HTTP_GET,  coredump_handler,            false},
            {"/ws",                        HTTP_GET,  ws_handler,                  true},
        };
        ok = register_routes(server, sta_routes,
                             sizeof(sta_routes) / sizeof(sta_routes[0]));
        if (ok) {
            /* STA 모드에서만 로그 스트리밍 활성화 */
            s_server = server;
            init_log_streaming();
        }
    }

    if (!ok) {
        return nullptr;
    }

    ESP_LOGI(TAG, "HTTP server started (mode=%s, port=%d)",
             mode == WifiMode::SoftAP ? "SoftAP" : "STA", config.server_port);
    return server;
}
