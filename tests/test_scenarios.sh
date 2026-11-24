#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

for command_name in make grep awk sed sleep cp mv rm mktemp tail; do
    command -v "$command_name" >/dev/null 2>&1 || {
        printf 'ERROR: required command not found: %s\n' "$command_name" >&2
        exit 2
    }
done

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/watchdog-tests.XXXXXX")"
LOG_FILE="$ROOT_DIR/watchdog.log"
WATCHDOG_PID=""
HAD_OLD_LOG=0
PASS_COUNT=0

if [[ -f "$LOG_FILE" ]]; then
    cp -- "$LOG_FILE" "$TMP_DIR/original-watchdog.log"
    HAD_OLD_LOG=1
fi

cleanup()
{
    local exit_code=$?

    if [[ -n "$WATCHDOG_PID" ]] && kill -0 "$WATCHDOG_PID" 2>/dev/null; then
        kill -TERM "$WATCHDOG_PID" 2>/dev/null || true
        wait "$WATCHDOG_PID" 2>/dev/null || true
    fi

    if (( HAD_OLD_LOG == 1 )); then
        cp -- "$TMP_DIR/original-watchdog.log" "$LOG_FILE"
    else
        rm -f -- "$LOG_FILE"
    fi

    printf '\nTest logs/artifacts: %s\n' "$TMP_DIR"
    exit "$exit_code"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

fail()
{
    printf 'FAIL: %s\n' "$1" >&2
    if [[ -f "$LOG_FILE" ]]; then
        printf '%s\n' '--- watchdog.log (tail) ---' >&2
        tail -n 30 "$LOG_FILE" >&2 || true
    fi
    if [[ -f "$TMP_DIR/watchdog.stdout" ]]; then
        printf '%s\n' '--- watchdog stdout/stderr (tail) ---' >&2
        tail -n 30 "$TMP_DIR/watchdog.stdout" >&2 || true
    fi
    exit 1
}

pass()
{
    PASS_COUNT=$((PASS_COUNT + 1))
    printf 'PASS: %s\n' "$1"
}

wait_for_log()
{
    local pattern="$1"
    local timeout_seconds="${2:-8}"
    local elapsed=0

    while (( elapsed < timeout_seconds * 10 )); do
        if [[ -f "$LOG_FILE" ]] && grep -E -q -- "$pattern" "$LOG_FILE"; then
            return 0
        fi
        sleep 0.1
        elapsed=$((elapsed + 1))
    done
    return 1
}

wait_for_count()
{
    local pattern="$1"
    local wanted="$2"
    local timeout_seconds="${3:-8}"
    local elapsed=0
    local found=0

    while (( elapsed < timeout_seconds * 10 )); do
        if [[ -f "$LOG_FILE" ]]; then
            found="$(grep -E -c -- "$pattern" "$LOG_FILE" || true)"
            if (( found >= wanted )); then
                return 0
            fi
        fi
        sleep 0.1
        elapsed=$((elapsed + 1))
    done
    return 1
}

start_watchdog()
{
    local duration="${1:-}"

    [[ -z "$WATCHDOG_PID" ]] || fail "start_watchdog called while another watchdog is tracked"
    rm -f -- "$LOG_FILE"
    : > "$TMP_DIR/watchdog.stdout"

    if [[ -n "$duration" ]]; then
        WATCHDOG_WORKER_DURATION="$duration" ./watchdog >"$TMP_DIR/watchdog.stdout" 2>&1 &
    else
        ./watchdog >"$TMP_DIR/watchdog.stdout" 2>&1 &
    fi
    WATCHDOG_PID=$!

    wait_for_log 'event=process_created worker=worker-1' 8 || fail "watchdog/worker-1 did not start"
    wait_for_log 'event=process_created worker=worker-2' 8 || fail "worker-2 did not start"
}

latest_worker_pid()
{
    local worker_name="$1"
    awk -v worker="worker=$worker_name" '
        index($0, "event=process_created") && index($0, worker) {
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^pid=[0-9]+$/) {
                    sub(/^pid=/, "", $i)
                    pid = $i
                }
            }
        }
        END { if (pid != "") print pid }
    ' "$LOG_FILE"
}

assert_safe_worker_pid()
{
    local pid="$1"
    [[ "$pid" =~ ^[0-9]+$ ]] || fail "worker PID is not numeric: '$pid'"
    (( pid > 1 )) || fail "refusing to signal a PID <= 1"
    (( pid != WATCHDOG_PID )) || fail "refusing to signal the watchdog PID as a worker"
    kill -0 "$pid" 2>/dev/null || fail "worker PID is not alive: $pid"
}

stop_watchdog()
{
    local signal_name="${1:-TERM}"
    local status=0

    if [[ -n "$WATCHDOG_PID" ]] && kill -0 "$WATCHDOG_PID" 2>/dev/null; then
        kill -s "$signal_name" "$WATCHDOG_PID" || fail "could not send SIG$signal_name to watchdog"
        if wait "$WATCHDOG_PID"; then
            status=0
        else
            status=$?
        fi
    fi

    WATCHDOG_PID=""
    (( status == 0 )) || fail "watchdog exited with status $status during SIG$signal_name shutdown"
}

save_log()
{
    local name="$1"
    cp -- "$LOG_FILE" "$TMP_DIR/$name.log"
}

