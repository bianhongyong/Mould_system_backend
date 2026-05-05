#!/usr/bin/env bash
# 校验 compete 压测：三份 recv_*.bin 合并后应为 1..N 各出现一次（无丢、无重复）。
# 用法：在生成 compete_stress_out 的当前工作目录执行；或
#   ./validate_compete_stress.sh /path/to/compete_stress_out
set -euo pipefail
OUT="${1:-./compete_stress_out}"
META="${OUT}/meta.txt"
if [[ ! -f "${META}" ]]; then
  echo "missing ${META} (producer may not have finished)" >&2
  exit 2
fi
N="$(grep -E '^publish_count=' "${META}" | head -1 | cut -d= -f2)"
if [[ -z "${N}" || "${N}" -lt 1 ]]; then
  echo "invalid publish_count in ${META}" >&2
  exit 3
fi
python3 - <<PY
import os, struct, sys
out = os.path.abspath("${OUT}")
n = int("${N}")
seqs = []
for name in ("recv_A.bin", "recv_B.bin", "recv_C.bin"):
    path = os.path.join(out, name)
    if not os.path.isfile(path):
        print("missing", path, file=sys.stderr)
        sys.exit(4)
    data = open(path, "rb").read()
    if len(data) % 8 != 0:
        print("corrupt length", path, len(data), file=sys.stderr)
        sys.exit(5)
    for i in range(0, len(data), 8):
        seqs.append(struct.unpack("<Q", data[i : i + 8])[0])
if len(seqs) != n:
    print("FAIL total deliveries", len(seqs), "expected", n, file=sys.stderr)
    sys.exit(6)
seqs.sort()
for i, s in enumerate(seqs, start=1):
    if s != i:
        print("FAIL gap or duplicate at position", i, "got", s, file=sys.stderr)
        sys.exit(7)
print("OK compete stress: merged", n, "unique seq 1..", n)
PY
