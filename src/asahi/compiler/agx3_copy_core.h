/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * AGX3 `copy` -- the register-file move.
 *
 * AGX3 operands carry a kind selector: 1 = immediate, 2 = register, 3 =
 * uniform, with the operand value deposited in a different field per kind
 * (slot A: f15 immediate / f16 register / f17 uniform; slot B: f31 register /
 * f32 uniform).  That mapping is not a guess: it is read straight off Apple's
 * own instructions, where `lddev` selects a uniform base with f38 = 3 + f32,
 * `iadd` adds a uniform with f38 = 3 + f32, and 49 of the 100 G16G `copy`
 * instructions move a uniform into a register with f23 = 3 + f17.
 *
 * A uniform therefore reaches a GPR on AGX3 exactly the way Apple's compiler
 * puts it there: one 4-byte `copy` (ep0), 32-bit, no wait bits.  Mesa's
 * agx_lower_uniform_sources() already materializes such a move for every
 * generic-ALU uniform source on AGX3; without this encoder that move lowers
 * to a `bitop` whose source is still a uniform, which the packer refuses --
 * so every shader that reads a pushed uniform in an ALU op fails to compile.
 *
 * Only the witnessed form is encoded.  Everything else (16- and 64-bit
 * widths, modifiers, wait masks, the register-source `copy`) fails closed.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "asahi/lib/agx_chip.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ordinary COPY is four bytes. Extended forms need up to ten bytes. */
#define AGX3_COPY_MAX_LENGTH 10u

/* The compact COPY's destination field (f2) is seven bits wide at EVERY
 * width, so no four-byte copy can name a destination at or above this half
 * index. G17P half copies refuse there; G17S uses its eight-byte ep1 half
 * copy, and the 32-bit uniform form uses ten-byte ep1. Register allocation
 * uses this boundary for G17P half-copy restrictions. The SOURCE fields are not
 * narrow this way: Apple's own ep0 copies carry sources well past it. */
#define AGX3_COPY_REG16_DEST_HALVES 128u

/*
 * Encode `copy dest32, uniform32`.  Both indices are 16-bit half-register
 * units, the unit agx_index.value already carries.  Returns false -- writing
 * nothing -- for an unknown encoding, a misaligned or out-of-range index, or
 * any value the composer refuses.
 *
 * Only the DESTINATION is narrow in the compact four-byte point: it lives in
 * a seven-bit f2 and truncates at half 127, while the uniform source spans
 * the whole 512-half file there. Destinations at or above half 128 move to
 * the ten-byte ep1, exactly where Apple's own compiler moves them; see the
 * G17P witnesses in agx3_copy_core.c.
 */
/*
 * Encode `copy dest16, src16` -- a 16-bit register-to-register move. Mesa
 * reaches this through the same bitop-with-MOV-truth-table lowering, but the
 * 32-bit `iadd d, s, #0` trick the select core uses is grounded only at 32
 * bits, and booleans live in 16-bit halves. Apple's own corpus carries this
 * form (copy.ep0 with a register source and f24 = 1), including odd
 * destination halves. G17S destinations from half 128 use ep1 to preserve
 * the high destination bit rather than truncating it in compact ep0.
 *
 * Scoreboard waits ride on f30 + slot - 1, the same convention
 * agx3_tex_stage_copy() documents; a corpus copy with f30 set reproduces
 * byte-exactly through that field.
 */
bool agx3_copy_encode_reg16(enum agx_isa_encoding encoding, uint16_t dest_half,
                            uint16_t src_half, bool dest_cache, bool src_cache,
                            bool src_discard, uint16_t wait_mask,
                            uint8_t out[AGX3_COPY_MAX_LENGTH], uint8_t *len);

bool agx3_copy_encode_reg32(enum agx_isa_encoding encoding, uint16_t dest_half,
                            uint16_t src_half, bool dest_cache, bool src_cache,
                            bool src_discard, uint16_t wait_mask,
                            uint8_t out[AGX3_COPY_MAX_LENGTH], uint8_t *len);

