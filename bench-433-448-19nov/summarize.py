#!/usr/bin/env python3
"""summarize.py <raw dir>: the tables of bench-433-448-19nov from run.sh's tagged output lines.

Each tagged line is '<label> v=<build> i=<run> rc=<rc> | <program output>'. Prints:
- vkmsaa2x: per label and build, runs, median ms/frame (wall and GPU timestamps), min-max, spread
  ((max-min)/median), change of the median against the first build, image hashes, and the resolve check;
- aurora-r11g11b10f: per build and run, every step's plain-image hash and differing-word count, and for the
  resolve steps how many pixels are not the mean, with the first word;
- aurora-tile-visit: per label, build and run, the shapes that failed.
"""
import re
import statistics
import sys
from collections import OrderedDict, defaultdict
from pathlib import Path

TAG = re.compile(r'^(\S+) v=(\S+) i=(\d+) rc=(\d+) \| (.*)$')
WALL = re.compile(r': ([0-9.]+) ms/frame wall')
GPU = re.compile(r'gpu ([0-9.]+) ms/frame')
HASHES = re.compile(r'\b(samples|depth-samples|resolved|depth-resolve|post|resolved-after-pass)=([0-9a-f]{16})')
CHECK = re.compile(r'differs on (\d+) of (\d+) pixels \((\d+) edge, (\d+) interior\), max channel difference ([^\s,]+)')
STEP = re.compile(r'^(\S+(?: \d)?)\s+(\d+) of (\d+) (words differ|pixels are not the mean)(.*)$')


def tagged(raw, name):
    f = Path(raw) / name
    if not f.exists():
        return []
    return [m.groups() for m in map(TAG.match, f.read_text(errors='replace').splitlines()) if m]


def vk(raw, name):
    data = OrderedDict()
    for lab, v, i, rc, rest in tagged(raw, name):
        d = data.setdefault(lab, OrderedDict()).setdefault(v, defaultdict(list))
        w = WALL.search(rest)
        if w:
            d['wall'].append(float(w.group(1)))
            d['rc'].append(int(rc))
            g = GPU.search(rest)
            d['gpu'].append(float(g.group(1)) if g else float('nan'))
            d['hash'].append(' '.join(f'{k}={h}' for k, h in HASHES.findall(rest)))
        c = CHECK.search(rest)
        if c:
            d['check'].append('%s/%s px differ (%s edge, %s interior), max %s' % c.groups())
            d['hash_after'].append(HASHES.findall(rest)[0][1])
    for lab, vs in data.items():
        print(f'\n### {name}: {lab}\n')
        print('| build | runs | median ms/frame wall (GPU) | min-max wall | spread wall / GPU | change wall (GPU) vs first |')
        print('|---|---|---|---|---|---|')
        base = None
        for v, d in vs.items():
            if not d['wall']:
                continue
            mw, mg = statistics.median(d['wall']), statistics.median(d['gpu'])
            base = base or (mw, mg)
            sw = (max(d['wall']) - min(d['wall'])) / mw * 100
            sg = (max(d['gpu']) - min(d['gpu'])) / mg * 100
            chg = '' if base == (mw, mg) and v == next(iter(vs)) else \
                f'{(mw / base[0] - 1) * 100:+.1f} % ({(mg / base[1] - 1) * 100:+.1f} %)'
            bad = sum(1 for r in d['rc'] if r)
            print(f"| {v} | {len(d['wall'])}{f' ({bad} rc!=0)' if bad else ''} | {mw:.3f} ({mg:.3f}) | "
                  f"{min(d['wall']):.3f}-{max(d['wall']):.3f} | {sw:.1f} % / {sg:.1f} % | {chg} |")
        for v, d in vs.items():
            hs = sorted(set(d['hash']))
            print(f"\n{v}: {len(hs)} distinct hash set(s) in {len(d['hash'])} runs: " + ' || '.join(hs))
            if d['check']:
                print(f"{v} resolve check: " + '; '.join(d['check']))
                print(f"{v} resolve after the pass: {len(set(d['hash_after']))} distinct: {' '.join(sorted(set(d['hash_after'])))}")
        print('\nper-run ms/frame wall (GPU), run order: ' + '; '.join(
            f"{v}: {' '.join(f'{a:.3f} ({b:.3f})' for a, b in zip(d['wall'], d['gpu']))}" for v, d in vs.items()))


def r11(raw, name):
    runs = OrderedDict()
    for lab, v, i, rc, rest in tagged(raw, name):
        r = runs.setdefault((lab, v, i), {'rc': rc, 'steps': []})
        m = STEP.match(rest.strip())
        if m:
            step, n, tot, kind, tail = m.groups()
            h = re.search(r'plain ([0-9a-f]{16})', tail)
            first = re.search(r'first ([0-9a-f]{8})', tail)
            if kind.startswith('words'):
                r['steps'].append(f"{step} {n}w/{h.group(1)[:8] if h else '?'}")
            else:
                r['steps'].append(f"{step} {n}px-not-mean(first {first.group(1) if first else '?'})")
        elif rest.strip() in ('PASS', 'FAIL'):
            r['result'] = rest.strip()
    print(f'\n### {name}\n')
    print('label build run rc result | step differing-words/plain-hash ... ; resolve steps: pixels not the mean')
    for (lab, v, i), r in runs.items():
        print(f"{lab} {v} {i} rc={r['rc']} {r.get('result', '?')} | " + ' '.join(r['steps']))


def visit(raw, name):
    res = OrderedDict()
    for lab, v, i, rc, rest in tagged(raw, name):
        r = res.setdefault((lab, v, i), {'rc': rc, 'fail': [], 'pass': 0, 'note': []})
        s = rest.strip()
        if s.startswith('FAIL '):
            r['fail'].append(s[5:])
        elif s.startswith('PASS '):
            r['pass'] += 1
        elif s not in ('PASS', 'FAIL'):
            r['note'].append(s)
    print(f'\n### {name}\n')
    for (lab, v, i), r in res.items():
        print(f"{lab} {v} {i} rc={r['rc']}: {r['pass']} shapes pass, fail: {', '.join(r['fail']) or '-'}"
              + (f" | {'; '.join(r['note'][:3])}" if r['note'] else ''))


if __name__ == '__main__':
    raw = sys.argv[1] if len(sys.argv) > 1 else 'raw'
    r11(raw, 'r11-2x.txt')
    r11(raw, 'r11-1x.txt')
    visit(raw, 'visit.txt')
    vk(raw, 'vk-r11.txt')
    vk(raw, 'vk-other.txt')
    vk(raw, "vk-2x.txt")
