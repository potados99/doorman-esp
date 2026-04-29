---
title: "feat: 프론트엔드 Preact + TanStack Query 마이그레이션"
type: feat
status: active
date: 2026-04-29
brainstorm: docs/brainstorms/2026-04-29-frontend-preact-tanstack-migration-brainstorm.md
---

# 프론트엔드 Preact + TanStack Query 마이그레이션

## Overview

`frontend/index.html` (1355줄, vanilla XHR + 수동 DOM patch)을 동일한 디자인/동일한 백엔드 API/동일한 임베드 정책을 유지한 채 **Preact + htm + @tanstack/preact-query + @preact/signals** 스택으로 재작성. `frontend/setup.html`(SoftAP 페어링 페이지)은 손대지 않는다.

작업 결과물: `frontend/index_v2.html` 신규 + `tests/frontend/` 테스트 인프라. mock e2e 5개 시나리오 + 1개 회귀 가드 통과 시 CMakeLists embed 타깃을 swap.

부수효과: 2026-04-29 발견된 401 init race(매 fresh login마다 false-positive audit 알림)를 `useQuery({ enabled })` 의존성 체이닝으로 원천 차단.

## Problem Statement

### 1. DX 부채

- 단일 파일 1355줄. JS 파트 826줄(line 527-1353).
- 글로벌 변수 다수: `deviceData`, `cardState`, `lastBtTime`, `detectCount`, `wsBuffer`, `wsEpoch`, `syncing`, `wsToken`, `reconnectDelay`, `ws`, `host`, `rawLogs` 등.
- 수동 DOM 조작: `document.getElementById('card-' + macToId(mac))` 패턴 반복. WS 메시지 도착 시 어느 카드/필드가 갱신되는지 추적 어려움.
- 8개 DOM 패치 함수(`updateCardDisplay`, `updateCardRssi`, `updateCardState`, `onDevicePaired`, `applyFullState`, `renderDeviceCards`, `renderPairEvents`, `addPairEvent`) 사이 데이터 흐름이 암묵적.
- 신규 기능 추가 시 인지 부하 + 회귀 위험 누적.

### 2. 401 init race (2026-04-29 진단)

- 페이지 init JS가 `loadAutoUnlockStatus()` + `wsConnect()→/api/ws-token` + `(IIFE)→/api/info` 3개 XHR을 인증 다이얼로그 통과 전에 병렬 발사.
- 144ms 안에 3건 401 → `audit_401`이 임계 도달 → 매 fresh login마다 Slack false-positive 알림.
- 동시에 진단 중 `ua_present=0 ip_present=0`이라는 또 다른 미스터리(브라우저 헤더 정상이어야 함) 발견 — 이건 별도 추적이지만, init race는 클라이언트 측 직렬화로 즉시 해결 가능.

### 3. 패턴 검증 완료

`frontend/preact_practice.html` (171줄)에서 ESM CDN + htm + tanstack-query + IIFE 모듈 패턴이 doorman 같은 임베디드 single-file SPA에 적합한지 학습본으로 검증 완료. 실전 적용 단계.

## Proposed Solution

빌드 도구 없이 단일 HTML 파일에 ESM CDN으로 의존성을 import하는 패턴(preact_practice.html 기준)을 그대로 확장. 책임을 두 계층으로 명확히 분할:

| 계층 | 도구 | 담당 데이터 | 갱신 주기 |
|---|---|---|---|
| **HTTP 스냅샷** | `useQuery` / `useMutation` | `/api/info`, `/api/devices`, `/api/auto-unlock/status`, `/api/pairing/status`, `/api/ws-token` 응답 | 명시적 fetch |
| **WS 푸시 telemetry** | per-device `signal` Map | RSSI, state(detecting/present/absent/unlock), pair event stream, log line buffer, sysStats | 30Hz~ |

두 계층은 **같은 필드를 공유하지 않는다** — info 같은 메타데이터는 HTTP만, RSSI 같은 텔레메트리는 WS만 만짐. 책임 경계 명확.

## Technical Approach

### Architecture

