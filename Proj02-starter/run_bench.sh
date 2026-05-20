#!/usr/bin/env bash
# run_bench.sh — runs the four benchmark configurations required by BENCHMARK.md
#
# Usage: ./run_bench.sh [port] [workers] [buckets]
# Defaults: port=9000, workers=8, buckets=1024
#chmod +x run_bench.sh

PORT=${1:-9000}
WORKERS=${2:-8}
BUCKETS=${3:-1024}
SWEEPER_MS=500

OPS_PER_CLIENT=10000
READ_PCT=90

KVSERVER=./kvserver
BENCH=./bench_client

# ── Sanity checks ──────────────────────────────────────────
if [[ ! -x "$KVSERVER" ]]; then
    echo "ERROR: $KVSERVER not found. Run 'make' first."
    exit 1
fi
if [[ ! -x "$BENCH" ]]; then
    echo "ERROR: $BENCH not found. Run 'make bench' first."
    exit 1
fi

# ── Start server in background ─────────────────────────────
echo "Starting kvserver on port $PORT (workers=$WORKERS, buckets=$BUCKETS)..."
"$KVSERVER" "$PORT" "$WORKERS" "$BUCKETS" "$SWEEPER_MS" &
SERVER_PID=$!
sleep 0.5

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "ERROR: server failed to start."
    exit 1
fi

# Always kill server on exit, even on error
trap 'kill "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null' EXIT

# ── Run benchmarks ─────────────────────────────────────────
echo ""
echo "Benchmark: ${READ_PCT}% GET / $((100 - READ_PCT))% PUT, ${OPS_PER_CLIENT} ops/client"
echo ""

printf "%-10s %-15s %-12s %-18s\n" "Clients" "Total ops" "Wall (s)" "Throughput (ops/s)"
printf "%-10s %-15s %-12s %-18s\n" "-------" "---------" "--------" "------------------"

for CLIENTS in 1 4 16 64; do
    OUTPUT=$("$BENCH" localhost "$PORT" "$CLIENTS" "$OPS_PER_CLIENT" "$READ_PCT" 2>&1)
    EXIT_CODE=$?

    if [ $EXIT_CODE -ne 0 ] || [ -z "$OUTPUT" ]; then
        printf "%-10s %-15s %-12s %-18s\n" "$CLIENTS" "ERROR" "-" "-"
        echo "  bench output: $OUTPUT" >&2
        sleep 1
        continue
    fi

    # Single awk pass: parse all three values from the two output lines.
    # Works whether throughput ends with " ops/s" or not.
    read -r TOTAL WALL TPUT < <(echo "$OUTPUT" | awk '
        /total_ops=/ {
            for (i=1; i<=NF; i++) {
                if ($i ~ /^total_ops=/)  { split($i, a, "="); total = a[2] }
                if ($i ~ /^wall=/)       { split($i, a, "="); gsub(/s$/, "", a[2]); wall = a[2] }
                if ($i ~ /^throughput=/) { split($i, a, "="); tput = a[2] }
            }
        }
        END { print total, wall, tput }
    ')

    printf "%-10s %-15s %-12s %-18s\n" \
        "$CLIENTS" \
        "${TOTAL:-ERR}" \
        "${WALL:-ERR}s" \
        "${TPUT:-ERR}"

    sleep 1
done

echo ""
echo "Done. Server (PID $SERVER_PID) will be stopped now."