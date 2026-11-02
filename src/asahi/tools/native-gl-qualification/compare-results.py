#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare complete deqp-runner CSV files with an explicit case list."""
import argparse
import csv
from pathlib import Path
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("caselist", type=Path)
p.add_argument("baseline", type=Path)
p.add_argument("candidate", type=Path)
a = p.parse_args()
cases = a.caselist.read_text().splitlines()
assert cases and len(cases) == len(set(cases)), "empty/duplicate caselist"

def read(path):
    rows = {}
    with path.open(newline="") as f:
        for row in csv.reader(f):
            if not row:
                continue
            assert len(row) >= 2, (path, row)
            case, result = row[:2]
            assert case not in rows, (path, "duplicate", case)
            rows[case] = result
    assert set(rows) == set(cases), (path, "missing/extra cases", set(cases) ^ set(rows))
    return rows

old, new = read(a.baseline), read(a.candidate)
good = {"Pass", "CompatibilityWarning", "QualityWarning"}
regressions = []
for case in cases:
    if old[case] != new[case]:
        print(f"{case}\t{old[case]}\t{new[case]}")
    # An unexpected loss of advertised support is also a regression.
    if old[case] in good and new[case] not in good:
        regressions.append(case)
    if old[case] == "Pass" and new[case] != "Pass":
        regressions.append(case)
    if new[case] not in good | {"NotSupported", "Skip"}:
        regressions.append(case)
print(f"cases={len(cases)} blocking_results={len(set(regressions))}")
sys.exit(bool(regressions))
