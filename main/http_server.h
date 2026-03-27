#pragma once

#include "wifi.h"

#include <esp_http_server.h>

httpd_handle_t start_webserver(WifiMode mode);
