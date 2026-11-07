#!/usr/bin/env python3
# Copyright 2026 Aurora Silicon
# SPDX-License-Identifier: MIT
"""Generate g18g_capture_vectors.h from native J873 (G18G) Metal captures.

  gen_g18g_capture_vectors.py SESSION [SESSION ...] [-o OUT] [--check FILE]

SESSION is a capture session directory of the native Metal oracle
(kept outside this tree), holding one <mode>/ per oracle mode with
<mode>/oracle.stderr and the gpucapture directory <mode>/cap (index.jsonl
plus blobs/). Every session given must produce byte-identical vectors;
--check additionally requires them to equal FILE byte for byte.

Only record words are emitted: the VDM or CDM control stream of the command
buffer under test, the PPP state records the VDM stream points at, and the
PBE descriptors that name one of the pass's attachments. No buffer contents,
process ids, timestamps or host paths are written.

The kernel-command (kcmd) offsets used here come from the capture analysis
of these sessions, cross-checked against the M5 register program:

  +0x150  u32  offset of the attachment list, from kcmd+0x178
  +0x154  u32  command type: 1 render, 3 compute
  +0x174  u64  VDM (render) or CDM (compute) control stream base
  +0x1d4  u64  CDM control stream end (compute)
  +0x60c  u32  render width, +0x610 height

The attachment list is {u32 count, u32 1, u32 0} followed by 0x18-byte
entries whose first u64 is the attachment VA.
"""
import argparse
import json
import os
import re
import struct
import sys

MODES = ('color', 'depth-less', 'depth-always', 'stencil', 'msaa4', 'r32f',
         'r8ui', 'points', 'lines', 'blend-add', 'discard', 'texture-sample',
         'compute-add')

KCMD_ATTACHMENT_LIST = 0x150
KCMD_ATTACHMENT_BASE = 0x178
KCMD_TYPE = 0x154
KCMD_STREAM_BASE = 0x174
KCMD_CDM_END = 0x1d4
KCMD_WIDTH = 0x60c
KCMD_HEIGHT = 0x610
KCMD_TYPE_RENDER = 1
KCMD_TYPE_COMPUTE = 3

# agx_vdm_ppp_state_core.h, agx_vdm_state_update_core.h,
# agx_vdm_stream_terminate_core.h, agx_vdm_index_list_core.h
VDM_PPP_STATE_LENGTH = 8
VDM_PPP_STATE_RESERVED_MASK = 0xffff0000
VDM_STATE_UPDATE_HEADER = 0x4000002e
VDM_STATE_UPDATE_LENGTH = 28
VDM_INDEX_LIST_TAILED_NONINDEXED = 0x69c40000
VDM_INDEX_LIST_TAILED_NONINDEXED_LENGTH = 20
VDM_STREAM_TERMINATE = 0xc0000000
# cmdbuf.xml "CDM Block Type": Stream Terminate = 2
CDM_STREAM_TERMINATE = 0x40000000
PBE_WORDS = 8


class CaptureError(Exception):
    pass


def u32(data, off):
    return struct.unpack_from('<I', data, off)[0]


def u64(data, off):
    return struct.unpack_from('<Q', data, off)[0]