```
index_v2.html
├── <head>
│   └── <style> ... </style>          ← 기존 index.html line 4-366 통째 복사
├── <body>
│   └── <div id="app"></div>
└── <script type="module">
    ── 의존성 import (esm.sh) ──
    preact, preact/hooks, htm, @tanstack/preact-query, @preact/signals

    ── 글로벌 ──
    const html = htm.bind(h)
    const queryClient = new QueryClient()

    ── 모듈(IIFE) ──
    api    : fetch wrapper, endpoint wrapper 16개
    ws     : 연결 매니저, line accumulator, 메시지 라우터, epoch 가드
    state  : signal map (devices), pair event 버퍼, log 버퍼, sysStats
    queries: useInfo, useDevices, useAutoUnlock, usePairingStatus, useWsToken
    mutations: useDoorOpen, useAutoUnlockToggle, usePairingToggle, useAuthUpdate, ...
    components:
      App, Header, Tabs, MainTab, ManageTab,
      DeviceCard, PairModal, DetailModal, LogOverlay, OtaUploader,
      Section, Button, Pill, ...
    ── render ──
    render(html`<${App}/>`, document.getElementById('app'))
</script>
```

`preact_practice.html`의 IIFE 모듈 패턴 + 코드 접기 ergonomics 유지. 모듈 정의 순서 의존성은 알려진 한계로 수용.

### Key Decisions (brainstorm 확정 사항)

| # | 결정 | 근거 |
|---|---|---|
| D1 | **DX 개선이 1순위 목표** | 신규 기능/race fix는 부수효과 |
| D2 | **병렬 파일 (`index_v2.html`)** | 중간 상태 0, git revert 즉시 |
| D3 | **Hybrid: signal(WS 30Hz) + query(HTTP 스냅샷)** | 30Hz를 setQueryData로 처리하면 query 구독자 전부 re-render. signal은 per-device fine-grained — 현재 vanilla 코드의 per-card DOM patch 동등. |
| D4 | **`useQuery({ enabled })` 체인으로 401 race 차단** | tanstack 표준 의존성 체이닝, 코드 증가 ~3줄, false-positive 알림 원천 차단 |
| D5 | **golden path 5개 + 회귀 1개 e2e** | mock + playwright. 실기기 검증 생략 — API 불변, client만 교체이므로 mock 동등성으로 충분 |
| D6 | **빌드 도구/TS/CSS 프레임워크 도입 없음** | preact_practice.html 정책 유지. 기존 `<style>` 통째 복사. |

### WS 메시지 라우팅 명세

현재 `frontend/index.html:1141-1185`의 라우팅을 그대로 보존(서버 측 wire 포맷 변경 없음). v2 router는 한 곳에서 dispatch:

| 정규식 | 도착 데이터 | 갱신 대상 |
|---|---|---|
| `/bt: ([0-9A-F:]{17}) rssi=(-?\d+)/` | (mac, rssi) | `deviceSignals.get(mac).rssi.value = rssi` |
| `/bt: ([0-9A-F:]{17}) paired (.+)/` | (mac, name) | pair event signal append + `queryClient.invalidateQueries(['devices'])` |
| `/BLE auth complete: success=yes addr=([0-9A-F:]{17})/` | (mac) | 위와 동일 (alias='BLE 기기' 폴백) |
| `/sm: ([0-9A-F:]{17}) (detecting\|present\|absent\|unlock)/` | (mac, state, fullLine) | `deviceSignals.get(mac).state.value = parseState(line)` |
| `/monitor: ram=(\d+)\/(\d+) psram=(\d+)\/(\d+) tasks=(\d+)/` | (ram, psram, tasks) | `sysStats.value = {...}` |
| (기타 모든 라인) | raw text | `logBuffer.value = [...prev, line].slice(-MAX_LOG_LINES)` |

**모든 라인은 무조건 logBuffer에 push**. 라우팅 매칭은 그 위에 추가 액션. 현재 동작과 동일.

### Signal map lifecycle (SpecFlow 보강)

```
deviceSignals: Map<mac, { rssi: Signal<number|null>, state: Signal<string|null>, lastSeen: Signal<number> }>
```

규칙:
1. **Lazy create**: WS 메시지에서 처음 본 mac이면 그 자리에서 signal 생성 (devices 쿼리 응답 도착 전이라도). 초기값 `null`.
2. **Reconcile on devices fetch**: `useDevices` 응답 도착 시 effect로 reconcile — 응답에 없는 mac은 `deviceSignals.delete(mac)` (메모리 누수 방지). 응답에 있지만 signal 없는 mac은 signal 생성하되 telemetry는 null (WS 도착 전).
3. **Render gating**: 카드 컴포넌트는 `deviceSignals.get(mac).rssi.value === null` 이면 "—" 표시 (현재 vanilla도 동일).

