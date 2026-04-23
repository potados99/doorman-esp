---
title: "feat: Slack 알림을 audit 로그로 전환 (sousveillance)"
type: feat
status: active
date: 2026-04-23
deepened: 2026-04-23
brainstorm: docs/brainstorms/2026-04-23-audit-log-sousveillance-brainstorm.md
---

# Slack 알림을 audit 로그로 전환 (sousveillance)

## Enhancement Summary

**Deepened on:** 2026-04-23
**Review agents used:** architecture-strategist, security-sentinel, performance-oracle, code-simplicity-reviewer, best-practices-researcher

### Key Improvements (리뷰 기반 변경)

1. **`AuditContext` 풀 + `audit_thunk` 제거 → 각 handler 내부에 `audit_log(req, "라벨")` 한 줄 박기 (Alt A)**
   - Architecture + Simplicity + Security 세 리뷰가 수렴. 풀 고갈 리스크(R1) 완전 소거 + thunk auth-blind 이슈(L1) 해결 + 코드 LOC 절반 감소.
   - `check_auth` 통과 **후**에만 audit → 인증되지 않은 요청에 "🔍 로그 스트리밍 시작" 알림이 나가던 버그 예방.

2. **Rate limiter: 8-slot LRU → 단일 IP 엔트리 + 전역 롤링 카운터**
   - Security CRITICAL C1: 분산 IP 9개 로테이션으로 LRU evict → 침묵 공격 가능. 슬롯 기반 감지는 분산 공격에 무력화됨.
   - 단일 entry(최근 IP) + 글로벌 60초 카운터 조합으로 분산·집중 공격 모두 감지. 메모리 ~500B → ~80B.

3. **audit_401 호출을 401 응답 전송 **후**로 이동 (Security M1)**
   - Timing side channel 차단. 정상 응답 경로 지연 영향도 제거.

4. **IP 추출: X-Real-IP 우선 + XFF 신뢰성 주석 (Security H1)**
   - XFF 스푸핑 완화. Caddy가 X-Real-IP를 덮어쓰므로 신뢰 가능.
   - `<lwip/sockets.h>` 복원 불필요 (socket peer fallback 제거) — Simplicity 지적 수용.

5. **`slack_notifier` 로그에서 webhook URL prefix 40자 노출 제거 → `len=%zu`만** (Security L2).

Deepen agent가 제안했으나 **채택하지 않은 것**:
- `<!date^ts^...>` 토큰으로 이벤트 시각 포함 → 이 프로젝트는 SNTP 미사용이라 epoch가 uptime 기반. 1970-01-01 렌더링 위험. **Slack 자체 타임스탬프에 의존하는 기존 방식 유지**.
- 1분 N회 또는 시간대 휴리스틱으로 door/open 감시 → 브레인스토밍에서 이미 **프라이버시 우선으로 의식적 포기**한 결정. 리뷰 권고지만 수용 안 함.

### New Considerations Discovered

- **Httpd task stack 여유 실측 필요**: audit_log가 지역 버퍼 464B 추가(`ip + ua + msg`). `CONFIG_HTTPD_STACK_SIZE` 기본 4096B라면 HWM 위험. Phase 4 전에 `uxTaskGetStackHighWaterMark` 로깅 1회 권장.
- **Caddy 설정 전제**: 이 플랜은 Caddy가 X-Real-IP를 **신뢰 가능하게 세팅**한다고 가정. 실기기 반영 전 `Caddyfile`에서 `trusted_proxies` 또는 자동 헤더 정책 확인 1회 필요.
- **Webhook URL 변경 race (Security H2)**: 현재 `slack_notifier_update_url`이 mutex 내 atomic swap + notifier consumer가 mutex 안에서 url snapshot strdup → **거의 안전**. 완벽 해결(old URL로 동기 전송 후 swap)은 notifier 인터페이스 변경 필요라 YAGNI로 보류.

## Overview

doorman-esp의 Slack 알림 정책을 **서비스 이용 로그 → audit 로그**로 전환한다. 이전엔 사용자의 **행동**(문 열기)을 기록했다면, 이제는 시스템에 대한 **감시·관리 행위**(설정 변경, 로그 열람, 인증 실패)를 기록한다.

**철학**: 들여다보려는 사람이 자신의 접근을 자동 기록으로 남기는 구조(sousveillance). 권력의 방향을 뒤집어 관리자 자기 견제 + 비인가 탐지를 한 체계로 해결.

**구현 요지**: 7개 특권 handler 본문의 `check_auth` 통과 직후에 `audit_log(req, "<라벨>")` 한 줄 삽입 (Alt A — Deepen에서 Route struct 확장안 기각). `check_auth` 401 분기엔 단일 IP + 전역 카운터 기반 rate limiter 추가. 문 열기 알림은 제거.

## Problem Statement

### 현재 상태 (서비스 이용 로그)
- `/api/door/open` POST 시마다 `🚪 문열림 요청 (via API)` Slack 알림 (`main/http/server.cpp:337`)
- 시간대별로 쌓이면 **출입 패턴 = 근태 데이터셋**
- 과거 한 번은 IP/UA까지 포함했지만 프라이버시 이유로 축소됨
- 그럼에도 **"언제 호출했나"라는 신호가 근태성 로그로 작용**

