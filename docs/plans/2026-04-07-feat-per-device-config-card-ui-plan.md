---
title: "feat: Per-Device Config + Card UI Redesign"
type: feat
status: active
date: 2026-04-07
deepened: 2026-04-07
brainstorm: docs/brainstorms/2026-04-07-per-device-config-and-ui-brainstorm.md
---

# Per-Device Config + Card UI Redesign

## Enhancement Summary

**Deepened on:** 2026-04-07
**Research agents:** NVS blob 패턴, FreeRTOS 동기화, CSS 리플 애니메이션, WebSocket 로그 파싱, 성능 리뷰, 코드 단순성 리뷰

### Key Improvements (리서치 기반 변경)
1. DeviceSnapshot 별도 구조체 제거 → DeviceState를 public으로 직접 사용 (스택 3.8KB 절감, 변환 코드 제거)
2. dump_states를 전역 배열에 직접 쓰기 → SM Task 스택 4KB 유지 (8KB 증가 불필요)
3. cJSON 대신 snprintf + chunked encoding → 힙 할당 0 (ESP-IDF v6.0에서 cJSON이 외부 컴포넌트로 이동됨)
4. BTU 콜백에서 NVS 직접 쓰기 금지 → SM 큐로 비동기 config 생성 (BTU task 블로킹 방지)
5. 큐 drain 루프 → tick당 최대 16개 dequeue (15대 동시 burst 시 큐 포화 방지)
6. 프론트엔드: `::after` + opacity 리플 (GPU 가속), `<dialog>` 모달 (네이티브 접근성), line accumulator + rAF 배칭

## Overview

전역 단일 AppConfig를 기기(MAC) 단위로 분리하고, 웹 UI를 로그 중심 대시보드에서 기기 카드 리스트 기반 관리 앱으로 전환한다.

**설계 결정은 브레인스톰 문서에 확정됨.** 이 플랜은 "어떻게 구현할 것인가"에 집중한다.

## Technical Architecture

### 현재 데이터 흐름

```
BT Manager ──FeedMsg──> SM Task ──ControlCmd──> Control Task
                          ↑
                   app_config_get()  (매 루프, 전역 config pull)
                          ↑
HTTP handler ─────> config_service  (mutex + NVS)
```

### 변경 후 데이터 흐름

```
BT Manager ──SmMsg──────> SM Task ──ControlCmd──> Control Task
  │                         ↑  ↓
  │                         │  s_snapshots[] (mutex, DeviceState 직접)
  │                         ↑       ↓
  │              device_config_get() │
  │                         ↑       ↓
HTTP handler ─> device_config_service   sm_get_snapshots()
  │                    ↑                     │
  │                    NVS "dev"             │
  │                                          │
  └──── GET /api/devices ← merge(bonded + config + snapshot)
```

## Implementation Phases

---

### Phase 1: DeviceConfig 구조체 + NVS 서비스

**목표**: 기기별 설정 저장/로드 인프라 구축. SM 변경 없이 독립 테스트 가능.

#### 1-1. DeviceConfig 구조체

`components/config/include/config.h`에 추가:

```cpp
struct DeviceConfig {
    uint8_t  version = 1;               // NVS blob 버전
    int8_t   rssi_threshold = -70;
    uint8_t  _pad[2] = {};              // 명시적 패딩 (Xtensa 4-byte 정렬)
    uint32_t presence_timeout_ms = 15000;
    uint32_t enter_window_ms = 5000;
    uint32_t enter_min_count = 3;
    char     alias[32] = {};            // 별명 (UTF-8, null-terminated)
};
// sizeof = 48 bytes (패딩 포함, 컴파일러 독립적)
```

- **명시적 패딩**: `__attribute__((packed))` 대신 수동 패딩. packed는 Xtensa에서 비정렬 접근으로 크래시 위험.
- `version` 필드: 구조체 첫 바이트에 고정. blob_len 체크 + version 체크 이중 검증. sizeof가 같아도 내부 레이아웃이 다른 경우를 잡음.
- NVS 읽기: `blob_len != sizeof(DeviceConfig)` 또는 `version != 1` → 기본값 폴백 + 경고 로그.

`components/config/config.cpp`에 추가:

```cpp
bool validate_device_config(const DeviceConfig &cfg);
```

#### 1-2. device_config_service

`main/device_config_service.h` / `main/device_config_service.cpp` 신규 파일:

