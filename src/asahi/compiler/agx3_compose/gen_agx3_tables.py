#!/usr/bin/env python3
# Copyright 2026 Aurora Silicon
# SPDX-License-Identifier: MIT
"""Generate agx3{b,c,d}_tables.c and AGX3d.xml from Apple's encoder tables.

Input: the arm64 slice of libapplegpu-nt.dylib from Apple's Metal toolchain
(Metal toolchain 27.1.266.1, metal 32023.921, file
Metal.xctoolchain/usr/metal/32023/lib/libapplegpu-nt.dylib). The dylib stays
out of the tree. Its llvm::AGX3 packed encoder tables carry one variant per
encoding group; the ones Mesa uses are

  variant 0 -> agx3b_tables.c  (G15, G16, G17P)
  variant 1 -> agx3c_tables.c  (G17G, G17S, G17D, G18P; "g17g-encodings")
  variant 2 -> agx3d_tables.c  (G18G, G18M, G19P; "g18g-encodings")

The table addresses below were recovered from the table-driven encoder at
0x3f6510 (form root lookup), 0x3f6810 (predicate check), 0x3f6b34 (choice
group resolution) and the command loop at 0x3f66b8. They are only valid for
that exact build, so the slice's SHA-256 is pinned and anything else is
refused.

The same code regenerates agx3b_tables.c and agx3c_tables.c byte-identically
from variants 0 and 1. Those two files were produced independently, by
packgen.py from Apple's tables, so that is the check that this reader and
emitter are right; agx3d_tables.c is variant 2 through the same path.

  gen_agx3_tables.py tables <libapplegpu-nt.dylib> {b,c,d} <out.c>
  gen_agx3_tables.py xml <agx3d_tables.c> <out.xml>

The xml command transcribes a generated table into the disassembler grammar
of src/asahi/isa (it produces AGX3d.xml).
"""

import argparse
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import agx3_table  # noqa: E402

# SHA-256 of the arm64 slice (lipo -thin arm64) of the toolchain's
# libapplegpu-nt.dylib. The universal file hashes to
# 2c1e80a0c2f716229332411903ed9b12f081668d85b59af1beae6e9f87989c3f.
DYLIB_ARM64_SHA256 = \
    '3495d3a9578eb7dd2dd90dae9149ceb15e2e89022da07d1e1e8e18c22e9cb39d'

