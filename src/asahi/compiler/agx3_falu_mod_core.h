/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * AGX3 f32 ALU with source/output modifiers (abs, neg, saturate).
 *
 * The grade-A no-modifier fadd/fmul/ffma are the compact hand-written packers.
 * When a modifier is present the compact forms cannot express it, so these
 * encoders emit the always-capable long form through the composer: both
 * commutative source slots (f35, f18) carry independent abs/neg bits, the ffma
 * addend (f52) carries its own, and f14 saturates the result. There is no slot
 * swap in those legacy interfaces. The runtime-aware ADD additionally tries
 * the equivalent commuted order, moving both modifiers and lifetime hints,
 * to keep subtraction in the shortest native form.
 *
 * All 96 witnessed Apple encodings are reproduced byte-exactly from the
 * baseline, the operands and the modifiers.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "asahi/lib/agx_chip.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A slot's WIDTH is a field of the same encoding point as its modifier bits,
 * not a different instruction: f43 belongs to the f35 slot exactly as f47 and
 * f48 do, f26 to the f18 slot alongside f30 and f31, and f60 to the ffma
 * addend alongside f64 and f65. Apple composes every combination. One kernel
 * per cell, `float a`, `half b`, recovered through tools/decomp.c:
 *
 *    a + b        09011c0400c0   f35=4 f43=1  f18=0 f26=2
 *    a - b        09011c0400c8   the same, plus f48
 *    b - a        09041c0100c8   f35=0 f43=2  f18=4 f26=1, plus f48
 *    a + fabs(b)  09011c04020000800200   f47
 *    fabs(a) + b  09011c04020000800100   f30
 *    b - fabs(a)  09041c01020800800200   f47 and f48 together
 *   -a - b        09041c0101080280       f48 and f31 together
 *    b * -a       09041d0100c8           the same pair on f32mul
 *    fma(a,c,h)   09011e05810800c0       f52=8 f60=1, the addend narrow
 *    fma(a,c,-h)  09011e05910800c0       the same, plus f65
 *    fma(h,c,a)   09081e01810402c0       f18=8 f26=1, the multiplicand narrow
 *
 * so the width axis and the modifier axis are independent. Keeping them in
 * separate encoders is what refused `half + (-float)`: it is on both axes and
 * had no home in either.
 */
struct agx3_falu_mods {
   bool src0_abs; /* f47 on the f35 slot */
   bool src0_neg; /* f48 */
   bool src0_16;  /* f43 */
   bool src1_abs; /* f30 on the f18 slot */
   bool src1_neg; /* f31 */
   bool src1_16;  /* f26 */
   bool addend_abs; /* f64, ffma only */
   bool addend_neg; /* f65, ffma only */
   bool addend_16;  /* f60, ffma only */
   bool saturate;   /* f14 */
};

#define AGX3_FALU_MOD_MAX_LENGTH 16u

/* dest = sat?(srcmods(src0) OP srcmods(src1)). abs applies before neg. */
bool agx3_falu_add_mod(enum agx_isa_encoding enc, uint16_t dest, uint16_t src0,
                       uint16_t src1, const struct agx3_falu_mods *m,
                       uint8_t out[AGX3_FALU_MOD_MAX_LENGTH], uint8_t *len);
bool agx3_falu_mul_mod(enum agx_isa_encoding enc, uint16_t dest, uint16_t src0,
                       uint16_t src1, const struct agx3_falu_mods *m,
                       uint8_t out[AGX3_FALU_MOD_MAX_LENGTH], uint8_t *len);
/* dest = sat?(srcmods(src0) * srcmods(src1) + addendmods(addend)). */
bool agx3_falu_fma_mod(enum agx_isa_encoding enc, uint16_t dest, uint16_t src0,
                       uint16_t src1, uint16_t addend,
                       const struct agx3_falu_mods *m,
                       uint8_t out[AGX3_FALU_MOD_MAX_LENGTH], uint8_t *len);

/* Actual post-RA hints, in API source order; no specimen lifetime defaults. */
struct agx3_falu_runtime {
   bool cache[3];
   bool discard[3];
   bool dest_cache;
   uint8_t wait_mask;
};

bool agx3_falu_add_mod_runtime(enum agx_isa_encoding enc, uint16_t dest,
                              uint16_t src0, uint16_t src1,
                              const struct agx3_falu_mods *m,
                              const struct agx3_falu_runtime *runtime,
                              uint8_t out[AGX3_FALU_MOD_MAX_LENGTH],
                              uint8_t *len);

/* G17G only: dest = srcmods(src0) + minifloat. The same long form with the
 * immediate-operand alternative (f25=1, the constant in f17). Witnessed
 * byte-exactly against applegpu_g17g (gpu/m5-xfb-alu4-reference.metal):
 *
 *   a + 2.0         29 00 04 0a 22 20 ac 02 01 08 11 00   f17 = 0x40
 *   fabs(a) + 0.5   29 00 04 4a 22 20 ac 02 01 08 10 40   f17 = 0x20, f47
 *   0.5 - a         29 00 24 4a 22 20 ac 02 01 08 10 00   f48
 *   -fabs(a) + 0.5  29 00 24 4a 22 20 ac 02 01 08 10 40   f47, f48
 *
 * all with a dead source waiting for the load in slot 8. These are the forms
 * Mesa's fabs and fneg take (fmov.abs/neg lower to `fadd mod(x), -0.0`). A
 * saturated variant has no witness and fails closed. */