def words(data):
    return list(struct.unpack('<%dI' % (len(data) // 4), data))


def bits(ws, start, size):
    value = 0
    for i, w in enumerate(ws):
        value |= w << (32 * i)
    return (value >> start) & ((1 << size) - 1)


class Submission:
    """The single kernel submission of one oracle mode's command buffer under
    test, as recorded by gpucapture."""

    def __init__(self, mode_dir, mode):
        cap = os.path.join(mode_dir, 'cap')
        records = []
        with open(os.path.join(cap, 'index.jsonl')) as f:
            for line in f:
                if line.strip():
                    records.append(json.loads(line))

        # ORACLE_COMMIT/ORACLE_DONE bracket the command buffer under test on
        # the capture's clock (CLOCK_UPTIME_RAW), as validate_capture.py uses.
        window = {}
        with open(os.path.join(mode_dir, 'oracle.stderr')) as f:
            for line in f:
                m = re.match(r'ORACLE_(COMMIT|DONE) mode=(\S+) cb=test t_ns=(\d+)', line)
                if m and m.group(2) == mode:
                    window[m.group(1)] = int(m.group(3))
        if set(window) != {'COMMIT', 'DONE'}:
            raise CaptureError(f'{mode}: no test commit window in oracle.stderr')
        submits = [r for r in records if r['type'] == 'submit' and
                   window['COMMIT'] <= r['t_ns'] <= window['DONE']]
        if len(submits) != 1 or len(submits[0]['cmdbufs']) != 1:
            raise CaptureError(f'{mode}: expected one submission with one command buffer')

        self.cap = cap
        self.submit = submits[0]
        kcmd = self.submit['cmdbufs'][0]['kcmd']
        self.kcmd = self.blob(kcmd['blob'])[:kcmd['used']]
        self.buffers = [b for b in self.submit['buffers'] if b.get('blob')]
        self._cache = {}

    def blob(self, sha):
        with open(os.path.join(self.cap, 'blobs', sha[:2], sha), 'rb') as f:
            return f.read()

    def contents(self, buf):
        if buf['blob'] not in self._cache:
            self._cache[buf['blob']] = self.blob(buf['blob'])
        return self._cache[buf['blob']]

    def read(self, va, size):
        """Bytes at a GPU VA as they were at submit time. Like
        read_capture.find_buffer, prefer a referenced root, then the
        smallest buffer that holds the whole range."""
        best = None
        for b in self.buffers:
            base = int(b['gpu_va'], 16) if isinstance(b['gpu_va'], str) else b['gpu_va']
            if base <= va and va + size <= base + b['bytes']:
                key = (b['refs'] == ['live'], b['bytes'])
                if best is None or key < best[0]:
                    best = (key, b, base)
        if best is None:
            raise CaptureError(f'VA {va:#x}+{size:#x} is not in a captured buffer')
        _, b, base = best
        data = self.contents(b)[va - base:va - base + size]
        if len(data) != size:
            raise CaptureError(f'VA {va:#x}+{size:#x} runs past the captured bytes')
        return data

    def attachments(self):
        off = KCMD_ATTACHMENT_BASE + u32(self.kcmd, KCMD_ATTACHMENT_LIST)
        count = u32(self.kcmd, off)
        return [u64(self.kcmd, off + 12 + 0x18 * i) for i in range(count)]


def walk_vdm(sub, base):
    """VDM stream words from base through the Stream Terminate, and the PPP
    state records it points at. Only the record forms these captures contain
    are accepted; anything else stops generation."""
    stream, ppp, off = [], [], 0
    while True:
        w0 = u32(sub.read(base + off, 4), 0)
        block = w0 >> 29
        if block == 0:
            rec = sub.read(base + off, VDM_PPP_STATE_LENGTH)
            w1 = u32(rec, 4)
            if w0 & VDM_PPP_STATE_RESERVED_MASK or w1 & 3:
                raise CaptureError(f'bad PPP state record at vdm+{off:#x}')
            address = ((w0 & 0xff) << 32) | w1
            size_words = (w0 >> 8) & 0xff
            ppp.append((off, address, words(sub.read(address, 4 * size_words))))
            length = VDM_PPP_STATE_LENGTH
        elif block == 2 and w0 == VDM_STATE_UPDATE_HEADER:
            length = VDM_STATE_UPDATE_LENGTH
        elif block == 3 and (w0 & 0xffff00ff) == VDM_INDEX_LIST_TAILED_NONINDEXED:
            length = VDM_INDEX_LIST_TAILED_NONINDEXED_LENGTH
        elif w0 == VDM_STREAM_TERMINATE:
            length = 4
        else:
            raise CaptureError(f'unexpected VDM word {w0:#010x} at vdm+{off:#x}')
        stream += words(sub.read(base + off, length))
        off += length
        if w0 == VDM_STREAM_TERMINATE:
            return stream, ppp


def pbe_descriptors(sub, attachments, width, height):
    """Distinct 32-byte PBE Gen10 descriptors (cmdbuf.xml "PBE Gen10") that
    name an attachment and describe a width x height surface. A descriptor
    that also parses as a Texture Gen10 of that size is an error."""
    found = set()
    for va in attachments:
        needle = struct.pack('<I', (va >> 4) & 0xffffffff)
        for buf in sub.buffers:
            data = sub.contents(buf)
            i = data.find(needle)
            while i >= 0:
                start = i - 8
                size = 4 * PBE_WORDS
                if start >= 0 and start % size == 0 and start + size <= len(data):
                    w = words(data[start:start + size])
                    same_va = bits(w, 64, 44) << 4 == va
                    pbe = (same_va and bits(w, 24, 15) == width - 1 and
                           bits(w, 39, 15) == height - 1 and w[6] == 0 and w[7] == 0)
                    tex = (same_va and bits(w, 28, 15) == width - 1 and
                           bits(w, 43, 15) == height - 1)
                    if pbe and tex:
                        raise CaptureError(f'ambiguous descriptor for {va:#x}')
                    if pbe:
                        found.add((va, tuple(w)))
                i = data.find(needle, i + 1)
    return sorted(found)


def extract_mode(session, mode):
    sub = Submission(os.path.join(session, mode), mode)
    kind = u32(sub.kcmd, KCMD_TYPE)
    base = u64(sub.kcmd, KCMD_STREAM_BASE)
    if kind == KCMD_TYPE_RENDER:
        stream, ppp = walk_vdm(sub, base)
        width, height = u32(sub.kcmd, KCMD_WIDTH), u32(sub.kcmd, KCMD_HEIGHT)
        pbe = pbe_descriptors(sub, sub.attachments(), width, height)
        if not pbe:
            raise CaptureError(f'{mode}: no PBE descriptor names an attachment')
        return dict(mode=mode, compute=False, base=base, stream=stream, ppp=ppp, pbe=pbe)
    if kind == KCMD_TYPE_COMPUTE:
        end = u64(sub.kcmd, KCMD_CDM_END)
        if end <= base or (end - base) % 4:
            raise CaptureError(f'{mode}: bad CDM range {base:#x}..{end:#x}')
        stream = words(sub.read(base, end - base + 4))
        if stream[-1] != CDM_STREAM_TERMINATE:
            raise CaptureError(f'{mode}: CDM stream end is not a Stream Terminate')
        return dict(mode=mode, compute=True, base=base, stream=stream, ppp=[], pbe=[])
    raise CaptureError(f'{mode}: unknown kcmd type {kind}')


def ident(mode):
    return mode.replace('-', '_')


def hex_rows(ws, indent='   '):
    rows = []
    for i in range(0, len(ws), 6):
        rows.append(indent + ' '.join('0x%08x,' % w for w in ws[i:i + 6]))
    return '\n'.join(rows)


def render(vectors):
    out = ['''/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * GENERATED -- do not edit. Regenerate with gen_g18g_capture_vectors.py from
 * the native Metal oracle captures on the J873 (Apple M6, G18G, macOS
 * 26A428); sessions 20260925-a and 20260925-b produce this file byte for
 * byte.
 *
 * Per oracle mode, the command buffer under test as Apple's driver built it:
 * the VDM (render) or CDM (compute) control stream words through its Stream
 * Terminate, every PPP state record the VDM stream points at, and the
 * distinct PBE Gen10 descriptors that name one of the pass's attachments.
 * Addresses are the GPU VAs of the capture.
 */

#ifndef G18G_CAPTURE_VECTORS_H
#define G18G_CAPTURE_VECTORS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct g18g_capture_ppp {
   /* Byte offset of the pointing record in the VDM stream. */
   uint32_t vdm_offset;
   uint64_t address;
   uint32_t size_words;
   const uint32_t *words;
};

struct g18g_capture_pbe {
   uint64_t attachment;
   uint32_t words[8];
};

struct g18g_capture_mode {
   const char *name;
   bool compute;
   uint64_t stream_base;
   const uint32_t *stream;
   uint32_t stream_words;
   const struct g18g_capture_ppp *ppp;
   uint32_t ppp_count;
   const struct g18g_capture_pbe *pbe;
   uint32_t pbe_count;
};
''']
    for v in vectors:
        name = ident(v['mode'])
        kind = 'CDM' if v['compute'] else 'VDM'
        out.append(f'/* {v["mode"]}: {kind} stream at {v["base"]:#x} */')
        out.append(f'static const uint32_t g18g_{name}_stream[] = {{\n'
                   f'{hex_rows(v["stream"])}\n}};\n')
        for i, (_off, _address, ws) in enumerate(v['ppp']):
            out.append(f'static const uint32_t g18g_{name}_ppp{i}[] = {{\n'
                       f'{hex_rows(ws)}\n}};\n')
        if v['ppp']:
            rows = '\n'.join(f'   {{0x{off:x}, 0x{address:x}, {len(ws)}, g18g_{name}_ppp{i}}},'
                             for i, (off, address, ws) in enumerate(v['ppp']))
            out.append(f'static const struct g18g_capture_ppp g18g_{name}_ppp[] = {{\n'
                       f'{rows}\n}};\n')
        if v['pbe']:
            rows = '\n'.join(f'   {{0x{va:x},\n'
                             f'    {{{", ".join("0x%08x" % w for w in ws[:4])},\n'
                             f'     {", ".join("0x%08x" % w for w in ws[4:])}}}}},'
                             for va, ws in v['pbe'])
            out.append(f'static const struct g18g_capture_pbe g18g_{name}_pbe[] = {{\n'
                       f'{rows}\n}};\n')

    out.append('static const struct g18g_capture_mode g18g_capture_modes[] = {')
    for v in vectors:
        name = ident(v['mode'])
        ppp = (f'g18g_{name}_ppp, {len(v["ppp"])}' if v['ppp'] else 'NULL, 0')
        pbe = (f'g18g_{name}_pbe, {len(v["pbe"])}' if v['pbe'] else 'NULL, 0')
        out.append(f'   {{"{v["mode"]}", {"true" if v["compute"] else "false"}, '
                   f'0x{v["base"]:x},\n    g18g_{name}_stream, {len(v["stream"])},\n'
                   f'    {ppp}, {pbe}}},')
    out.append('};\n')
    out.append('#endif /* G18G_CAPTURE_VECTORS_H */\n')
    return '\n'.join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('session', nargs='+')
    parser.add_argument('-o', '--output', help='write the header here')
    parser.add_argument('--check', help='fail unless the header equals this file')
    args = parser.parse_args()

    headers = []
    for session in args.session:
        try:
            headers.append(render([extract_mode(session, m) for m in MODES]))
        except CaptureError as e:
            sys.exit(f'{session}: {e}')
    for session, header in zip(args.session[1:], headers[1:]):
        if header != headers[0]:
            sys.exit(f'{session}: vectors differ from {args.session[0]}')
    if args.output:
        with open(args.output, 'w') as f:
            f.write(headers[0])
    if args.check:
        with open(args.check) as f:
            if f.read() != headers[0]:
                sys.exit(f'{args.check} differs from the regenerated vectors')
    if not args.output and not args.check:
        sys.stdout.write(headers[0])
    print(f'{len(args.session)} session(s), {len(MODES)} modes: identical', file=sys.stderr)


if __name__ == '__main__':
    main()
