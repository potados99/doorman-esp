# 2차 MVP — WiFi 프로비저닝 + 웹 제어 + GPIO 도어 펄스

**날짜**: 2026-03-27
**상태**: 확정
**선행 문서**: `docs/brainstorms/2026-03-25-doorman-architecture-brainstorm.md`

-----

## 무엇을 만드는가

1차 MVP(SoftAP OTA + BT presence PoC)를 기반으로, **실제로 웹에서 문을 열 수 있는 상태**까지 만든다.
BT presence 자동 감지는 아직 연결하지 않는다. 이번 단계의 핵심은:

- WiFi 프로비저닝: SoftAP에서 SSID/PW 입력 → NVS 저장 → 재부팅 → STA로 AP 접속
- 웹 서버: HTTP Basic Auth 보호 하에 문열기 버튼, OTA 업로드, 계정 변경, WiFi 재설정
- GPIO 도어 펄스: 웹 버튼 클릭 → 특정 핀 HIGH 0.5초 → LOW

-----

## 왜 이 접근인가

- **WiFi 프로비저닝이 먼저**: BT presence → 자동 문열림은 3차 이후. 그 전에 기기가 네트워크에 안정적으로 붙어 있어야 웹 제어, OTA, 모니터링이 가능하다.
- **수동 문열기부터**: 자동 감지 전에 "웹에서 버튼 눌러서 문 여는 것"만으로도 즉시 실용 가치가 있다.
- **SoftAP 최소 노출**: SoftAP에서는 WiFi 설정만 받고, OTA나 문열기는 STA 모드에서만 Basic Auth 뒤에 둔다. 공격 표면 최소화.
- **mDNS**: DHCP IP를 외울 필요 없이 `doorman.local`로 접근.

-----

## 핵심 결정사항

### 1. WiFi 부팅 흐름

```
부팅
 ├─ NVS에 저장된 SSID/PW 있음?
 │   ├─ Yes → STA 접속 시도
 │   │   ├─ 성공 → STA 모드 (mDNS 등록, 웹서버 시작)
 │   │   └─ 실패 → 즉시 SoftAP 폴백
 │   └─ No → SoftAP 모드
 └─ SoftAP 모드: 프로비저닝 페이지 서빙
```

- STA 접속 실패 시 재시도 없이 **즉시 SoftAP 폴백**.
- SoftAP SSID: `Doorman-Setup`, PW: 기존과 동일 (`12345678` 또는 기기별 고유값).
- STA 모드에서 `doorman.local`로 mDNS 등록.

### 2. 웹 페이지 구성

**두 개의 분리된 페이지**:

| 페이지 | 모드 | 인증 | 기능 |
|--------|------|------|------|
| 프로비저닝 | SoftAP | 없음 | WiFi SSID/PW 입력 폼 |
| 메인 | STA | HTTP Basic Auth | 문열기 버튼, OTA 업로드, 계정 변경, WiFi 재설정 |

- SoftAP 모드에서는 **프로비저닝 페이지만** 서빙. OTA, 문열기 없음.
- STA 모드에서는 **메인 페이지만** 서빙. 모든 기능은 Basic Auth 뒤에.

### 3. 인증

- **기본 계정**: `admin` / `admin`
- **저장**: NVS에 username + password 저장
- **방식**: HTTP Basic Auth (브라우저 네이티브 로그인 팝업)
- **변경**: STA 모드 웹 UI에서 username/password 변경 가능
- **SoftAP 보안**: SoftAP 자체의 WiFi 비밀번호가 보안 경계. 프로비저닝 페이지에 인증 없음.

**이유**: SoftAP에서 계정 정보를 노출하면 도용 위험. 기본 계정으로 시작하되, STA 모드(네트워크 내부)에서만 변경 가능하게 하여 SoftAP 공격 표면을 최소화.

### 4. GPIO 도어 펄스

- **핀 번호**: GPIO 4 (WROVER-IE 기준 안전한 범용 출력 핀. 코드에서 상수로 쉽게 변경 가능하게)
- **동작**: HIGH 0.5초 → LOW (active-high)
- **트리거**: STA 모드 웹 UI의 "문열기" 버튼 → HTTP API 호출
- **동시 호출 방지**: 펄스 진행 중에는 추가 요청 무시 또는 대기

### 5. OTA