# Opcode index -> Apple mnemonic. Entry i is the function that entry i of the
# opcode-indexed llvm::AGX3::mapInstrSpecFields_<mnemonic> pointer table of
# the AGXMetalG18G image points to. The index is the one the encoder tables
# below are keyed by.
OPCODE_NAMES = (
    'allocate_uvb', 'atomicdev', 'atomictg', 'b', 'barrier', 'bit_and',
    'bit_andc', 'bit_nand', 'bit_nandc', 'bit_nor', 'bit_norc', 'bit_or',
    'bit_orc', 'bit_xnor', 'bit_xor', 'bl', 'bn', 'brk', 'cbs', 'cem_calc',
    'clr', 'cmsk', 'cndcall', 'cndcf', 'cndef', 'cndend', 'cndlt',
    'cndret', 'cndst', 'continue_ray', 'copy', 'dbgrtn', 'depthf',
    'edgechk', 'end', 'extract', 'f16add', 'f16fma', 'f16fmashffdn',
    'f16fmashffup', 'f16matmad4x4', 'f16matmad8x8', 'f16mul', 'f32add',
    'f32dfdx', 'f32dfdy', 'f32div', 'f32exp2', 'f32fastdivstep1',
    'f32fastdivstep2', 'f32fma', 'f32fmashffdn', 'f32fmashffup',
    'f32interp', 'f32log2', 'f32matmad4x4', 'f32matmad8x8', 'f32mul',
    'f32powr', 'f32rcp', 'f32rint', 'f32rsqrt', 'f32sinc', 'f32sqrtstep',
    'fGeMM16x16x16', 'fcmp', 'flt2int', 'fltnorm', 'fsel', 'fself16',
    'fself32', 'fseli16', 'fseli32', 'fselp16', 'fselp32', 'ft1', 'iadd',
    'iGeMM16x16x16', 'iadd32', 'iadd64', 'icmp', 'imadd', 'imadd32',
    'imadd64', 'imgwblk', 'imgwci', 'imsub', 'imsub32', 'imsub64',
    'incStk', 'int2flt', 'intersect_ray', 'inv', 'isel', 'isub', 'iself16',
    'iself32', 'iseli16', 'iseli32', 'iselp16', 'iselp32', 'isub32',
    'isub64', 'itrcoords', 'ldcfr', 'lddev', 'ldimm', 'ldscs', 'ldshdr',
    'ldtg', 'ldtps', 'lldcol', 'lstcol', 'movmsk', 'msg', 'nop', 'normflt',
    'pack_imgwir', 'pack_smpir', 'pack', 'packhfp8', 'pixinfo', 'pixrel',
    'pixwait', 'qRedAND', 'qRedOR', 'qRedXOR', 'qRedf16add', 'qRedf16mul',
    'qRedf32add', 'qRedf32mul', 'qRedfmax', 'qRedfmin', 'qRediadd',
    'qRedsmax', 'qRedsmin', 'qRedumax', 'qRedumin', 'quadbfly',
    'quadfballot', 'quadiballot', 'quadshfl', 'quadshftdn', 'quadshftup',
    'rev', 'rld', 'sRedAND', 'sRedOR', 'sRedXOR', 'sRedf16add',
    'sRedf16mul', 'sRedf32add', 'sRedf32mul', 'sRedfmax', 'sRedfmin',
    'sRediadd', 'sRedsmax', 'sRedsmin', 'sRedumax', 'sRedumin', 'sel',
    'self16', 'self32', 'seli16', 'seli32', 'selp16', 'selp32',
    'setProfileCtl', 'set', 'shfl', 'simdbcast', 'simdbfly', 'simdfballot',
    'simdiballot', 'simdshffdn', 'simdshffup', 'simdshftdn', 'simdshftup',
    'sll', 'sllu', 'smp', 'specLM', 'specTPR', 'sra', 'srau', 'srl',
    'srlu', 'srrestore', 'srsave', 'stdev', 'stscs', 'sttg', 'sttps',
    'unpack', 'unpackhfp8', 'update_bvh', 'update_node', 'uvst', 'uvsw',
    'vseli32', 'yield', 'fcmp3', 'fsel3f16', 'fsel3f32', 'fsel3i16',
    'icmp3', 'isel3i16', 'isel3i32', 'bu', 'fGeMM16x8x16', 'fGeMM8x16x16',
    'iGeMM16x8x16', 'iGeMM8x16x16', 'atomic', 'atomictps', 'ld', 'st',
    'ldptr', 'f32ilogb', 'f32setexp', 'f32divroundstep1',
    'f32divroundstep2', 'f64add', 'f64mul', 'f64fma', 'f64cmp', 'f64rint',
    'f64div', 'f64sqrt', 'f64powr', 'f64setexp', 'f64ilogb', 'int2f64',
    'f642int', 'fGeMM', 'iGeMM', 'packflt', 'unpackflt', 'packint',
    'unpackint', 'initacc', 'ldacc', 'fgemm_dd', 'igemm_dd', 'fgemv_tt',
    'fgemv_dt', 'fgemv_td', 'fgemv_dd', 'igemv_tt', 'igemv_dt', 'igemv_td',
    'igemv_dd', 'fconv_dd', 'iconv_dd', 'allocate_mxu', 'deallocate_mxu',
    'fgemm_sg', 'igemm_sg', 'mxu_cfi', 'f32sincos_outcond',
    'f32sincos_rangesel'
)
assert len(OPCODE_NAMES) == 261

VARIANT = {'b': 0, 'c': 1, 'd': 2}

# opcode -> per-variant list of form root nodes
OPC_IDX, OPC_REC, OPC_MASK = 0x1de4104, 0x1ddf164, 0x1fff
# opcode -> per-variant (ep, size in bytes) list, in the encoder's try order
SIZE_IDX, SIZE_REC, SIZE_MASK = 0x1dded50, 0x1dddc60, 0x1fff
# node -> per-variant predicate stream (u16 units)
PRED_IDX, PRED_REC, PRED_MASK = 0x1fe8600, 0x1de4518, 0x1fffff
# node -> parent (u16 parent slot at +0 of a 4-byte entry; 0xffff = none)
PARENT_SLOT, PARENT_NODE = 0x227388c, 0x2253098
# node -> per-variant choice-group list
GRP_IDX, GRP_REC, GRP_MASK = 0x25a4544, 0x24de324, 0x3ffff
# group -> per-variant alternative list
ALT_IDX, ALT_REC, ALT_MASK = 0x2d2d97c, 0x280efdc, 0x1fffff
# node -> per-variant command stream
CMD_IDX, CMD_REC, CMD_MASK = 0x3785240, 0x2d72c78, 0x3fffff

