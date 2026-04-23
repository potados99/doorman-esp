---
title: Slack 알림을 audit 로그(sousveillance)로 전환
date: 2026-04-23
status: ready-for-plan
---

> **📌 플랜 Deepen 단계에서 일부 "확정" 항목이 번복됨** (2026-04-23)
> - 구현 구조: Route 테이블 자동 dispatch → **각 handler에 `audit_log()` 한 줄 박기 (Alt A)**.
>   사유: thunk가 `check_auth` **전에** 실행되어 401 요청에도 🔍 알림이 나가는 auth-blind 버그 발견.
> - 401 rate limiter: 8-slot LRU 링 버퍼 → **단일 IP entry + 전역 60초 카운터**.
>   사유: 9개 IP 분산 공격 시 LRU evict 회전으로 침묵 공격 가능 (Security C1).
> - IP 추출: XFF 우선 + getpeername() fallback → **X-Real-IP 우선 + XFF 마지막 엔트리 fallback**.
>   사유: XFF append 체인 스푸핑, Caddy가 X-Real-IP를 trusted로 덮어씀.
> 상세는 `docs/plans/2026-04-23-feat-audit-log-sousveillance-plan.md` Enhancement Summary 참조.

# Slack 알림 → audit 로그 (sousveillance)

## What We're Building

doorman-esp의 Slack 알림 정책을 **180° 뒤집는다**. 이전엔 사용자의 **행동**(문 열기)을 기록했다면, 이제는 시스템 **관리·감시 행위**(설정 변경, 로그 열람, 인증 실패)를 기록한다.

- **서비스 이용(문 열기)** → 알림 없음. 근태성 데이터 누적 차단.
- **관리 특권 행사** → 🔍 Slack 알림 (IP + UA 포함)
- **비인가 시도(401)** → ⚠️ Slack 알림 (연속 감지 후 요약 1회)

## Why This Approach

### 철학 — "들여다보려는 사람이 부담을 감수한다"

권력의 방향을 뒤집는다. 관리 권한을 쓰는 사람 자신이 자동으로 기록에 남고, 그걸 알고 행동한다. 관리자 자기 견제 + 비인가 접근 탐지를 한 체계로 해결.

### 이전 문제 (서비스 이용 로그)

- 문 열기 API 호출 시 IP + UA + 시간이 영구 기록 → **근태성 데이터 누적** (누가 언제 어디서 드나들었나)
- 기기 핑거프린트 위험 (UA로 개인 디바이스 식별)
- 보안적 추가 가치는 낮음 (실제 공격자는 VPN)

### 포기한 것

원래 설계 목적이었던 "사무실 혼자 있는데 누가 API로 문 열어서 깜놀" 방지 시나리오. 프라이버시 우선으로 의식적 포기.

## Key Decisions

### 이벤트 라벨 테이블 (확정)

| 조건 | 라벨 | 이모지 |
|---|---|---|
| `GET /ws` | 로그 스트리밍 시작 | 🔍 |
| `POST /api/firmware/upload` | 펌웨어 업로드 | 🔍 |
| `POST /api/auth/update` | 로그인 계정 변경 | 🔍 |
| `POST /api/wifi/update` | WiFi 설정 변경 | 🔍 |
| `POST /api/slack/update` | Slack 웹훅 변경 | 🔍 |
| `POST /api/reboot` | 기기 재부팅 | 🔍 |
| `GET /api/coredump` | 크래시 덤프 다운로드 | 🔍 |
| 모든 401 응답 | 인증 실패: `<METHOD> <PATH>` | ⚠️ |

### 제외된 경로 (프라이버시)

- `POST /api/door/open` — 서비스 이용
- `POST /api/devices/config`, `/api/devices/delete` — 개별 기기 일상 조작
- `POST /api/pairing/toggle`, `POST /api/auto-unlock/toggle` — 일상 토글
- `GET /`, `/api/info`, `/api/stats`, `/api/devices`, `/api/*/status` — 조회·UI 자동 폴링

### 메시지 포맷

```
🔍 <라벨>
• IP: <client_ip>
• UA: <user_agent>

⚠️ 인증 실패: <METHOD> <PATH>
• IP: <client_ip>
• UA: <user_agent>

⚠️ 연속 인증 실패 N회 (10초 내)
• IP: <client_ip>
• UA: <user_agent>
```

