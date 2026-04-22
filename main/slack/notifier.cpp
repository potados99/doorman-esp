#include "slack/notifier.h"

#include <cstdio>
#include <cstring>

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs.h>

static const char *TAG = "slack";

// ── 설정 상수 ────────────────────────────────────────────────────────────────

/** 큐 깊이. 짧은 burst(20회 동시 문열기) 전부 안 받을 수 있음을 수락 — 문 동작엔 영향 없음. */
static constexpr int kQueueDepth = 3;

/** 메시지 body 최대 길이. 초과분은 producer에서 잘림. */
static constexpr size_t kMaxMsgLen = 256;

/** notifier_task 스택. 내부 RAM. TLS handshake + crt_bundle 파싱 + JSON
 *  빌드 버퍼(~544B)를 모두 수용하기 위해 12288B로 여유 확보.
 *  실측 HWM은 매 POST 직후 로그로 관찰 가능 — sm_task 사고처럼 burst peak
 *  60% 룰로 수축 여부를 나중에 결정.
 *  docs/solutions/runtime-errors/sm-task-stack-overflow-cascade.md */
static constexpr uint32_t kTaskStackSize = 12288;

/** notifier_task 우선순위. monitor(1) 위, sm(5) 아래. */
static constexpr UBaseType_t kTaskPriority = 4;

/** webhook URL NVS 저장 */
static constexpr const char *kNvsNamespace = "slack";
static constexpr const char *kKeyUrl = "url";

/** webhook URL 형식 검증 */
static constexpr const char *kUrlPrefix = "https://hooks.slack.com/services/";
static constexpr size_t kUrlPrefixLen = 33;   // strlen(kUrlPrefix)
static constexpr size_t kUrlMinLen = 64;      // prefix + 최소 T/B/secret
static constexpr size_t kUrlMaxLen = 256;

// ── 모듈 상태 ────────────────────────────────────────────────────────────────

/** 큐 아이템: body는 PSRAM heap, 소유권은 send 성공 시 consumer로 이전. */
struct SlackMsg {
    char  *body;
    size_t len;
};

static QueueHandle_t     s_queue       = nullptr;  // 내부 RAM (24B)
static TaskHandle_t      s_task        = nullptr;
static char             *s_webhook_url = nullptr;  // PSRAM heap_caps_malloc
static SemaphoreHandle_t s_url_mutex   = nullptr;

// ── PSRAM 유틸 ──────────────────────────────────────────────────────────────

/** 문자열을 PSRAM heap에 복제. 실패 시 nullptr. caller가 heap_caps_free 해야 함. */
static char *psram_strdup(const char *src, size_t len) {
    char *dst = static_cast<char *>(heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM));
    if (dst == nullptr) {
        return nullptr;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return dst;
}

// ── NVS ─────────────────────────────────────────────────────────────────────

/** NVS에서 URL을 로드해 PSRAM strdup. 없으면 nullptr 반환 (에러 아님). */
static char *load_url_from_nvs() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return nullptr;
    }

    size_t len = 0;
    esp_err_t err = nvs_get_str(handle, kKeyUrl, nullptr, &len);
    if (err != ESP_OK || len < 2 || len > kUrlMaxLen + 1) {
        nvs_close(handle);
        return nullptr;
    }

    /* stack 버퍼로 일단 읽은 뒤 PSRAM으로 복사 — URL 길이 제한 덕에 stack 부담 없음 */
    char tmp[kUrlMaxLen + 1];
    err = nvs_get_str(handle, kKeyUrl, tmp, &len);
    nvs_close(handle);
    if (err != ESP_OK) {
        return nullptr;
    }

    return psram_strdup(tmp, len - 1);  // len includes trailing null
}

/** NVS에 URL 저장. 빈 문자열은 키 삭제. */
static esp_err_t save_url_to_nvs(const char *url) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    if (url == nullptr || url[0] == '\0') {
        nvs_erase_key(handle, kKeyUrl);  // 없는 키 삭제는 조용히 OK
    } else {
        err = nvs_set_str(handle, kKeyUrl, url);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_set_str failed: %s", esp_err_to_name(err));
            nvs_close(handle);
            return err;
        }
    }

    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}