```cpp
void device_config_service_init();
DeviceConfig device_config_get(const uint8_t (&mac)[6]);
void device_config_set(const uint8_t (&mac)[6], const DeviceConfig &cfg);
void device_config_delete(const uint8_t (&mac)[6]);
int device_config_get_all(uint8_t (*macs)[6], DeviceConfig *configs, int max);
bool device_config_changed();  // atomic flag consume
```

**NVS 설계**:
- namespace: `"dev"`, key: MAC 12hex 대문자 (예: `"842F57A0C4EA"`, 12자 ≤ 15자 제한)
- value: DeviceConfig blob 48바이트. 15대 × 4엔트리 = 60엔트리. 24KB 파티션 사용률 ~10%.
- NVS wear: 수동 UI 조작(하루 수십 회) 기준 수명 수천 년. 자동 주기 저장은 하지 않음.

**캐시**:

```cpp
struct DeviceConfigEntry {
    uint8_t mac[6];
    DeviceConfig config;
    bool used;
};
static DeviceConfigEntry s_entries[15];
static SemaphoreHandle_t s_mutex;
static std::atomic<bool> s_changed{false};
```

- MAC + config를 하나의 구조체로 묶어 인덱스 불일치 방지.
- `device_config_set()`: validate → mutex lock → 캐시 갱신 → NVS 저장 → unlock → `s_changed.store(true)`
- `device_config_changed()`: `s_changed.exchange(false)` — SM Task가 매 루프에서 호출.

**NVS 전체 로드 (`init` 시)**:

```cpp
// nvs_entry_find_in_handle()로 "dev" namespace의 모든 blob을 순회
nvs_iterator_t it = nullptr;
esp_err_t err = nvs_entry_find_in_handle(handle, NVS_TYPE_BLOB, &it);
while (err == ESP_OK) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    // info.key → MAC hex → blob 로드 → 캐시 적재
    err = nvs_entry_next(&it);
}
nvs_release_iterator(it);
```

**에러 처리 원칙**:
- 읽기 실패 → 절대 크래시 안 함. 기본값 반환.
- 쓰기 실패 → 캐시에는 반영 (재부팅 전까지 동작). NVS full 로그 경고.
- `ESP_ERROR_CHECK`는 초기화에만 사용. 런타임에서는 에러 핸들링.

**첫 기기 config 생성**: bt_manager auth_cmpl 콜백에서 직접 NVS 쓰기 **금지** (BTU task 블로킹 위험). 대신 SM 큐에 `CreateConfig` 메시지를 보내고 SM Task에서 비동기로 `device_config_set()` 호출.

#### 1-3. 테스트

`host_test/config_test.cpp`에 `validate_device_config()` 테스트 추가 (기존 패턴 동일).

---

### Phase 2: StateMachine 기기별 Config + SM Task 통합

**목표**: SM이 기기별 개별 설정으로 판단하고, SM Task가 device_config_service + 상태 덤프를 통합.

#### 2-1. DeviceState를 public으로 + DeviceConfig 내장

`components/statemachine/include/statemachine.h`:

```cpp
// DeviceState를 class 밖 public으로 이동 (dump_states 외부 노출용)
struct DeviceState {
    uint8_t mac[6] = {};
    bool valid = false;
    bool detected = false;
    bool went_undetected = false;
    uint32_t last_seen_ms = 0;
    uint32_t last_unlock_ms = 0;
    int8_t last_rssi = 0;

    static constexpr int kMaxRecentObs = 10;
    uint32_t recent_obs[kMaxRecentObs] = {};
    int obs_idx = 0;
    int obs_count = 0;

    DeviceConfig dev_config;   // 신규: 기기별 설정
};
```

**DeviceSnapshot 별도 구조체는 만들지 않는다.** DeviceState 자체가 스냅샷.

#### 2-2. SM 인터페이스 변경

```cpp
class StateMachine {
public:
    static constexpr int kMaxDevices = 30;
    explicit StateMachine(AppConfig cfg);
    void feed(const uint8_t (&mac)[6], bool seen, uint32_t now_ms, int8_t rssi = 0);
    [[nodiscard]] Action tick(uint32_t now_ms);
    void update_config(AppConfig cfg);

    // 신규
    void update_device_config(const uint8_t (&mac)[6], const DeviceConfig &cfg);
    void remove_device(const uint8_t (&mac)[6]);
    int dump_states(DeviceState *out, int max) const;
};
```

#### 2-3. feed() / tick() config 참조 변경