- IP: X-Forwarded-For 우선, 없으면 getpeername()
- UA: 160B 버퍼 (이전과 동일, 긴 브라우저 UA 수용)
- Username은 **절대 포함 안 함** (오타로 본인 노출 방지, 공격 패턴 분석도 포기)
- Password도 당연히 포함 안 함

### 구현 구조 (확정)

**Route struct에 `audit_label` 필드 추가 → `register_routes`가 자동 dispatch**

```cpp
struct Route {
    const char *path;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
    bool is_websocket;
    const char *audit_label;  // nullptr이면 audit 안 함
};
```

- 새 handler 추가 시 audit 여부를 라우트 테이블 한 곳에서 결정 → **누락 방지**
- register 시 audit_label != nullptr이면 thunk handler로 감싸서 pre-call에 audit_log() 실행
- 각 handler 함수 내부는 변경 없음 (기존 로직 그대로)

대안이었던 "각 handler에 `audit_log()` 호출 박기"는 유지보수 시 누락 리스크 있어 비채택.

### 401 rate limiting (확정)

**연속 감지 후 요약 1회** 전략:
- 최근 IP + 마지막 실패 timestamp + 카운트 링 버퍼 (5~8 엔트리, 내부 RAM ~64B)
- **윈도우**: 10초 (기본 제안, 조정 가능)
- **임계치**: 3회 (기본 제안)
- **쿨다운**: 첫 알림 후 60초간 같은 IP 추가 401은 카운트만 쌓고 조용
- 쿨다운 종료 시점에 누적 카운트 > 임계치면 "연속 N회" 메시지 전송 후 리셋

간단 pseudocode:
```
on 401 from IP:
  entry = find_or_evict(IP)
  entry.count++
  entry.last_ts = now()
  if first_in_window and count == threshold:
    send_alert(f"연속 인증 실패 {count}회 (10초 내)")
    entry.cooldown_until = now + 60s
```

### IP + UA를 Slack에 포함하는 정당성

서비스 이용 로그에서는 프라이버시 침해였지만, audit 로그에서는 **식별이 본질적 목적**. "들여다보려는 사람이 누구인지" 가 audit의 핵심 질문이라 오히려 **빠지면 무의미**. 철학적으로 정합적.

## Open Questions

1. **401 rate limiter 세부 파라미터** — 위에 제시한 기본값(10초 / 3회 / 60초 쿨다운)이 실제 환경에 적절한지 실측 필요. 공격 빈도 / 정당한 오타 빈도 고려.

2. **notifier 큐가 OTA 도중 꽉 찰 때** — 현 정책(non-blocking send + drop) 유지로 결정. OTA handler 자체가 audit 알림을 트리거(펌웨어 업로드 시작)하고, 업로드가 수십 초 걸리는 동안 다른 audit이 쌓이면 일부 drop. 시스템 영향 없음, 알림 소실만. **수용**.

3. **다중 사용자 확장성** — 현재 single-user basic auth. 미래 per-user session 도입 시 "누가 감시했나" 식별성 개선 가능. 지금은 YAGNI.

4. **기존 서비스 이용 로그 코드 제거 여부** — `door_open_handler`의 `slack_notifier_send("🚪 문열림 요청 (via API)")` 호출 제거 확정. 문 열기 경로에서 audit 아예 없음.

## Resolved Questions

- ✅ **401 DoS 대응**: 연속 감지 후 1회 요약
- ✅ **username 포함 여부**: 포함 안 함 (IP/UA만)
- ✅ **구현 구조**: Route 테이블 확장 (자동 dispatch)
- ✅ **IP/UA 포함 정당성**: audit 본질에 부합, 포함 유지
- ✅ **원래 "깜놀 방지" 시나리오**: 포기 (audit only)
- ✅ **이모지 전략**: 2가지만 (🔍 인가 / ⚠️ 비인가)
- ✅ **slack_notifier 코어 API**: 변경 불필요. 메시지 내용만 다름.

## References

- 이전 plan: `docs/plans/2026-04-22-refactor-module-layout-and-slack-notifier-plan.md`
- 이전 커밋 `5e23781`: 문열기 메시지를 한 줄로 축소 (철학 전환 직전 상태)
- 관련 코드: `main/http/server.cpp` (Route struct, register_routes, check_auth), `main/slack/notifier.{cpp,h}` (slack_notifier_send)
