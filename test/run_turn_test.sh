#!/usr/bin/env bash
set -euo pipefail

# Use the runner's real interface for TURN relay allocations. A loopback
# relay address cannot reliably reach ICE host candidates on this interface.
relay_ip=$(ip -4 route get 1.1.1.1 | awk '{ for (i = 1; i <= NF; ++i) if ($i == "src") { print $(i + 1); exit } }')
if [ -z "$relay_ip" ]; then
    echo "Unable to determine runner interface address" >&2
    exit 1
fi
echo "Violet listening/relay interface: $relay_ip"

violet_dir="${RUNNER_TEMP:-/tmp}/violet-src"
log_file="${RUNNER_TEMP:-/tmp}/libdatachannel-violet.log"
rm -rf "$violet_dir"

git clone --depth 1 https://github.com/paullouisageneau/violet.git "$violet_dir"
git -C "$violet_dir" submodule update --init --recursive
cmake -S "$violet_dir" -B "$violet_dir/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$violet_dir/build" --parallel "$(nproc)"

export RTC_TEST_TURN_URL="turn:testuser:testpass@$relay_ip:3479"

"$violet_dir/build/violet" \
    --bind="$relay_ip" \
    --port=3479 \
    --range=49160:49200 \
    --credentials=testuser:testpass \
    > "$log_file" 2>&1 &
violet_pid=$!

cleanup() {
    status=$?
    kill "$violet_pid" 2>/dev/null || true
    wait "$violet_pid" 2>/dev/null || true
    if [ "$status" -ne 0 ]; then
        echo "=== Violet Server Logs ==="
        cat "$log_file"
    fi
}
trap cleanup EXIT

sleep 2
kill -0 "$violet_pid"
timeout 150s ./build/tests --turn