### 사용자 환경의 특수성
- 일상 출입: 각자 클라이언트(iOS Shortcut / Alfred / curl 스크립트)가 `POST /api/door/open`만 쏨 → 브라우저 **거치지 않음**
- 관리·점검: 브라우저로 `index.html` 열면 `wsConnect()`가 자동 호출되어 `GET /ws` 발생 → **자연스레 audit 대상**
- 즉 "브라우저 진입 ≈ /ws 연결 ≈ audit 알림"이 이미 설계 내부에 내재

### 해결하려는 것
1. 근태성 데이터 누적 차단
2. 관리 특권 사용 시 자기 기록
3. 비인가 접근(401) 실시간 탐지
4. 낯선 사람이 관리 UI에 접근하는 전조를 포착

## Proposed Solution

### 3-카테고리 이벤트 모델

**🔍 인가된 특권 행사** (7개 라우트)
- 시스템 상태/구조에 영향을 주는 행위
- Route 테이블에 `audit_label` 지정 → `register_routes`가 자동으로 `audit_thunk`로 감쌈
- 메시지: `🔍 <라벨>\n• IP: <x>\n• UA: <y>`

**⚠️ 비인가 시도** (401, 모든 라우트)
- `check_auth`의 `unauthorized:` 분기에서 연속 감지 rate limiter 호출
- 10초 내 3회 이상 같은 IP에서 실패 → 1회 요약 알림 + 60초 쿨다운
- 메시지: `⚠️ 연속 인증 실패 N회 (최근 Xs 내)\n• IP: <x>\n• UA: <y>`

**🔕 알림 없음** (프라이버시 영역)
- 서비스 이용: `POST /api/door/open`
- 개별/일상 조작: devices/config, devices/delete, pairing/toggle, auto-unlock/toggle
- 조회·UI 폴링: `GET /`, `/api/info`, `/api/stats`, `/api/devices`, `/api/*/status`

## Technical Approach

### Architecture

#### 데이터 흐름
```
HTTP 요청 도착
  │
  ├─ [audit 지정 라우트] ──▶ audit_thunk(req)
  │                             │
  │                             ├─▶ audit_log(req, label)  ──▶ slack_notifier_send
  │                             └─▶ orig_handler(req)
  │
  └─ [일반 라우트] ──────────▶ handler(req)
                                 │
                                 └─▶ check_auth() 실패 시 ──▶ audit_401(req)
                                                               │
                                                               └─▶ (rate limited) slack_notifier_send
```

#### 구조 변경 (Alt A — Deepen 반영)

**`Route` struct 변경 없음.** thunk와 풀을 도입하는 대신 **각 handler 본문 시작부(check_auth 통과 직후)에 `audit_log(req, "라벨")` 한 줄**을 박는다.

**장점** (3개 리뷰 수렴):
- `AuditContext` 풀 고갈 리스크 완전 소거 (기존 플랜의 R1)
- thunk가 `check_auth` 전에 실행되어 **인증 실패 요청도 "🔍 로그 스트리밍 시작" 같은 알림을 유발**하던 이슈(Security L1) 해결
- 라벨이 handler 본문에 **명시적으로** 드러남 → 데이터 흐름 가시성 ↑ (CLAUDE.md의 "거짓 단순함 금지" 원칙과 합치)
- 코드 LOC 절반 감소

**단점 (수용)**: 새 audit 대상 handler 추가 시 한 줄 넣기를 깜박할 수 있음. 대응:
- 코드리뷰 체크리스트에 "audit 필요 여부" 명시
- PR 템플릿에 체크박스 추가 (향후)
- `grep "audit_log(req," main/http/` 로 빠른 확인

**패턴 (예시)**:
```cpp
static esp_err_t wifi_update_handler(httpd_req_t *req) {
    if (!check_auth(req)) {
        return ESP_OK;
    }
    audit_log(req, "WiFi 설정 변경");  // ← check_auth 직후. 인증된 요청만 기록.

    // ... 기존 로직 ...
}
```

**7개 handler에 추가할 라벨** (Alt A):
| Handler 함수 | Label |
|---|---|
| `ws_handler` | "로그 스트리밍 시작" |
| `ota_upload_handler` | "펌웨어 업로드" |
| `auth_update_handler` | "로그인 계정 변경" |
| `wifi_update_handler` | "WiFi 설정 변경" |
| `slack_update_handler` | "Slack 웹훅 변경" |
| `reboot_handler` | "기기 재부팅" |
| `coredump_handler` | "크래시 덤프 다운로드" |

**컴파일 타임 검증**: 감사 대상 handler 목록을 놓치지 않기 위해 **구현 시 빌드 스크립트나 static_assert 기반 검증은 불필요** (Alt A는 `Route` struct에 label 담지 않으므로 풀 크기 제약 자체가 없음). 대신 `grep` 기반 수동 확인.

**door_open_handler**: `slack_notifier_send` 호출 제거만. audit_log 추가 **안 함** (서비스 이용이라 의도적 제외).

**softap_routes**: 변경 없음. SoftAP는 초기 provisioning 전용, audit 대상 아님.

#### `audit_log` + `get_client_ip` 구현 (Deepen 반영)

