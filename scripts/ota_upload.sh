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
    echo "  target    MAC address (c0:5d:89:df:1f:f0)"
    echo "            or host:port (office.cartanova.ai:20080)"
    echo "            or IP address (192.168.1.42)"
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

# ── Target 판별: MAC vs host(:port) ──

is_mac() { [[ "$1" =~ ^([0-9a-fA-F]{2}[:\-]){5}[0-9a-fA-F]{2}$ ]]; }

if is_mac "$TARGET"; then
    # MAC → ARP → IP
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

    HOST="$IP"
    LABEL="$IP ($MAC)"
else
    # host:port 또는 host 그대로 사용
    HOST="$TARGET"
    LABEL="$TARGET"
fi

# ── OTA 업로드 ──

SIZE=$(wc -c < "$FW" | tr -d ' ')
echo "Device : $LABEL"
echo "Firmware: $(basename "$FW") ($SIZE bytes)"
echo ""

if $DRY_RUN; then
    echo "(dry run — skipping upload)"
    exit 0
fi

log "POST http://$HOST/api/firmware/upload"

curl -u "$USER:$PASS" \
     -X POST \
     -H "Content-Type: application/octet-stream" \
     --data-binary "@$FW" \
     --progress-bar \
     --max-time 120 \
     "http://$HOST/api/firmware/upload" \
    | cat

echo ""
echo "Upload complete. Device is rebooting."