```cpp
// feed(): config_.rssi_threshold → dev->dev_config.rssi_threshold
// tick(): config_.presence_timeout_ms → dev.dev_config.presence_timeout_ms
//         config_.enter_window_ms → dev.dev_config.enter_window_ms
//         config_.enter_min_count → dev.dev_config.enter_min_count
```

`update_config(AppConfig)`: auto_unlock_enabled 등 전역 설정만 의미.

#### 2-4. SM 메시지 큐 재설계

```cpp
enum class SmMsgType { Feed, RemoveDevice, CreateConfig };

struct SmMsg {
    SmMsgType type;
    union {
        struct { uint8_t mac[6]; bool seen; uint32_t now_ms; int8_t rssi; } feed;
        struct { uint8_t mac[6]; } remove;
        struct { uint8_t mac[6]; char alias[32]; } create_config;
    };
};

static_assert(sizeof(SmMsg) <= 48, "SmMsg too large for queue");
```

- `Feed`: 기존 sm_feed_queue_send() 래핑
- `RemoveDevice`: HTTP 기기 삭제 시 SM 슬롯 즉시 정리
- `CreateConfig`: BT auth_cmpl 콜백에서 BTU task 블로킹 없이 비동기 config 생성

#### 2-5. SM Task 루프 변경

```cpp
while (true) {
    sm.update_config(app_config_get());   // 전역 (auto_unlock)

    // per-device config 변경 감지 (atomic flag)
    if (device_config_changed()) {
        uint8_t macs[15][6]; DeviceConfig cfgs[15];
        int n = device_config_get_all(macs, cfgs, 15);
        for (int i = 0; i < n; i++)
            sm.update_device_config(reinterpret_cast<const uint8_t(&)[6]>(macs[i]), cfgs[i]);
    }

    // 큐 drain: tick당 최대 16개 dequeue (15대 burst 대응)
    SmMsg msg;
    int drained = 0;
    while (xQueueReceive(s_queue, &msg, drained == 0 ? pdMS_TO_TICKS(kTickIntervalMs) : 0) == pdTRUE) {
        switch (msg.type) {
        case SmMsgType::Feed:
            sm.feed(msg.feed.mac, msg.feed.seen, msg.feed.now_ms, msg.feed.rssi);
            break;
        case SmMsgType::RemoveDevice:
            sm.remove_device(msg.remove.mac);
            break;
        case SmMsgType::CreateConfig: {
            DeviceConfig cfg;
            snprintf(cfg.alias, sizeof(cfg.alias), "%s", msg.create_config.alias);
            device_config_set(reinterpret_cast<const uint8_t(&)[6]>(msg.create_config.mac), cfg);
            break;
        }
        }
        if (++drained >= 16) break;
    }

    sm.tick(now_ms);

    // 스냅샷 갱신: 전역 배열에 직접 dump (스택 버퍼 없음)
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    s_snapshot_count = sm.dump_states(s_snapshots, 30);
    xSemaphoreGive(s_snapshot_mutex);
}
```

**스택**: dump_states가 전역 `s_snapshots`에 직접 쓰므로 중간 스택 버퍼 불필요. **SM Task 스택 4KB 유지.**

#### 2-6. 상태 스냅샷 (외부 API)

```cpp
// sm_task.cpp
static SemaphoreHandle_t s_snapshot_mutex;
static DeviceState s_snapshots[30];  // DeviceState 직접 사용
static int s_snapshot_count;

int sm_get_snapshots(DeviceState *out, int max) {
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    int n = s_snapshot_count < max ? s_snapshot_count : max;
    memcpy(out, s_snapshots, n * sizeof(DeviceState));
    xSemaphoreGive(s_snapshot_mutex);
    return n;
}
```

mutex hold time: ~2KB memcpy = ~5-10us. 2초 tick 대비 0.001%. 문제 없음.

#### 2-7. 테스트 마이그레이션 + 신규 테스트

기존 14개 테스트: `StateMachine sm(cfg)` 후 `sm.update_device_config(mac_a, dev_cfg)` 추가.

신규 테스트 7개: PerDeviceRssiThreshold, PerDeviceTimeout, PerDeviceEnterWindow, UpdateDeviceConfigMidSession, RemoveDevice, DumpStates, DefaultConfigForNewDevice.

---

### Phase 3: 로그 포맷 표준화

**목표**: bt/sm 로그를 브라우저 파싱 가능한 고정 포맷으로 통일.

#### 3-1. MAC 대문자 통일

`bt_manager.cpp`의 `bda_to_str()`: `%02x` → `%02X`. 한 군데 수정으로 전체 반영.