bool agx3_falu_add_imm_runtime(uint16_t dest, uint16_t src0,
                               uint8_t minifloat,
                               const struct agx3_falu_mods *m,
                               const struct agx3_falu_runtime *runtime,
                               uint8_t out[AGX3_FALU_MOD_MAX_LENGTH],
                               uint8_t *len);

/* G17G only: dest = srcmods(src0) * minifloat, the long f32mul with the
 * immediate-operand alternative. Apple folds a negated source into the
 * constant's sign, so only the absolute value is a source modifier here
 * (gpu/m5-xfb-alu4-reference.metal, -alu6-):
 *
 *   a * 2.0          29 00 05 10 22 80 ac 02 0b 00 20 00 00 10   f17 = 0x40
 *   fabs(a) * 2.0    29 00 05 10 22 80 ac 02 0b 00 20 08 00 10   f47
 *   -fabs(a) * 2.0   29 00 05 10 22 80 ad 02 0b 00 20 08 00 10   f17 = 0xc0
 *
 * all with a dead source waiting for the load in slot 8. */
bool agx3_falu_mul_imm_runtime(uint16_t dest, uint16_t src0,
                               uint8_t minifloat,
                               const struct agx3_falu_mods *m,
                               const struct agx3_falu_runtime *runtime,
                               uint8_t out[AGX3_FALU_MOD_MAX_LENGTH],
                               uint8_t *len);

bool agx3_falu_mul_mod_runtime(enum agx_isa_encoding enc, uint16_t dest,
                              uint16_t src0, uint16_t src1,
                              const struct agx3_falu_mods *m,
                              const struct agx3_falu_runtime *runtime,
                              uint8_t out[AGX3_FALU_MOD_MAX_LENGTH],
                              uint8_t *len);

/* f32 to f16 conversion, including odd destination halves. Runtime source0
 * is the sole real operand; the other input is exact negative zero. */
bool agx3_falu_narrow_f32(enum agx_isa_encoding enc, uint16_t dest, uint16_t src,
                         bool saturate, const struct agx3_falu_runtime *runtime,
                         uint8_t out[AGX3_FALU_MOD_MAX_LENGTH], uint8_t *len);

/* minifloat - src, narrowed to a half. Runtime source0 describes src. */
bool agx3_falu_narrow_imm_sub_f32(enum agx_isa_encoding enc, uint16_t dest,
                                 uint8_t minifloat, uint16_t src,
                                 const struct agx3_falu_runtime *runtime,
                                 uint8_t out[AGX3_FALU_MOD_MAX_LENGTH],
                                 uint8_t *len);

bool agx3_falu_fma_mod_runtime(enum agx_isa_encoding enc, uint16_t dest,
                              uint16_t src0, uint16_t src1, uint16_t addend,
                              const struct agx3_falu_mods *m,
                              const struct agx3_falu_runtime *runtime,
                              uint8_t out[AGX3_FALU_MOD_MAX_LENGTH],
                              uint8_t *len);

/* Native add/multiply with a single rounding to a scalar half result. */
bool agx3_falu_add_narrow_m5(uint16_t dest, uint16_t src0, uint16_t src1,
                            const struct agx3_falu_mods *mods,
                            const struct agx3_falu_runtime *runtime,
                            uint8_t out[AGX3_FALU_MOD_MAX_LENGTH], uint8_t *len);
bool agx3_falu_mul_narrow_m5(uint16_t dest, uint16_t src0, uint16_t src1,
                            const struct agx3_falu_mods *mods,
                            const struct agx3_falu_runtime *runtime,
                            uint8_t out[AGX3_FALU_MOD_MAX_LENGTH], uint8_t *len);

/* Native F32 arithmetic with one final rounding directly to a half result. */
bool agx3_falu_fma_narrow_m5(uint16_t dest, uint16_t src0, uint16_t src1,
                            uint16_t addend, const struct agx3_falu_mods *m,
                            const struct agx3_falu_runtime *runtime,
                            uint8_t out[AGX3_FALU_MOD_MAX_LENGTH], uint8_t *len);

/* C FMA32 also consumes half GPRs with an immediate addend. Preserve the
 * independent widths, modifiers, lifetime and saturation in one instruction. */
bool agx3_falu_fma_addend_imm_width_mod(
   enum agx_isa_encoding enc, uint16_t dest, uint16_t src0, uint16_t src1,
   uint8_t immediate, const struct agx3_falu_mods *m,
   const struct agx3_falu_runtime *runtime,
   uint8_t out[AGX3_FALU_MOD_MAX_LENGTH], uint8_t *len);