CPU_TYPE_ARM64 = 0x0100000c
CPU_SUBTYPE_ARM64E = 2


def arm64_slice(data):
    """The arm64 (not arm64e) slice of a universal Mach-O, or data itself."""
    magic = struct.unpack_from('>I', data, 0)[0]
    if magic not in (0xcafebabe, 0xcafebabf):
        return data
    wide = magic == 0xcafebabf
    n = struct.unpack_from('>I', data, 4)[0]
    p = 8
    for _ in range(n):
        if wide:
            cpu, sub, off, size, _, _ = struct.unpack_from('>iiQQII', data, p)
            p += 32
        else:
            cpu, sub, off, size, _ = struct.unpack_from('>iiIII', data, p)
            p += 20
        if cpu == CPU_TYPE_ARM64 and (sub & 0xffffff) != CPU_SUBTYPE_ARM64E:
            return data[off:off + size]
    sys.exit('no arm64 slice in the dylib')


class AppleTables:
    """Reader for the llvm::AGX3 packed encoder tables."""

    def __init__(self, path):
        data = arm64_slice(open(path, 'rb').read())
        digest = hashlib.sha256(data).hexdigest()
        if digest != DYLIB_ARM64_SHA256:
            sys.exit('%s: arm64 slice has SHA-256 %s, expected %s; the table '
                     'addresses are only valid for that build' %
                     (path, digest, DYLIB_ARM64_SHA256))
        self.data = data

    def u8(self, a):
        return self.data[a]

    def u16(self, a):
        return struct.unpack_from('<H', self.data, a)[0]

    def u32(self, a):
        return struct.unpack_from('<I', self.data, a)[0]

    def u64(self, a):
        return struct.unpack_from('<Q', self.data, a)[0]

    def rec32(self, base, idx, var):
        r = base + idx * 4
        off, cnt = self.u32(r + 8 * var), self.u32(r + 8 * var + 4)
        s = r + off * 4 + 0x28
        return [self.u32(s + 4 * i) for i in range(cnt)]

    def opcode_forms(self, opc, var):
        idx = self.u32(OPC_IDX + 4 * opc) & OPC_MASK
        return self.rec32(OPC_REC, idx, var)

    def opcode_sizes(self, opc, var):
        idx = self.u32(SIZE_IDX + 4 * opc) & SIZE_MASK
        if idx == SIZE_MASK:
            return []
        r = SIZE_REC + idx
        off, cnt = self.u8(r + 2 * var), self.u8(r + 2 * var + 1)
        s = r + off + 0xa
        return [(self.u8(s + 2 * i), self.u8(s + 2 * i + 1))
                for i in range(cnt // 2)]

    def node_preds(self, node, var):
        u16 = self.u16
        idx = self.u32(PRED_IDX + 4 * node) & PRED_MASK
        if idx == PRED_MASK:
            return []
        r = PRED_REC + idx * 2
        off, cnt = u16(r + 4 * var), u16(r + 4 * var + 2)
        p = r + off * 2 + 0x14
        end = p + cnt * 2
        out = []
        while p < end:
            field, kind = u16(p), u16(p + 2)
            if kind == 0:
                out.append(('eq', field, u16(p + 4)))
                p += 6
            elif kind == 1:
                n = u16(p + 4)
                vals = tuple(u16(p + 6 + 2 * i) for i in range(n))
                out.append(('set', field, vals))
                p += 6 + 2 * n
            elif kind == 2:
                out.append(('range', field, u16(p + 4), u16(p + 6)))
                p += 8
            elif kind == 3:
                out.append(('bf', field, u16(p + 4), u16(p + 6), u16(p + 8)))
                p += 10
            elif kind == 4:
                q = p + 4
                vals = []
                for _ in range(2):
                    n = u16(q)
                    v = 0
                    for i in range(n):
                        v |= u16(q + 2 + 2 * i) << (8 * i)
                    vals.append(v)
                    q += 2 + 2 * n
                out.append(('range64', field, vals[0], vals[1]))
                p = q
            else:
                raise ValueError('predicate kind %d at 0x%x' % (kind, p))
        assert p == end, (hex(p), hex(end))
        return out

    def node_parent(self, node):
        slot = self.u16(PARENT_SLOT + 4 * node)
        if slot == 0xffff:
            return None
        return self.u32(PARENT_NODE + 4 * slot)

    def node_groups(self, node, var):
        idx = self.u32(GRP_IDX + 4 * node) & GRP_MASK
        if idx == GRP_MASK:
            return []
        return self.rec32(GRP_REC, idx, var)

    def group_alts(self, g, var):
        idx = self.u32(ALT_IDX + 4 * g) & ALT_MASK
        if idx == ALT_MASK:
            return None
        return self.rec32(ALT_REC, idx, var)

    def node_cmds(self, node, var):
        u32 = self.u32
        idx = self.u64(CMD_IDX + 8 * node) & CMD_MASK
        if idx == CMD_MASK:
            return []
        r = CMD_REC + idx * 4
        off, cnt = u32(r + 8 * var), u32(r + 8 * var + 4)
        p = r + off * 4 + 0x28
        end = p + cnt * 4
        out = []
        while p < end:
            src, dst, width, word, kind = [u32(p + 4 * i) for i in range(5)]
            if kind == 0:
                out.append(('field', u32(p + 20), word, dst, width, src))
                p += 24
            elif kind == 1:
                f, n = u32(p + 20), u32(p + 24)
                lut = tuple(u32(p + 28 + 4 * i) for i in range(n))
                out.append(('lut', f, word, dst, width, src, lut))
                p += 28 + 4 * n
            elif kind == 2:
                out.append(('const', u32(p + 20), word, dst, width, src))
                p += 24
            else:
                raise ValueError('command kind %d at 0x%x' % (kind, p))
        assert p == end, (hex(p), hex(end))
        return out


def fixeds_from(cmds):
    """Constant deposits merged per 16-bit word, sorted by word."""
    d = {}
    for c in cmds:
        if c[0] != 'const':
            continue
        _, val, word, dst, width, src = c
        m = (agx3_table.wmask(width) << dst) & 0xffff
        v = (((val >> src) & agx3_table.wmask(width)) << dst) & 0xffff
        d.setdefault(word, [0, 0])
        d[word][0] |= m
        d[word][1] |= v
    return tuple((w, m, v) for w, (m, v) in sorted(d.items()))


def generate(A, var):
    """The packgen.py layout: forms sorted by (mnemonic, ep); nodes emitted in
    post-order (a choice group's alternatives are emitted, and their altpool
    run appended, before the node that owns the group); the ancestor chain of
    a form's root folded into the form node; identical LUTs and sets
    shared."""
    forms = []
    for opc in range(len(OPCODE_NAMES)):
        roots = A.opcode_forms(opc, var)
        if not roots:
            continue
        sizes = dict(A.opcode_sizes(opc, var))
        for ep, root in enumerate(roots):
            forms.append((OPCODE_NAMES[opc], ep, sizes[ep], root))
    forms.sort(key=lambda f: (f[0], f[1]))

    fixeds, preds, cmds, groups, nodes = [], [], [], [], []
    altpool, lutpool, setpool = [], [], []
    lutidx, setidx = {}, {}

    def lut_off(lut):
        if lut not in lutidx:
            lutidx[lut] = len(lutpool)
            lutpool.extend(lut)
        return lutidx[lut]

    def set_off(vals):
        if vals not in setidx:
            setidx[vals] = len(setpool)
            setpool.extend(vals)
        return setidx[vals]

    def emit(n, extra=()):
        grp = []
        for g in A.node_groups(n, var):
            ids = [emit(a) for a in A.group_alts(g, var)]
            grp.append((len(altpool), len(ids)))
            altpool.extend(ids)

        cm = list(extra) + A.node_cmds(n, var)
        fx = fixeds_from(cm)
        rec = [len(fixeds), len(fx)]
        fixeds.extend(fx)

        pr = []
        for p in A.node_preds(n, var):
            k = p[0]
            if k == 'eq':
                pr.append((p[1], 0, 0, 0, p[2], 0))
            elif k == 'set':
                pr.append((p[1], 1, 0, 0, set_off(p[2]), len(p[2])))
            elif k in ('range', 'range64'):
                pr.append((p[1], 2, 0, 0, p[2], p[3]))
            elif k == 'bf':
                pr.append((p[1], 3, p[2], p[3], p[4], 0))
            else:
                raise ValueError(p)
        rec += [len(preds), len(pr)]
        preds.extend(pr)

        cc = []
        for c in cm:
            if c[0] == 'field':
                _, f, word, dst, width, src = c
                cc.append((f, word, dst, width, src, -1, 0))
            elif c[0] == 'lut':
                _, f, word, dst, width, src, lut = c
                cc.append((f, word, dst, width, src, lut_off(lut), len(lut)))
        rec += [len(cmds), len(cc)]
        cmds.extend(cc)

        rec += [len(groups), len(grp)]
        groups.extend(grp)
        nodes.append(tuple(rec))
        return len(nodes) - 1

    out_forms = []
    for name, ep, size, root in forms:
        chain = []
        a = A.node_parent(root)
        while a is not None:
            chain.append(a)
            a = A.node_parent(a)
        extra = []
        for a in reversed(chain):
            extra += A.node_cmds(a, var)
        out_forms.append((name, ep, size, emit(root, tuple(extra))))

    return dict(fixeds=fixeds, preds=preds, cmds=cmds, groups=groups,
                nodes=nodes, forms=out_forms, altpool=altpool,
                lutpool=lutpool, setpool=setpool)


def render_tables(T, letter):
    L = []
    if letter in 'bc':
        # The header packgen.py wrote; kept so these reproduce byte-exactly.
        L.append('/* generated by packgen.py from agx3%s_fields_composed.json;\n'
                 '   do not edit.  See agx3_compose.h for the consumer. */'
                 % letter)
    else:
        L.append('/* generated by gen_agx3_tables.py from variant %d '
                 '("g18g-encodings")\n'
                 '   of Apple\'s llvm::AGX3 packed encoder tables in '
                 'libapplegpu-nt.dylib\n'
                 '   (Metal toolchain 32023), in the packgen.py layout; the '
                 'same generator\n'
                 '   reproduces agx3b_tables.c and agx3c_tables.c '
                 'byte-exactly from variants\n'
                 '   0 and 1.  Do not edit; regenerate as README.md says.  '
                 'See\n'
                 '   agx3_compose.h for the consumer. */' % VARIANT[letter])
    L.append('#include "agx3_compose.h"')

    def arr(decl, rows):
        L.append(decl + ' = {')
        L.append(',\n'.join(rows))
        L.append('};')

    arr('static const agx3_fixed fixeds[]',
        ['{%d,0x%x,0x%x}' % f for f in T['fixeds']])
    arr('static const agx3_pred preds[]',
        ['{%d,%d,%d,%d,0x%xull,0x%xull}' % p for p in T['preds']])
    arr('static const agx3_cmd cmds[]',
        ['{%d,%d,%d,%d,%d,%d,%d}' % c for c in T['cmds']])
    arr('static const agx3_group groups[]',
        ['{%d,%d}' % g for g in T['groups']])
    arr('static const agx3_node nodes[]',
        ['{%d,%d,%d,%d,%d,%d,%d,%d}' % n for n in T['nodes']])
    arr('static const agx3_form forms[]',
        ['{"%s",%d,%d,%d}' % f for f in T['forms']])
    arr('static const uint32_t altpool[]',
        [','.join('%d' % x for x in T['altpool'])])
    arr('static const uint16_t lutpool[]',
        [','.join('%d' % x for x in T['lutpool'])])
    arr('static const uint64_t setpool[]',
        [','.join('0x%xull' % x for x in T['setpool'])])
    L.append('const agx3_table agx3%s_table = {' % letter)
    L.append('  forms, %d, nodes, groups, altpool, fixeds, preds, cmds, '
             'lutpool, setpool' % len(T['forms']))
    L.append('};')
    return '\n'.join(L) + '\n'


XML_HEADER = """<!--
  AGX3d.xml : Apple G18 (M6) native GPU instruction set,
  encoding group A3-d == Apple encoder GPU variant 2.
  Applies to: applegpu_g18g, g18m, and g19p (subtarget feature
  g18g-encodings; g17g, g17s, g17d and g18p stay on variant 1, AGX3c.xml).

  Transcribed mechanically from the llvm::AGX3 packed encoder tables
  inside Apple's MetalToolchain libapplegpu-nt.dylib (toolchain 32023),
  with the transcription that reproduces every AGX3c.xml instruction from
  variant 1.  Mnemonics are Apple's own: variant 2 adds f32fastdivstep2,
  f32ilogb, f32setexp, f32divroundstep1/2, fcmp3, icmp3, fsel3f16/f32/i16,
  isel3i16/i32, bu, atomic, atomictps, ld, st, ldptr and the 16x8x16 and
  8x16x16 GeMM shapes, and drops f32div.

  Same grammar as Mesa src/asahi/isa/AGX2.xml.  Field names are Apple's
  numeric field ids (fNN); their semantic names are not in the binary.
  Only a form's own constant bits and field deposits are transcribed; bits
  set by its choice groups decode as unknown bits.

  Generated by src/asahi/compiler/agx3_compose/gen_agx3_tables.py xml
  from agx3d_tables.c; do not edit, regenerate (see that directory's
  README.md).
-->
<isa>
"""


def bit_ranges(bits):
    out = []
    for b in bits:
        if out and out[-1][1] + 1 == b:
            out[-1][1] = b
        else:
            out.append([b, b])
    return ' '.join('%d' % a if a == b else '%d:%d' % (a, b) for a, b in out)


def pred_text(p):
    f, k, _, _, a, _ = p
    if k == 0:
        return 'f%deq%d' % (f, a)
    if k == 1:
        return 'f%dset' % f
    if k == 2:
        return 'f%drange' % f
    if k == 3:
        return 'f%dbitfield_eq%d' % (f, a)
    raise ValueError(p)


def ins_xml(T, form_index, name):
    """One <ins> per form. The form node's own constant deposits become
    <exact>, its field deposits become <immediate>s (field order, deposit
    order within a field, contiguous runs merged; a placement onto an
    already placed bit is dropped), and its predicates are recorded in a
    comment. Choice groups are not transcribed."""
    mnem, _, size, nid = T['forms'][form_index]
    fo, fn, po, pn, co, cn, _, _ = T['nodes'][nid]
    L = ['  <ins name="%s" length="%d" display="%s">' % (name, size, mnem)]

    bits = []
    for w, m, v in T['fixeds'][fo:fo + fn]:
        for i in range(16):
            if m >> i & 1:
                bits.append((16 * w + i, v >> i & 1))
    if bits:
        L.append('    <exact bit="%s">%s</exact>' %
                 (bit_ranges([b for b, _ in bits]),
                  ''.join(str(x) for _, x in reversed(bits))))

    fields = {}
    for c in T['cmds'][co:co + cn]:
        fields.setdefault(c[0], []).append(c)
    for f in sorted(fields):
        placed, dropped, lut = [], False, None
        for _, word, dst, width, _, lo, ln in fields[f]:
            for i in range(width):
                b = 16 * word + dst + i
                if b in placed:
                    dropped = True
                else:
                    placed.append(b)
            if lo >= 0 and lut is None:
                lut = T['lutpool'][lo:lo + ln]
        line = '    <immediate name="f%d" bit="%s"/>' % (f, bit_ranges(placed))
        if lut is not None:
            line += ('  <!-- value passes through an encoder LUT %s -->'
                     % list(lut))
        elif dropped:
            line += '  <!-- overlapping placements dropped -->'
        L.append(line)

    if pn:
        L.append('    <!-- select when: %s -->' %
                 ' '.join(pred_text(p) for p in T['preds'][po:po + pn]))
    L.append('  </ins>')
    return '\n'.join(L)


def render_xml(T):
    count = {}
    for m, _, _, _ in T['forms']:
        count[m] = count.get(m, 0) + 1
    body = []
    for i, (m, ep, _, _) in enumerate(T['forms']):
        name = m if count[m] == 1 else '%s_ep%d' % (m, ep)
        body.append(ins_xml(T, i, name))
    return XML_HEADER + '\n'.join(body) + '\n</isa>\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    t = sub.add_parser('tables', help='dylib -> agx3{b,c,d}_tables.c')
    t.add_argument('dylib')
    t.add_argument('letter', choices=sorted(VARIANT))
    t.add_argument('output')
    x = sub.add_parser('xml', help='agx3d_tables.c -> AGX3d.xml')
    x.add_argument('tables')
    x.add_argument('output')
    args = ap.parse_args()

    if args.cmd == 'tables':
        A = AppleTables(args.dylib)
        T = generate(A, VARIANT[args.letter])
        text = render_tables(T, args.letter)
    else:
        text = render_xml(agx3_table.load(args.tables))

    with open(args.output, 'w') as f:
        f.write(text)


if __name__ == '__main__':
    main()