### Query 의존성 체인 (D4 구현)

```js
const info = useQuery({
  queryKey: ['info'],
  queryFn: () => api.fetchJSON('/api/info'),
  retry: 0,  // 401은 즉시 실패 → 브라우저가 dialog 띄움 → 재시도는 사용자 행위
});
const enabled = !!info.data;

const devices      = useQuery({ queryKey: ['devices'],      queryFn: ..., enabled });
const autoUnlock   = useQuery({ queryKey: ['autoUnlock'],   queryFn: ..., enabled });
const wsToken      = useQuery({ queryKey: ['wsToken'],      queryFn: ..., enabled });
const pairing      = useQuery({ queryKey: ['pairingStatus'], queryFn: ..., enabled });
```

WS 연결도 `wsToken.data` 도착 후에 시작. 즉 첫 401은 `info` 한 건만, 사용자가 dialog 통과하면 후속 query/WS는 깨끗한 상태에서 직렬 진행.

**중요**: `info` 쿼리에 `retry: 0` 명시. 기본 3회 재시도가 401을 또 부르면 race 부활. 인증 실패는 즉시 표면화 → 브라우저 dialog가 처리.

### Implementation Phases

각 phase는 독립적으로 review 가능한 commit 단위.

#### Phase 1: Mock 서버 인프라

**산출물**:
- `tests/frontend/mock-server.mjs` (~400줄 추정, Node http + ws)
- `tests/frontend/fixtures/devices.json`, `info.json` (시드 데이터)
- `package.json` (devDependencies: `@playwright/test`, `ws`)
- `playwright.config.mjs` (`webServer` 설정으로 mock 자동 기동, `httpCredentials`, `baseURL`)

**기능**:
- Basic Auth 검사 (config 가능한 user/pass)
- 16 endpoint 라우트 테이블 (기본 응답 + 일부는 stateful — auto-unlock toggle, slack url 저장 등)
- WS 서버 (`/ws?token=...`) — token 검증, fixture 시나리오에 따라 시뮬레이션 메시지 push
- 제어 endpoint (인증 면제):
  - `POST /__ctl/reset` — state 초기화
  - `POST /__ctl/push` — 임의 WS 라인 즉시 push
  - `POST /__ctl/burst` — 30Hz RSSI 시뮬레이션 시작/중지
  - `POST /__ctl/delay?path=...&ms=...` — 특정 endpoint 응답 지연
  - `GET /__ctl/health` — webServer ready 체크

**검증**: `node tests/frontend/mock-server.mjs` 직접 기동 후 brave/chrome으로 `http://localhost:8787/index_v2.html` 수동 접근 가능해야 함 (이 시점에 v2는 없으니 정적 파일 서브 모드로 frontend/ 디렉토리도 expose).

#### Phase 2: index_v2.html 스캐폴드 + 인증 게이트

**산출물**: `frontend/index_v2.html` (HTML + CSS + 빈 module 구조 + App+QueryClientProvider+useInfo 게이트만)

- `<head>` + `<style>` 기존 index.html에서 통째 복사 (변경 0)
- `<body>` 최소 마크업 (`<div id="app">`)
- ESM imports
- `api` 모듈 — `fetchJSON`, `fetchText`, Basic Auth 헤더는 브라우저가 자동 처리(realm 캐시)
- `useInfo` + `App` 컴포넌트 — info 받으면 "✓ authenticated" 한 줄, 못 받으면 빈 화면
- QueryClientProvider 세팅

**검증**: mock 띄우고 진입 → Basic Auth dialog 1회 → 통과 후 "authenticated" 표시. mock 로그에서 401 한 번만 발생 확인.

#### Phase 3: WS 브릿지 + signal map

**산출물**: `ws`, `state` 모듈

- WS 연결 매니저 (token 받은 후 시작, onclose → 지수 backoff reconnect, epoch 가드)
- Line accumulator (`\n` split + 마지막 미완 라인 buffer)
- 메시지 라우터 (위 명세 정규식 6개)
- `deviceSignals` Map + lazy create + reconcile on `useDevices` data 변경
- `pairEventBuffer`, `logBuffer`, `sysStats` 시그널

