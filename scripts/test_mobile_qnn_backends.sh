#!/usr/bin/env bash
# Real-device acceptance for the sherpa-onnx QNN backends (SenseVoice QNN +
# Paraformer QNN) - TAKEOVER_NOTES.md §13.4 item 1.
#
# Drives the PocketVoice Android app over adb: switches backend policy via a
# root prefs write, restarts the app, then sends the bench WAV set to port
# 27000 and compares the recognized text against build/model-packs/bench_refs.txt.
#
# Flow follows docs/DEV_PITFALLS.md §五:
#   D1  svc power stayon usb         (HyperOS screen-off kills adb)
#   D2  one script run = one adb session; every step verified
#   D3  forward warm-up + >=2s between requests
#   D4  root prefs write (input tap is blocked on HyperOS)
#   D5  prefs write needs chown + restorecon
#   D6  start MainActivity first (loads native libs), then the service
#
# Also deploys per-model QNN soc_id / dsp_arch override files (qnn_soc_id.txt /
# qnn_dsp_arch.txt) derived from the device's ro.soc.model, so the HTP config
# written by stt_engine.cpp matches the actual hardware (e.g. SM8735 -> v73).
#
# Requirements:
#   adb in PATH (or ADB env var), a rooted device (KernelSU/Magisk `su`),
#   the PocketVoice QNN APK installed, model packs under
#   /sdcard/Android/data/com.stt.mobile/files/models/.
#
# Usage:
#   scripts/test_mobile_qnn_backends.sh [--install] [--policies standard,fast]
#       [--wavs DIR] [--refs FILE] [--out DIR] [--skip-restore] [--force]
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ADB_BIN="${ADB:-}"
PKG="com.stt.mobile"
ACTIVITY="$PKG/.MainActivity"
PORT=27000
DEVICE_MODELS_BASE="/sdcard/Android/data/$PKG/files/models"
PREFS_PATH="/data/data/$PKG/shared_prefs/pocketvoice_prefs.xml"
RUNTIME_DIR="/data/data/$PKG/files/qnn-runtime"
APK="$ROOT_DIR/build/mobile-linux/PocketVoice-Android-qnn.apk"
WAVS_DIR="$ROOT_DIR/build/model-packs/bench_wavs"
REFS_FILE="$ROOT_DIR/build/model-packs/bench_refs.txt"
OUT_DIR="$ROOT_DIR/build/mobile-qnn-backend-test"
SEND_SCRIPT="$ROOT_DIR/scripts/send_wav_to_phone.js"
CER_SCRIPT="$ROOT_DIR/scripts/text_cer.js"
DEVICE_HELPER="/data/local/tmp/pv_set_policy.sh"
POLICIES="standard,fast"
REQUEST_INTERVAL="${REQUEST_INTERVAL:-2.5}"
INIT_TIMEOUT="${INIT_TIMEOUT:-90}"
INSTALL=0
SKIP_RESTORE=0
FORCE=0
NO_ROOT=0

declare -A POLICY_EXPECTED_BACKEND=( [standard]=sensevoice_qnn [fast]=paraformer_qnn )
declare -A POLICY_MODEL_SUBDIR=( [standard]=sensevoice [fast]=paraformer-qnn )

log()  { printf '[INFO] %s\n' "$*"; }
warn() { printf '[WARN] %s\n' "$*" >&2; }
err()  { printf '[ERROR] %s\n' "$*" >&2; }