```cpp
/* 프록시 뒤에서 클라이언트 IP를 추출합니다.
 *
 * Caddy는 X-Real-IP를 자동으로 덮어쓰므로(trusted proxy) 스푸핑 방지에
 * 유리합니다. XFF는 append 체인이라 첫 엔트리가 공격자 조작 대상이
 * 될 수 있어 fallback에서도 마지막 엔트리를 씁니다 (프록시가 가장
 * 나중에 append하므로 "Caddy가 본 실제 IP"에 가장 가까움).
 *
 * socket peer 조회는 Caddy loopback으로 고정되어 무가치이므로 생략
 * (lwip/sockets.h 복원 불필요 — Simplicity 리뷰 반영).
 * 둘 다 실패 시 빈 문자열 → audit_log가 "unknown"으로 표시. */
static void get_client_ip(httpd_req_t *req, char *out, size_t out_size) {
    if (out_size == 0) return;
    out[0] = '\0';

    /* 1순위: X-Real-IP (Caddy가 trusted proxy 모드에서 항상 세팅) */
    if (httpd_req_get_hdr_value_str(req, "X-Real-IP", out, out_size) == ESP_OK
        && out[0] != '\0') {
        return;
    }

    /* 2순위: XFF 마지막 엔트리. Caddy가 append하므로 가장 nearest-hop이
     * 로컬에서 관찰한 공격자의 실제 remote_addr에 가까움. */
    char xff[128];
    if (httpd_req_get_hdr_value_str(req, "X-Forwarded-For", xff, sizeof(xff))
        == ESP_OK) {
        const char *last = strrchr(xff, ',');
        const char *start = last ? last + 1 : xff;
        while (*start == ' ') start++;  // trim
        size_t n = strlen(start);
        if (n >= out_size) n = out_size - 1;
        memcpy(out, start, n);
        out[n] = '\0';
    }
}

/* 공통 헬퍼: IP + UA 추출 후 Slack으로 보냄. */
static void audit_log(httpd_req_t *req, const char *label) {
    char ip[INET6_ADDRSTRLEN] = {};  // 46 바이트 — IPv6 전체 수용
    get_client_ip(req, ip, sizeof(ip));

    char ua[160] = {};
    httpd_req_get_hdr_value_str(req, "User-Agent", ua, sizeof(ua));

    /* 시각 표시는 Slack 자체 타임스탬프에 의존. 이 시스템은 SNTP를
     * 쓰지 않아 time(nullptr)이 uptime 기반이라 메시지 본문에 epoch를
     * 박으면 1970-01-01로 렌더링됨. 큐잉 지연은 수 초 수준이라 실용상
     * Slack 도착 시각과 이벤트 시각의 간극은 감수 가능. */
    char msg[256];
    snprintf(msg, sizeof(msg), "🔍 %s\n• IP: %s\n• UA: %s",
             label,
             ip[0] ? ip : "unknown",
             ua[0] ? ua : "unknown");

    slack_notifier_send(msg);
    /* ESP_LOGI에 IP만 남기고 UA는 서버 로그 크기 고려 생략. */
    ESP_LOGI(TAG, "audit: %s from %s", label, ip[0] ? ip : "unknown");
}
```

**중요**: `<lwip/sockets.h>`, `<lwip/inet.h>` include 복원 **불필요**. 기존 plan의 복원 요구는 철회. 이 프로젝트는 Caddy 뒤에서만 동작하므로 socket peer fallback은 가치 없음 (Simplicity 리뷰 수용).

#### 401 Rate Limiter (Deepen 반영 — 단일 IP + 전역 카운터)

기존 플랜의 8-slot LRU는 **Security CRITICAL C1**에 취약 (9개 IP 분산 공격 시 evict 회전으로 침묵 공격 가능). Simplicity 리뷰는 single entry 권고. 둘을 결합해 **단일 최근 IP entry + 전역 롤링 카운터**로 재설계.

**데이터 구조 (축소)**:
```cpp
/* 최근 같은 IP의 연속 실패 추적용. 본질적으로 "사람 오타 vs 공격자 구분"
 * 목적이라 가장 최근 1명의 IP만 추적하면 충분. */
static struct {
    char     ip[INET6_ADDRSTRLEN];  // 최대 46B
    uint32_t window_start_ms;
    uint32_t count;
    bool     alerted;
} s_latest_fail = {};

/* 분산 공격(여러 IP 회전) 대응용 글로벌 카운터. 60초 롤링 윈도우 내
 * 전체 401 수가 임계치 초과하면 "전역 폭증" 알림 1회. */
static struct {
    uint32_t window_start_ms;
    uint32_t count;
    bool     alerted;
} s_global_fail = {};

static SemaphoreHandle_t s_fail_mutex;  // xSemaphoreCreateMutex (우선순위 상속)

static constexpr uint32_t kWindowMs          = 10000;   // 10초 (단일 IP 윈도우)
static constexpr uint32_t kGlobalWindowMs    = 60000;   // 60초 (전역 윈도우)
static constexpr uint32_t kAlertThreshold    = 3;       // 같은 IP 3회
static constexpr uint32_t kGlobalThreshold   = 10;      // 전역 10회/분
static constexpr uint32_t kCooldownMs        = 60000;   // 알림 재발송 방지
```

메모리: `s_latest_fail` ~60B + `s_global_fail` ~12B + mutex ~80B = **약 150B** (기존 512B에서 감소).

