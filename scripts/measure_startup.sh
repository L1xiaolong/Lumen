#!/usr/bin/env bash
# Launches Lumen a fixed number of times and prints the startup waterfall recorded by the
# diagnostics service, so launch optimizations can be verified against real numbers.
#
# Usage:
#   scripts/measure_startup.sh [app-bundle] [directory]
# Environment:
#   RUNS=N     Launch count (default 3)
#   DELAY=ms   Screenshot delay that ends each run (default 1800)
#
# Each run launches the app with --screenshot --screenshot-native so the process exits on its
# own after the delay, then parses that session's JSONL log. All timings are milliseconds from
# process start (diagnostics::startupElapsedMs).
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
app="${1:-$script_dir/../build/macos-preset-release/src/qml/Lumen.app}"
directory="${2:-$HOME/Pictures}"
runs="${RUNS:-3}"
delay="${DELAY:-1800}"
diagnostics_root="${LUMEN_DIAGNOSTICS_ROOT:-$HOME/Library/Application Support/MvpView/Lumen/diagnostics}"

if [[ ! -d "$app" ]]; then
    echo "App bundle not found: $app" >&2
    exit 1
fi
if [[ ! -d "$directory" ]]; then
    echo "Directory to open not found: $directory" >&2
    exit 1
fi

binary="$app/Contents/MacOS/Lumen"
for i in $(seq "$runs"); do
    mkdir -p /tmp/lumen-startup-measure
    screenshot="/tmp/lumen-startup-measure/run-$i.png"
    rm -f "$screenshot"
    "$binary" --screenshot "$screenshot" --screenshot-native \
        --screenshot-delay "$delay" "$directory" >/dev/null 2>&1 || true

    session="$(ls -t "$diagnostics_root" 2>/dev/null | head -1)"
    if [[ -z "$session" ]]; then
        echo "run $i: no diagnostics session found under $diagnostics_root" >&2
        exit 1
    fi
    log="$(ls "$diagnostics_root/$session"/log-*.jsonl 2>/dev/null | sort | tail -1)"
    if [[ -z "$log" || ! -s "$log" ]]; then
        echo "run $i: session $session has no log file yet" >&2
        exit 1
    fi
    echo "== run $i  session $session  items=$(find "$directory" -maxdepth 1 | wc -l | tr -d ' ') =="
    python3 - "$log" <<'PY'
import json, sys

milestone_order = [
    "settings.migrated", "application.start", "qml.loading", "qml.loaded",
    "scenegraph.initialized", "window.shown", "startup.first_frame",
    "directory.restore_start", "directory.restored", "directory.open",
    "directory.snapshot_applied", "directory.first_content", "thumbnail.first_ready",
    "directory.scan_complete",
]
rows = {}
with open(sys.argv[1], encoding="utf-8") as handle:
    for line in handle:
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            continue
        name = record.get("event", "")
        context = record.get("context", {})
        if isinstance(context, dict) and "elapsedMs" in context:
            rows.setdefault(name, int(context["elapsedMs"]))

deltas = {}
previous = 0
for name in milestone_order:
    if name in rows:
        deltas[name] = rows[name] - previous
        previous = rows[name]
width = max(len(name) for name in milestone_order if name in rows)
for name in milestone_order:
    if name in rows:
        print(f"  {name:<{width}}  {rows[name]:>6} ms  (+{deltas[name]:>4} ms)")
extra = sorted(k for k in rows if k not in milestone_order)
for name in extra:
    print(f"  {name:<{width}}  {rows[name]:>6} ms")
PY
    echo
done