usage() {
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

# --- adb plumbing -----------------------------------------------------------

adb_sh() { "$ADB_BIN" shell "$@" 2>&1 | tr -d '\r'; }

su_sh() { "$ADB_BIN" shell "su -c '$1'" 2>&1 | tr -d '\r'; }

adb_logcat() { "$ADB_BIN" logcat -d "$@" 2>/dev/null | tr -d '\r'; }

resolve_adb() {
    if [ -z "$ADB_BIN" ]; then
        if command -v adb >/dev/null 2>&1; then
            ADB_BIN="$(command -v adb)"
        elif [ -x "$HOME/tools/android-sdk/platform-tools/adb" ]; then
            ADB_BIN="$HOME/tools/android-sdk/platform-tools/adb"
        else
            err "adb not found in PATH; set ADB=/path/to/adb"
            exit 2
        fi
    fi
    log "adb: $ADB_BIN"
}

# --- preflight --------------------------------------------------------------

check_device() {
    local state
    state="$("$ADB_BIN" get-state 2>&1 | tr -d '\r')"
    if [ "$state" != "device" ]; then
        err "no adb device (get-state='$state'). Connect the phone and re-run."
        exit 2
    fi
    log "device: $("$ADB_BIN" get-serialno 2>/dev/null | tr -d '\r')"
}

check_root() {
    local who
    who="$(su_sh "id -u" | tr -d '\r')"
    if [ "$who" = "0" ]; then
        log "root: OK (su available)"
    else
        warn "root su not available (id -u='$who'); policy switching will fail."
        NO_ROOT=1
        if [ "$FORCE" -eq 0 ]; then
            err "re-run with --force to attempt anyway, or enable root"
            exit 2
        fi
    fi
}

device_file_exists() { # $1 = path
    [ -n "$(adb_sh "ls '$1' 2>/dev/null")" ]
}

list_device_dir() { # $1 = path
    adb_sh "ls -1 '$1' 2>/dev/null"
}

check_models() {
    log "checking model packs under $DEVICE_MODELS_BASE ..."
    local problems=0
    local sub files expected

    sub="$DEVICE_MODELS_BASE/sensevoice"
    files="$(list_device_dir "$sub")"
    if printf '%s\n' "$files" | grep -q "tokens.txt" \
        && { printf '%s\n' "$files" | grep -Eq "model.bin|libmodel.so"; }; then
        log "  sensevoice: OK ($(printf '%s\n' "$files" | tr '\n' ' '))"
    else
        err "  sensevoice: MISSING required files in $sub (tokens.txt + model.bin|libmodel.so)"
        problems=$((problems + 1))
        if printf '%s\n' "$(list_device_dir "$DEVICE_MODELS_BASE")" | grep -q "sensevoice-qnn"; then
            warn "    found sensevoice-qnn/ but resolveModelDir() looks for sensevoice/ - rename or copy it"
        fi
    fi

    sub="$DEVICE_MODELS_BASE/paraformer-qnn"
    files="$(list_device_dir "$sub")"
    if printf '%s\n' "$files" | grep -q "tokens.txt" \
        && printf '%s\n' "$files" | grep -q "libencoder.so" \
        && printf '%s\n' "$files" | grep -q "libpredictor.so" \
        && printf '%s\n' "$files" | grep -q "libdecoder.so"; then
        log "  paraformer-qnn: OK ($(printf '%s\n' "$files" | tr '\n' ' '))"
    else
        err "  paraformer-qnn: MISSING libencoder/libpredictor/libdecoder.so + tokens.txt in $sub"
        problems=$((problems + 1))
    fi

    log "  models dir listing:"
    adb_sh "ls -la '$DEVICE_MODELS_BASE' 2>/dev/null" | sed 's/^/    /'

    local free
    free="$(adb_sh "df -h /sdcard | tail -1")"
    log "  /sdcard: $free"

    if [ "$problems" -gt 0 ] && [ "$FORCE" -eq 0 ]; then
        err "model packs incomplete; push them first (see docs/TAKEOVER_NOTES.md §13.3) or use --force"
        exit 2
    fi
}

check_qnn_runtime() {
    log "checking QNN runtime dir $RUNTIME_DIR ..."
    local files missing=""
    files="$(su_sh "ls -1 '$RUNTIME_DIR' 2>/dev/null")"
    if [ -z "$files" ]; then
        warn "  qnn-runtime dir empty/absent (will fall back to nativeLibraryDir or vendor)"
        return
    fi
    for lib in libQnnHtp.so libQnnSystem.so libQnnHtpV73Skel.so libQnnHtpNetRunExtensions.so; do
        if printf '%s\n' "$files" | grep -q "$lib"; then
            log "  $lib: present"
        else
            warn "  $lib: MISSING"
            missing="$missing $lib"
        fi
    done
    if printf '%s\n' "$files" | grep -q "Skel" && ! printf '%s\n' "$files" | grep -q "V73Skel"; then
        warn "  no V73Skel found (device HTP arch may differ from v73)"
    fi
    [ -n "$missing" ] && warn "  missing QNN runtime libs:$missing (see DEV_PITFALLS S3)"
}

# --- soc/dsp arch override deployment ---------------------------------------

soc_to_arch() { # $1 = ro.soc.model ; prints "soc_id dsp_arch" or nothing
    case "$1" in
        SM8735) echo "85 v73" ;;
        SM8750) echo "69 v79" ;;
        SM8550) echo "43 v73" ;;
        SM8650) echo "57 v75" ;;
        SM8850) echo "87 v81" ;;
        SM8450) echo "36 v69" ;;
        SM8475) echo "42 v69" ;;
        SM8350) echo "30 v68" ;;
        *)      echo "" ;;
    esac
}