// ── HTTPS POST ──────────────────────────────────────────────────────────────

/**
 * JSON body 구성 with escape. {"text":"<escaped msg>"} 형태.
 *
 * 이스케이프 규칙 (Slack에서 JSON 파싱 깨지지 않도록):
 *   - " → \"
 *   - \ → \\
 *   - \n, \r, \t → 각각 \n, \r, \t (메시지에서 개행은 Slack이 줄바꿈 렌더링)
 *   - 기타 제어문자(0x00~0x1F, 0x7F) → 제거 (단순 drop, \uXXXX 전개 안 함)
 *   - 나머지 UTF-8 바이트는 그대로 통과 (Slack이 UTF-8 허용)
 *
 * 반환: 작성된 바이트 수 (null 제외), out 초과 시 -1.
 */
static int build_json_body(const char *msg, size_t msg_len, char *out, size_t out_size) {
    size_t i = 0;
    auto put = [&](char c) -> bool {
        if (i + 1 >= out_size) return false;
        out[i++] = c;
        return true;
    };
    auto put_escaped = [&](char c) -> bool {
        return put('\\') && put(c);
    };

    const char *prefix = "{\"text\":\"";
    for (const char *p = prefix; *p; ++p) {
        if (!put(*p)) return -1;
    }
    for (size_t j = 0; j < msg_len; ++j) {
        unsigned char c = static_cast<unsigned char>(msg[j]);
        bool ok = true;
        if (c == '"' || c == '\\') {
            ok = put_escaped(static_cast<char>(c));
        } else if (c == '\n') {
            ok = put_escaped('n');
        } else if (c == '\r') {
            ok = put_escaped('r');
        } else if (c == '\t') {
            ok = put_escaped('t');
        } else if (c < 0x20 || c == 0x7F) {
            continue;  // 기타 제어문자는 제거
        } else {
            ok = put(static_cast<char>(c));
        }
        if (!ok) return -1;
    }
    if (!put('"') || !put('}')) return -1;
    out[i] = '\0';
    return static_cast<int>(i);
}

/**
 * Slack Webhook에 body를 POST.
 *
 * TLS handshake는 mbedtls가 담당 (CONFIG_MBEDTLS_DYNAMIC_BUFFER로
 * idle 메모리 최소화, CMN bundle로 hooks.slack.com 인증서 검증).
 * 응답은 status만 확인, body 파싱 안 함.
 */
static void post_to_slack(const char *url, const char *body, size_t len) {
    /* JSON 버퍼는 스택. kMaxMsgLen=256 + JSON 오버헤드 + 이스케이프 여유. */
    char json[kMaxMsgLen * 2 + 32];
    int json_len = build_json_body(body, len, json, sizeof(json));
    if (json_len < 0) {
        ESP_LOGW(TAG, "JSON build overflow — drop");
        return;
    }

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 5000;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "http_client_init failed");
        return;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json, json_len);

    esp_err_t err = esp_http_client_perform(client);
    /* 스택 HWM은 성공/실패 경로 둘 다 찍어야 실측 커버리지가 완전.
       handshake 실패 경로가 피크를 찍는 경우가 잦음 (TLS alert 파싱 등). */
    unsigned hwm_bytes = static_cast<unsigned>(
        uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t));

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            ESP_LOGI(TAG, "Notified Slack (%d bytes, stack hwm=%u)", json_len, hwm_bytes);
        } else {
            ESP_LOGW(TAG, "Slack returned HTTP %d (stack hwm=%u)", status, hwm_bytes);
        }
    } else {
        ESP_LOGW(TAG, "HTTP perform failed: %s (stack hwm=%u)",
                 esp_err_to_name(err), hwm_bytes);
    }

    esp_http_client_cleanup(client);
}

// ── 태스크 ──────────────────────────────────────────────────────────────────

