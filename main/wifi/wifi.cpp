#include "wifi/wifi.h"
#include "nvs_config.h"

#include <cstring>

#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <mdns.h>

static const char *TAG = "wifi";

static const char *kApSsid = "Doorman-Setup";
static const char *kApPassword = "12345678";
static const int kStaTimeoutMs = 15000;
static const int kMaxRetries = 5;

static const int CONNECTED_BIT = BIT0;
static const int FAIL_BIT = BIT1;

static EventGroupHandle_t s_event_group = nullptr;
static int s_retry_count = 0;

// ── Event handler ──

static void event_handler(void *, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_DISCONNECTED: {
            auto *e = static_cast<wifi_event_sta_disconnected_t *>(data);
            ESP_LOGW(TAG, "STA disconnected (reason=%d)", e->reason);
            if (s_event_group) {
                // 초기 접속 시도 중 — 재시도 또는 실패
                if (s_retry_count < kMaxRetries) {
                    s_retry_count++;
                    ESP_LOGI(TAG, "Retrying... (%d/%d)", s_retry_count, kMaxRetries);
                    esp_wifi_connect();
                } else {
                    xEventGroupSetBits(s_event_group, FAIL_BIT);
                }
            } else {
                // 운영 중 끊김 — 자동 재접속
                ESP_LOGI(TAG, "Reconnecting...");
                esp_wifi_connect();
            }
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED: {
            auto *e = static_cast<wifi_event_ap_staconnected_t *>(data);
            ESP_LOGI(TAG, "station " MACSTR " joined, AID=%d", MAC2STR(e->mac), e->aid);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            auto *e = static_cast<wifi_event_ap_stadisconnected_t *>(data);
            ESP_LOGI(TAG, "station " MACSTR " left, AID=%d", MAC2STR(e->mac), e->aid);
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *e = static_cast<ip_event_got_ip_t *>(data);
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        if (s_event_group) {
            xEventGroupSetBits(s_event_group, CONNECTED_BIT);
        }
    }
}

// ── mDNS ──

static void start_mdns() {
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("doorman"));
    ESP_ERROR_CHECK(mdns_instance_name_set("Doorman"));
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    ESP_LOGI(TAG, "mDNS: doorman.local");
}

// ── Internal helpers ──

static void init_common() {
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    // Create both netifs upfront — only the active mode's interface is used.
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, nullptr, nullptr));
}

static bool try_sta(const WifiConfig &cred) {
    s_retry_count = 0;
    s_event_group = xEventGroupCreate();

    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char *>(config.sta.ssid), cred.ssid, sizeof(config.sta.ssid));
    std::strncpy(reinterpret_cast<char *>(config.sta.password), cred.password, sizeof(config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to '%s' ...", cred.ssid);

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        esp_wifi_stop();
        vEventGroupDelete(s_event_group);
        s_event_group = nullptr;
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_event_group, CONNECTED_BIT | FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(kStaTimeoutMs));

    vEventGroupDelete(s_event_group);
    s_event_group = nullptr;

    if (bits & CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to '%s'", cred.ssid);
        start_mdns();
        return true;
    }

    ESP_LOGW(TAG, "Failed to connect to '%s' (timeout or rejected)", cred.ssid);
    esp_wifi_disconnect();
    esp_wifi_stop();
    return false;
}

static void start_softap() {
    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char *>(config.ap.ssid), kApSsid, sizeof(config.ap.ssid));
    config.ap.ssid_len = std::strlen(kApSsid);
    std::strncpy(reinterpret_cast<char *>(config.ap.password), kApPassword, sizeof(config.ap.password));
    config.ap.channel = 1;
    config.ap.max_connection = 4;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started — SSID: %s  PW: %s  IP: 192.168.4.1", kApSsid, kApPassword);
}

// ── Public API ──

WifiMode wifi_start() {
    init_common();

    WifiConfig cred = {};
    if (nvs_load_wifi(cred)) {
        if (try_sta(cred)) {
            return WifiMode::STA;
        }
        ESP_LOGW(TAG, "STA failed — falling back to SoftAP");
    } else {
        ESP_LOGI(TAG, "No stored WiFi credentials");
    }

    start_softap();
    return WifiMode::SoftAP;
}