**검증**: mock의 `/__ctl/push`로 가짜 bt:/sm:/monitor 라인 push → 콘솔에서 signal 값 변화 확인 (UI 컴포넌트 아직 없으므로 devtools).

#### Phase 4: 메인 탭 컴포넌트

**산출물**: `Header`, `Tabs`, `MainTab`, `DeviceCard`, `DoorButton`, `AutoUnlockToggle`, `SafeBanner`

- 메인 탭 전체 마크업 (현재 index.html line 393-406 동등)
- DeviceCard는 props로 mac만 받고, `deviceSignals.get(mac)`에서 rssi/state/lastSeen 구독
- 빈 상태/safe-mode 배너
- Mutations: `useDoorOpen`, `useAutoUnlockToggle` (단순 POST + onSuccess invalidate)

**검증**: mock에 시드 디바이스 3개 + `/__ctl/burst`로 RSSI 30Hz push → 카드 RSSI bar가 부드럽게 갱신, 다른 카드 영향 없음 확인.

#### Phase 5: 관리 탭 컴포넌트

**산출물**: `ManageTab`, `WifiSection`, `AuthSection`, `SlackSection`, `RebootButton`, `BuildInfo`, `SysStats`

- 관리 탭 마크업 (line 408-460 동등)
- 각 form: `useMutation` + 로컬 useState로 입력값 관리 + show/error 표시
- Reboot은 fire-and-forget + "재부팅 중..." 배너

**검증**: mock에 mutation 호출 누적 → 각 form submit 시 mock 로그에 POST + body 확인.

#### Phase 6: 모달 (상세 + 페어링)

**산출물**: `DetailModal`, `PairModal`

- `<dialog>` element 사용 (현재 동작 보존)
- DetailModal: alias 편집 + per-device config (rssi threshold, timeout 등) — `useMutation` for `/api/devices/config`
- PairModal: state machine — `usePairingStatus` 폴링 + WS pair event 청취 + 진행 단계 표시(idle → scanning → device-found → done). `usePairingToggle` mutation.

**검증**: mock 페어링 토글 → mock이 시뮬 시퀀스 push (pair scan 시작 → BLE auth complete) → 모달이 단계별 갱신 → "연결됨!" 표시 후 닫기 가능.

#### Phase 7: 로그 오버레이 + OTA 업로드

**산출물**: `LogOverlay`, `WsStatusPill`, `LogFilters`, `OtaUploader`

- LogOverlay: `logBuffer` signal 구독, tag/text 필터 (현재 vanilla와 동일 UX)
- WsStatusPill: 연결 상태 표시 (connecting/ok/err)
- OtaUploader: `useMutation` 안에서 XMLHttpRequest 직접 사용. progress signal로 진행률 표시.

**검증**: mock의 `/api/firmware/upload`가 1MB 받는 척 (slow stream) → progress bar 0→100 % 부드럽게 갱신.

#### Phase 8: Playwright 테스트

**산출물**: `tests/frontend/fixtures.mjs`, `tests/frontend/e2e/*.spec.mjs`

테스트 fixture로 worker-scoped mock server lifecycle + `beforeEach`에서 `/__ctl/reset`.

**Golden path 5개**:
1. `01-login.spec.mjs` — 진입 → Basic Auth → 메인 탭 렌더 확인 + 디바이스 카드 N개 표시
2. `02-door-open.spec.mjs` — 문열기 버튼 → mock의 POST `/api/door/open` 호출 검증 + UI 피드백
3. `03-auto-unlock-toggle.spec.mjs` — 토글 → 상태 반영 → mock 응답 확인
4. `04-detail-modal.spec.mjs` — 카드 클릭 → 상세 모달 open → alias 변경 → 닫기
5. `05-ws-rssi-update.spec.mjs` — `/__ctl/burst`로 RSSI push → 해당 카드만 RSSI 텍스트 변경 (다른 카드는 변화 없음 verify)

**회귀 가드 1개**:
6. `99-regression-init-race.spec.mjs` — mock의 모든 `/api/*` 응답에 200ms 지연 → 페이지 진입 → mock의 `audit_log`(401 카운터) 가 한 번만 401 받았는지 검증 (여러 번이면 init race 재발). 추가로 `info` 응답을 1초 지연시킨 동안 다른 query가 발사되지 않았음을 mock access log로 검증.