**알고리즘** (`audit_401`):
```
xSemaphoreTake(s_fail_mutex)

now = esp_timer_get_time() / 1000  # μs → ms
ip = get_client_ip()

# ── 단일 IP 추적 ──
if s_latest_fail.ip != ip:
    # 새 IP. 덮어쓰기 (= LRU 1-slot).
    s_latest_fail = {ip, now, 1, false}
elif now - s_latest_fail.window_start_ms > kWindowMs + kCooldownMs:
    # 쿨다운 포함 완전 리셋
    s_latest_fail = {ip, now, 1, false}
else:
    s_latest_fail.count++

# 같은 IP 임계치 도달 & 아직 미알림
should_alert_ip = (s_latest_fail.count == kAlertThreshold && !s_latest_fail.alerted)
if should_alert_ip:
    s_latest_fail.alerted = true
    # 스냅샷 로컬 변수에 복사

# ── 전역 카운터 ──
if now - s_global_fail.window_start_ms > kGlobalWindowMs:
    s_global_fail = {now, 1, false}
else:
    s_global_fail.count++

should_alert_global = (s_global_fail.count == kGlobalThreshold && !s_global_fail.alerted)
if should_alert_global:
    s_global_fail.alerted = true

xSemaphoreGive(s_fail_mutex)  # ← mutex 여기서 해제

# ── mutex 밖에서 slack_notifier_send ──
if should_alert_ip:
    slack_notifier_send("⚠️ 연속 인증 실패 3회 (최근 Xs)\n• IP: ...\n• UA: ...")
if should_alert_global:
    slack_notifier_send("⚠️ 전역 인증 실패 10회/분 (분산 공격 의심)")
```

메시지 예:
- `⚠️ 연속 인증 실패 3회 (최근 7초)\n• IP: 198.51.100.7\n• UA: python-requests/2.32`
- `⚠️ 전역 인증 실패 10회/분 (분산 공격 의심)` (IP 목록 없이, 분산이라 표시 무의미)

**`check_auth` 수정** (Security M1 — timing side channel 완화):
```cpp
unauthorized:
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Doorman\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Unauthorized");
    audit_401(req);   // ← 응답 완료 후 호출. 공격자가 timing으로 audit 경로를
                      //    관측할 수 없게 함.
    return false;
```

**Mutex 정책**:
- `xSemaphoreCreateMutex()`로 **우선순위 상속 mutex** 명시 (Architecture 리뷰 수용)
- critical section: IP 비교 + 카운터 갱신만, **Slack send는 해제 후**
- 스냅샷은 지역 변수 (ip, ua, count, start_ms 등)로 mutex 안에서 복사

#### door_open_handler 정리

기존 `slack_notifier_send("🚪 문열림 요청 (via API)");` 호출 **제거**. 라인 337 주변 주석도 정리.

### Implementation Phases (Deepen 반영)

#### Phase 1: 헬퍼 함수 (audit_log + get_client_ip)

**Deliverables**:
- `get_client_ip(req, out, out_size)` 신설 — X-Real-IP 우선, XFF 마지막 엔트리 fallback
- `audit_log(req, label)` 신설 — IP + UA 포함 메시지 구성, `slack_notifier_send` 호출 (이벤트 시각은 Slack 자체 타임스탬프에 의존)
- 새 include: 없음 (lwip 복원 철회)

**Success criteria**:
- 빌드 통과
- 단위 smoke: 수동 curl로 handler 1개에 임시로 `audit_log(req, "test")` 추가해서 Slack 수신 확인 후 되돌리기

**Estimated effort**: 20분

#### Phase 2: 7개 handler에 audit_log 호출 + door_open 알림 제거

**Deliverables**:
- `check_auth` 통과 직후에 `audit_log(req, "<라벨>")` 한 줄 삽입 — 7곳:
  - `ws_handler` — "로그 스트리밍 시작"
  - `ota_upload_handler` — "펌웨어 업로드"
  - `auth_update_handler` — "로그인 계정 변경"
  - `wifi_update_handler` — "WiFi 설정 변경"
  - `slack_update_handler` — "Slack 웹훅 변경"
  - `reboot_handler` — "기기 재부팅"
  - `coredump_handler` — "크래시 덤프 다운로드"
- `door_open_handler`의 `slack_notifier_send("🚪 문열림 요청 (via API)")` 호출 **제거**
- 관련 주석 정리 (특히 door_open_handler의 의도 설명 업데이트)

**Success criteria**:
- 빌드 통과
- 7개 경로 각각 정상 curl 호출 → Slack 🔍 메시지 수신 (IP + UA + 시각 포함)
- `/api/door/open` 호출 → Slack 알림 **없음**
- `GET /`, 조회성 endpoint → 알림 없음
- **잘못된 creds로 `/ws?token=...` 등 호출 → 🔍 알림 없음** (Security L1 해결 검증)

**Estimated effort**: 15분

#### Phase 3: 401 Rate Limiter (단일 IP + 전역 카운터)

**Deliverables**:
- `s_latest_fail`, `s_global_fail` static 변수 정의
- `s_fail_mutex = xSemaphoreCreateMutex()` (우선순위 상속 명시)
- `audit_401(req)` 함수 — 위 pseudocode 그대로
- `check_auth`의 `unauthorized:` 분기에서 **401 응답 전송 후** `audit_401(req)` 호출 (timing side channel 방지)
- 상수 5개 정의 (kWindowMs, kGlobalWindowMs, kAlertThreshold, kGlobalThreshold, kCooldownMs)