static void notifier_task(void *) {
    ESP_LOGI(TAG, "notifier task started (stack=%lu, core=%d)",
             (unsigned long)kTaskStackSize, xPortGetCoreID());

    SlackMsg item;
    while (true) {
        if (xQueueReceive(s_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* URL 스냅샷: mutex 짧게 잡고 PSRAM strdup. POST 중 update로 free되는 레이스 방지. */
        char *url_snap = nullptr;
        if (s_url_mutex != nullptr) {
            xSemaphoreTake(s_url_mutex, portMAX_DELAY);
            if (s_webhook_url != nullptr) {
                url_snap = psram_strdup(s_webhook_url, strlen(s_webhook_url));
            }
            xSemaphoreGive(s_url_mutex);
        }

        if (url_snap != nullptr) {
            post_to_slack(url_snap, item.body, item.len);
            heap_caps_free(url_snap);
        } else {
            ESP_LOGW(TAG, "Drop: webhook URL not configured");
        }

        heap_caps_free(item.body);
    }
}

// ── URL 검증 ────────────────────────────────────────────────────────────────

static bool is_valid_webhook_url(const char *url) {
    if (url == nullptr) return false;
    size_t len = strlen(url);
    if (len < kUrlMinLen || len > kUrlMaxLen) return false;
    if (strncmp(url, kUrlPrefix, kUrlPrefixLen) != 0) return false;
    /* 제어문자/공백 거부 */
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = static_cast<unsigned char>(url[i]);
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

// ── Public API ──────────────────────────────────────────────────────────────

void slack_notifier_init() {
    s_url_mutex = xSemaphoreCreateMutex();
    configASSERT(s_url_mutex);

    s_webhook_url = load_url_from_nvs();
    if (s_webhook_url == nullptr) {
        ESP_LOGI(TAG, "No webhook URL configured — notifier starts in dormant mode");
    } else {
        ESP_LOGI(TAG, "Webhook URL loaded (%.40s...)", s_webhook_url);
    }

    s_queue = xQueueCreate(kQueueDepth, sizeof(SlackMsg));
    configASSERT(s_queue);

    /* PSRAM task stack 금지. 단일 코어 핀 (tskNO_AFFINITY 금지).
     * docs/solutions/runtime-errors/psram-task-stack-bricks-device.md */
    BaseType_t ok = xTaskCreatePinnedToCore(
        notifier_task,
        "slack_notif",
        kTaskStackSize,
        nullptr,
        kTaskPriority,
        &s_task,
        APP_CPU_NUM);
    configASSERT(ok == pdPASS);
}

void slack_notifier_send(const char *msg) {
    if (s_queue == nullptr || msg == nullptr) return;  // init 전 / 잘못된 입력 무시

    size_t len = strnlen(msg, kMaxMsgLen);
    char *body = psram_strdup(msg, len);
    if (body == nullptr) {
        ESP_LOGW(TAG, "PSRAM alloc failed — drop");
        return;
    }

    SlackMsg item{body, len};
    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Queue full — drop");
        heap_caps_free(body);  // 소유권 원복
    }
}

esp_err_t slack_notifier_update_url(const char *url) {
    if (s_url_mutex == nullptr) return ESP_ERR_INVALID_STATE;

    /* 빈 문자열/nullptr은 비활성화 요청. 그 외엔 형식 검증. */
    bool clearing = (url == nullptr || url[0] == '\0');
    if (!clearing && !is_valid_webhook_url(url)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = save_url_to_nvs(clearing ? nullptr : url);
    if (err != ESP_OK) return err;

    /* 메모리 캐시 atomic swap */
    xSemaphoreTake(s_url_mutex, portMAX_DELAY);
    char *old_url = s_webhook_url;
    s_webhook_url = clearing ? nullptr : psram_strdup(url, strlen(url));
    xSemaphoreGive(s_url_mutex);

    if (old_url != nullptr) heap_caps_free(old_url);

    ESP_LOGI(TAG, "Webhook URL %s", clearing ? "cleared" : "updated");
    return ESP_OK;
}
