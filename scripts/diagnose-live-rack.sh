#!/usr/bin/env bash
# scripts/diagnose-live-rack.sh
# Non-destructive diagnostic snapshot of live SC7Labs Rack and COSMIC Files state.
# Does NOT modify, terminate, or perturb any processes.
set -euo pipefail

TIMESTAMP="$(date +'%Y%m%d_%H%M%S')"
SNAPSHOT_BASE="${XDG_RUNTIME_DIR:-/run/user/$UID}/sc7-rack-diagnostics"
TARGET_DIR="${1:-$SNAPSHOT_BASE/snapshot-$TIMESTAMP}"
mkdir -p "$TARGET_DIR"

log() {
    echo "[$(date +'%T')] $*"
}

log "Writing diagnostic snapshot to $TARGET_DIR..."

# 1. PIDs & Core Environment
cat << 'EOF' > "$TARGET_DIR/info.txt"
SC7Labs Rack Live Diagnostic Snapshot
======================================
EOF
echo "Timestamp: $(date -Iseconds)" >> "$TARGET_DIR/info.txt"

RACK_RUN_DIR="${XDG_RUNTIME_DIR:-/run/user/$UID}/sc7-rack"
LAUNCHER_PID=""
SWAY_PID=""
if [[ -f "$RACK_RUN_DIR/sc7-rack.pid" ]]; then
    SWAY_PID="$(cat "$RACK_RUN_DIR/sc7-rack.pid" 2>/dev/null || true)"
fi

# Find launcher PID (parent of Sway or matching pattern)
if [[ -n "$SWAY_PID" ]] && [[ -d "/proc/$SWAY_PID" ]]; then
    LAUNCHER_PID="$(awk '{print $4}' "/proc/$SWAY_PID/stat" 2>/dev/null || true)"
fi
if [[ -z "$LAUNCHER_PID" ]]; then
    LAUNCHER_PID="$(pgrep -f '[b]ash.*sc7-rack' | head -n1 || true)"
fi

# Component PIDs
BRIDGE_PID="$(pgrep -f '[s]c7-clipboard-bridge' | head -n1 || true)"
FILES_PIDS="$(pgrep -f '[c]osmic-files' || true)"
TERM_PIDS="$(pgrep -f '[c]osmic-term' || true)"
MONITOR_PIDS="$(pgrep -f '[c]osmic-monitor' || true)"

{
    echo "Launcher PID:        ${LAUNCHER_PID:-not found}"
    echo "Sway PID:            ${SWAY_PID:-not found}"
    echo "Clipboard Bridge PID:${BRIDGE_PID:-not found}"
    echo "COSMIC Files PIDs:   ${FILES_PIDS:-not found}"
    echo "COSMIC Term PIDs:    ${TERM_PIDS:-not found}"
    echo "COSMIC Monitor PIDs: ${MONITOR_PIDS:-not found}"
    echo ""
    echo "Host WAYLAND_DISPLAY:   ${WAYLAND_DISPLAY:-}"
    echo "SC7_HOST_WAYLAND_DISPLAY: ${SC7_HOST_WAYLAND_DISPLAY:-}"
    echo "DBUS_SESSION_BUS_ADDRESS: ${DBUS_SESSION_BUS_ADDRESS:-}"
    echo "XDG_RUNTIME_DIR:        ${XDG_RUNTIME_DIR:-}"
} >> "$TARGET_DIR/info.txt"

# 2. Sway socket & Sway IPC inspection
SWAY_SOCK=""
if [[ -n "$SWAY_PID" ]]; then
    for sock in "${XDG_RUNTIME_DIR:-/run/user/$UID}"/sway-ipc.*."$SWAY_PID".sock; do
        if [[ -S "$sock" ]]; then
            SWAY_SOCK="$sock"
            break
        fi
    done
fi
echo "SWAYSOCK:               ${SWAY_SOCK:-not found}" >> "$TARGET_DIR/info.txt"

if [[ -n "$SWAY_SOCK" && -S "$SWAY_SOCK" ]]; then
    SWAYSOCK="$SWAY_SOCK" swaymsg -t get_tree -r > "$TARGET_DIR/sway_tree.json" 2>&1 || true
    SWAYSOCK="$SWAY_SOCK" swaymsg -t get_outputs -r > "$TARGET_DIR/sway_outputs.json" 2>&1 || true
    SWAYSOCK="$SWAY_SOCK" swaymsg -t get_inputs -r > "$TARGET_DIR/sway_inputs.json" 2>&1 || true
    SWAYSOCK="$SWAY_SOCK" swaymsg -t get_seats -r > "$TARGET_DIR/sway_seats.json" 2>&1 || true
    SWAYSOCK="$SWAY_SOCK" swaymsg -t get_version -r > "$TARGET_DIR/sway_version.json" 2>&1 || true
