/**
 * doorman-esp 프론트엔드 v2 마이그레이션용 mock HTTP/WS 서버.
 *
 * 단일 Node 프로세스로 server.cpp의 sta_routes 16개 + /ws를 흉내냅니다.
 * /__ctl/* 엔드포인트로 테스트가 상태를 조작합니다 (인증 면제).
 */

import http from 'node:http';
import { readFileSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { randomBytes } from 'node:crypto';
import { WebSocketServer } from 'ws';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PROJECT_ROOT = path.resolve(__dirname, '..', '..');

const PORT = Number(process.env.MOCK_PORT || 8787);
const HOST = process.env.MOCK_HOST || '127.0.0.1';
const MOCK_USER = process.env.MOCK_USER || 'admin';
const MOCK_PASS = process.env.MOCK_PASS || 'doorman';
const SERVE_FILE = process.env.MOCK_SERVE_FILE || 'frontend/index.html';

/* fixtures 로드 */
const fixturesDir = path.join(__dirname, 'fixtures');
const seedDevices = JSON.parse(
  readFileSync(path.join(fixturesDir, 'devices.json'), 'utf8'),
);
const seedInfo = JSON.parse(
  readFileSync(path.join(fixturesDir, 'info.json'), 'utf8'),
);

/* 디바이스 배열 → mac-키 객체. 응답 시에는 다시 배열로 직렬화. */
function devicesToMap(arr) {
  const m = {};
  for (const d of arr) {
    m[d.mac] = { ...d };
  }
  return m;
}

function makeInitialState() {
  return {
    auth: { username: MOCK_USER, password: MOCK_PASS },
    autoUnlock: false,
    pairing: { active: false, timers: [] },
    devices: devicesToMap(seedDevices),
    slackUrl: '',
    delays: {},
    authFailures: { count: 0, lastPath: null },
    bootedAt: Date.now(),
  };
}

const state = {
  ...makeInitialState(),
  wsToken: randomHex(16),
};

/* 활성 burst 핸들 (cleanup용) */
const activeBursts = new Set();

/* WS 클라이언트 (broadcast 대상) */
const wsClients = new Set();

function randomHex(bytes) {
  return randomBytes(bytes).toString('hex');
}

function nowStamp() {
  return new Date().toISOString().slice(11, 23);
}

function logReq(req, status) {
  console.log(`[${nowStamp()}] ${req.method} ${req.url} → ${status}`);
}

function send(req, res, status, body, headers = {}) {
  res.writeHead(status, headers);
  res.end(body);
  logReq(req, status);
}

function sendText(req, res, status, text) {
  send(req, res, status, text, { 'Content-Type': 'text/plain; charset=utf-8' });
}

function sendJson(req, res, status, obj) {
  send(req, res, status, JSON.stringify(obj), {
    'Content-Type': 'application/json; charset=utf-8',
  });
}

function send401(req, res) {
  state.authFailures.count++;
  state.authFailures.lastPath = req.url;
  send(req, res, 401, 'Unauthorized', {
    'WWW-Authenticate': 'Basic realm="Doorman"',
    'Content-Type': 'text/plain; charset=utf-8',
  });
}

function checkAuth(req) {
  const header = req.headers.authorization || '';
  if (!header.startsWith('Basic ')) {
    return false;
  }
  const decoded = Buffer.from(header.slice(6), 'base64').toString('utf8');
  const idx = decoded.indexOf(':');
  if (idx < 0) return false;
  const user = decoded.slice(0, idx);
  const pass = decoded.slice(idx + 1);
  return user === state.auth.username && pass === state.auth.password;
}

async function readBody(req, maxBytes = 4 * 1024 * 1024) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let total = 0;
    req.on('data', (c) => {
      total += c.length;
      if (total > maxBytes) {
        reject(new Error('body too large'));
        req.destroy();
        return;
      }
      chunks.push(c);
    });
    req.on('end', () => resolve(Buffer.concat(chunks)));
    req.on('error', reject);
  });
}

function parseForm(buf) {
  const text = buf.toString('utf8');
  const out = {};
  for (const pair of text.split('&')) {
    if (!pair) continue;
    const eq = pair.indexOf('=');
    const key = decodeURIComponent((eq < 0 ? pair : pair.slice(0, eq)).replace(/\+/g, ' '));
    const val = eq < 0 ? '' : decodeURIComponent(pair.slice(eq + 1).replace(/\+/g, ' '));
    out[key] = val;
  }
  return out;
}

