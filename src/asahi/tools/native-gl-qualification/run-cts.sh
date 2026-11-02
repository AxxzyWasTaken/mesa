#!/bin/bash
# SPDX-License-Identifier: MIT
set -euo pipefail
# Usage: run-cts.sh OVERLAY native|zink CTS_ROOT LIST_DIR OUTPUT_DIR
overlay=$(realpath "${1:?overlay}")
driver=${2:?native or zink}
cts=$(realpath "${3:?CTS root containing modules/ and external/}")
lists=$(realpath "${4:?directory with gles2.txt gles3.txt gles31.txt}")
mkdir -p "${5:?output directory}"
out=$(realpath "$5")
kit=$(cd -- "$(dirname -- "$0")" && pwd)
test "$driver" = native || test "$driver" = zink
# An approved hardware reservation must cover this entire run.
failed=0
for suite in gles2 gles3 gles31; do
    binary="$cts/modules/$suite/deqp-$suite"
    test -x "$binary"
    test -s "$lists/$suite.txt"
    test ! -e "$out/$suite"
    mkdir -p "$out/$suite"
    sha256sum "$binary" "$lists/$suite.txt" > "$out/$suite/inputs.sha256"
    deqp-runner --version > "$out/$suite/runner-version.txt"
    # Continue to the other suites after a CTS failure, preserving its exit code.
    rc=0
    (cd "$(dirname "$binary")" && EGL_PLATFORM=surfaceless python3 "$kit/with-mesa.py" \
      --overlay "$overlay" --driver "$driver" -- \
      deqp-runner run --deqp "$binary" --caselist "$lists/$suite.txt" \
      --output "$out/$suite/results" --jobs "${JOBS:-1}" \
      --timeout "${TIMEOUT:-120}" --tests-per-group 100 -- \
      --deqp-surface-type=pbuffer --deqp-gl-config-name=rgba8888d24s8ms0 \
      --deqp-visibility=hidden --deqp-surface-width=256 --deqp-surface-height=256 \
      --deqp-log-images=disable --deqp-shadercache=disable) \
      > "$out/$suite/runner.log" 2>&1 || rc=$?
    echo "$rc" > "$out/$suite/exit-code"
    test "$rc" = 0 || failed=1
done
exit "$failed"