deploy_qnn_arch_overrides() {
    local model pair soc_id dsp_arch
    model="$(adb_sh "getprop ro.soc.model")"
    model="$(printf '%s' "$model" | tr -d '\r[:space:]')"
    log "device soc model: ${model:-unknown}"
    pair="$(soc_to_arch "$model")"
    if [ -z "$pair" ]; then
        warn "no soc->arch mapping for '$model'; skipping qnn_*_override deployment"
        return
    fi
    soc_id="${pair%% *}"
    dsp_arch="${pair##* }"
    log "target HTP config: soc_id=$soc_id dsp_arch=$dsp_arch"

    mkdir -p "$OUT_DIR"
    printf '%s\n' "$soc_id"  > "$OUT_DIR/qnn_soc_id.txt"
    printf '%s\n' "$dsp_arch" > "$OUT_DIR/qnn_dsp_arch.txt"

    local sub dir
    for sub in sensevoice paraformer-qnn; do
        dir="$DEVICE_MODELS_BASE/$sub"
        [ -n "$(list_device_dir "$dir")" ] || { warn "  skip $sub (dir empty)"; continue; }
        "$ADB_BIN" push "$OUT_DIR/qnn_soc_id.txt" /data/local/tmp/qnn_soc_id.txt >/dev/null
        "$ADB_BIN" push "$OUT_DIR/qnn_dsp_arch.txt" /data/local/tmp/qnn_dsp_arch.txt >/dev/null
        su_sh "cp /data/local/tmp/qnn_soc_id.txt '$dir/qnn_soc_id.txt'; cp /data/local/tmp/qnn_dsp_arch.txt '$dir/qnn_dsp_arch.txt'"
        log "  $sub: deployed qnn_soc_id.txt($soc_id) + qnn_dsp_arch.txt($dsp_arch)"
    done
    "$ADB_BIN" shell "rm -f /data/local/tmp/qnn_soc_id.txt /data/local/tmp/qnn_dsp_arch.txt" >/dev/null 2>&1
}

# --- policy switching (root prefs write, DEV_PITFALLS D4/D5) ----------------

write_policy_helper() {
    mkdir -p "$OUT_DIR"
    cat > "$OUT_DIR/pv_set_policy.sh" << 'EOF'
#!/system/bin/sh
# Root helper: rewrite backend_policy in the PocketVoice prefs file.
# DEV_PITFALLS D5: after writing, chown to the app uid + restorecon, otherwise
# the app silently falls back to the default policy.
POLICY="$1"
PREFS="/data/data/com.stt.mobile/shared_prefs/pocketvoice_prefs.xml"
APPDATA="/data/data/com.stt.mobile"
if [ ! -f "$PREFS" ]; then
    printf "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<map>\n    <string name=\"backend_policy\">%s</string>\n</map>\n" "$POLICY" > "$PREFS"
elif grep -q 'name="backend_policy"' "$PREFS"; then
    sed -i "s|<string name=\"backend_policy\">[^<]*</string>|<string name=\"backend_policy\">$POLICY</string>|" "$PREFS"
else
    sed -i "s|</map>|    <string name=\"backend_policy\">$POLICY</string>\n</map>|" "$PREFS"
fi
OWNER="$(stat -c '%u:%g' "$APPDATA/shared_prefs" 2>/dev/null || stat -c '%u:%g' "$APPDATA")"
chown "$OWNER" "$PREFS"
chmod 660 "$PREFS" 2>/dev/null
restorecon "$PREFS" 2>/dev/null
echo "policy_now=$(grep -o 'name="backend_policy">[^<]*' "$PREFS")"
EOF
    "$ADB_BIN" push "$OUT_DIR/pv_set_policy.sh" "$DEVICE_HELPER" >/dev/null
}

get_current_policy() {
    local raw
    raw="$(su_sh "cat '$PREFS_PATH' 2>/dev/null")"
    local policy
    policy="$(printf '%s\n' "$raw" | sed -n 's/.*<string name="backend_policy">\([^<]*\)<\/string>.*/\1/p' | tail -1)"
    if [ -z "$policy" ]; then policy="auto"; fi
    echo "$policy"
}

set_policy() { # $1 = policy
    local policy="$1" out
    log "setting backend_policy=$policy ..."
    "$ADB_BIN" shell "am force-stop $PKG" >/dev/null 2>&1
    sleep 1
    out="$(su_sh "sh '$DEVICE_HELPER' '$policy'")"
    log "  $out"
    printf '%s\n' "$out" | grep -q "policy_now=.*$policy" || {
        warn "policy write may not have taken effect: $out"
    }
}

