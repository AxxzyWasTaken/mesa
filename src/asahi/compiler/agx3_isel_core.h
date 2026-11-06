/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * AGX3 `isel` -- the fused integer compare-and-select.
 *
 * AGX3 keeps AGX2's fused compare-select: one instruction compares two
 * registers under a condition and returns one of two further operands. That
 * is Mesa's AGX_OPCODE_ICMPSEL exactly, which matters because
 * agx_lower_pseudo() turns *every* AGX_OPCODE_ICMP into an icmpsel -- without
 * this encoder no shader containing any integer comparison can be compiled.
 *
 * The GPU0 forms below are probe-witnessed. GPU1 additionally has a separate
 * 32-bit iseli32 encoder derived from the AGX3c tables; its field mapping and
 * encoding regression do not by themselves establish hardware correctness.
 * A ladder of 12 MSL
 * kernels differing only in the comparison operator was compiled by Apple's
 * own offline translator (`metal-nt -arch applegpu_g16g`, the AGX3b encoding
 * G17P uses) and decoded through Apple's own encoder tables. The condition
 * field f106 tracked the operator one-for-one and turned out to be numbered
 * exactly like `enum agx_icond`:
 *
 *     ult = 1   ugt = 2   eq = 4   slt = 5   sgt = 6
 *
 * which is the same numbering agx3_cf_core.c already carries for `icmp`'s
 * f42. There is no le/ge/ne condition and no invert bit: Apple emits the
 * inverse condition with the two selected operands swapped, which is also how
 * Mesa spells them, so no translation is needed. Condition 3 is witnessed in
 * Apple's corpus with unknown meaning and is refused here, as in the CF core.
 *
 * Each selected operand is independently a register or an 8-bit immediate,
 * and the two kinds use different value fields, which the probes pin
 * separately:
 *
 *   both registers   ep27  kind 3, values f66 / f83
 *   both immediates  ep27  kind 1, values f64 / f81   (ep25 when 16-bit)
 *   register + imm   ep27  kind 2 register in f65, immediate in f81
 *   imm + register   ep27  kind 1 immediate in f64, register in f82
 *
 * The mixed register fields f65/f82 were pinned by a kernel containing two
 * such selects over different registers: only those fields tracked the two
 * register numbers. Every shape below reproduces its witnessed instruction
 * byte-for-byte. Both widths support all four selected-operand shapes.
 *
 * Scoreboard waits ride on fields f98..f103, one per slot 1..6. That is
 * probe-derived at both ends: a select consuming a load that signalled slot 6
 * sets f103, and one consuming a load that signalled slot 1 sets f98.
 *
 * The production explicit-hint form uses kind-2 register operands and actual
 * post-RA cache/discard bits. Fresh G17P uint/ushort dead/live witnesses pin
 * comparator fields f44/f46 and f59/f61, and selection fields f74/f76 and
 * f91/f93. Native 16-bit signed comparisons/equality carry f47/f62 on register
 * comparands, matching the signed short and unsigned equality witnesses.
 * A right-hand u8 immediate uses f49/kind1; the packer reverses comparisons
 * with an immediate on the left. Signed/unsigned 16-bit and u8-comparand forms stay
 * fused, without temporary register materialization.
 *
 * f12 is destination cache. A fresh direct-store/continued-use pair changes
 * only destination register allocation and f12; the continued-use form sets
 * it for a 16-bit boolean result even though both selected values are
 * immediates. The compact native wait selector represents one slot or slots1+2;
 * other producer combinations must be retired separately before this form.
 *
 * GPU1 (applegpu_g17g) witnesses from gpu/m5-xfb-alu8-reference.metal pin a
 * ushort comparand against the immediate 2 with a word result -- selecting a
 * register or zero, and two registers -- plus the uint-comparand twin. All
 * three store their result directly and carry f11=0 whatever the selected
 * operands, so on GPU1 f11 is the destination-cache hint (as in every other
 * GPU1 form with the f9/f10/f2/f11 header), not a mixed-selection flag.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "asahi/lib/agx_chip.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AGX3_ISEL_MAX_LENGTH 14u

