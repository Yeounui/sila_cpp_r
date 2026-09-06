#!/usr/bin/env bash
# pi-validate.sh — single entry point run ON a Raspberry Pi to cover the
# Pi-side portion of raspberry-pi-manual-validation.md (build, 4.4a
# destruction-order ASan check, interop server residency). PC-side items
# (1.1e, 1.4e/3.3a, 2.1a/2.1b, 3.2a/3.2e, 3.9a, mDNS) are out of scope — see
# runbook L23-31 "실행 위치" column.
set -euo pipefail

on_err() {
    log "FAILED: line $1: $2"
    exit 1
}
trap 'on_err "$LINENO" "$BASH_COMMAND"' ERR

# --- constants ---
# Script lives at tests/physical/pi-validate.sh, so repo root is two levels up.
REPO="$(readlink -f "$(dirname "$0")/../..")"
STATE_DIR=/var/lib/pi-validate
APT_LIST="$STATE_DIR/apt.list"
VCPKG_MARK="$STATE_DIR/vcpkg-bootstrapped"
ARTIFACTS="$REPO/artifacts/pi-manual"
PORT=50052        # runbook L113
ASAN_PORT=50099   # runbook L265
# First 11 packages: runbook L87-88. rsync added: runbook L66 source transfer.
APT_PKGS=(build-essential cmake ninja-build pkg-config git curl
           ca-certificates zip unzip tar autoconf libtool rsync)

RESULTS=()

# --- common ---
log() { echo "[pi-validate] $*" >&2; }
die() { log "$*"; exit 1; }

need_sudo() {
    if [ "$EUID" -ne 0 ] && ! command -v sudo >/dev/null 2>&1; then
        die "root or sudo required"
    fi
}

record() {
    # $1=verdict(PASS|FAIL|GAP) $2=id $3=detail
    RESULTS+=("$1"$'\t'"$2"$'\t'"$3")
}

# --- args ---
MODE=validate

parse_args() {
    case "$#" in
        0) MODE=validate ;;
        1)
            case "$1" in
                -i) MODE=install ;;
                -d) MODE=deinstall ;;
                -h|--help) usage; exit 0 ;;
                *) usage; exit 2 ;;
            esac
            ;;
        *) usage; exit 2 ;;
    esac
}

usage() {
    cat <<'EOF'
Usage: pi-validate.sh [-i | -d | -h]
  (no args)  run build + 4.4a validation, then serve interop server
  -i         install prerequisites (apt packages, vcpkg bootstrap)
  -d         undo what -i installed
  -h/--help  show this help
EOF
}

# --- install ---
mode_install() {
    need_sudo
    ensure_state_dir
    apt_install_tracked
    vcpkg_bootstrap
    log "install complete"
}

ensure_state_dir() {
    sudo mkdir -p "$STATE_DIR"
}

apt_install_tracked() {
    local new_pkgs=()
    local pkg status
    for pkg in "${APT_PKGS[@]}"; do
        # Only track packages we actually install here, so -d never removes
        # something that was already on the system before -i ran.
        status="$(dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null || true)"
        if [ "$status" != "install ok installed" ]; then
            new_pkgs+=("$pkg")
        fi
    done

    if [ "${#new_pkgs[@]}" -eq 0 ]; then
        log "apt packages already present, skipping apt-get"
        return 0
    fi

    sudo apt-get update
    sudo apt-get install -y "${new_pkgs[@]}"

    # Merge with whatever is already recorded so repeated -i runs accumulate
    # correctly instead of clobbering an earlier partial install's record.
    { [ -f "$APT_LIST" ] && sudo cat "$APT_LIST"; printf '%s\n' "${new_pkgs[@]}"; } \
        | sort -u | sudo tee "$APT_LIST" >/dev/null
}

vcpkg_bootstrap() {
    if [ -x "$REPO/third_party/vcpkg/vcpkg" ]; then
        return 0
    fi
    "$REPO/third_party/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
    sudo mkdir -p "$STATE_DIR"
    sudo touch "$VCPKG_MARK"
}

# --- deinstall ---
mode_deinstall() {
    if [ ! -d "$STATE_DIR" ]; then
        log "nothing to undo ($STATE_DIR absent)"
        exit 0
    fi
    need_sudo
    apt_remove_tracked
    remove_vcpkg_artifacts
    remove_build_artifacts
    remove_state
    print_deinstall_caveats
}

apt_remove_tracked() {
    if [ ! -s "$APT_LIST" ]; then
        return 0
    fi
    # shellcheck disable=SC2046
    sudo apt-get remove -y $(cat "$APT_LIST")
}

remove_vcpkg_artifacts() {
    if [ ! -f "$VCPKG_MARK" ]; then
        return 0
    fi
    sudo rm -rf "$REPO/third_party/vcpkg/vcpkg" \
        "$REPO/third_party/vcpkg/downloads" \
        "$REPO/third_party/vcpkg/buildtrees" \
        "$REPO/third_party/vcpkg/packages" \
        "$REPO/third_party/vcpkg/installed"
}

remove_build_artifacts() {
    sudo rm -rf "$REPO/build/gcc" "$REPO/build/asan" "$REPO/server-cert.pem" "$ARTIFACTS"
}

