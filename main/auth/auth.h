#ifndef DOORMAN_ESP_AUTH_H
#define DOORMAN_ESP_AUTH_H

/**
 * HTTP Basic Auth 크레덴셜. NVS namespace "auth"에 저장됩니다.
 * 값이 없으면 기본값 admin/admin이 반환됩니다 (초기 provisioning 용).
 */
struct AuthConfig {
    char username[32];
    char password[64];
};

/** NVS에서 인증 크레덴셜을 로드합니다. 없으면 admin/admin 기본값. */
AuthConfig auth_load();

/** NVS에 인증 크레덴셜을 저장합니다. */
void auth_save(const char *username, const char *password);

#endif //DOORMAN_ESP_AUTH_H