**Success criteria**:
- 빌드 통과
- 같은 IP 3회 연속 wrong creds → `⚠️ 연속 인증 실패 3회` 1건
- 4~10회 추가 → 알림 없음 (쿨다운)
- 60초 후 실패 → 새 알림
- 여러 IP 10회 누적 (curl X-Forwarded-For 스푸핑으로 시뮬레이션 어려우면 Caddy 거치지 않고 로컬 IP로 테스트) → `⚠️ 전역 인증 실패 10회/분` 1건

**Estimated effort**: 30분

#### Phase 4: 실기기 검증

**Deliverables**:
- 로컬 빌드 후 OTA: `./scripts/ota_upload.sh doorman.cartanova.ai build/doorman.bin potados <pass> -v`
- 재부팅 확인 + safe_mode=false
- 8개 이벤트 수동 트리거
- Slack MCP로 메시지 수신 검증
- **첫 audit_log 호출 후 httpd task HWM 로깅** (Performance 리뷰 권고) — 1회만 ESP_LOGI로

**Success criteria**:
- 각 audit 이벤트 Slack 수신 (7 × 🔍 + 2 × ⚠️ 시나리오)
- `/api/door/open` 호출 시 알림 **없음**
- monitor 로그 heap 이상 없음, internal_free 변화 < 1KB
- httpd task HWM ≥ 1KB 여유 (audit_log 스택 +464B 수용 확인)

**Estimated effort**: 15분 (관찰 포함)

## Acceptance Criteria

### Functional Requirements (Deepen 반영)
- [x] 7개 handler 각각 `check_auth` 통과 직후 `audit_log(req, "<라벨>")` 호출 (ws_handler는 토큰 검증 직후)
- [x] **인증 실패 시에는 audit_log 호출되지 않음** (Security L1 검증 — 코드상 보장)
- [x] `audit_log`가 `🔍 <label>\n• IP: <x>\n• UA: <y>` 형식으로 전송 (이벤트 시각은 Slack 자체 타임스탬프에 의존)
- [x] `audit_401`이 단일 IP + 전역 카운터 기반 rate limiter로 작동
- [ ] 같은 IP 3회 연속 wrong → 🔍 1건 + 60초 쿨다운 (Phase 4 실기기 실측)
- [ ] 분산 IP 10회/분 → 🔍 "전역 폭증" 1건 (Security C1 검증, Phase 4 실측)
- [x] `check_auth`의 401 분기에서 **응답 전송 후** `audit_401(req)` 호출 (Security M1)
- [x] `POST /api/door/open` 호출 시 Slack 알림 **전송되지 않음**
- [x] 제외 라우트 (devices/config·delete, pairing/toggle, auto-unlock/toggle, 조회성 GET들) 호출 시 알림 없음
- [x] `Route` struct **변경 없음** (Alt A 반영)

### Non-Functional Requirements
- [x] 내부 RAM 사용 증가 ≤ **300B** (rate limiter ~150B + mutex ~80B + audit_log 코드 .text는 flash). 8-slot 배열 폐기로 크게 감소.
- [x] Flash 크기 증가 ≤ 8KB — 실측 +944B
- [ ] audit_log handler task stack HWM 여유 ≥ 1KB (Phase 4에 실측 로깅 포함)
- [x] audit_401 critical section < 100μs (단일 IP 비교 + 카운터 갱신)
- [x] `s_fail_mutex`는 `xSemaphoreCreateMutex()` (우선순위 상속)
- [x] `get_client_ip`는 X-Real-IP 우선, 실패 시 XFF 마지막 엔트리. socket peer 조회 **사용 안 함**

### Quality Gates
- [x] `idf.py build` 통과
- [x] host_test 기존 49/49 통과 (audit 로그는 target-only라 host_test 영향 없음)
- [ ] 실기기 OTA 후 정상 부팅
- [ ] general-purpose agent OTA 안전성 감사 통과
- [ ] monitor WS 로그에서 heap 이상 없음
- [ ] notifier_task stack hwm 기존 수준 유지 (~4500B 여유)

## Alternative Approaches Considered (Deepen 이후 재정리)

### A. 각 handler에 `audit_log()` 직접 호출 ← **채택 (Deepen 결과)**
- **장점**: Route struct 변경 없음. 풀 고갈 리스크 무효. 인증된 요청만 audit. 데이터 흐름 명시적 (CLAUDE.md 합치).
- **단점**: 7 지점 수동 추가. 누락 위험은 grep/PR 리뷰로 보완.
- **채택 사유**: Architecture + Simplicity + Security 세 리뷰가 수렴. 특히 Security L1(thunk auth-blind)이 결정적.

### B. Route struct에 `audit_label` + `audit_thunk` + 풀 ← **초기 제안, Deepen에서 기각**
- **장점**: audit 의무가 Route 테이블에 선언적으로 표현, 누락 방지.
- **단점**: (1) 풀 고갈 시 register_routes 실패 → 부팅 경로 위험 (R1), (2) thunk가 `check_auth` 전에 실행되어 **인증 실패 요청도 audit** 유발, (3) Simplicity 관점 "거짓 단순함" — 라벨이 handler 본문에서 보이지 않음.
- **기각 사유**: 문제 3개 모두 Alt A에서 자동 해소.

### C. `req->user_ctx`에 `&Route` 자체를 넘기는 thunk
- Architecture 리뷰가 B의 고갈 리스크를 없애는 수정안으로 제시.
- **장점**: B의 풀 고갈(R1)은 해결.
- **단점**: Security L1(auth-blind) 여전. 코드 LOC 증가.
- **기각 사유**: Alt A가 L1까지 해결하므로 C도 불필요.

