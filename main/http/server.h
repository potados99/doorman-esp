#ifndef DOORMAN_ESP_HTTP_SERVER_H
#define DOORMAN_ESP_HTTP_SERVER_H

#include "wifi/wifi.h"

#include <esp_http_server.h>

/**
 * HTTP(+WS) 서버를 시작합니다.
 *
 * SoftAP 모드: WiFi 프로비저닝 페이지만 서빙합니다.
 * STA 모드: 대시보드, 설정 API, OTA, WebSocket 로그 스트리밍을 제공합니다.
 *
 * STA 모드에서는 esp_log_set_vprintf()를 후킹하여
 * 모든 로그를 Ring Buffer에 복사하고,
 * 별도 태스크가 WS 연결된 클라이언트에게 주기적으로 전송합니다.
 */
httpd_handle_t start_webserver(WifiMode mode);

#endif //DOORMAN_ESP_HTTP_SERVER_H
