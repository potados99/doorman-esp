---
title: 프론트엔드 Preact + TanStack Query 마이그레이션
date: 2026-04-29
status: ready-for-plan
---

# 프론트엔드 Preact + TanStack Query 마이그레이션

## What We're Building

`frontend/index.html` (1355줄, vanilla XHR + 수동 DOM 조작)을 **Preact + htm + @tanstack/preact-query + Preact signals** 기반 단일 파일 SPA로 재작성한다. 디자인(CSS)과 백엔드 API(16개 endpoint + WS)는 일절 변경하지 않는다.

기준 패턴은 `frontend/preact_practice.html` (171줄, ESM CDN + IIFE 모듈 + htm). 빌드 도구 없음, single-file embed 유지.

## Why Now

1. **DX 부채 누적**: 1355줄 단일 파일에 글로벌 변수(`deviceData`, `cardState`, `lastBtTime`, `detectCount`...) + 수동 `getElementById` + DOM 패치 함수 8개가 흩어져 있어 신규 기능 추가/리뷰 인지 부하가 높다.
2. **`preact_practice.html` 패턴 검증 완료**: ESM CDN + htm + tanstack-query 조합이 doorman 같은 임베디드 single-file SPA에 적합한지 본인이 학습본으로 이미 증명. 실전 적용 단계.
3. **2026-04-29 발견된 401 init race**: 페이지 init JS가 인증 게이트 전에 3개 XHR 병렬 발사 → 매 fresh login마다 false-positive audit 알림 발생. tanstack-query의 `enabled` 옵션으로 자연스럽게 직렬화 가능 — 마이그레이션과 함께 가져갈 수 있는 공짜 부수효과.

## Key Decisions

### 1. 1순위 목표: DX 개선
- 선언적 코드, 명시적 상태 구독, 재사용 가능한 컴포넌트.
- 신규 기능 추가는 부수적 결과이지 driver 아님.

### 2. 진행 방식: 병렬 파일 (parallel file)
- `frontend/index_v2.html` 신규 생성. 기존 `index.html`은 일절 손대지 않음.
- 완성 후 CMakeLists의 embed 타깃을 `index_v2.html`로 swap → 검증 후 구파일 삭제.
- **사유**: 중간 상태 없는 깔끔한 swap. git revert 즉시 가능. 인지 부하 격리.

### 3. WS-driven state: Hybrid (Preact signal + tanstack query)
- **`useQuery`**: HTTP-driven 스냅샷 — `/api/info`, `/api/devices`, `/api/auto-unlock/status`, `/api/pairing/status`, `/api/ws-token`.
- **Preact signal**: WS-driven 고빈도 푸시 — RSSI(30Hz), state(detecting/present/absent/unlock), pair event stream.
- **사유**:
  - 30Hz RSSI를 `setQueryData`로 처리하면 query 구독자 전부 re-render. 카드 N개 × 30Hz = 초당 수백 번 트리 흔들기.
  - 현재 vanilla 코드는 이미 per-device 직접 DOM patch — Preact signal은 정확히 동일한 granularity(signal 구독자만 갱신)를 선언적으로 제공.
  - Preact 설계 철학(fine-grained reactivity)과 일치. signal을 안 쓰면 Preact 채택 의미 반감.
- **책임 분할 명확**: 같은 필드를 query와 signal이 동시에 만지는 일 없음. info는 HTTP만, RSSI는 WS만.

### 4. 401 init race 동시 해결
- `useQuery({ queryKey: ['info'], ... })`를 첫 인증 관문으로 두고, 나머지 query들은 `enabled: !!info.data` gate.
- 사파리가 페이지 로드 시 inert HTML만 받고, 첫 query 한 번만 401 발생 → dialog 통과 후 직렬로 후속 query 진행.
- 코드 증가량 2~3줄. false-positive audit 알람 원천 차단.
- **유의**: `/api/ws-token` 응답 후에야 WS 연결을 시도하도록 의존 그래프 명시.

### 5. 테스트: golden path + mock 서버
- Node mock 서버로 16 endpoint + WS 모킹.
- playwright-cli 시나리오 5개:
  1. 로그인 → 메인/관리 탭 렌더 확인
  2. 문열기 → `/api/door/open` POST 검증
  3. 자동잠금 토글 → 상태 변경 반영
  4. 상세 모달 open/close
  5. WS push → RSSI/state UI 갱신 확인
- **회귀 가드 추가**: mock에서 일부러 헤더 strip 시뮬레이션 → init race가 실제로 막혀있는지 검증.

### 6. Done 기준
- e2e mock 시나리오 5개 모두 통과 시 swap.
- **실기기 검증 불필요** — 백엔드 API 불변, client만 교체. mock으로 충분.

### 7. 의존성 / 빌드
- 빌드 도구 없음 (preact_practice.html 정책 유지).
- esm.sh CDN: preact, preact/hooks, htm, @tanstack/preact-query, @preact/signals.
- TypeScript 없음 (한계 수용).
- pico.css 같은 외부 CSS 도입 안 함 — 기존 `<style>` 블록(~365줄) 그대로 복사.

## Non-Goals (명시적 제외)

- **신규 기능 추가 없음**. v2는 정확히 v1의 feature parity.
- **API 변경 없음**. 백엔드 server.cpp는 손 안 댐.
- **빌드 파이프라인 도입 안 함** — Vite/esbuild 등.
- **TypeScript 도입 안 함**.
- **CSS 리팩터링 안 함** — 기존 inline style 그대로.
- **실기기 검증 안 함** — mock e2e로 동등성 충분.

## Open Questions

(없음 — Phase 1.2 대화에서 모두 해소)

## Resolved Questions

| 질문 | 결정 |
|---|---|
| 1순위 목표? | DX 개선 |
| 진행 방식? | 병렬 파일 (index_v2.html) |
| WS 결합? | Hybrid (signal + query) |
| 401 race도 고치나? | 예 (`enabled` gate) |
| 테스트 범위? | golden path 5개 시나리오 |
| Done 기준? | mock e2e 통과 (실기기 불필요) |

## Risks & Mitigations

| 리스크 | 완화 |
|---|---|
| esm.sh 다운/지연 시 페이지 로드 실패 | preact_practice.html이 이미 동일 가정. 첫 로드 후 브라우저 캐시. 사고 발생 시 외부 의존성 한계 인지 후 self-host 결정. |
| 30Hz signal 갱신이 실제로 카드 외 영역 흔든다면 | playwright e2e에서 React DevTools profiler로 검증, 회귀 시 throttle 도입. |
| OTA progress bar — fetch는 upload progress 미지원 | `useMutation` mutationFn 안에서 XHR 직접 사용. 표준 우회 패턴. |
| htm 표현이 JSX와 달라 인지 비용 | preact_practice.html에서 본인이 이미 경험. 수용. |
| init race가 v2에서도 잠재적으로 재발 | mock에서 헤더 strip 시뮬레이션 시나리오로 회귀 가드. |

## File / Path Plan

- **신규**: `frontend/index_v2.html` (작업 중 이름)
- **신규**: `tests/frontend/mock-server.mjs` (Node http + ws stub)
- **신규**: `tests/frontend/e2e/*.spec.mjs` (playwright 5개)
- **수정**: 완료 시 CMakeLists.txt embed 타깃 변경
- **삭제**: swap 후 검증 끝나면 `frontend/index.html` 제거 + `index_v2.html` → `index.html` 리네임

## Next

`/workflows:plan` 으로 구현 단계 분해.