### D. Slack chat.postMessage (봇 토큰)로 메시지 쓰레드화
- 동일 세션/공격자 시도를 Slack 스레드로 묶기.
- **장점**: 채널 깨끗. 침입 시도를 한 스레드로 추적 용이.
- **단점**: 복잡도 3배(ts 저장, thread_ts 관리), 봇 scope 승인, 토큰 관리.
- **기각 사유**: 본 기능엔 과도. 향후 별도 검토.

### E. 로컬 NVS에 audit 로그 round-robin 저장 (Security H2 권고)
- Slack 채널이 공격자 통제 하에 들어갈 경우 후방 증거 역할.
- **YAGNI 보류**: 이번 스코프 초과. Future Considerations에 보존.

## Success Metrics

- **프라이버시 개선**: 한 주간 Slack 히스토리에서 "문열림 요청" 메시지 수 → **0건**
- **탐지성**: 테스트용 401 폭격 (50회 연속) → Slack 메시지 **1건** (스팸 없음)
- **운영 가시성**: 관리 UI 웹 접근 시마다 로그 스트리밍 시작 알림 수신 (체감 확인)

## Dependencies & Prerequisites

### Dependencies
- 기존 `slack_notifier` 모듈 변경 없이 재사용
- ESP-IDF httpd `user_ctx` 사용 (공식 API)
- 브랜치 베이스: 현재 `main` (`5e23781` 이후)

### Prerequisites
- Slack webhook URL NVS에 설정되어 있어야 알림 수신 (미설정이면 내부 drop)
- WiFi 연결 상태 (STA 모드) — SoftAP에서는 audit 대상 없음

## Risk Analysis & Mitigation

### R1 (Solved): ~~AuditContext 풀 고갈~~ **Deepen에서 Alt A로 전환하며 소거**. 풀 자체가 없음.

### R2 (Low): Rate limiter 동시성
- **시나리오**: 여러 httpd worker가 동시에 `audit_401` 호출 → 단일 entry race.
- **완화**: `xSemaphoreCreateMutex()`(우선순위 상속). critical section: IP 비교 + 카운터 갱신만. Slack send는 mutex 밖.
- **검증**: Phase 3 테스트에 "병렬 3개 curl로 동시 401" 케이스 포함.

### R3 (Low): IP/UA 유출
- **시나리오**: Slack 로그에 IP/UA가 남는 것 자체가 유출 리스크.
- **완화**: audit 본질이 식별이므로 의도된 동작. Slack 채널 접근 권한 관리가 외부 통제.
- **정책**: 이 프로젝트 범위 밖 (워크스페이스 admin 영역).

### R4 (Low): 401 rate limit으로 인한 slow brute-force 지연 탐지
- **시나리오**: 공격자가 임계치 아래 빈도(분당 2회)로 시도 → 조용.
- **수용**: 분당 2회 = 10자 비번 기준 10^14 초 ≈ 수백만 년. 실전 무의미.
- **완화(향후 고려)**: 일/주 누적 카운터(예: 24시간 30회 초과 시 별도 알림). Open Question에 기록.

### R5 (Low): SoftAP 모드에서 audit 예외
- **시나리오**: SoftAP 모드로 빠졌을 때 `/api/wifi/setup` 공격당해도 audit 없음.
- **수용**: SoftAP는 초기 provisioning 전용, auth 없는 open 엔드포인트. 여기까지 보호하려면 별도 설계 필요. 이번 범위 밖.

### R6 (Medium, Deepen 추가): XFF 스푸핑 가능성
- **시나리오**: 내부망 접근 가능한 공격자가 X-Real-IP 헤더를 직접 세팅해 가짜 IP 기록.
- **완화**: Caddy는 client 요청의 X-Real-IP를 **덮어쓰므로**(`reverse_proxy` 기본 동작) 외부 HTTPS 트래픽엔 안전. 단 내부 LAN에서 ESP32로 직결 접근 가능한 공격자는 조작 가능.
- **정책**: Caddy 설정 검증을 Prerequisites에 포함. 내부망 직결 공격은 물리 보안 영역으로 치환.

### R7 (Medium, Deepen 추가): Webhook 변경 race
- **시나리오**: 탈취된 creds로 `POST /api/slack/update`를 공격자 채널로 교체. 원래 채널에 알림이 도달하기 전에 swap 완료 가능성.
- **완화 (부분)**: `slack_notifier_update_url`의 mutex 내 atomic swap + notifier consumer가 mutex 안에서 url strdup 스냅샷 → **notifier 큐에 이미 들어간 메시지는 구 URL로 전송**. 다만 audit_log → queue send 직전 cycle에서 producer가 newly swapped url로 보낼 레이스는 남음.
- **완벽 해결 (보류)**: `slack_notifier_update_url`이 변경 직전 동기로 old URL에 경고 메시지 전송하도록 인터페이스 확장. 이번 스코프 초과.
- **수용**: 일반 single-user 도어락 위협모델에서 허용 가능. Open Question에 기록.


## Resource Requirements

