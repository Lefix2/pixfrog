#!/usr/bin/env bash
# Drive the firmware control console (see AGENT.md "Control console (UART)").
#   tools/uartctl.sh [-q] "status" "ch 0" ...
# Runs uartctl.py inside the IDF image: pyserial + root (the host user isn't
# in dialout). PORT env overrides /dev/ttyACM0.
set -euo pipefail
PORT="${PORT:-/dev/ttyACM0}"
exec docker run --rm --device "$PORT" \
    -v "$(cd "$(dirname "$0")" && pwd)/uartctl.py:/uartctl.py:ro" \
    --entrypoint bash espressif/idf:v5.5 \
    -c '. "$IDF_PATH/export.sh" >/dev/null 2>&1; exec python3 /uartctl.py "$@"' \
    _ -p "$PORT" "$@"
