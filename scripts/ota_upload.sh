#!/usr/bin/env bash
set -euo pipefail

VERBOSE=false
DRY_RUN=false

# ── 옵션 파싱 (아무 위치에서나 허용) ──
ARGS=()
for arg in "$@"; do
    case "$arg" in
        -v) VERBOSE=true ;;
        -n|--dry-run) DRY_RUN=true ;;
        *)  ARGS+=("$arg") ;;
    esac
done

usage() {
    echo "Usage: $0 <target> <firmware.bin> <user> <pass> [-v] [-n]"
    echo ""
    echo "  target    MAC address            (c0:5d:89:df:1f:f0)         — 로컬 LAN, http"
    echo "            IP                     (192.168.1.42)              — 로컬 LAN, http"
    echo "            host[:port]            (office.cartanova.ai:20080) — http (scheme 미지정)"
    echo "            URL with scheme        (https://doorman.example/)  — 그대로 사용"
    echo ""
    echo "            scheme 미지정 + 공인 도메인인 경우 http://로 접속합니다."
    echo "            TLS 프록시 뒤의 기기는 반드시 https:// prefix를 명시하세요."
    echo ""
    echo "  firmware  Path to .bin file"
    echo "  user      HTTP Basic Auth username"
    echo "  pass      HTTP Basic Auth password"
    echo "  -v        Verbose output"
    echo "  -n        Dry run (resolve only, skip upload)"
    exit 1
}

log() { $VERBOSE && echo "[*] $*" || true; }

[ ${#ARGS[@]} -eq 4 ] || usage

TARGET="${ARGS[0]}"
FW="${ARGS[1]}"
USER="${ARGS[2]}"
PASS="${ARGS[3]}"

[ -f "$FW" ] || { echo "Error: $FW not found"; exit 1; }

# ── Target 판별: scheme / MAC / host ──

is_mac() { [[ "$1" =~ ^([0-9a-fA-F]{2}[:\-]){5}[0-9a-fA-F]{2}$ ]]; }
has_scheme() { [[ "$1" =~ ^https?:// ]]; }
# 숫자로 시작하는 IPv4(옵션 포트 포함)는 로컬 LAN으로 간주하고 http로 간주.
# 점이 포함된 비-IP는 FQDN → reverse proxy 뒤라 가정하고 https 기본.
is_ipv4() { [[ "$1" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}(:[0-9]+)?$ ]]; }

if has_scheme "$TARGET"; then
    # 사용자가 scheme을 명시한 경우 그대로 사용 (http:// 또는 https://)
    BASE_URL="${TARGET%/}"
    LABEL="$TARGET"
elif is_mac "$TARGET"; then
    # MAC → ARP → IP (로컬 LAN)
    MAC=$(echo "$TARGET" | tr '[:upper:]' '[:lower:]' | tr '-' ':')

    log "Detecting Wi-Fi interface..."
    WIFI_IF=$(networksetup -listallhardwareports 2>/dev/null \
        | awk '/Wi-Fi/{getline; print $2}')
    WIFI_IF=${WIFI_IF:-en0}
    log "Interface: $WIFI_IF"

    BCAST=$(ifconfig "$WIFI_IF" 2>/dev/null | awk '/broadcast/{print $NF}')
    if [ -n "$BCAST" ]; then
        log "Broadcast ping to $BCAST (ARP cache refresh)..."
        ping -c 1 -t 1 "$BCAST" >/dev/null 2>&1 || true
    else
        log "No broadcast address found, skipping ARP refresh"
    fi

    log "Searching ARP table for $MAC..."
    IP=$(arp -an | grep -i "$MAC" | head -1 | grep -oE '\([0-9.]+\)' | tr -d '()')

    if [ -z "$IP" ]; then
        echo "Error: MAC $MAC not found in ARP table."
        echo "Ensure the device is connected to the same network."
        exit 1
    fi

    BASE_URL="http://$IP"
    LABEL="$IP ($MAC)"
elif is_ipv4 "$TARGET"; then
    # 로컬 LAN IP — plain HTTP
    BASE_URL="http://$TARGET"
    LABEL="$TARGET"
else
    # FQDN으로 간주 — reverse proxy(Caddy/nginx) TLS 종단 가정하여 https 기본
    # http로 하고 싶으면 "http://<host>" 형태로 명시적 prefix 필요
    BASE_URL="https://$TARGET"
    LABEL="$TARGET"
fi

URL="$BASE_URL/api/firmware/upload"

# ── OTA 업로드 ──

SIZE=$(wc -c < "$FW" | tr -d ' ')
echo "Device : $LABEL"
echo "Firmware: $(basename "$FW") ($SIZE bytes)"
echo "Endpoint: $URL"
echo ""

if $DRY_RUN; then
    echo "(dry run — skipping upload)"
    exit 0
fi

log "POST $URL"

# -L: 3xx redirect 자동 follow (Caddy 등 리버스 프록시의 http→https 308 대비)
# --post301/302/303: redirect 시 POST body 재전송 (기본은 GET으로 전환됨)
# --location-trusted: redirect 후에도 Basic Auth creds 유지 (same-host 가정).
#   주의: 의도치 않은 외부 호스트로 redirect되면 credential 유출. FQDN이
#   자기 도메인인 경우에만 안전.
# --max-time 300: 원격 HTTPS + TLS handshake + flash write 포함 여유
# --fail-with-body: HTTP 4xx/5xx 응답 시 exit code 비영으로 (false positive 방지)
curl -u "$USER:$PASS" \
     -X POST \
     -H "Content-Type: application/octet-stream" \
     --data-binary "@$FW" \
     --progress-bar \
     --max-time 300 \
     -L --location-trusted --post301 --post302 --post303 \
     --fail-with-body \
     "$URL" \
    | cat

echo ""
echo "Upload complete. Device is rebooting."
