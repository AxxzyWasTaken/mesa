# Copyright 2026 Aurora Silicon
# SPDX-License-Identifier: MIT
"""Read a generated agx3{b,c,d}_tables.c, and encode or decode with it.

load() parses the arrays of a generated table back into Python tuples in the
C struct field order of agx3_compose.h.

Composer mirrors agx3_compose.c exactly, including every fail-closed rule.

Decoder is the inverse: for an instruction's bytes it walks the candidate
forms' choice groups (alternatives in table order, pruned by their constant
bits), solves each path's field constraints, and accepts the first field
vector that Composer turns back into exactly those bytes. A decoded vector is
therefore always one the composer reproduces; it is not independent evidence
that the table's field-to-bit mapping is right.
"""

import re

NUM_FIELDS = 160

ARRAYS = ('fixeds', 'preds', 'cmds', 'groups', 'nodes', 'forms', 'altpool',
          'lutpool', 'setpool')


def wmask(width):
    return (1 << width) - 1


def load(path):
    src = open(path).read()
    T = {}
    for name in ARRAYS:
        m = re.search(r'static const \w+ ' + name + r'\[\] = \{\n?(.*?)\n?\};',
                      src, re.S)
        body = m.group(1)
        if name == 'forms':
            T[name] = [(a, int(b), int(c), int(d)) for a, b, c, d in
                       re.findall(r'\{"([^"]*)",(\d+),(\d+),(\d+)\}', body)]
        elif name in ('altpool', 'lutpool', 'setpool'):
            T[name] = [int(x.replace('ull', ''), 0)
                       for x in body.replace('\n', '').split(',') if x.strip()]
        else:
            T[name] = [tuple(int(v.replace('ull', ''), 0) for v in t.split(','))
                       for t in re.findall(r'\{([^{}]*)\}', body)]
    m = re.search(r'const agx3_table (\w+) = \{\s*forms, (\d+),', src)
    T['name'] = m.group(1)
    T['num_forms'] = int(m.group(2))
    return T


class Composer:
    """agx3_compose.c in Python: same contract, same failures."""

    def __init__(self, T):
        self.T = T

    def compose(self, form, fields):
        T = self.T
        _, _, size, root = T['forms'][form]
        self.nwords = size // 2
        self.words = [0] * self.nwords
        self.written = [0] * self.nwords
        self.consumed = set()
        self.is_lut = set()
        self.is_pred = set()
        self.direct = []
        self.fields = fields

        _, _, po, pn, _, _, _, _ = T['nodes'][root]
        if not self.preds_test(po, pn):
            return None
        self.preds_mark(po, pn)
        if not self.apply_node(root):
            return None

        src = [0] * NUM_FIELDS
        for f, w, s in self.direct:
            src[f] |= wmask(w) << s
        for i in range(NUM_FIELDS):
            if fields[i] and i not in self.consumed:
                return None
            if (i in self.consumed and i not in self.is_lut and
                    i not in self.is_pred and (fields[i] & ~src[i])):
                return None

        out = bytearray()
        for w in self.words:
            out += bytes((w & 0xff, w >> 8))
        return bytes(out)

    def pred_ok(self, p):
        f, k, sh, w, a, b = p
        v = self.fields[f]
        if k == 0:
            return v == a
        if k == 1:
            return v < 0x10000 and v in self.T['setpool'][a:a + b]
        if k == 2:
            return a <= v <= b
        if k == 3:
            return ((v >> sh) & wmask(w)) == a
        return False

    def preds_test(self, off, n):
        return all(self.pred_ok(p) for p in self.T['preds'][off:off + n])

    def preds_mark(self, off, n):
        for p in self.T['preds'][off:off + n]:
            self.consumed.add(p[0])
            self.is_pred.add(p[0])

    def deposit(self, word, mask, value):
        if word >= self.nwords:
            return False
        if self.written[word] & mask & (self.words[word] ^ value):
            return False
        self.words[word] = (self.words[word] & ~mask) | (value & mask)
        self.written[word] |= mask
        return True

    def snapshot(self):
        return (list(self.words), list(self.written), set(self.consumed),
                set(self.is_lut), set(self.is_pred), len(self.direct))

    def restore(self, s):
        self.words, self.written, self.consumed, self.is_lut, self.is_pred, nd = s
        del self.direct[nd:]

    def apply_group(self, g):
        T = self.T
        ao, an = T['groups'][g]
        for a in T['altpool'][ao:ao + an]:
            _, _, po, pn, _, _, _, _ = T['nodes'][a]
            if not self.preds_test(po, pn):
                continue
            s = self.snapshot()
            self.preds_mark(po, pn)
            if self.apply_node(a):
                return True
            self.restore(s)
        return False

    def apply_node(self, n):
        T = self.T
        fo, fn, _, _, co, cn, go, gn = T['nodes'][n]
        for w, m, v in T['fixeds'][fo:fo + fn]:
            if not self.deposit(w, m, v):
                return False
        for f, word, dst, width, src, lo, ln in T['cmds'][co:co + cn]:
            v = self.fields[f]
            self.consumed.add(f)
            if lo >= 0:
                self.is_lut.add(f)
                if v >= ln:
                    return False
                v = T['lutpool'][lo + v]
            else:
                self.direct.append((f, width, src))
            bits = (v >> src) & wmask(width)
            if not self.deposit(word, (wmask(width) << dst) & 0xffff,
                                (bits << dst) & 0xffff):
                return False
        for g in range(go, go + gn):
            if not self.apply_group(g):
                return False
        return True