- **개발 시간**: ~95분 (Phase 1 20분 + Phase 2 15분 + Phase 3 30분 + Phase 4 15분 + 감사/review 15분)
- **메모리**: 내부 RAM ~150B (단일 IP entry + 전역 카운터 + mutex). 풀/슬롯 배열 없음
- **Flash**: ~8KB (코드 증가)
- **인프라**: 기존 Slack webhook 활용, 변경 없음

## Future Considerations

### 잠재 확장
- **다중 사용자 시**: session ID별 audit + username 포함 가능 (current basic auth는 단일 user)
- **감지 정교화**: audit 패턴 학습 — "평소 관리자가 오는 시간대 vs 비정상" 식별 가능
- **Audit log UI**: Slack 외에 로컬 NVS에 round-robin으로 마지막 N건 저장, 웹 UI에서 조회
- **외부 SIEM 연동**: Syslog, Elasticsearch 등으로 내보내기 (지금은 Slack이 SIEM 역할)

### 안 할 것 (YAGNI)
- Slack chat.postMessage로 thread화 (복잡도 대비 가치 낮음)
- Username 포함 (브레인스토밍에서 거절됨)
- 세션 기반 dedup (rate-limit만으로 충분)

## Documentation Plan

- 이 플랜 자체 (`docs/plans/`)
- 브레인스토밍 문서는 이미 존재 (`docs/brainstorms/2026-04-23-audit-log-sousveillance-brainstorm.md`)
- 구현 후 `README.md` 또는 별도 문서에 "Audit 로그 정책" 섹션 추가 검토 (Slack 채널 운영자가 참고)
- `docs/solutions/` 에는 **구현 중 사고가 있으면** 추가 (평상시에는 불필요)

## References

### Internal (현재 코드)
- `main/http/server.cpp:910` — 현 `Route` struct (Alt A 채택으로 **변경 없음**)
- `main/http/server.cpp:917` — `register_routes` (Alt A 채택으로 **변경 없음**)
- `main/http/server.cpp:58` — `check_auth` (unauthorized 분기에 **401 응답 후** audit_401 호출 추가)
- `main/http/server.cpp:326` — `door_open_handler` (slack_notifier_send 제거 대상)
- 7개 특권 handler (`ws_handler`, `ota_upload_handler`, `auth_update_handler`, `wifi_update_handler`, `slack_update_handler`, `reboot_handler`, `coredump_handler`) — `check_auth` 통과 직후 `audit_log(req, "<라벨>")` 한 줄 삽입
- `main/slack/notifier.{h,cpp}` — 재사용, 변경 없음
- 삭제 커밋 `5e23781` — `get_client_ip`가 여기서 제거됐었음 (복원 reference)

### Institutional Learnings
- `docs/solutions/runtime-errors/psram-task-stack-bricks-device.md` — 이번 변경은 task stack 추가 없음, heap 변화 미미
- `docs/solutions/runtime-errors/sm-task-stack-overflow-cascade.md` — nullptr 가드 원칙 유지 (audit_401, audit_log 모두 가드)
- 메모리 정책 `feedback_psram_stacks.md` — audit 관련 새 태스크 없음, 영향 없음

### Brainstorm
- `docs/brainstorms/2026-04-23-audit-log-sousveillance-brainstorm.md` — 이 플랜의 의사결정 근거

### External
- ESP-IDF HTTP server `user_ctx`: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/esp_http_server.html
- FreeRTOS `xTaskGetTickCount`: 표준 API, 별도 문서 불요

## Test Plan

### Phase 3 (401 rate limiter) 시나리오

| 케이스 | 동작 | 기대 |
|---|---|---|
| 정상 auth 통과 | POST with correct creds | audit_401 호출 안 됨 (check_auth return true) |
| 잘못된 creds 1회 | POST with wrong | count=1, 알림 X |
| 잘못된 creds 3회 연속 (10초 내) | POST × 3 | 3회째 "⚠️ 연속 3회" 알림 1건 |
| 추가 실패 (4~10회) | POST × 7 | 알림 X (alerted=true) |
| 60초 대기 후 실패 | POST | 카운터 리셋 후 count=1, 알림 X |
| 60초 후 다시 3회 | POST × 3 | 새 알림 1건 |
| 다른 IP 교차 실패 | IP_A 2회 → IP_B 1회 → IP_A 1회 | 단일 entry 덮어쓰기로 각 count가 임계치 미달 → 알림 X. (브레인스톰의 "사람 오타 구분" 의도상 수용) |
| 분산 IP 10회/분 | 10개 IP 각 1회 (60초 내) | `⚠️ 전역 인증 실패 10회/분 (분산 공격 의심)` 1건 (Security C1 검증) |
| 전역 알림 후 추가 분산 실패 | 위 상태에서 IP_11 추가 | 60초 윈도우 내 추가 알림 없음 (alerted=true) |