**검증**: `npx playwright test` 6개 모두 green.

#### Phase 9: 동등성 비교 + Swap

**산출물**: `main/CMakeLists.txt` 수정 + (별도 commit) 구파일 삭제

- 매뉴얼 비교 체크리스트 작성: 메인 탭 / 관리 탭 / 모달 / 로그 / OTA 각 화면 캡처 옆 비교
- CMakeLists.txt `EMBED_TXTFILES`에 `index.html` → `index_v2.html` 교체
- `main/http/server.cpp`의 extern asm 심볼 이름 변경 (`_binary_index_html_start` → `_binary_index_v2_html_start`) 또는 파일명을 미리 `index.html`로 rename 후 swap (이 쪽이 deps 최소).
- 빌드 후 binary 사이즈 변동 측정 (스택/PSRAM 영향 없음 확인)
- 구파일 삭제는 별도 PR/commit (롤백 윈도우 확보)

**Swap 전략 권장**: Phase 8 통과 후 `frontend/index.html` → `frontend/index_v1_legacy.html`, `frontend/index_v2.html` → `frontend/index.html` 으로 rename. CMakeLists 변경 0 (embed 경로 그대로). 1주 운영 후 legacy 파일 삭제.

## Edge Cases & Decisions (SpecFlow 보강)

| 항목 | v2 처리 | 근거 |
|---|---|---|
| WS 끊긴 채 mutation | 그대로 발사 (WS 상태 무관). UI는 WS pill로 stale 표시 | 현재 동작 보존. mutation은 HTTP, WS는 통지용. |
| Mutation 실패 후 재시도 | tanstack 기본 retry=3 (5xx만), 4xx는 즉시 실패 | 표준. UI에 에러 메시지 표시. |
| OTA 도중 ESP 재부팅 | XHR onload 200 = 성공 표시, 그 후 자동 reload 시도 | 현재 동작과 동일. 재부팅 알림은 Slack 사이드 채널. |
| Basic Auth 잘못된 비번 N회 | `info` 쿼리 `retry: 0` → 즉시 실패 → 브라우저 dialog만 동작 | 무한 401 루프 방지. server.cpp의 audit_401에는 정상 카운트. |
| 다중 탭 동시 사용 | 토큰 multi-use (현재 동작). 동시 페어링은 백엔드 직렬화. | scope 외. |
| signal Map cleanup | `useDevices` data 변경 시 effect로 reconcile (delete unknown mac) | 메모리 누수 방지. |
| 첫 RSSI 도착 전 렌더 | rssi.value === null → "—" 표시 | 현재 vanilla 동작과 동일. |
| 30Hz signal write 부하 | 1차는 그대로(per-device fine-grained라 컴포넌트 외부 영향 없음). 측정 후 throttle 결정. | YAGNI. 실측 없이 throttle 도입 안 함. |
| WS 메시지 순서: paired 전 sm | 그대로 lazy create (devices에 없는 mac도 signal 생성) | 단순함 유지. devices fetch 도착 후 reconcile에서 정리. |
| esm.sh 오프라인 | v2는 STA 모드 전용. SoftAP은 setup.html(vanilla 유지)이라 무관. | 현재도 STA에선 인터넷 없는 케이스 사실상 없음 (리버스 프록시 경유). |
| iOS Safari bfcache | `visibilitychange` listener로 visible 시 WS 상태 점검 + 필요 시 reconnect | 표준 패턴. |
| 100vh iOS 버그 | CSS 그대로 복사 — 현재도 100vh 사용 안 함 (확인 필요) | 회귀 0 |
| epoch race guard | 유지. WS open → wsBuffer 보관 → `useDevices` data 도착(또는 invalidate 완료) 후 buffer flush | 2026-04-07 plan에서 입증된 패턴. |

## Acceptance Criteria

### Functional Requirements

- [ ] `frontend/index_v2.html` 단일 파일 단독 import로 동작 (별도 빌드 산출물 없음)
- [ ] 16개 백엔드 endpoint 모두 호출 가능 (각각 useQuery 또는 useMutation으로 래핑)
- [ ] WebSocket 라우팅 6개 패턴 모두 처리 (rssi, paired, BLE auth, sm state, monitor, raw log)
- [ ] 페이지 진입 시 401은 정확히 1건만 발생 (info 쿼리만)
- [ ] WS 연결은 wsToken.data 도착 후에만 시작
- [ ] WS reconnect 시 epoch race guard 유지
- [ ] OTA 업로드 progress bar 동작
- [ ] 모든 모달/탭/필터 UX가 v1과 시각적으로 동등