# --- app restart + readiness -------------------------------------------------

port_listening() {
    "$ADB_BIN" shell "ss -tln 2>/dev/null | grep -q ':$PORT' || netstat -tln 2>/dev/null | grep -q ':$PORT'" >/dev/null 2>&1
}

engine_initialized() {
    adb_logcat -s STT_Engine:I | grep -q "Initialized OK"
}

restart_and_wait() {
    log "restarting app ..."
    "$ADB_BIN" shell "am start -n '$ACTIVITY'" >/dev/null 2>&1
    local i=0
    while [ $i -lt "$INIT_TIMEOUT" ]; do
        if port_listening && engine_initialized; then
            log "  service ready after $((i * 2))s"
            return 0
        fi
        sleep 2
        i=$((i + 2))
    done
    warn "  service did not become ready within ${INIT_TIMEOUT}s"
    return 1
}

warmup_forward() {
    "$ADB_BIN" forward --remove "tcp:$PORT" >/dev/null 2>&1
    "$ADB_BIN" forward "tcp:$PORT" "tcp:$PORT"
    log "forward established tcp:$PORT"
    # D3: open one warm-up connection before the real requests
    timeout 3 bash -c "exec 3<>/dev/tcp/127.0.0.1/$PORT" >/dev/null 2>&1 \
        && log "  warm-up connection OK" \
        || warn "  warm-up connection failed (server may still be initializing)"
    sleep "$REQUEST_INTERVAL"
}

active_backend() {
    adb_logcat -s STT_Engine:I | grep "Initialized OK" | tail -1 \
        | sed -n 's/.*backend=\([A-Za-z0-9_]*\).*/\1/p'
}

# --- bench loop --------------------------------------------------------------

ref_for() { # $1 = wav path
    awk -F '\t' -v name="$(basename "$1")" '$1 == name { print $3 }' "$REFS_FILE"
}

send_and_measure() { # $1 = wav, $2 = ref ; prints a TSV row
    local wav="$1" ref="$2" t0 t1 e2e out rc text decode cer_row dist reflen cer
    "$ADB_BIN" logcat -c >/dev/null 2>&1
    t0="$(date +%s%3N)"
    out="$(timeout 40 node "$SEND_SCRIPT" "$wav" 127.0.0.1 "$PORT" 2>&1)"
    rc=$?
    t1="$(date +%s%3N)"
    e2e=$((t1 - t0))
    text="$(printf '%s\n' "$out" | sed -n 's/^Text: //p' | tail -1 | tr '\t' ' ')"
    decode="$(adb_logcat -s STT_GATE_D3:I | sed -n 's/.*decode_ms=\([0-9]*\).*/\1/p' | tail -1)"

    if [ "$rc" -ne 0 ] || [ -z "$text" ]; then
        echo "$(basename "$wav")	FAIL	$(printf '%s' "$out" | tail -2 | tr '\n\t' '  ')"
        return
    fi

    cer_row="$(node "$CER_SCRIPT" "$text" "$ref" 2>/dev/null)"
    dist="$(printf '%s' "$cer_row" | cut -f1)"
    reflen="$(printf '%s' "$cer_row" | cut -f2)"
    cer="$(printf '%s' "$cer_row" | cut -f3)"

    echo "$(basename "$wav")	OK	$text	$ref	$e2e	${decode:--}	$dist	$reflen	${cer:--}"
}

run_policy() { # $1 = policy
    local policy="$1" expected sub backend wav ref row
    expected="${POLICY_EXPECTED_BACKEND[$policy]}"
    sub="${POLICY_MODEL_SUBDIR[$policy]}"
    log ""
    log "================ policy=$policy (expected backend=$expected, model=$sub) ================"
    set_policy "$policy"
    restart_and_wait
    warmup_forward
    backend="$(active_backend)"
    if [ "$backend" = "$expected" ]; then
        log "active backend: $backend (expected)"
    else
        warn "active backend: ${backend:-<none>} (expected $expected) - check model pack / fallback"
    fi

    local rows="$OUT_DIR/${policy}.rows"
    : > "$rows"
    for wav in "$WAVS_DIR"/bench_*.wav; do
        [ -e "$wav" ] || continue
        ref="$(ref_for "$wav")"
        row="$(send_and_measure "$wav" "$ref")"
        printf '%s\n' "$row" >> "$rows"
        log "  $(printf '%s' "$row" | cut -f1-2): text='$(printf '%s' "$row" | cut -f3)' e2e=$(printf '%s' "$row" | cut -f5)ms decode=$(printf '%s' "$row" | cut -f6)ms cer=$(printf '%s' "$row" | cut -f9)"
        sleep "$REQUEST_INTERVAL"
    done
    echo "$backend" > "$OUT_DIR/${policy}.backend"
}