remove_state() {
    sudo rm -rf "$STATE_DIR"
}

print_deinstall_caveats() {
    cat <<'EOF'
[pi-validate] -d does not remove:
  - transitive apt dependencies pulled in alongside tracked packages
    (run `sudo apt autoremove` yourself if you want those gone)
  - the apt package index refreshed by `apt-get update`
  - build/ subdirectories that predate -i (e.g. build/tsan, build/clang)
  - packages that were already installed before -i ran
EOF
}

# --- validate ---
mode_validate() {
    preflight
    build_preset default
    build_preset asan
    check_destruction_asan
    report_summary
    serve_interop
}

preflight() {
    if [ ! -x "$REPO/third_party/vcpkg/vcpkg" ]; then
        die "vcpkg not bootstrapped — run '$0 -i' first"
    fi
    command -v cmake >/dev/null 2>&1 || die "cmake missing — run '$0 -i' first"
    command -v ninja >/dev/null 2>&1 || die "ninja missing — run '$0 -i' first"

    local busy
    # sudo needed for -p (PID): plain ss hides the owning PID for sockets
    # this user doesn't own, which is the common case for a stray old run.
    busy="$(sudo ss -ltnp "( sport = :$PORT or sport = :$ASAN_PORT )" 2>/dev/null | tail -n +2 || true)"
    if [ -n "$busy" ]; then
        die "port $PORT or $ASAN_PORT already in use: $busy"
    fi

    mkdir -p "$ARTIFACTS"
}

build_preset() {
    local name="$1"
    local dir
    case "$name" in
        default) dir=gcc ;;
        asan) dir=asan ;;
        *) die "unknown preset: $name" ;;
    esac

    if [ ! -d "$REPO/build/$dir" ]; then
        log "first build of '$name' preset — vcpkg source build can take 30-60 minutes"
    fi

    cmake --preset "$name" -S "$REPO"
    cmake --build "$REPO/build/$dir" --target sila2_interop_server -j"$(nproc)"
}

check_destruction_asan() {
    local out="$ARTIFACTS/4.4a-destruction-asan.txt"
    local status

    # set +e around this pipeline only: `timeout` is expected to exit 124
    # (the 3s deadline firing is the intended path now that SIGTERM returns
    # cleanly from main() — see server_main.cc) and `set -e` would otherwise
    # abort the whole script on that expected non-zero exit. PIPESTATUS[0]
    # captures the sanitized binary's own exit code past the `tee` in front
    # of it, since $? after a pipe reflects tee, not the binary.
    set +e
    ASAN_OPTIONS=detect_leaks=0 timeout 3 \
        "$REPO/build/asan/tests/interop/sila2_interop_server" --port "$ASAN_PORT" \
        2>&1 | tee "$out"
    status="${PIPESTATUS[0]}"
    set -e

    if [ "$status" -ne 124 ] && [ "$status" -ne 0 ]; then
        log "4.4a: unexpected exit code $status (see $out)"
    fi

    # This check targets destruction ORDER (use-after-free), not memory
    # leaks; a short-lived process's LeakSanitizer output would otherwise
    # clutter the destruction-order verdict, hence detect_leaks=0 above.
    local failed=0
    if grep -q 'ERROR: AddressSanitizer' "$out"; then
        record FAIL 4.4a "use-after-free or other ASan error during destruction, see $out"
        failed=1
    fi
    if grep -q 'runtime error:' "$out"; then
        record FAIL 4.4a "UBSan finding during destruction, see $out"
        failed=1
    fi
    if [ "$failed" -eq 0 ]; then
        record PASS 4.4a "clean destruction under SIGTERM without Shutdown(), see $out"
    fi
}

report_summary() {
    local has_fail=0
    local verdict id detail
    log "=== validation summary ==="
    for line in "${RESULTS[@]}"; do
        IFS=$'\t' read -r verdict id detail <<<"$line"
        printf '%-4s %-8s %s\n' "$verdict" "$id" "$detail" >&2
        if [ "$verdict" = FAIL ]; then
            has_fail=1
        fi
    done

    if [ "$has_fail" -eq 1 ]; then
        # Stop here instead of exec-ing into serve_interop: exec replaces
        # this process, so the final exit code would become the server's,
        # silently swallowing the 4.4a failure. Build cache is intact, so a
        # re-run after a fix is cheap.
        log "FAIL present — not starting interop server. Fix and re-run (build cache is intact)."
        exit 1
    fi
}

serve_interop() {
    log "PC next steps:"
    log "  scp \$PI_USER@\$(hostname):$REPO/server-cert.pem artifacts/pi-manual/"
    log "  export PI_HOST=$(hostname -I | awk '{print $1}')"
    exec "$REPO/build/gcc/tests/interop/sila2_interop_server" \
        --port "$PORT" --cert-out "$REPO/server-cert.pem" \
        --hostname "$(hostname)" --ip "$(hostname -I | awk '{print $1}')"
}

parse_args "$@"
case "$MODE" in
    install) mode_install ;;
    deinstall) mode_deinstall ;;
    validate) mode_validate ;;
esac