function delay(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

/* /__ctl/delay 가 등록한 인공 지연을 적용. */
async function applyDelay(pathOnly) {
  const ms = state.delays[pathOnly];
  if (typeof ms === 'number' && ms > 0) {
    await delay(ms);
  }
}

/* ─────────── HTTP 핸들러 ─────────── */

function handleIndex(req, res) {
  const filePath = path.isAbsolute(SERVE_FILE)
    ? SERVE_FILE
    : path.join(PROJECT_ROOT, SERVE_FILE);
  let buf;
  try {
    statSync(filePath);
    buf = readFileSync(filePath);
  } catch (err) {
    /* 파일이 아직 없는 경우 (테스트 빈 스캐폴드 단계) 명시적으로 안내 */
    return send(req, res, 404, `Not Found: ${SERVE_FILE} (${err.code})`, {
      'Content-Type': 'text/plain; charset=utf-8',
    });
  }
  send(req, res, 200, buf, { 'Content-Type': 'text/html; charset=utf-8' });
}

async function handleDoorOpen(req, res) {
  await readBody(req).catch(() => {});
  sendText(req, res, 200, 'OK');
}

async function handleFirmwareUpload(req, res) {
  /* multipart 본문은 폐기. content-length만 빠르게 소비. */
  await readBody(req, 16 * 1024 * 1024).catch(() => {});
  const ms = state.delays['__ota'] ?? 100;
  await delay(ms);
  sendText(req, res, 200, 'OK');
}

async function handleAuthUpdate(req, res) {
  const body = await readBody(req);
  const { user, pass } = parseForm(body);
  if (user) state.auth.username = user;
  if (pass) state.auth.password = pass;
  sendText(req, res, 200, 'OK');
}

async function handleWifiUpdate(req, res) {
  await readBody(req);
  /* 실제 펌웨어는 여기서 NVS 저장 후 재부팅. mock에선 200만. */
  sendText(req, res, 200, 'OK');
}

function startPairingSequence() {
  const mac = 'AA:BB:CC:DD:EE:FF';
  const t1 = setTimeout(() => {
    pushAll(`bt: ${mac} rssi=-50`);
  }, 1000);
  const t2 = setTimeout(() => {
    pushAll(`BLE auth complete: success=yes addr=${mac}`);
    if (!state.devices[mac]) {
      state.devices[mac] = {
        mac,
        alias: '',
        rssi_threshold: -65,
        presence_timeout_ms: 30000,
        enter_window_ms: 5000,
        enter_min_count: 3,
        detected: true,
        rssi: -50,
        last_seen_ms: 0,
      };
    }
    state.pairing.active = false;
  }, 3000);
  state.pairing.timers.push(t1, t2);
}

function stopPairingSequence() {
  for (const t of state.pairing.timers) clearTimeout(t);
  state.pairing.timers = [];
}

async function handlePairingToggle(req, res) {
  await readBody(req).catch(() => {});
  state.pairing.active = !state.pairing.active;
  if (state.pairing.active) {
    startPairingSequence();
    sendText(req, res, 200, 'on');
  } else {
    stopPairingSequence();
    sendText(req, res, 200, 'off');
  }
}

function handlePairingStatus(req, res) {
  sendJson(req, res, 200, { active: state.pairing.active });
}

function handleInfo(req, res) {
  sendJson(req, res, 200, seedInfo);
}

function handleStats(req, res) {
  /* server.cpp 실제 키와 plan 키 둘 다 노출 (호환). */
  const uptime_ms = Date.now() - state.bootedAt;
  sendJson(req, res, 200, {
    /* plan 명세 */
    ram_free: 200000,
    psram_free: 4000000,
    tasks: 20,
    uptime_ms,
    /* server.cpp 실제 응답 키 */
    internal_free: 200000,
    internal_total: 320000,
    spiram_free: 4000000,
    spiram_total: 8000000,
    task_count: 20,
  });
}

function handleWsToken(req, res) {
  state.wsToken = randomHex(16);
  sendText(req, res, 200, state.wsToken);
}

async function handleReboot(req, res) {
  await readBody(req).catch(() => {});
  setTimeout(() => {
    state.wsToken = randomHex(16);
    /* 기존 ws 강제 종료 (재부팅 시뮬) */
    for (const c of wsClients) {
      try { c.close(1012, 'reboot'); } catch {}
    }
    wsClients.clear();
  }, 5000);
  sendText(req, res, 200, 'Rebooting...');
}

async function handleAutoUnlockToggle(req, res) {
  await readBody(req).catch(() => {});
  state.autoUnlock = !state.autoUnlock;
  sendJson(req, res, 200, { enabled: state.autoUnlock });
}

function handleAutoUnlockStatus(req, res) {
  sendJson(req, res, 200, { enabled: state.autoUnlock });
}

async function handleSlackUpdate(req, res) {
  const body = await readBody(req);
  const { url } = parseForm(body);
  state.slackUrl = url || '';
  sendText(req, res, 200, 'OK');
}

function handleDevices(req, res) {
  /* server.cpp는 {auto_unlock, devices:[...]} 구조. plan은 단순 배열만 언급하므로
   * 실제 펌웨어 호환성을 우선해 객체로 응답. */
  const list = Object.values(state.devices);
  sendJson(req, res, 200, { auto_unlock: state.autoUnlock, devices: list });
}

async function handleDevicesConfig(req, res) {
  const body = await readBody(req);
  const form = parseForm(body);
  const mac = (form.mac || '').toUpperCase();
  if (!mac) {
    return sendText(req, res, 400, 'mac required');
  }
  const cur = state.devices[mac] || {
    mac,
    alias: '',
    rssi_threshold: -65,
    presence_timeout_ms: 30000,
    enter_window_ms: 5000,
    enter_min_count: 3,
    detected: false,
    rssi: 0,
    last_seen_ms: 0,
  };
  if (form.alias !== undefined) cur.alias = form.alias;
  if (form.rssi !== undefined) cur.rssi_threshold = Number(form.rssi);
  if (form.rssi_threshold !== undefined) cur.rssi_threshold = Number(form.rssi_threshold);
  if (form.presence_timeout_ms !== undefined) cur.presence_timeout_ms = Number(form.presence_timeout_ms);
  if (form.enter_window_ms !== undefined) cur.enter_window_ms = Number(form.enter_window_ms);
  if (form.enter_min_count !== undefined) cur.enter_min_count = Number(form.enter_min_count);
  state.devices[mac] = cur;
  sendText(req, res, 200, 'OK');
}

async function handleDevicesDelete(req, res) {
  const body = await readBody(req);
  const { mac } = parseForm(body);
  const key = (mac || '').toUpperCase();
  if (!key || !state.devices[key]) {
    return sendText(req, res, 404, 'not found');
  }
  delete state.devices[key];
  sendText(req, res, 200, 'OK');
}

function handleCoredump(req, res) {
  /* plan: 빈 binary 또는 404. 기본은 404 (없음). */
  send(req, res, 404, 'no coredump', { 'Content-Type': 'text/plain' });
}

/* ─────────── /__ctl/* (인증 면제) ─────────── */

function handleCtlHealth(req, res) {
  sendText(req, res, 200, 'OK');
}

function handleCtlReset(req, res) {
  /* wsToken은 유지. burst/타이머는 정리. */
  for (const b of activeBursts) clearInterval(b);
  activeBursts.clear();
  stopPairingSequence();
  const keepToken = state.wsToken;
  Object.assign(state, makeInitialState(), { wsToken: keepToken });
  sendText(req, res, 200, 'OK');
}

async function handleCtlPush(req, res) {
  const body = await readBody(req);
  const text = body.toString('utf8');
  pushAll(text);
  sendText(req, res, 200, 'OK');
}

async function handleCtlBurst(req, res) {
  const body = await readBody(req);
  let cfg;
  try {
    cfg = JSON.parse(body.toString('utf8') || '{}');
  } catch {
    return sendText(req, res, 400, 'bad json');
  }
  startBurst(cfg);
  sendText(req, res, 200, 'OK');
}

async function handleCtlDelay(req, res) {
  const body = await readBody(req);
  let cfg;
  try {
    cfg = JSON.parse(body.toString('utf8') || '{}');
  } catch {
    return sendText(req, res, 400, 'bad json');
  }
  if (!cfg.path) return sendText(req, res, 400, 'path required');
  state.delays[cfg.path] = Number(cfg.ms) || 0;
  sendText(req, res, 200, 'OK');
}

function handleCtlAuthLog(req, res) {
  sendJson(req, res, 200, { ...state.authFailures });
}

/* ─────────── burst / push ─────────── */

function pushAll(line) {
  const msg = line.endsWith('\n') ? line : line + '\n';
  for (const c of wsClients) {
    if (c.readyState === 1 /* OPEN */) {
      c.send(msg);
    }
  }
}

function startBurst(cfg) {
  const type = cfg.type;
  const mac = cfg.mac || 'AA:BB:CC:DD:EE:FF';
  const hz = Math.max(1, Number(cfg.hz) || 5);
  const durationMs = Math.max(50, Number(cfg.durationMs) || 1000);
  const intervalMs = Math.max(10, Math.floor(1000 / hz));

  if (type === 'rssi') {
    const handle = setInterval(() => {
      const r = -(30 + Math.floor(Math.random() * 51));
      pushAll(`bt: ${mac} rssi=${r}`);
    }, intervalMs);
    activeBursts.add(handle);
    setTimeout(() => {
      clearInterval(handle);
      activeBursts.delete(handle);
    }, durationMs);
    return;
  }

  if (type === 'sm') {
    const seq = ['detecting 1/3', 'detecting 2/3', 'detecting 3/3', 'present', 'unlock', 'absent'];
    const step = Math.max(50, Math.floor(durationMs / seq.length));
    seq.forEach((s, i) => {
      setTimeout(() => pushAll(`sm: ${mac} ${s}`), step * i);
    });
    return;
  }

  if (type === 'monitor') {
    const handle = setInterval(() => {
      pushAll(`monitor: ram_free=200000 psram_free=4000000 tasks=20`);
    }, 1000);
    activeBursts.add(handle);
    setTimeout(() => {
      clearInterval(handle);
      activeBursts.delete(handle);
    }, durationMs);
    return;
  }
}

/* ─────────── 라우팅 ─────────── */

const routes = {
  /* GET */
  'GET /': handleIndex,
  'GET /api/pairing/status': handlePairingStatus,
  'GET /api/info': handleInfo,
  'GET /api/stats': handleStats,
  'GET /api/ws-token': handleWsToken,
  'GET /api/auto-unlock/status': handleAutoUnlockStatus,
  'GET /api/devices': handleDevices,
  'GET /api/coredump': handleCoredump,
  /* POST */
  'POST /api/door/open': handleDoorOpen,
  'POST /api/firmware/upload': handleFirmwareUpload,
  'POST /api/auth/update': handleAuthUpdate,
  'POST /api/wifi/update': handleWifiUpdate,
  'POST /api/pairing/toggle': handlePairingToggle,
  'POST /api/reboot': handleReboot,
  'POST /api/auto-unlock/toggle': handleAutoUnlockToggle,
  'POST /api/slack/update': handleSlackUpdate,
  'POST /api/devices/config': handleDevicesConfig,
  'POST /api/devices/delete': handleDevicesDelete,
};

const ctlRoutes = {
  'GET /__ctl/health': handleCtlHealth,
  'POST /__ctl/reset': handleCtlReset,
  'POST /__ctl/push': handleCtlPush,
  'POST /__ctl/burst': handleCtlBurst,
  'POST /__ctl/delay': handleCtlDelay,
  'GET /__ctl/auth_log': handleCtlAuthLog,
};

const server = http.createServer(async (req, res) => {
  try {
    const urlObj = new URL(req.url, `http://${req.headers.host}`);
    const pathOnly = urlObj.pathname;
    const key = `${req.method} ${pathOnly}`;

    /* /__ctl/* 인증 면제, 우선 처리 */
    if (pathOnly.startsWith('/__ctl/')) {
      const handler = ctlRoutes[key];
      if (!handler) return send(req, res, 404, 'Not Found');
      return await handler(req, res);
    }

    if (!checkAuth(req)) {
      return send401(req, res);
    }

    const handler = routes[key];
    if (!handler) {
      return send(req, res, 404, 'Not Found');
    }

    await applyDelay(pathOnly);
    await handler(req, res);
  } catch (err) {
    console.error('handler error:', err);
    if (!res.headersSent) {
      send(req, res, 500, `Internal: ${err.message}`);
    } else {
      try { res.end(); } catch {}
    }
  }
});

/* ─────────── WebSocket ─────────── */

const wss = new WebSocketServer({ noServer: true });

server.on('upgrade', (req, socket, head) => {
  const urlObj = new URL(req.url, `http://${req.headers.host}`);
  if (urlObj.pathname !== '/ws') {
    socket.destroy();
    return;
  }
  const token = urlObj.searchParams.get('token');
  if (!token || token !== state.wsToken) {
    /* 토큰 불일치 → 1008 close. ws lib는 handleUpgrade 후 close 가능. */
    wss.handleUpgrade(req, socket, head, (ws) => {
      ws.close(1008, 'invalid token');
    });
    return;
  }
  wss.handleUpgrade(req, socket, head, (ws) => {
    wsClients.add(ws);
    console.log(`[${nowStamp()}] WS connect (clients=${wsClients.size})`);
    ws.on('close', () => {
      wsClients.delete(ws);
      console.log(`[${nowStamp()}] WS close (clients=${wsClients.size})`);
    });
    ws.on('error', (e) => console.error('ws err:', e.message));
  });
});

server.listen(PORT, HOST, () => {
  console.log(`mock-server listening on http://${HOST}:${PORT}`);
  console.log(`  serve file: ${SERVE_FILE}`);
  console.log(`  basic auth: ${MOCK_USER} / ${MOCK_PASS}`);
  console.log(`  initial ws token: ${state.wsToken}`);
});

/* graceful shutdown */
function shutdown() {
  console.log('shutting down…');
  for (const b of activeBursts) clearInterval(b);
  stopPairingSequence();
  for (const c of wsClients) { try { c.close(); } catch {} }
  server.close(() => process.exit(0));
  /* 안전망: 1초 후 강제 종료 */
  setTimeout(() => process.exit(0), 1000).unref();
}
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