printf '%s\n' '[1/8] Building project...'
make clean
make || fail 'make failed'
[[ -x ./watchdog && -x ./worker ]] || fail 'make did not create ./watchdog and ./worker'
pass 'T1 build creates watchdog and worker executables'

printf '%s\n' '[2/8] Normal runtime: two workers...'
start_watchdog
wait_for_log 'event=process_created worker=worker-2 pid=[1-9][0-9]*' 5 || fail 'worker-2 creation was not logged'
save_log T1-normal-runtime
stop_watchdog TERM
pass 'T1 normal runtime starts two workers'

printf '%s\n' '[3/8] Normal child exit...'
start_watchdog 2
wait_for_log 'event=process_exited_normally worker=worker-1' 8 || fail 'worker-1 did not exit normally in duration mode'
wait_for_log 'event=process_exited_normally worker=worker-2' 8 || fail 'worker-2 did not exit normally in duration mode'
if grep -E -q 'event=process_restarted worker=worker-(1|2)' "$LOG_FILE"; then
    fail 'normally exited worker was restarted'
fi
save_log T2-normal-exit
stop_watchdog TERM
pass 'T2 normal exit is recognized and does not trigger restart'

printf '%s\n' '[4/8] Unexpected child termination...'
start_watchdog
old_pid="$(latest_worker_pid worker-1)"
assert_safe_worker_pid "$old_pid"
kill -KILL "$old_pid" || fail "failed to SIGKILL worker $old_pid"
wait_for_log 'event=process_restarted worker=worker-1' 8 || fail 'worker-1 was not restarted after SIGKILL'
new_pid="$(latest_worker_pid worker-1)"
[[ -n "$new_pid" && "$new_pid" != "$old_pid" ]] || fail 'worker restart did not receive a new PID'
save_log T3-unexpected-exit
stop_watchdog TERM
pass 'T3 unexpected termination triggers a replacement worker'

printf '%s\n' '[5/8] Restart limit...'
start_watchdog
for attempt in 1 2 3 4; do
    current_pid="$(latest_worker_pid worker-1)"
    assert_safe_worker_pid "$current_pid"
    kill -KILL "$current_pid" || fail "failed to kill worker PID $current_pid on attempt $attempt"

    if (( attempt <= 3 )); then
        wait_for_count 'event=process_restarted worker=worker-1' "$attempt" 8 || fail "restart $attempt was not recorded"
    else
        wait_for_log 'event=restart_limit_reached worker=worker-1' 8 || fail 'restart limit was not recorded after the fourth unexpected exit'
    fi
done
restart_count="$(grep -E -c 'event=process_restarted worker=worker-1' "$LOG_FILE" || true)"
(( restart_count == 3 )) || fail "expected exactly 3 restarts, found $restart_count"
save_log T4-restart-limit
stop_watchdog TERM
pass 'T4 each worker slot is limited to three restarts'

printf '%s\n' '[6/8] SIGUSR1 status report...'
start_watchdog
kill -USR1 "$WATCHDOG_PID" || fail 'could not send SIGUSR1 to watchdog'
wait_for_log 'event=STATUS_REPORT_BEGIN' 5 || fail 'status report start was not logged'
wait_for_log 'event=STATUS_WORKER worker=worker-1 pid=[1-9][0-9]* details=state=RUNNING' 5 || fail 'worker-1 PID/status missing from status report'
wait_for_log 'event=STATUS_WORKER worker=worker-2 pid=[1-9][0-9]* details=state=RUNNING' 5 || fail 'worker-2 PID/status missing from status report'
save_log T5-status-report
stop_watchdog TERM
pass 'T5 SIGUSR1 writes worker PIDs and current states'

printf '%s\n' '[7/8] SIGTERM and SIGINT shutdown...'
start_watchdog
stop_watchdog TERM
wait_for_log 'event=SHUTDOWN_COMPLETE' 1 || fail 'SIGTERM shutdown completion was not logged'
if grep -E -q 'event=worker_sigkill_escalation' "$LOG_FILE"; then
    fail 'SIGKILL escalation occurred during ordinary SIGTERM shutdown'
fi
save_log T6a-sigterm-shutdown

start_watchdog
stop_watchdog INT
wait_for_log 'event=SHUTDOWN_COMPLETE' 1 || fail 'SIGINT shutdown completion was not logged'
save_log T6b-sigint-shutdown
pass 'T6 SIGTERM and SIGINT both complete graceful shutdown'

printf '%s\n' '[8/8] Shutdown timeout escalation and timestamp audit...'
start_watchdog
stopped_pid="$(latest_worker_pid worker-1)"
assert_safe_worker_pid "$stopped_pid"
kill -STOP "$stopped_pid" || fail "could not stop worker $stopped_pid for timeout test"
stop_watchdog TERM
wait_for_log 'event=worker_sigkill_escalation worker=worker-1' 1 || fail 'SIGKILL escalation was not recorded for the stopped worker'
wait_for_log 'event=process_exited_unexpectedly worker=worker-1 .*terminating_signal=9' 1 || fail 'stopped worker was not reaped after SIGKILL'
save_log T6c-shutdown-timeout

for saved_log in "$TMP_DIR"/T*.log; do
    if grep -E -v '^\[[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\] event=' "$saved_log"; then
        fail "malformed or missing timestamp found in $saved_log"
    fi
done
pass 'T7 all saved log records contain timestamps; timeout escalation is verified'

printf '\nAll scripted checks passed (%d).\n' "$PASS_COUNT"
