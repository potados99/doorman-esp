#pragma once

#include <cstddef>

struct WifiConfig {
    char ssid[33];
    char password[65];
};

struct AuthConfig {
    char username[32];
    char password[64];
};

// Returns true if WiFi credentials were found in NVS.
bool nvs_load_wifi(WifiConfig &out);
void nvs_save_wifi(const char *ssid, const char *password);

// Returns auth credentials from NVS (defaults to admin/admin if not set).
AuthConfig nvs_load_auth();
void nvs_save_auth(const char *username, const char *password);