- 1차 MVP의 OTA 업로드 로직 재활용.
- **STA 모드에서만** OTA 가능 (SoftAP에서는 제거).
- Basic Auth 뒤에 보호.

### 6. STA 모드 웹 UI 기능 정리

1. **문열기 버튼**: 누르면 GPIO 펄스 발생. 즉각 피드백 ("열림!" 또는 "이미 동작 중").
2. **OTA 업로드**: 기존 1차 MVP와 동일한 .bin 업로드 + 진행률.
3. **계정 변경**: username + password 변경 폼. 저장 시 NVS에 기록.
4. **WiFi 재설정**: 새 SSID/PW 입력 → NVS 저장 → 재부팅 → 새 AP에 접속 시도.

### 7. NVS 저장 항목

NVS namespace를 기능 영역별로 분리한다. 향후 BT, OTA 등 설정이 늘어나도 충돌 없이 확장 가능.

| Namespace | Key | 값 | 기본값 |
|-----------|-----|----|--------|
| `net` | `ssid` | WiFi SSID | (없음 → SoftAP 모드) |
| `net` | `pass` | WiFi 비밀번호 | (없음) |
| `auth` | `user` | 웹 로그인 사용자명 | `admin` |
| `auth` | `pass` | 웹 로그인 비밀번호 | `admin` |

**향후 확장 예시** (2차 MVP 범위 밖):

| Namespace | Key | 용도 |
|-----------|-----|------|
| `door` | `gpio_pin` | 도어 제어 GPIO 핀 번호 |
| `door` | `pulse_ms` | 펄스 지속 시간 (ms) |
| `bt` | `scan_int` | BT 스캔 간격 |
| `ota` | `poll_url` | GitHub Releases 폴링 URL |

- ESP-IDF NVS namespace/key 모두 최대 15자. 위 네이밍은 여유 있음.
- 비밀번호는 평문 저장 (2차 MVP). NVS 암호화는 후순위.

### 8. 프론트엔드

- 1차 MVP와 동일: Vanilla HTML + JS, 바이너리 임베드.
- 두 파일: `setup.html` (SoftAP 프로비저닝) + `index.html` (STA 메인).
- CSS는 인라인 또는 최소 `<style>` 블록. CDN 의존 없음 (SoftAP에서는 인터넷 없으므로).

-----

## 1차 MVP와의 차이

| 항목 | 1차 MVP | 2차 MVP |
|------|---------|---------|
| WiFi | SoftAP 상시 | SoftAP → STA 전환 (프로비저닝) |
| 웹 서버 | OTA 업로드만 | 문열기 + OTA + 계정 + WiFi 설정 |
| 인증 | 없음 | HTTP Basic Auth (STA 모드) |
| GPIO | 없음 | 도어 펄스 (0.5초 HIGH) |
| mDNS | 없음 | doorman.local |
| SoftAP OTA | 있음 | 제거 (보안) |
| NVS | 미사용 | WiFi + 계정 정보 저장 |

-----

## 범위 밖 (3차 이후)

- BT presence → Gatekeeper → 자동 문열림
- GitHub Releases 폴링 자동 OTA
- 웹소켓 실시간 상태 모니터링
- 등록 기기 관리 UI
- HTTPS / TLS
- Captive portal (SoftAP DNS 리다이렉트)

-----

## 해결된 질문

- **SoftAP에서 계정 설정 노출?** → 안 한다. 기본 admin/admin으로 시작, STA에서만 변경.
- **SoftAP에서 OTA 유지?** → 제거. 공격 표면 최소화 우선. 복구는 시리얼로.
- **STA 실패 시 정책?** → 즉시 SoftAP 폴백. 재시도 없음.
- **기기 IP 확인 방법?** → mDNS (`doorman.local`).
- **GPIO 핀 번호?** → GPIO 4. WROVER-IE 기준 안전한 범용 출력 핀. 상수로 쉽게 변경 가능하게.
- **SoftAP WiFi 비밀번호?** → 고정 PW(`12345678`) 유지.
- **Basic Auth 비밀번호 저장 방식?** → NVS 평문 저장. 암호화는 후순위.
- **STA 접속 타임아웃?** → 10초. 실패 시 즉시 SoftAP 폴백.

## 미해결 질문

(모두 해결됨 — 아래 "해결된 질문" 섹션으로 이동)

-----

## 다음 단계

→ 미해결 질문 정리 후 `/workflows:plan`으로 구현 플랜 작성
