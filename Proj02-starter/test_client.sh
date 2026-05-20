#!/bin/bash
# test_client.sh -- quick smoke test for kvserver
#
# Usage: ./test_client.sh <port>
# Assumes kvserver is already running on that port.
#
# This is NOT a grading test suite. It is a sanity check to help you verify
# basic protocol conformance before your viva. Write your own more thorough
# tests as you build.

PORT=${1:-9000}

send() {
    # Send a command and print the reply. Uses nc with a short timeout.
    printf "%s\n" "$1" | nc -q 1 localhost "$PORT"
}

echo "=== Smoke test against localhost:$PORT ==="
echo

echo "--- PUT color red ---"
send "PUT color red"

echo "--- GET color ---"
send "GET color"

echo "--- GET missing ---"
send "GET missing"

echo "--- PUT temp 72 2   (TTL = 2s, Stage 4) ---"
send "PUT temp 72 2"

echo "--- GET temp ---"
send "GET temp"

echo "--- (sleeping 3s for TTL to expire)"
sleep 3

echo "--- GET temp  (expect NOT_FOUND in Stage 4) ---"
send "GET temp"

echo "--- DEL color ---"
send "DEL color"

echo "--- STATS ---"
send "STATS"

echo
echo "Done."