#### 3-2. bt 로그 변경

| 위치 | 현재 | 변경 후 |
|---|---|---|
| `:677` BLE RSSI | `"BLE %s RSSI=%d"` | `"%s rssi=%d"` |
| `:905` Classic probe | `"Classic %s"` | `"%s rssi=0"` |
| `:869` Classic auth | `"Classic auth success: %s [%s]"` | `"%s paired %s"` |

Classic auth에서 `device_config_set()` 직접 호출 대신 `SmMsg{.type=CreateConfig}` 큐 전송.

#### 3-3. sm 로그 변경

| 위치 | 현재 | 변경 후 |
|---|---|---|
| `feed():94` | `"%s %lums내 %d/%lu건 (RSSI %d)"` | `"%s detecting %d/%lu %lu"` (현재 feed() 로그 타이밍 유지, 포맷만 변경) |
| `tick():114` | `"%s 퇴실 (타임아웃 %lums)"` | `"%s absent %lu"` |
| `tick():133-138` | `"%s 재실 (...)"` | `"%s present"` |
| `tick():151,162` | `"%s → Unlock (...)"` | `"%s unlock"` |

---

### Phase 4: HTTP API 변경

**목표**: /api/devices를 JSON으로 전환, 기기별 설정 CRUD.

#### 4-1. GET /api/devices → JSON (snprintf + chunked, 힙 할당 0)

```cpp
static esp_err_t devices_handler(httpd_req_t *req) {
    // 데이터 수집
    uint8_t macs[30][6];
    int bond_count = bt_get_bonded_devices(macs, 30);
    DeviceState snapshots[30];
    int snap_count = sm_get_snapshots(snapshots, 30);

    httpd_resp_set_type(req, "application/json");

    // auto_unlock 상태
    AppConfig global = app_config_get();
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"auto_unlock\":%s,\"devices\":[",
             global.auto_unlock_enabled ? "true" : "false");
    httpd_resp_sendstr_chunk(req, buf);

    // 기기별 JSON chunk
    for (int i = 0; i < bond_count; i++) {
        DeviceConfig cfg = device_config_get(reinterpret_cast<const uint8_t(&)[6]>(macs[i]));
        // snapshot에서 MAC 매칭
        DeviceState *snap = find_snapshot(snapshots, snap_count, macs[i]);

        snprintf(buf, sizeof(buf),
            "%s{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
            "\"alias\":\"%s\",\"detected\":%s,\"rssi\":%d,"
            "\"last_seen_ms\":%lu,\"config\":{"
            "\"rssi_threshold\":%d,\"presence_timeout_ms\":%lu,"
            "\"enter_window_ms\":%lu,\"enter_min_count\":%lu}}",
            i > 0 ? "," : "",
            macs[i][0], macs[i][1], macs[i][2],
            macs[i][3], macs[i][4], macs[i][5],
            cfg.alias,
            snap ? (snap->detected ? "true" : "false") : "false",
            snap ? snap->last_rssi : 0,
            snap ? (unsigned long)snap->last_seen_ms : 0UL,
            cfg.rssi_threshold,
            (unsigned long)cfg.presence_timeout_ms,
            (unsigned long)cfg.enter_window_ms,
            (unsigned long)cfg.enter_min_count);
        httpd_resp_sendstr_chunk(req, buf);
    }

    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, nullptr);  // 종료
    return ESP_OK;
}
```