fi

# 3. Process Tree & Process Table
ps auxf > "$TARGET_DIR/process_tree.txt" 2>&1 || true

# 4. Detailed Per-Process Dumps for COSMIC Files and Core Components
dump_process() {
    local pid="$1"
    local name="$2"
    [[ -n "$pid" && -d "/proc/$pid" ]] || return 0
    local pdir="$TARGET_DIR/proc_${name}_${pid}"
    mkdir -p "$pdir"

    # Cmdline, stat, status, limits
    tr '\0' ' ' < "/proc/$pid/cmdline" > "$pdir/cmdline.txt" 2>/dev/null || true
    echo "" >> "$pdir/cmdline.txt"
    cat "/proc/$pid/status" > "$pdir/status.txt" 2>/dev/null || true
    cat "/proc/$pid/limits" > "$pdir/limits.txt" 2>/dev/null || true
    cat "/proc/$pid/stat" > "$pdir/stat.txt" 2>/dev/null || true
    cat "/proc/$pid/io" > "$pdir/io.txt" 2>/dev/null || true
    cat "/proc/$pid/wchan" > "$pdir/wchan.txt" 2>/dev/null || true
    cat "/proc/$pid/stack" > "$pdir/stack.txt" 2>/dev/null || true

    # Environ (filter sensitive auth/pass if any, keep wayland/dbus/desktop/paths)
    tr '\0' '\n' < "/proc/$pid/environ" | grep -E -v 'AUTH|PASS|SECRET|KEY|TOKEN|COOKIE' > "$pdir/environ.txt" 2>/dev/null || true

    # File Descriptors & targets
    ls -l "/proc/$pid/fd" > "$pdir/fd_list.txt" 2>/dev/null || true
    local fd_count
    fd_count="$(ls -1 "/proc/$pid/fd" 2>/dev/null | wc -l || echo 0)"
    echo "FD count: $fd_count" > "$pdir/fd_summary.txt"

    # Categorize FDs (pipes, sockets, inotify, eventfd, anon_inode)
    {
        echo "=== FD breakdown ==="
        ls -l "/proc/$pid/fd" 2>/dev/null | awk '{print $NF}' | sort | \
            sed -E 's#/.*##; s#socket:\[[0-9]+\]#socket#; s#pipe:\[[0-9]+\]#pipe#; s#anon_inode:\[.*\]#anon_inode#' | \
            sort | uniq -c | sort -nr
        echo ""
        echo "=== Inotify / anon_inode details ==="
        for fd in "/proc/$pid/fd/"*; do
            [[ -e "$fd" ]] || continue
            local target
            target="$(readlink "$fd" 2>/dev/null || true)"
            if [[ "$target" == *"anon_inode"* || "$target" == *"socket"* || "$target" == *"pipe"* ]]; then
                local fdnum
                fdnum="$(basename "$fd")"
                echo "FD $fdnum -> $target"
                if [[ -f "/proc/$pid/fdinfo/$fdnum" ]]; then
                    sed 's/^/  /' "/proc/$pid/fdinfo/$fdnum"
                fi
            fi
        done
    } >> "$pdir/fd_summary.txt" 2>/dev/null || true

    # Thread states
    if [[ -d "/proc/$pid/task" ]]; then
        local tcount
        tcount="$(ls -1 "/proc/$pid/task" 2>/dev/null | wc -l || echo 0)"
        echo "Thread count: $tcount" > "$pdir/threads_summary.txt"
        for tid in "/proc/$pid/task/"*; do
            [[ -d "$tid" ]] || continue
            local tnum
            tnum="$(basename "$tid")"
            local tcomm
            tcomm="$(cat "$tid/comm" 2>/dev/null || true)"
            local twchan
            twchan="$(cat "$tid/wchan" 2>/dev/null || true)"
            local tstat
            tstat="$(awk '{print $3}' "$tid/stat" 2>/dev/null || true)"
            printf 'Thread %s [%s] state=%s wchan=%s\n' "$tnum" "$tcomm" "$tstat" "$twchan" >> "$pdir/threads_summary.txt"
        done
    fi

    # Children
    pgrep -P "$pid" > "$pdir/children.txt" 2>/dev/null || true
}

