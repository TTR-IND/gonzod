#!/usr/bin/env bash
#
# install.sh -- build and install gonzod, then apply policy.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$SCRIPT_DIR/gonzod.c"
BIN="/usr/local/sbin/gonzod"
INSTALL_LOG="/var/log/gonzod-install-$(date +%Y%m%d-%H%M%S).log"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log()  { echo -e "${GREEN}[gonzod-install]${NC} $*"; }
warn() { echo -e "${YELLOW}[gonzod-install] WARNING:${NC} $*"; }
die()  { echo -e "${RED}[gonzod-install] ERROR:${NC} $*" >&2; exit 1; }

require_root() {
    [ "$(id -u)" -eq 0 ] || die "must be run as root"
}

install_rt_kernel() {
    log "installing PREEMPT_RT kernel (linux-image-rt-amd64 + headers)"
    export DEBIAN_FRONTEND=noninteractive
    apt-get install -y linux-image-rt-amd64 linux-headers-rt-amd64 \
        || die "apt failed installing linux-image-rt-amd64 / linux-headers-rt-amd64"
    if command -v update-grub >/dev/null 2>&1; then
        update-grub || die "update-grub failed"
    elif command -v grub-mkconfig >/dev/null 2>&1; then
        grub-mkconfig -o /boot/grub/grub.cfg || die "grub-mkconfig failed"
    else
        warn "update-grub not found — RT kernel is installed, bootloader not refreshed"
    fi
    log "RT kernel packaged. Reboot onto the -rt image before expecting PREEMPT_RT."
}

install_all() {
    [ -f "$SRC" ] || die "gonzod.c not found at $SRC"
    install_rt_kernel
    log "building gonzod..."
    local tmp="/tmp/gonzod-build-$$"
    gcc -O2 -Wall -Wextra -Wno-unused-parameter -o "$tmp" "$SRC" \
        || die "build failed"
    install -m 0755 -o root -g root "$tmp" "$BIN"
    rm -f "$tmp"
    log "installed $BIN"
    log "wiring lifecycle hooks..."
    "$BIN" install
    log "applying policy..."
    "$BIN" enable
    log "status:"
    "$BIN" status || true
}

uninstall_all() {
    if [ -x "$BIN" ]; then
        "$BIN" disable || true
        "$BIN" uninstall || true
    fi
    rm -f "$BIN"
    rm -rf /run/gonzo
    log "uninstall complete"
}

main_inner() {
    require_root
    case "${1:-}" in
        --uninstall) uninstall_all ;;
        -h|--help)
            echo "Usage: sudo $0 [--uninstall]"
            ;;
        "") install_all ;;
        *) die "unrecognized argument: $1" ;;
    esac
    log "done. log: $INSTALL_LOG"
}

main() {
    touch "$INSTALL_LOG" 2>/dev/null || INSTALL_LOG="/tmp/gonzod-install-$(date +%Y%m%d-%H%M%S).log"
    main_inner "$@" 2>&1 | tee -a "$INSTALL_LOG"
    exit "${PIPESTATUS[0]}"
}

main "$@"
