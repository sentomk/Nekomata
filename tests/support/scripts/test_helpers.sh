#!/usr/bin/env bash
# Shared assertions for process-based acceptance tests.

log_cursor() { wc -l < "$1"; }

wait_for_new() { # <pattern> <timeout_s> <label> <log> <cursor> [min_count]
  local pattern="$1" timeout="$2" label="$3" log="$4" cursor="$5" min="${6:-1}"
  local waited=0
  while [ "$(tail -n "+$((cursor + 1))" "$log" | grep -ac -- "$pattern" || true)" -lt "$min" ]; do
    if [ "$waited" -ge $((timeout * 5)) ]; then
      echo "FAIL($label): expected new '$pattern' output within ${timeout}s"
      tail -4 "$log"
      return 1
    fi
    sleep 0.2
    waited=$((waited + 1))
  done
}

check_clean_log() {
  local status
  if grep -aEq 'AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer|runtime error:' "$1"; then
    echo "FAIL: sanitizer diagnostic in $1"
    return 1
  else
    status=$?
    if [ "$status" -ne 1 ]; then
      echo "FAIL: could not inspect log $1"
      return 1
    fi
  fi
}

finish_runner() { # <pid> <stop_file> <log>
  local pid="$1" stop_file="$2" log="$3" status
  # Request a normal return from main, including destructors and sanitizer
  # shutdown checks. Never discard a child's status on the success path.
  : > "$stop_file"
  if wait "$pid"; then
    status=0
  else
    status=$?
  fi
  if [ "$status" -ne 0 ]; then
    echo "FAIL: runner exited with status $status (output: $log)"
    return 1
  fi
  if ! grep -aq '^\[harness\] stopped$' "$log"; then
    echo "FAIL: runner exited before acknowledging shutdown (output: $log)"
    return 1
  fi
  check_clean_log "$log"
}