### Non-Functional Requirements

- [ ] 30Hz RSSI 푸시 처리 시 영향 받지 않는 카드의 re-render 0회 (Preact devtools 검증)
- [ ] 페이지 첫 로드 시간이 v1 대비 같은 네트워크에서 +500ms 이내 (esm.sh 첫 fetch 비용)
- [ ] embed 후 펌웨어 binary 사이즈 변동 ±5% 이내
- [ ] CSS class 이름은 v1과 100% 동일 (selector 회귀 가드)

### Quality Gates

- [ ] Playwright golden path 5개 모두 통과
- [ ] Playwright 회귀 가드 (init race) 통과
- [ ] mock 서버 단독 기동 + 매뉴얼 진입 가능
- [ ] git revert 한 번으로 이전 상태 복원 가능 (병렬 파일 정책)

## Alternative Approaches Considered

### Alt A: 빌드 도구 도입 (Vite + TypeScript + JSX)

- **Pro**: TS 안전성, JSX > htm 가독성, 트리쉐이킹으로 번들 크기 감소
- **Con**: 빌드 단계 추가, esp-idf 빌드 파이프라인에 npm 단계 추가, single-file embed 정책 위반
- **결정**: 거부. doorman은 ESP-IDF 단일 빌드 흐름 유지가 가치. preact_practice.html에서 본인이 이미 build-less 정책 명시.

### Alt B: vanilla JS 유지하면서 점진적 모듈화

- **Pro**: 마이그레이션 비용 0, 학습 곡선 0
- **Con**: DX 부채 그대로. 401 race fix만 별도 PR.
- **결정**: 거부. brainstorm에서 "DX 개선이 1순위 목표"로 결정.

### Alt C: setQueryData만 사용 (signal 없이)

- **Pro**: tanstack-query 단일 멘탈 모델
- **Con**: 30Hz RSSI에서 query 구독자 전부 re-render → 카드 N개 × 30Hz 트리 흔들기. memo 우회는 가능하나 layout 신경 많이 쓰임.
- **결정**: 거부 (brainstorm Q&A에서 결정). hybrid가 Preact 설계 의도와 일치.

## Risk & Mitigation

| 리스크 | 영향 | 완화 |
|---|---|---|
| esm.sh CDN 다운/지연 | 페이지 로드 실패 | preact_practice.html이 동일 가정. 사고 시 self-host 결정 (별도 PR). |
| 30Hz signal write가 실측에선 부담 | UI 끊김 | Phase 8 테스트에서 frame timing 측정. 임계 초과 시 100~200ms throttle batch 도입. |
| OTA 업로드 정책 변경 | 기존 사용자 혼란 | 동작 보존 (XHR.upload.onprogress 그대로). |
| htm 인지 비용 | 개발 속도 저하 | preact_practice.html에서 본인이 이미 경험. 수용. |
| init race가 v2에서도 잠재 재발 | false-positive 알림 부활 | 회귀 가드 spec 6번이 명시적 검증. |
| CSS 그대로 복사인데 마크업 구조가 다르면 selector 깨짐 | UI 깨짐 | DeviceCard 등 핵심 컴포넌트는 현재 마크업 구조 그대로 (`<article class="status-card" id="card-...">` 등) 보존. |
| TypeScript 미도입으로 타입 오류 런타임 노출 | 디버깅 비용 | API response shape를 IIFE 모듈에서 JSDoc으로 명시. |
| Mock fidelity 부족으로 e2e 통과해도 실기기 회귀 | 사용자 직격 | API 변경 0 + setup.html 무수정 → 백엔드 회귀 면 0. 실기기 OTA 후 평소 사용 흐름이 사실상 smoke. |

## File / Path Plan