class Decoder:
    def __init__(self, T):
        self.T = T
        self.C = Composer(T)
        self.by_size = {}
        for i, (_, _, size, _) in enumerate(T['forms']):
            self.by_size.setdefault(size, []).append(i)

    @staticmethod
    def word(data, w):
        return data[2 * w] | (data[2 * w + 1] << 8)

    def fixed_ok(self, node, data):
        fo, fn = self.T['nodes'][node][:2]
        for w, m, v in self.T['fixeds'][fo:fo + fn]:
            if 2 * w + 1 >= len(data) or (self.word(data, w) & m) != v:
                return False
        return True

    def candidates(self, data):
        """Forms of this size whose own constant bits match."""
        return [f for f in self.by_size.get(len(data), [])
                if self.fixed_ok(self.T['forms'][f][3], data)]

    def paths(self, node, data):
        """Yield node lists (node plus one alternative per choice group,
        recursively) whose constant bits all match data."""
        T = self.T
        _, _, _, _, _, _, go, gn = T['nodes'][node]
        groups = list(range(go, go + gn))

        def rec(gi):
            if gi == len(groups):
                yield []
                return
            ao, an = T['groups'][groups[gi]]
            for a in T['altpool'][ao:ao + an]:
                if not self.fixed_ok(a, data):
                    continue
                for sub in self.paths(a, data):
                    for rest in rec(gi + 1):
                        yield sub + rest

        for r in rec(0):
            yield [node] + r

    def solve(self, nodes, data):
        """Field values that satisfy the deposits and predicates of nodes."""
        T = self.T
        known = {}   # field -> [mask, value]
        luts = {}    # field -> [(lut, src, width, bits)]
        preds = {}   # field -> [pred]
        for n in nodes:
            _, _, po, pn, co, cn, _, _ = T['nodes'][n]
            for p in T['preds'][po:po + pn]:
                preds.setdefault(p[0], []).append(p)
            for f, word, dst, width, src, lo, ln in T['cmds'][co:co + cn]:
                bits = (self.word(data, word) >> dst) & wmask(width)
                if lo >= 0:
                    luts.setdefault(f, []).append(
                        (tuple(T['lutpool'][lo:lo + ln]), src, width, bits))
                else:
                    k = known.setdefault(f, [0, 0])
                    m = wmask(width) << src
                    if k[0] & m & (k[1] ^ (bits << src)):
                        return None
                    k[0] |= m
                    k[1] = (k[1] & ~m) | (bits << src)

        fields = [0] * NUM_FIELDS
        for f in set(known) | set(luts) | set(preds):
            km, kv = known.get(f, [0, 0])
            ps = preds.get(f, [])
            # Bitfield predicates fix slices of the value.
            for p in ps:
                if p[1] == 3:
                    m = wmask(p[3]) << p[2]
                    if km & m & (kv ^ (p[4] << p[2])):
                        return None
                    km |= m
                    kv = (kv & ~m) | (p[4] << p[2])
            eqs = [p[4] for p in ps if p[1] == 0]
            if eqs:
                cands = [eqs[0]]
            elif f in luts:
                cands = range(min(len(l[0]) for l in luts[f]))
            else:
                cands = [kv]

            ok = None
            for v in cands:
                if (v & km) != kv:
                    continue
                if any(v >= len(l[0]) or
                       ((l[0][v] >> l[1]) & wmask(l[2])) != l[3]
                       for l in luts.get(f, [])):
                    continue
                good = True
                for p in ps:
                    k = p[1]
                    if k == 0 and v != p[4]:
                        good = False
                    elif k == 1 and v not in T['setpool'][p[4]:p[4] + p[5]]:
                        good = False
                    elif k == 2 and not (p[4] <= v <= p[5]):
                        good = False
                    elif k == 3 and ((v >> p[2]) & wmask(p[3])) != p[4]:
                        good = False
                if good:
                    ok = v
                    break
            if ok is None:
                return None
            fields[f] = ok
        return fields

    def decode(self, data, forms=None, limit=20000):
        """(form, fields) that compose back to exactly data, or None."""
        tried = 0
        for f in (forms if forms is not None else self.candidates(data)):
            root = self.T['forms'][f][3]
            for nodes in self.paths(root, data):
                tried += 1
                if tried > limit:
                    return None
                fields = self.solve(nodes, data)
                if fields is None:
                    continue
                if self.C.compose(f, fields) == bytes(data):
                    return f, fields
        return None