/*
 * Encode `copy uniform, reg32` -- the preamble's uniform write, which is what
 * Mesa calls uniform_store.
 *
 * Apple's own constant program (the `_agc.main.constant_program` half of every
 * shader Mach-O) writes its uniforms with an ordinary `copy` whose f9 is 3
 * rather than 2, moving the destination from f2 into f3 and making it a
 * UNIFORM half. Four consecutive writes to uniform halves 20, 22, 24 and 26
 * witness it byte-exactly. Destinations through half 126 use that four-byte
 * ep0; higher aligned pairs use the corpus's ten-byte ep1 form, whose f39
 * suffix and upper destination deposits come from Apple's composer tables.
 *
 * `kill` is f27, the source register's last-use hint. It is liveness, not a
 * baseline bit -- forcing it on cost two compute probes on hardware once
 * already; see the note in agx3_iadd_core.h.
 */
bool agx3_copy_encode_reg_to_uniform(enum agx_isa_encoding encoding,
                                     uint16_t uniform_half, uint16_t src_half,
                                     bool kill, uint16_t wait_mask,
                                     uint8_t out[AGX3_COPY_MAX_LENGTH],
                                     uint8_t *len);

/*
 * G18G only (compiled with AGX_ISA_ENCODING_AGX3C, re-encoded as agx3d
 * afterwards): the uniform write as the G18G layer emits it -- always the
 * eight-byte ep1 with f42 set, carrying its own scoreboard wait in
 * f30 + slot - 1. The other GPUs on the G17G-family encoding (agx3c) keep
 * agx3_copy_encode_reg_to_uniform().
 */
bool agx3_copy_encode_reg_to_uniform_g18g(enum agx_isa_encoding encoding,
                                          uint16_t uniform_half,
                                          uint16_t src_half, bool kill,
                                          uint16_t wait_mask,
                                          uint8_t out[AGX3_COPY_MAX_LENGTH],
                                          uint8_t *len);

bool agx3_copy_encode_uniform_to_reg32(enum agx_isa_encoding encoding,
                                       uint16_t dest_half,
                                       uint16_t uniform_half,
                                       uint8_t out[AGX3_COPY_MAX_LENGTH],
                                       uint8_t *len);

/*
 * Encode `copy dest16, uniform16` -- the same move at 16 bits.
 *
 * Mesa keeps booleans and half-precision values in 16-bit halves, and
 * agx_lower_uniform_sources() materializes a move for every generic-ALU
 * uniform source. At 32 bits that move reaches
 * agx3_copy_encode_uniform_to_reg32(); at 16 bits it had no encoder, so it
 * stayed a `bitop` with a uniform source and the packer refused the whole
 * shader. That is what u_blitter's texel-fetch program hit on G17P.
 *
 * This is a distinct AGX3b form, not the 32-bit one narrowed: it combines the
 * uniform source selector f23 = 3 + f17 that the 32-bit uniform copy uses
 * with the 16-bit widths f10 = f24 = 1 that agx3_copy_encode_reg16() uses,
 * and Apple sets f11. Both halves are witnessed together: Apple's output
 * has two agx3b copy.ep0 instructions with exactly f9 = 2, f10 = 1, f23 = 3,
 * f24 = 1.
 *
 * Like the 32-bit uniform form, the witnesses carry no wait bits, so the
 * caller retires a pending producer through the grounded band no-op rather
 * than having scoreboard bits invented for a form with none in the corpus.
 */
bool agx3_copy_encode_uniform_to_reg16(enum agx_isa_encoding encoding,
                                       uint16_t dest_half,
                                       uint16_t uniform_half, bool dest_cache,
                                       uint8_t out[AGX3_COPY_MAX_LENGTH],
                                       uint8_t *len);

#ifdef __cplusplus
}
#endif