**신규**:
- `frontend/index_v2.html` (작업 중 파일명, swap 시 `index.html`로 rename)
- `tests/frontend/mock-server.mjs`
- `tests/frontend/fixtures.mjs` (playwright fixture)
- `tests/frontend/fixtures/devices.json`
- `tests/frontend/fixtures/info.json`
- `tests/frontend/e2e/01-login.spec.mjs`
- `tests/frontend/e2e/02-door-open.spec.mjs`
- `tests/frontend/e2e/03-auto-unlock-toggle.spec.mjs`
- `tests/frontend/e2e/04-detail-modal.spec.mjs`
- `tests/frontend/e2e/05-ws-rssi-update.spec.mjs`
- `tests/frontend/e2e/99-regression-init-race.spec.mjs`
- `package.json` (devDependencies + scripts)
- `playwright.config.mjs`
- `tests/frontend/README.md` (mock 사용법 + 테스트 실행법)

**수정**:
- (Phase 9) `main/CMakeLists.txt` — swap 방식에 따라 변경 또는 무수정
- (Phase 9) `main/http/server.cpp` — extern asm 심볼명 (rename 방식이면 무수정)

**삭제** (별도 PR, 1주 운영 후):
- `frontend/index_v1_legacy.html` (rename 후의 구파일)
- `frontend/preact_practice.html` (v2가 검증된 후 학습본 역할 종료)

## Testing Plan

### 단위 검증 (Phase별 매뉴얼 검증)

각 Phase 끝에 명시한 검증 단계 수행. mock 띄우고 브라우저로 직접 확인.

### e2e (Phase 8)

`npx playwright test --project=chromium` 6 spec 모두 green이어야 swap 가능.

추가 manual smoke (선택):
- Phase 9 swap 직전: 본인 디바이스에 OTA → 페이지 진입 → 디바이스 카드 표시 + 문열기 1회. 회귀 시 즉시 git revert.

### 회귀 가드 (영구 보존)

`99-regression-init-race.spec.mjs`는 영구 보존. 향후 init 패턴이 다시 race 가능한 상태로 변경되면 CI에서 잡아야 함.

## Out of Scope

- 백엔드 API 변경 (server.cpp 수정 0)
- `setup.html` 마이그레이션 (SoftAP 페이지는 인터넷 없는 환경 진입이라 ESM CDN 불가)
- TypeScript 도입
- 빌드 도구 도입 (Vite/esbuild 등)
- pico.css 등 CSS 프레임워크 도입
- 신규 기능 (현재 v1에 없는 기능 어떤 것도 추가하지 않음)
- `ua_present=0 ip_present=0` 미스터리 추적 (별도 워크: 본인 사파리 데이터 정리 / 리버스 프록시 설정 점검)

## References

### Internal

- 브레인스토밍: `docs/brainstorms/2026-04-29-frontend-preact-tanstack-migration-brainstorm.md`
- 기준 학습본: `frontend/preact_practice.html` (171줄)
- 마이그레이션 대상: `frontend/index.html` (1355줄)
- WS 라우팅 현재 코드: `frontend/index.html:1141-1185`
- Embed 메커니즘: `main/CMakeLists.txt:33-36`, `main/http/server.cpp:36-37,479,514`
- Audit log + 401 rate limiter (race 영향): `main/http/server.cpp:159-236`
- Per-device config (모달 reference): `docs/plans/2026-04-07-feat-per-device-config-card-ui-plan.md`
- WS reconnect epoch 패턴: `frontend/index.html:1201-1272`

### External

- TanStack Query v5 dependent queries: https://tanstack.com/query/latest/docs/framework/react/guides/dependent-queries
- TanStack Query setQueryData (push 데이터 업데이트): https://tanstack.com/query/latest/docs/framework/react/guides/updates-from-mutation-responses
- @preact/signals guide: https://preactjs.com/guide/v10/signals/
- htm README: https://github.com/developit/htm
- Playwright httpCredentials: https://playwright.dev/docs/network
- Playwright Test Fixtures: https://playwright.dev/docs/test-fixtures
- Playwright routeWebSocket (1.48+): https://playwright.dev/docs/api/class-page#page-route-web-socket
- fetch upload progress 미지원 (2026): https://jakearchibald.com/2025/fetch-streams-not-for-progress/

### Related Memory

- [OTA 전 git commit](`memory/feedback_commit_before_ota.md`) — 빌드 결과 검증 가능하게.
- [단일 파일 JS IIFE 모듈](`memory/feedback_single_file_module_folding.md`) — preact_practice.html 패턴의 근거.