/* == enum agx_icond. Only the five Apple emits are encodable. */
enum agx3_isel_icond {
   AGX3_ISEL_ICOND_ULT = 1,
   AGX3_ISEL_ICOND_UGT = 2,
   AGX3_ISEL_ICOND_EQ = 4,
   AGX3_ISEL_ICOND_SLT = 5,
   AGX3_ISEL_ICOND_SGT = 6,
};

enum agx3_isel_operand {
   AGX3_ISEL_REGISTER = 0,
   AGX3_ISEL_IMMEDIATE = 1,
   /* An aligned 32-bit uniform (f66/f83 with class f72/f89 = 3). Only
    * encoded through the explicit-hint path, beside register comparands. */
   AGX3_ISEL_UNIFORM = 2,
};

struct agx3_isel_desc {
   uint16_t dest;      /* half-register units */
   bool dest_16;       /* 16-bit result, including odd destination halves */
   uint16_t cmp0;      /* register half, compared first */
   uint16_t cmp1;      /* register half or unsigned8 immediate */
   enum agx3_isel_icond cond;

   /* Native unsigned register-versus-u8 comparison. The four register
    * hints follow Mesa source order: both comparands, true, false; immediate
    * selected operands have no hint. Existing register/register forms retain
    * their separate native encoding. */
   bool cmp1_immediate;
   /* An aligned 32-bit uniform comparand (cmp0: f36 with f42 = 3; cmp1: f51
    * with f57 = 3), witnessed only against a 32-bit register comparand and
    * register/immediate selects. */
   bool cmp0_uniform;
   bool cmp1_uniform;
   bool cmp_16;

   /* G18G only (agx3_isel_encode_g18g): GPU1 widens a half-register second
    * comparand against a full-register first comparand inside the select:
    * f39 takes the immediate/narrow value and f43 selects sign extension.
    * Witnessed on applegpu_g17g from `uint >= uint(ushort)` (zero extension)
    * and `int < int(short)` (sign extension); Apple puts the wide comparand
    * first and reverses the condition when the source had them the other
    * way round. agx3_isel_encode refuses both. */
   bool cmp1_16;
   bool cmp1_sign_extend;
   bool cmp_mixed_width; /* M5: the second register has the opposite width. */
   bool explicit_hints;
   /* The result is consumed through the register cache: f12 on GPU0, f11 on
    * GPU1. */
   bool dest_cache;
   bool cache[4];
   bool discard[4];

   /* The selected operands: each a 32-bit register half or an 8-bit
    * immediate. sel_true is taken when (cmp0 cond cmp1) holds. */
   enum agx3_isel_operand true_kind;
   enum agx3_isel_operand false_kind;
   uint16_t sel_true;
   uint16_t sel_false;

   /* Bit i waits for scoreboard slot i + 1, as elsewhere in this backend. */
   uint16_t wait_mask;
};

bool agx3_isel_encode(enum agx_isa_encoding encoding,
                      const struct agx3_isel_desc *desc,
                      uint8_t out[AGX3_ISEL_MAX_LENGTH], uint8_t *len);

/* G18G (M6) compiles as agx3c: the GPU1 integer select with cmp1_16 and
 * cmp1_sign_extend; uniform and cmp_mixed_width descriptors and every other
 * encoding go through agx3_isel_encode. */
bool agx3_isel_encode_g18g(enum agx_isa_encoding encoding,
                           const struct agx3_isel_desc *desc,
                           uint8_t out[AGX3_ISEL_MAX_LENGTH], uint8_t *len);

/* G17P condition 3 is a nonzero mask test. Fresh mask1/2/4 witnesses pin
 * f49 and f106; selected-negation contrasts pin f78/f95. The quadrant
 * lowering uses these fused selects, not a branch or a separate bit test. */
struct agx3_bittest_desc {
   uint16_t dest;
   uint16_t src[3]; /* bits, true float, false float */
   uint8_t mask;
   bool negate[2];
   bool cache[3];
   bool discard[3];
   uint16_t wait_mask;
};

bool agx3_bittest_encode(enum agx_isa_encoding encoding,
                        const struct agx3_bittest_desc *desc,
                        uint8_t out[AGX3_ISEL_MAX_LENGTH], uint8_t *len);

#ifdef __cplusplus
}
#endif
