#!/usr/bin/env python3
"""summarize.py <raw dir>: medians, spread and image hashes from run-all.sh's tagged output lines.

Each tagged line is '<label> v=<variant> i=<run> rc=<rc> | <benchmark output>'. Prints one markdown table
per benchmark label: runs, median ms/frame, min-max, spread ((max-min)/median), change of the median
against the first variant, and whether every run of every variant produced the same image hashes.
"""
import re
import statistics
import sys
from collections import OrderedDict, defaultdict
from pathlib import Path

TAG = re.compile(r'^(\S+) v=(\S+) i=(\d+) rc=(\d+) \| (.*)$')
MS = [re.compile(r': ([0-9.]+) ms/frame'), re.compile(r'\(([0-9.]+) ms/frame')]
GPU = re.compile(r'gpu ([0-9.]+) ms/frame')
HASHES = re.compile(r'\b(image|colour|ao|depth|samples|depth-samples|resolved|depth-resolve|post|hash)=([0-9a-f]{16})')


def main(raw):
    data = OrderedDict()           # label -> variant -> {'ms': [], 'gpu': [], 'hash': set(), 'rc': []}
    for f in sorted(Path(raw).glob('*.txt')):
        for line in f.read_text(errors='replace').splitlines():
            m = TAG.match(line)
            if not m:
                continue
            lab, v, i, rc, rest = m.groups()
            d = data.setdefault(f'{f.stem}: {lab}', OrderedDict()).setdefault(v, defaultdict(list))
            if rest.startswith('driver '):
                d['driver'] = [rest]
                continue
            if rest.startswith('image hash='):          # sodiumpace prints its hash on its own line
                d['hash'].append(rest.split()[1])
                continue
            for r in MS:
                x = r.search(rest)
                if x:
                    d['ms'].append(float(x.group(1)))
                    d['rc'].append(int(rc))
                    g = GPU.search(rest)
                    if g:
                        d['gpu'].append(float(g.group(1)))
                    hs = ' '.join(f'{k}={h}' for k, h in HASHES.findall(rest))
                    if hs:
                        d['hash'].append(hs)
                    break
    for lab, vs in data.items():
        print(f'\n### {lab}\n')
        print('| variant | runs | median ms/frame | min-max | spread | change vs first | GPU ms (median) |')
        print('|---|---|---|---|---|---|---|')
        base = None
        for v, d in vs.items():
            ms = d['ms']
            if not ms:
                continue
            med = statistics.median(ms)
            base = med if base is None else base
            chg = '' if med == base and v == next(iter(vs)) else f'{(med / base - 1) * 100:+.1f} %'
            gpu = f"{statistics.median(d['gpu']):.3f}" if d['gpu'] else 'n/a'
            bad = sum(1 for r in d['rc'] if r)
            print(f"| {v} | {len(ms)}{f' ({bad} rc!=0)' if bad else ''} | {med:.3f} | {min(ms):.3f}-{max(ms):.3f} | "
                  f"{(max(ms) - min(ms)) / med * 100:.1f} % | {chg} | {gpu} |")
        hsets = {v: sorted(set(d['hash'])) for v, d in vs.items() if d['hash']}
        allh = set(h for hs in hsets.values() for h in hs)
        if allh:
            same = len(allh) == 1
            print(f"\nimage hashes: {'identical across all runs and variants' if same else 'DIFFER'}: " +
                  '; '.join(f'{v}: {", ".join(hs)}' for v, hs in hsets.items()))
        print('\nper-run ms/frame (run order): ' + '; '.join(f"{v}: {' '.join(f'{x:.3f}' for x in d['ms'])}" for v, d in vs.items()))


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else '.')
