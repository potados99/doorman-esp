# tests/frontend

doorman-esp 프론트엔드 v2 마이그레이션용 mock 인프라.

## 구성

- `mock-server.mjs` — Node http + ws 단일 프로세스. ESP32 펌웨어의 16개 endpoint + `/ws` 흉내.
- `fixtures/devices.json` — 초기 디바이스 시드.
- `fixtures/info.json` — `/api/info` 응답 시드.
- `e2e/` — Playwright 테스트 (Phase 8에서 추가).

## mock 단독 기동

```bash
npm install
npm run mock
# → http://127.0.0.1:8787
```

환경변수:

| Var | Default | 설명 |
|---|---|---|
| `MOCK_PORT` | `8787` | listen 포트 |
| `MOCK_HOST` | `127.0.0.1` | listen 호스트 |
| `MOCK_USER` | `admin` | Basic Auth 사용자 |
| `MOCK_PASS` | `doorman` | Basic Auth 패스워드 |
| `MOCK_SERVE_FILE` | `frontend/index.html` | `GET /` 로 서빙할 파일 (프로젝트 루트 상대 경로). v2 테스트 시 `frontend/index_v2.html` |

## 테스트 실행

```bash
npx playwright install chromium    # 최초 1회
npm run test:e2e
```

`playwright.config.mjs` 의 `webServer` 가 mock-server를 자동 기동/종료합니다.

## /__ctl/* (테스트 제어 엔드포인트, 인증 면제)

| Method | Path | Body | 동작 |
|---|---|---|---|
| GET | `/__ctl/health` | — | webServer ready 체크 |
| POST | `/__ctl/reset` | — | 상태 초기화 (wsToken 유지) |
| POST | `/__ctl/push` | text | 모든 WS 클라이언트에 본문을 push (`\n` 자동 추가) |
| POST | `/__ctl/burst` | `{type:'rssi'\|'sm'\|'monitor', mac?, hz?, durationMs?}` | 비동기 burst 시뮬 시작 |
| POST | `/__ctl/delay` | `{path:'/api/info', ms:200}` | 다음 응답부터 인공 지연 |
| GET | `/__ctl/auth_log` | — | 누적 401 카운트 + 마지막 401 path |

### burst 종류

- `rssi` — `bt: <MAC> rssi=-<random>` 라인을 hz 만큼 durationMs 동안 push
- `sm` — `sm: <MAC> <상태>` 시퀀스 (detecting 1/3 → present → unlock → absent)
- `monitor` — 1초 주기로 `monitor:` 라인 push

### 페어링 시뮬

`POST /api/pairing/toggle` 으로 켜지면 mock이 자동으로:
- 1초 후 `bt: AA:BB:CC:DD:EE:FF rssi=-50`
- 3초 후 `BLE auth complete: success=yes addr=AA:BB:CC:DD:EE:FF` + state.devices에 entry 추가

## WS 연결 직접 테스트

```js
import { WebSocket } from 'ws';
const auth = 'Basic ' + Buffer.from('admin:doorman').toString('base64');
const token = (await fetch('http://127.0.0.1:8787/api/ws-token', {
  headers: { Authorization: auth },
})).then(r => r.text()).then(t => t.trim());
const ws = new WebSocket(`ws://127.0.0.1:8787/ws?token=${await token}`);
ws.on('open', () => console.log('OPEN'));
ws.on('message', m => console.log(m.toString()));
await fetch('http://127.0.0.1:8787/__ctl/push', {
  method: 'POST',
  body: 'bt: AA:BB:CC:DD:EE:FF rssi=-50',
});
```

## server.cpp 와의 차이 (의도적)

| 항목 | 실제 펌웨어 | mock |
|---|---|---|
| `/api/pairing/status` | `on`/`off` plain text | `{active: bool}` JSON (plan 명세) |
| `/api/auto-unlock/status` | `enabled`/`disabled` plain text | `{enabled: bool}` JSON (plan 명세) |
| `/api/auto-unlock/toggle` | `enabled`/`disabled` plain text | `{enabled: bool}` JSON (plan 명세) |
| `/api/stats` | `internal_free` / `spiram_free` / `task_count` | 둘 다 노출 (plan + 펌웨어 키 동시) |
| `/api/info` `version` | `v` prefix (`v0.5.0`) | `vmock-1.0.0` (prefix 유지) |

v2 프론트가 mock 응답에 맞춰 작성된 후, 펌웨어 응답 형식과 차이가 발견되면 mock을 펌웨어 쪽으로 수렴시키거나 펌웨어 핸들러를 plan 형식으로 정리합니다 (Phase 9 swap 시 결정).
