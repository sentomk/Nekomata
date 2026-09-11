#!/usr/bin/env bash
# Controlled faults in the test process, not in the hot-reload engine.
set -euo pipefail

mode="${MOCK_CASE:?}"
watched="$1"
stop_file="${2:-}"

if [[ "$0" == *reject_runner_pie ]]; then
  echo "(=･ω･=) process symbols: 2438 functions, 150 globals"
  echo "[hot] tick #1, g_counter=1"
  echo "(=^ω^=) reload applied: 1 function(s) redirected"
fi

if [[ "$0" == *hello_reload ]]; then
  for i in 1 2 3 4; do echo "[v1] tick #$i, g_counter=$i"; done
  while [ ! -f "$watched" ]; do sleep 0.02; done
  mv "$watched" "$watched.consumed"
  for i in 5 6 7; do echo "[v2] tick #$i, g_counter jumped to $((4 + (i - 4) * 10))"; done
  while [ ! -f "$watched" ]; do sleep 0.02; done
  mv "$watched" "$watched.consumed"
  for i in 8 9 10; do echo "[v3] tick #$i, third life, g_counter=$((34 + (i - 7) * 10))"; done
  case "$mode" in
    hello_nonzero) exit 42 ;;
    hello_early_zero) exit 0 ;;
    hello_signal) kill -KILL "$$" ;;
  esac
  while [ ! -f "$stop_file" ]; do sleep 0.02; done
  echo "[harness] stopped"
  if [ "$mode" = hello_shutdown_nonzero ]; then exit 42; fi
  if [ "$mode" = hello_sanitizer ]; then echo "runtime error: injected UBSan diagnostic" >&2; fi
  exit 0
fi

stage=0; calls=0; counter=0; recovered=0
# The arena-boundary case runs a second, fresh runner watching boundary.new.o;
# its single offer is a valid reload (stage 10), not a rejection. It must exit
# cleanly regardless of the fault mode: the injected nonzero exit belongs to
# the main runner, not to this boundary probe.
if [[ "$watched" == *boundary* ]]; then stage=10; boundary=1; fi
while [ ! -f "$stop_file" ]; do
  if [ -f "$watched" ]; then
    mv "$watched" "$watched.consumed"
    stage=$((stage + 1))
    case "$stage" in
      1) echo "bad magic" ;;
      2) echo "truncated object file" ;;
      3) if [ "$mode" != reject_stale_truncated ]; then echo "truncated object file"; fi ;;
      4) echo "reload applied" ;;
      5) echo "new globals are not supported yet" ;;
      6) # cross-TU call into the host: applies, and the host function runs
        echo "reload applied"
        echo "[host] host_only called" ;;
      7) echo "cannot resolve external symbol '_Z18missing_everywherev'" ;;
      8) echo "inconsistent state anchors — the global layout changed" ;;
      9) echo "GOT-style relocs need -fno-pic" ;;
      10)
        if [ "$mode" != reject_stale_final ]; then
          echo "reload applied"
          if [ "$mode" != reject_ignored_final ]; then recovered=1; fi
        fi
        if [ "$mode" = reject_state_reset ]; then calls=0; counter=0; fi
        ;;
      11) echo "reload applied" ;; # arena boundary: a valid reload that must apply
      *) echo "unexpected offer" >&2; exit 2 ;;
    esac
  fi
  calls=$((calls + 1))
  if [ "$recovered" -eq 1 ]; then
    counter=$((counter + 10))
    echo "[recovered] tick #$calls, g_counter=$counter"
  else
    counter=$((counter + 1))
    echo "[hot] tick #$calls, g_counter=$counter"
  fi
  sleep 0.05
done
echo "[harness] stopped"
if [ "${boundary:-0}" = 1 ]; then exit 0; fi
if [ "$mode" = reject_nonzero ]; then exit 42; fi