dump_process "$LAUNCHER_PID" "launcher"
dump_process "$SWAY_PID" "sway"
dump_process "$BRIDGE_PID" "bridge"

for fpid in $FILES_PIDS; do
    dump_process "$fpid" "cosmic_files"
done
for tpid in $TERM_PIDS; do
    dump_process "$tpid" "cosmic_term"
done
for mpid in $MONITOR_PIDS; do
    dump_process "$mpid" "cosmic_monitor"
done

# 5. DBus & Portal Services State
if command -v busctl >/dev/null 2>&1; then
    busctl --user list > "$TARGET_DIR/dbus_user_services.txt" 2>&1 || true
    busctl --user status org.freedesktop.portal.Desktop > "$TARGET_DIR/dbus_portal_status.txt" 2>&1 || true
    busctl --user status org.freedesktop.FileManager1 > "$TARGET_DIR/dbus_filemanager_status.txt" 2>&1 || true
    busctl --user status org.gtk.vfs.Daemon > "$TARGET_DIR/dbus_gvfs_status.txt" 2>&1 || true
fi

# 6. Filesystem Mounts & Inotify Limits
cat /proc/sys/fs/inotify/max_user_watches > "$TARGET_DIR/inotify_max_user_watches.txt" 2>&1 || true
cat /proc/sys/fs/inotify/max_user_instances > "$TARGET_DIR/inotify_max_user_instances.txt" 2>&1 || true
df -h > "$TARGET_DIR/df_mounts.txt" 2>&1 || true
mount > "$TARGET_DIR/mounts.txt" 2>&1 || true

# 7. Logs (Rack runtime logs, clipboard bridge logs)
if [[ -d "$RACK_RUN_DIR" ]]; then
    for logfile in "$RACK_RUN_DIR"/*.log; do
        [[ -f "$logfile" ]] || continue
        # Tail last 2000 lines to avoid dumping huge data
        tail -n 2000 "$logfile" > "$TARGET_DIR/$(basename "$logfile")" 2>&1 || true
    done
fi

# 8. User Journal since Rack Sway startup
if [[ -n "$SWAY_PID" ]] && [[ -d "/proc/$SWAY_PID" ]] && command -v journalctl >/dev/null 2>&1; then
    START_TIME="$(ps -o lstart= -p "$SWAY_PID" 2>/dev/null || true)"
    ISO_START=""
    if [[ -n "$START_TIME" ]]; then
        ISO_START="$(date -d "$START_TIME" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || true)"
    fi
    if [[ -n "$ISO_START" ]]; then
        journalctl --since "$ISO_START" --no-pager > "$TARGET_DIR/user_journal.log" 2>&1 || true
    else
        journalctl -n 2000 --no-pager > "$TARGET_DIR/user_journal.log" 2>&1 || true
    fi
fi

# 9. Top-level Summary
{
    echo "=== Summary ==="
    echo "Diagnostic completed at: $(date -Iseconds)"
    echo "Sway PID: ${SWAY_PID:-none}"
    for fpid in $FILES_PIDS; do
        if [[ -d "/proc/$fpid" ]]; then
            fdc="$(ls -1 "/proc/$fpid/fd" 2>/dev/null | wc -l || echo 0)"
            tc="$(ls -1 "/proc/$fpid/task" 2>/dev/null | wc -l || echo 0)"
            rss="$(awk '/VmRSS/ {print $2, $3}' "/proc/$fpid/status" 2>/dev/null || true)"
            echo "COSMIC Files PID $fpid: $tc threads, $fdc FDs, RSS: $rss"
        fi
    done
    if [[ -n "$BRIDGE_PID" && -d "/proc/$BRIDGE_PID" ]]; then
        bfdc="$(ls -1 "/proc/$BRIDGE_PID/fd" 2>/dev/null | wc -l || echo 0)"
        brss="$(awk '/VmRSS/ {print $2, $3}' "/proc/$BRIDGE_PID/status" 2>/dev/null || true)"
        echo "Clipboard Bridge PID $BRIDGE_PID: $bfdc FDs, RSS: $brss"
    fi
} > "$TARGET_DIR/summary.txt"

cat "$TARGET_DIR/summary.txt"
log "Diagnostic snapshot saved to $TARGET_DIR"
exit 0
