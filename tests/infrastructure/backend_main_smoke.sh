#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "usage: backend_main_smoke.sh <backend_main_binary> <launch_plan_path>" >&2
  exit 2
fi

bin_path="$1"
plan_path="$2"

# Normal scenario expectation: master keeps running until externally terminated.
set +e
timeout 2s "${bin_path}" "${plan_path}"
exit_code=$?
set -e
if [[ "${exit_code}" -ne 124 ]]; then
  echo "backend_main exited unexpectedly (code=${exit_code}), expected timeout(124)" >&2
  exit 1
fi