# --- summary -----------------------------------------------------------------

write_summary() {
    local stamp
    stamp="$(date +%Y%m%d-%H%M%S)"
    local summary="$OUT_DIR/result-$stamp.md"
    {
        echo "# PocketVoice QNN backend device test - $stamp"
        echo ""
        echo "device: $("$ADB_BIN" get-serialno 2>/dev/null | tr -d '\r')  soc: $(adb_sh 'getprop ro.soc.model')"
        echo ""
    } > "$summary"

    local policy expected backend
    for policy in $(echo "$POLICIES" | tr ',' ' '); do
        expected="${POLICY_EXPECTED_BACKEND[$policy]}"
        backend="$(cat "$OUT_DIR/${policy}.backend" 2>/dev/null || echo '?')"
        {
            echo "## $policy (expected backend=$expected, active=$backend)"
            echo ""
            echo "| wav | status | e2e ms | decode ms | CER | text (hyp) | ref |"
            echo "|---|---|---|---|---|---|---|"
        } >> "$summary"
        while IFS=$'\t' read -r name status text ref e2e decode dist reflen cer; do
            [ -n "$name" ] || continue
            echo "| $name | $status | $e2e | $decode | ${cer:--} | $text | $ref |" >> "$summary"
        done < "$OUT_DIR/${policy}.rows"
        echo "" >> "$summary"

        local avg_e2e avg_cer
        avg_e2e="$(awk -F '\t' '$2=="OK" { n++; s += $5 } END { if (n) printf "%.0f", s/n; else print "-" }' "$OUT_DIR/${policy}.rows")"
        avg_cer="$(awk -F '\t' '$2=="OK" && $9!="-" { n++; s += $9 } END { if (n) printf "%.4f", s/n; else print "-" }' "$OUT_DIR/${policy}.rows")"
        echo "avg e2e=${avg_e2e}ms  avg CER=${avg_cer}" >> "$summary"
        echo "" >> "$summary"
    done
    log "summary written to $summary"
}

# --- main --------------------------------------------------------------------

while [ $# -gt 0 ]; do
    case "$1" in
        --install) INSTALL=1 ;;
        --policies) POLICIES="$2"; shift ;;
        --wavs) WAVS_DIR="$2"; shift ;;
        --refs) REFS_FILE="$2"; shift ;;
        --out) OUT_DIR="$2"; shift ;;
        --skip-restore) SKIP_RESTORE=1 ;;
        --force) FORCE=1 ;;
        --help|-h) usage 0 ;;
        *) err "unknown arg: $1"; usage 1 ;;
    esac
    shift
done

resolve_adb
check_device
check_root
log "stayon usb (D1)..."
"$ADB_BIN" shell "svc power stayon usb" >/dev/null 2>&1 || true

if [ "$INSTALL" -eq 1 ]; then
    log "installing APK: $APK"
    "$ADB_BIN" install -r "$APK"
else
    local apk_path
    apk_path="$(adb_sh "pm path $PKG 2>/dev/null" | head -1)"
    log "installed app: ${apk_path:-NOT INSTALLED}"
    [ -n "$apk_path" ] || { err "app not installed; re-run with --install"; exit 2; }
fi

check_models
check_qnn_runtime

if [ "$NO_ROOT" -eq 0 ]; then
    write_policy_helper
    deploy_qnn_arch_overrides
else
    warn "no root; skipping policy helper and arch override deployment"
fi

ORIGINAL_POLICY="$(get_current_policy)"
log "original policy: $ORIGINAL_POLICY"

for policy in $(echo "$POLICIES" | tr ',' ' '); do
    run_policy "$policy"
done

if [ "$NO_ROOT" -eq 0 ] && [ "$SKIP_RESTORE" -eq 0 ]; then
    log "restoring original policy=$ORIGINAL_POLICY ..."
    set_policy "$ORIGINAL_POLICY"
    restart_and_wait
fi

"$ADB_BIN" shell "rm -f '$DEVICE_HELPER'" >/dev/null 2>&1
write_summary
log "done"