### Phase 4 (실기기) smoke test
```bash
# 정상 접근 — 각각 알림 수신 확인
curl -u potados:pass GET  https://doorman.cartanova.ai/ws?token=<t>           # 🔍 로그 스트리밍 시작
curl -u potados:pass POST https://doorman.cartanova.ai/api/firmware/upload ...# 🔍 펌웨어 업로드
curl -u potados:pass POST https://doorman.cartanova.ai/api/auth/update ...    # 🔍 로그인 계정 변경
curl -u potados:pass POST https://doorman.cartanova.ai/api/wifi/update ...    # 🔍 WiFi 설정 변경
curl -u potados:pass POST https://doorman.cartanova.ai/api/slack/update ...   # 🔍 Slack 웹훅 변경
curl -u potados:pass POST https://doorman.cartanova.ai/api/reboot             # 🔍 기기 재부팅
curl -u potados:pass GET  https://doorman.cartanova.ai/api/coredump           # 🔍 크래시 덤프 다운로드

# 알림 없어야 하는 경로
curl -u potados:pass POST https://doorman.cartanova.ai/api/door/open          # 🔕 (서비스 이용)
curl -u potados:pass GET  https://doorman.cartanova.ai/api/devices            # 🔕 (조회)

# 401 rate limiter
for i in 1..5; do curl -u wrong:wrong POST https://doorman.cartanova.ai/api/door/open; sleep 1; done
# → ⚠️ 연속 3회 알림 1건, 4/5번째는 조용
```

Slack MCP로 `#doorman` 채널 읽어서 기대값과 일치 검증.

## Rollback Plan

- 전체 PR을 `git revert`하면 이전 상태(`5e23781`)로 복귀 — `🚪 문열림 요청 (via API)` 메시지 복구됨
- sdkconfig 변경 없음이라 OTA 재부팅만으로 충분
- NVS namespace 변경 없음이라 설정 보존

## Research Insights (Deepen 결과 원본 요약)

### Architecture (architecture-strategist)
- **Route struct 확장 vs `user_ctx=&Route`**: 플랜의 B안을 C안(Route 자체 user_ctx)으로 간소화 가능 → 풀 고갈(R1) 소거. 이후 Alt A 채택으로 둘 다 무의미.
- `check_auth` 분리 리팩토링(인증과 401 응답 분리)은 SRP 관점 깔끔하지만 **이번 범위 초과**. 별도 백로그로 보류.
- `main/audit/` 도메인 폴더 분리는 향후 syslog/Vault 연동 시 고려 — 지금은 YAGNI.

### Security (security-sentinel)
- **CRITICAL C1** 분산 IP 침묵 공격 → 전역 카운터로 해소 (플랜 반영)
- **HIGH H1** XFF 스푸핑 → X-Real-IP 우선으로 변경 (플랜 반영)
- **HIGH H2** Webhook 변경 race → R7로 기록, 완벽 해결은 보류
- **HIGH H3** door/open 무흔적 → **반영하지 않음**. 브레인스토밍에서 이미 확정한 프라이버시 우선 결정을 번복할 이유 없음. 리뷰어는 공격 표면으로 인식했지만 이 프로젝트 위협 모델에선 의도된 수용.
- **MEDIUM M1** Timing side channel → audit_401 호출 순서 이동 (플랜 반영)
- **MEDIUM M2** 부팅 실패 경로 → Alt A 채택으로 자연 해소
- **LOW L1** Thunk auth-blind → Alt A 채택으로 자연 해소
- **LOW L2** Webhook URL 로그 prefix → `len=%zu`로 변경 (Phase 1 포함)

### Performance (performance-oracle)
- Rate limiter 기존 ~500B → Deepen 후 ~150B로 축소 (단일 IP + 전역 카운터)
- Mutex contention 실질 0 (httpd 단일 task 가정)
- **httpd task stack HWM 실측** 권고 → Phase 4에 포함
- `audit_thunk` indirection 오버헤드: Alt A 채택으로 무효

### Simplicity (code-simplicity-reviewer)
- "Alt A가 처음부터 단순했다" — **수용** (Deepen의 가장 큰 구조 변경)
- 8-slot LRU → single entry 권고 → 반영 (전역 카운터 추가는 Security 요구)
- XFF + socket peer fallback 과잉 → X-Real-IP + XFF only로 축소
- Phase 4 상세 curl 목록 축소 — 반영 (핵심 검증만 남김)

### Best Practices (best-practices-researcher)
- **10초/3회/60초**: OWASP 엄격 기준과 정합. 조정 불필요.
- **Slack rate limit**: 초당 1건. 60초 쿨다운은 알림 피로 관리용이지 Slack 한도와 무관.
- **이벤트 시각 본문 토큰** `<!date^ts^...>`: 재전송 지연 대비. 반영.
- **UA truncation 80~120자**: 현재 160B 버퍼 유지 (IDF httpd 측이 자동 trunc)
- **GDPR minimization**: single-user legitimate interest로 정당화. Slack 장기 저장은 별도 이슈(rotation).

## References & Deepen Agent Outputs

### Internal
- `main/http/server.cpp:58,910,917` — check_auth, Route struct, register_routes
- `main/slack/notifier.cpp:322` — slack_notifier_send 재사용 지점
- commit `5e23781` — `get_client_ip` 제거 commit (이번 플랜은 복원 **철회**)

### External (Deepen)
- [ESP-IDF HTTP server `user_ctx`](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/esp_http_server.html)
- [OWASP Authentication — Lockout / Throttling](https://cheatsheetseries.owasp.org/cheatsheets/Authentication_Cheat_Sheet.html)
- [OWASP Blocking Brute Force Attacks](https://owasp.org/www-community/controls/Blocking_Brute_Force_Attacks)
- [Slack API — Rate Limits (초당 1건)](https://docs.slack.dev/apis/web-api/rate-limits/)
- [GDPR log minimization](https://last9.io/blog/gdpr-log-management/)
- [Pangea tamper-evident audit logging](https://pangea.cloud/securebydesign/secure-audit-logging-overview/)