**alias 검증**: `validate_device_config()`에서 alias를 알파벳+한글+숫자+공백만 허용, 특수문자(`"`, `\`, `<`, `>` 등) 거부. JSON 이스케이핑 불필요.

#### 4-2. POST /api/devices/config

기기별 설정 저장. form-urlencoded. `validate_device_config()` 후 `device_config_set()`.

#### 4-3. 기존 API 처리

| API | 처리 |
|---|---|
| `GET/POST /api/tuning` | **제거** (per-device로 대체) |
| `POST /api/auto-unlock/toggle` | **유지** (전역) |
| `POST /api/devices/delete` | bond 삭제 + NVS config 삭제 + `sm_remove_device()` 추가 |

---

### Phase 5: 프론트엔드 재작성

**목표**: 기기 카드 리스트 기반 UI. 브레인스톰 목업 구현.

#### 5-1. 탭 구조 + 헤더

```
Doorman [+ 기기] [📋]
[메인] [관리]
```

#### 5-2. 리플 애니메이션 (GPU 가속)

```css
.status-card {
    position: relative;
    border-left: 3px solid var(--status-color, #e5e7eb);
    --ripple-color: var(--status-color);
    --ripple-speed: 1.5s;
}

/* ::after로 리플 → opacity만 애니메이트 → GPU 컴포지팅 */
.status-card::after {
    content: '';
    position: absolute;
    inset: -1px;
    border-radius: inherit;
    box-shadow: 0 0 0 0 var(--ripple-color);
    pointer-events: none;
    opacity: 0;
}
.status-card.pulsing::after {
    animation: cardRipple var(--ripple-speed) ease-out infinite;
}
.status-card.burst::after {
    animation: burstRipple 0.6s ease-out forwards;
}

@keyframes cardRipple {
    0%   { box-shadow: 0 0 0 0   var(--ripple-color); opacity: 1; }
    70%  { box-shadow: 0 0 0 12px var(--ripple-color); opacity: 0; }
    100% { box-shadow: 0 0 0 12px var(--ripple-color); opacity: 0; }
}
@keyframes burstRipple {
    0%   { box-shadow: 0 0 0 0   var(--ripple-color); opacity: 1; }
    100% { box-shadow: 0 0 0 20px rgba(22,163,74,0); opacity: 0; }
}
```

- detecting: `animationiteration` 이벤트로 사이클 완료 시 `--ripple-speed` 변경 (점프 방지)
- present 전환: `void card.offsetWidth` reflow 강제 후 `.burst` 클래스 추가 (원샷)
- `animationend`에서 `.burst` 자동 제거

#### 5-3. 모달 (`<dialog>` 네이티브)

```html
<dialog id="detailModal" class="modal">
    <div class="modal-head">
        <span class="modal-title"></span>
        <button class="modal-close" onclick="closeModal()">&times;</button>
    </div>
    <div class="modal-body"></div>
</dialog>
```

- `showModal()` 자동 제공: backdrop, ESC 닫기, aria-modal, inert
- backdrop 클릭 닫기: `modal.addEventListener('click', e => { if (e.target === modal) closeModal(); })`
- iOS Safari 스크롤 잠금: `document.body.style.overflow = 'hidden'` on open, `''` on close

#### 5-4. WebSocket 로그 파싱 (line accumulator + rAF 배칭)

```js
var lineBuf = '';      // 불완전 라인 보관
var logDirty = false;
var rafId = 0;

function processWsData(data) {
    var combined = lineBuf + data;
    var lines = combined.split('\n');
    lineBuf = lines[lines.length - 1];  // 마지막 불완전 라인 보관

    for (var i = 0; i < lines.length - 1; i++) {
        var l = lines[i];
        if (l === '') continue;
        rawLogs.push(l);

        // 카드 업데이트
        var bt = l.match(/bt: ([0-9A-F:]{17}) rssi=(-?\d+)/);
        if (bt) { updateCardRssi(bt[1], parseInt(bt[2])); continue; }
        var pair = l.match(/bt: ([0-9A-F:]{17}) paired (.+)/);
        if (pair) { onDevicePaired(pair[1], pair[2]); continue; }
        var sm = l.match(/sm: ([0-9A-F:]{17}) (detecting|present|absent|unlock)/);
        if (sm) { updateCardState(sm[1], sm[2], l); continue; }
    }
    logDirty = true;
    if (!rafId) rafId = requestAnimationFrame(flushRender);
}

function flushRender() {
    rafId = 0;
    if (logDirty) { renderFilteredLogs(); logDirty = false; }
}
```

#### 5-5. WS 재연결 (epoch 기반 + REST 동기화)

```js
var wsEpoch = 0;
var syncing = false;
var wsBuffer = [];

function wsConnect() {
    var currentEpoch = ++wsEpoch;
    syncing = true; wsBuffer = [];

    ws = new WebSocket('ws://' + host + '/ws?token=' + wsToken);
    ws.onopen = function() {
        // REST로 전체 상태 fetch
        var xhr = new XMLHttpRequest();
        xhr.open('GET', '/api/devices');
        xhr.onload = function() {
            if (currentEpoch !== wsEpoch) return;
            if (xhr.status === 200) applyFullState(JSON.parse(xhr.responseText));
            // 버퍼 flush
            for (var i = 0; i < wsBuffer.length; i++) processWsData(wsBuffer[i]);
            wsBuffer = []; syncing = false;
        };
        xhr.onerror = function() {
            if (currentEpoch !== wsEpoch) return;
            for (var i = 0; i < wsBuffer.length; i++) processWsData(wsBuffer[i]);
            wsBuffer = []; syncing = false;
        };
        xhr.send();
    };
    ws.onmessage = function(evt) {
        if (currentEpoch !== wsEpoch) return;
        if (syncing) wsBuffer.push(evt.data);
        else processWsData(evt.data);
    };
    ws.onclose = function() {
        if (currentEpoch !== wsEpoch) return;
        // 지수 백오프 + 지터
        var delay = Math.min(reconnectDelay + Math.random() * 1000, 30000);
        reconnectDelay = Math.min(reconnectDelay * 2, 30000);
        setTimeout(wsConnect, delay);
    };
}
```

- epoch로 stale 콜백 무시 (빠른 연속 재연결 안전)
- syncing 중 WS 메시지 버퍼링 → REST 응답 후 순서대로 flush (race condition 방지)

#### 5-6. Stale 감지 (클라이언트 타이머)

```js
var lastBtTime = {};  // MAC → Date.now()
setInterval(function() {
    var now = Date.now();
    // 모든 present 카드에 대해 3초 무응답 체크 → stale 클래스 토글
}, 2000);
```

단일 setInterval로 전체 카드 순회. 15개 카드 × 타임스탬프 비교 = 무시할 수준.

---

## Edge Cases & Error Handling

| 시나리오 | 처리 |
|---|---|
| NVS 풀/손상 | device_config_get() → 기본값 반환 + 로그 경고 |
| NVS blob 버전/크기 불일치 | 기본값 폴백 + 오래된 blob erase |
| 본딩됨 + SM 슬롯 없음 | /api/devices에서 `detected=false, last_seen_ms=0` |
| 15대 동시 burst | 큐 drain 루프로 tick당 최대 16개 처리 |
| 로그 ring buffer 드랍 | WS 재연결 시 /api/devices 재호출로 복구 |
| WS 프레임 경계에서 라인 분할 | line accumulator buffer로 불완전 라인 보관 |
| 기기 삭제 중 SM feed 도착 | bond 삭제됨 → 다음 리부팅 후 사라짐 |
| BTU 콜백에서 config 생성 | SmMsg{CreateConfig}로 비동기 처리 (BTU 블로킹 방지) |
| 모달 열린 상태 config 변경 | 저장 → API 호출 → 성공 시 모달 내 값 갱신 |

## Acceptance Criteria

### Functional

- [ ] 기기별 rssi_threshold, presence_timeout_ms, enter_window_ms, enter_min_count 독립 적용
- [ ] 별명 설정/수정, 페어링 시 기기명으로 자동 설정
- [ ] 카드 UI: 컴팩트 카드 리스트, 보더 색상 + 리플 애니메이션으로 상태 표현
- [ ] 기기 상세 모달: 실시간 상태 + 설정 편집 + 저장/삭제
- [ ] 페어링 모달: 스피너 + 안내 + 연결됨 피드백 + 닫기=종료
- [ ] 온보딩: 기기 0개 시 안내 표시
- [ ] 로그 뷰어: 우상단 [📋] → 전체화면 오버레이 (기존 기능 유지)
- [ ] [관리] 탭: Danger Zone (OTA, WiFi, 계정, 재부팅)

### Technical

- [ ] 기존 호스트 테스트 14개 전부 통과 (리팩토링 후)
- [ ] 신규 테스트 7개 이상 추가
- [ ] NVS blob 버전 관리 — 구조체 변경 시 안전한 폴백
- [ ] MAC 주소 대문자 통일
- [ ] SM Task 스택 4KB 유지 (dump_states가 전역에 직접 쓰므로)
- [ ] WS 재연결 시 epoch 기반 동기화

## References

- 브레인스톰: `docs/brainstorms/2026-04-07-per-device-config-and-ui-brainstorm.md`
- 현재 config: `components/config/include/config.h:10-45`
- 현재 SM: `components/statemachine/include/statemachine.h`, `statemachine.cpp`
- SM Task: `main/sm_task.cpp` (스택 4096, 큐 깊이 16, tick 2초)
- BT Manager 로그: `bt_manager.cpp:677` (BLE), `:905` (Classic), `:869` (auth)
- HTTP: `main/http_server.cpp:753-771` (라우트), `:556` (devices_list chunked 패턴)
- NVS: `main/config_service.cpp` (mutex + NVS 패턴), 파티션 24KB
- 호스트 테스트: `host_test/statemachine_test.cpp`, `host_test/config_test.cpp`
- CI: `.github/workflows/build.yml` (GTest v1.15.2, HOST_TEST)