bool agx3_falu_fma_addend_imm_mod(
   enum agx_isa_encoding enc, uint16_t dest, uint16_t src0, uint16_t src1,
   uint8_t immediate, bool src0_abs, bool src0_neg, bool src1_abs,
   bool src1_neg,
   const struct agx3_falu_runtime *runtime,
   uint8_t out[AGX3_FALU_MOD_MAX_LENGTH], uint8_t *len);

bool agx3_falu_fma_two_imm(enum agx_isa_encoding enc, uint16_t dest,
                          uint16_t src, uint8_t multiplier, uint8_t addend,
                          const struct agx3_falu_runtime *runtime,
                          uint8_t out[AGX3_FALU_MOD_MAX_LENGTH], uint8_t *len);

bool agx3_falu_fma_two_imm_narrow_m5(
   uint16_t dest, uint16_t src, uint8_t multiplier, uint8_t addend,
   bool src_abs, bool saturate, const struct agx3_falu_runtime *runtime,
   uint8_t out[AGX3_FALU_MOD_MAX_LENGTH], uint8_t *len);

/*
 * FMA whose MULTIPLIER is a minifloat immediate while the multiplicand and the
 * addend stay registers that carry their own modifiers and widths.
 *
 * Operand KIND is a field of the same encoding point as the modifier bits,
 * exactly as width is: f42 selects between "the f35 register slot" (2) and
 * "the f34 immediate" (1), and it sits alongside f30/f31 on the f18 slot and
 * f64/f65 on the f52 addend without disturbing either.  Modelling kind and
 * modifier as separate encoders is what refused `k * fabs(a) + fabs(b)` --
 * glmark2's terrain noise kernel, `1.0 - abs(x) - abs(y)` -- which stands on
 * both axes at once and had no home in either.  Apple composes the cells, one
 * kernel per cell (`fmaimm.metal`, decoded with tools/decomp.c):
 *
 *    fma(a,b,c)            09011e05810802c0      f42=2 f35=4
 *    fma(0.5,b,c)          09010ea1810402c2      f42=1 f34=32
 *    fma(0.5,fabs(b),c)    09010ea18304020200800100   plus f30
 *    fma(0.5,fabs(b),      09010ea18b04020200800100   plus f30 and f64
 *        fabs(c))
 *    fma(0.5,-b,c)         09011ea1810402c2      f34=160: a negated
 *                                                immediate multiplier is the
 *                                                minifloat's own sign bit,
 *                                                not a modifier bit
 *    fma(0.5,fabs(b),      09010ea19b04020200800100   plus f30, f64 and f65
 *        -fabs(c))
 *
 * `m->src0_*` are therefore not accepted here: fold the multiplier's sign into
 * `multiplier` the way Apple does.
 */
bool agx3_falu_fma_mul_imm_mod(enum agx_isa_encoding enc, uint16_t dest,
                              uint8_t multiplier, uint16_t src1,
                              uint16_t addend,
                              const struct agx3_falu_mods *m,
                              const struct agx3_falu_runtime *runtime,
                              uint8_t out[AGX3_FALU_MOD_MAX_LENGTH],
                              uint8_t *len);
/* The same G17G form narrowing its result into a scalar half (f10=1). */
bool agx3_falu_fma_mul_imm_narrow_m5(uint16_t dest, uint8_t multiplier,
                                     uint16_t src1, uint16_t addend,
                                     const struct agx3_falu_mods *m,
                                     const struct agx3_falu_runtime *runtime,
                                     uint8_t out[AGX3_FALU_MOD_MAX_LENGTH],
                                     uint8_t *len);

bool agx3_falu_add_imm_m5(uint16_t dest, uint16_t src, uint8_t immediate,
                         bool src_abs, bool src_neg, bool saturate,
                         const struct agx3_falu_runtime *runtime,
                         uint8_t out[AGX3_FALU_MOD_MAX_LENGTH], uint8_t *len);

bool agx3_falu_add_imm_narrow_m5(uint16_t dest, uint16_t src, uint8_t immediate,
   bool src_abs, bool src_neg, bool saturate,
   const struct agx3_falu_runtime *runtime,
   uint8_t out[AGX3_FALU_MOD_MAX_LENGTH], uint8_t *len);
bool agx3_falu_fma_addend_imm_narrow_m5(uint16_t dest, uint16_t src0, uint16_t src1,
   uint8_t immediate, const struct agx3_falu_mods *mods,
   const struct agx3_falu_runtime *runtime,
   uint8_t out[AGX3_FALU_MOD_MAX_LENGTH], uint8_t *len);

bool agx3_falu_fma_addend_imm(enum agx_isa_encoding enc, uint16_t dest,
                             uint16_t src0, uint16_t src1, uint8_t immediate,
                             const struct agx3_falu_runtime *runtime,
                             uint8_t out[AGX3_FALU_MOD_MAX_LENGTH], uint8_t *len);

#ifdef __cplusplus
}
#endif
