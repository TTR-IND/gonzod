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

ensure_preempt_lazy() {
    log "adding preempt=lazy to the default Linux GRUB entry"
    local grub="/etc/default/grub"
    local cfg="/boot/grub/grub.cfg"

    # Durable source: GRUB_CMDLINE_LINUX_DEFAULT is what the default
    # (non-recovery) linux line is built from.
    if [ -f "$grub" ]; then
        if grep -q '^GRUB_CMDLINE_LINUX_DEFAULT=.*preempt=lazy' "$grub"; then
            log "preempt=lazy already in GRUB_CMDLINE_LINUX_DEFAULT"
        elif grep -q '^GRUB_CMDLINE_LINUX_DEFAULT=' "$grub"; then
            sed -i '/^GRUB_CMDLINE_LINUX_DEFAULT=/ s/"$/ preempt=lazy"/' "$grub"
            # If the line used single quotes, handle that too.
            sed -i "/^GRUB_CMDLINE_LINUX_DEFAULT=/ s/'$/ preempt=lazy'/" "$grub"
            log "appended preempt=lazy to GRUB_CMDLINE_LINUX_DEFAULT"
        else
            echo 'GRUB_CMDLINE_LINUX_DEFAULT="quiet preempt=lazy"' >> "$grub"
            log "created GRUB_CMDLINE_LINUX_DEFAULT with preempt=lazy"
        fi
    else
        warn "$grub not found"
    fi

    if command -v update-grub >/dev/null 2>&1; then
        update-grub || die "update-grub failed"
    elif command -v grub-mkconfig >/dev/null 2>&1; then
        grub-mkconfig -o "$cfg" || die "grub-mkconfig failed"
    else
        warn "update-grub not found"
    fi

    # Confirm the default menuentry's linux line carries the flag.
    # The first 'menuentry' is the default; its first 'linux' line is the boot line.
    if [ -f "$cfg" ]; then
        if awk '
            /^menuentry / { in_default = (seen++ == 0) }
            in_default && /^[[:space:]]*linux / {
                if ($0 ~ /preempt=lazy/) found = 1
                exit
            }
            END { exit !found }
        ' "$cfg"; then
            log "default linux line already has preempt=lazy"
        else
            # Inject into the first linux line of the first menuentry only.
            awk '
                /^menuentry / { in_default = (seen++ == 0) }
                in_default && !done && /^[[:space:]]*linux / && $0 !~ /preempt=lazy/ {
                    sub(/$/, " preempt=lazy")
                    done = 1
                }
                { print }
            ' "$cfg" > "$cfg.tmp" && mv "$cfg.tmp" "$cfg"
            log "patched default menuentry linux line with preempt=lazy"
        fi
    else
        warn "$cfg not found — flag is in /etc/default/grub; regenerate the bootloader"
    fi
    log "reboot to boot the stock kernel with preempt=lazy"
}

install_all() {
    [ -f "$SRC" ] || die "gonzod.c not found at $SRC"
    ensure_preempt_lazy
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
