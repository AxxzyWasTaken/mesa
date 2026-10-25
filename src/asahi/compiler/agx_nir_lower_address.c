/*
 * Copyright 2022 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include "compiler/nir/nir_builder.h"
#include "agx_nir.h"
#include "nir.h"
#include "nir_intrinsics.h"
#include "nir_opcodes.h"

struct match {
   nir_scalar base, offset;
   bool sign_extend;
   uint8_t shift;
   int16_t byte_offset;
};

struct lower_address_options {
   bool byte_indices;
   bool byte_offsets;
};

static void
peel_scaled_u32(nir_scalar *offset, uint8_t *shift, unsigned max_shift)
{
   nir_scalar value = nir_scalar_chase_movs(*offset);
   if (!nir_scalar_is_alu(value))
      return;

   const nir_op op = nir_scalar_alu_op(value);
   if (op == nir_op_ishl) {
      nir_scalar amount = nir_scalar_chase_alu_src(value, 1);
      if (nir_scalar_is_const(amount) &&
          nir_scalar_as_uint(amount) <= max_shift) {
         *offset = nir_scalar_chase_alu_src(value, 0);
         *shift = nir_scalar_as_uint(amount);
      }
      return;
   }

   if (op != nir_op_imul)
      return;

   for (unsigned i = 0; i < 2; i++) {
      nir_scalar factor = nir_scalar_chase_alu_src(value, i);
      if (!nir_scalar_is_const(factor))
         continue;

      const uint64_t raw = nir_scalar_as_uint(factor);
      if (!util_is_power_of_two_nonzero(raw))
         continue;

      const unsigned amount = util_logbase2(raw);
      if (amount <= max_shift) {
         *offset = nir_scalar_chase_alu_src(value, 1 - i);
         *shift = amount;
      }
      return;
   }
}

static nir_scalar
strip_zero_shift(nir_scalar value)
{
   value = nir_scalar_chase_movs(value);
   while (nir_scalar_is_alu(value) &&
          nir_scalar_alu_op(value) == nir_op_ishl) {
      nir_scalar amount = nir_scalar_chase_alu_src(value, 1);
      if (!nir_scalar_is_const(amount) || nir_scalar_as_uint(amount) != 0)
         break;
      value = nir_scalar_chase_alu_src(value, 0);
   }
   return value;
}

/* Recover the native 32-bit index from an address-sized zero extension.
 * agx_nir_fuse_lea may wrap this in a 64-bit LEA before this pass gets to
 * select the load's base-plus-index addressing mode.
 */
static bool
peel_u2u64_index(nir_scalar wide, nir_scalar *narrow, uint8_t *shift,
                 unsigned max_shift)
{
   wide = strip_zero_shift(wide);
   if (!nir_scalar_is_alu(wide) || nir_scalar_alu_op(wide) != nir_op_u2u64)
      return false;

   *narrow = nir_scalar_chase_alu_src(wide, 0);
   if (narrow->def->bit_size != 32)
      return false;

   peel_scaled_u32(narrow, shift, max_shift);
   return true;
}

/* nir_lower_explicit_io scales each array index by its stride with amul. A
 * valid access has a nonnegative array index, so every such product is
 * nonnegative, and since UBOs and SSBOs are limited to INT32_MAX bytes their
 * sum stays below 2^31 (see the relaxed lea rules in agx_nir_algebraic.py).
 */
static bool
is_amul_sum(nir_scalar s, unsigned depth)
{
   s = nir_scalar_chase_movs(s);
   if (depth > 8 || !nir_scalar_is_alu(s))
      return false;

   const nir_op op = nir_scalar_alu_op(s);
   if (op == nir_op_amul)
      return true;

   return op == nir_op_iadd &&
          is_amul_sum(nir_scalar_chase_alu_src(s, 0), depth + 1) &&
          is_amul_sum(nir_scalar_chase_alu_src(s, 1), depth + 1);
}

/* AGX3C loads and stores add a signed 16-bit byte offset after scaling the
 * index, so a constant addend in the index -- a struct member or a constant
 * array element -- needs no ALU. The split must not change any valid address:
 *
 * - A sign-extended index gives sext(x + c) = sext(x) + c unless x + c
 *   overflows, which no valid signed index does.
 * - A zero-extended index gives zext(x + c) = zext(x) + c only if x + c does
 *   not wrap. An amul sum x is nonnegative and below 2^31 for a valid access,
 *   so this holds for c >= 0. Any other x may be negative (a reversed index
 *   N - i folds to c + -i), so leave it alone.
 * - An array index i + c scaled by amul is, for a valid access, nonnegative
 *   and below 2^31 / stride, so as signed integers (i + c) * stride equals
 *   sext(i) * stride + c * stride. With c < 0 and a byte stride, i + c could
 *   still wrap into the last bytes of a 2 GiB buffer, so require c >= 0 or a
 *   wider stride there.
 *
 * The same bounds let a power-of-two stride move into the index scale.
 */
static void
fold_byte_offset(nir_builder *b, struct match *match, unsigned format_shift,
                 unsigned max_shift)
{
   if (match->offset.def == NULL)
      return;

   const bool bytes = match->shift == AGX_NIR_MEMORY_INDEX_BYTES;
   const unsigned scale = bytes ? 0 : format_shift + match->shift;
   nir_scalar offset = nir_scalar_chase_movs(match->offset);
   int64_t byte_offset = 0;
   bool sign_extend = match->sign_extend;

   if (offset.def->bit_size != 32 || !nir_scalar_is_alu(offset))
      return;

   if (nir_scalar_alu_op(offset) == nir_op_iadd) {
      for (unsigned i = 0; i < 2; ++i) {
         nir_scalar k = nir_scalar_chase_alu_src(offset, i);
         nir_scalar rest = nir_scalar_chase_alu_src(offset, 1 - i);
         if (!nir_scalar_is_const(k) || nir_scalar_is_const(rest))
            continue;

         const int64_t c = nir_scalar_as_int(k);
         if (!sign_extend && (c < 0 || !is_amul_sum(rest, 0)))
            return;

         byte_offset = c * (INT64_C(1) << scale);
         offset = nir_scalar_chase_movs(rest);
         break;
      }
   }

   /* amul(i + c, stride) or amul(i, stride) */
   nir_scalar index = offset;
   uint64_t stride = 0;
   if (nir_scalar_is_alu(offset) && nir_scalar_alu_op(offset) == nir_op_amul) {
      for (unsigned i = 0; i < 2; ++i) {
         nir_scalar d = nir_scalar_chase_alu_src(offset, i);
         if (nir_scalar_is_const(d) && nir_scalar_as_uint(d) <= 0x10000) {
            stride = nir_scalar_as_uint(d);
            index = nir_scalar_chase_movs(nir_scalar_chase_alu_src(offset, 1 - i));
            break;
         }
      }
   }

   bool index_folded = false;
   if (stride && nir_scalar_is_alu(index) &&
       nir_scalar_alu_op(index) == nir_op_iadd) {
      for (unsigned i = 0; i < 2; ++i) {
         nir_scalar k = nir_scalar_chase_alu_src(index, i);
         nir_scalar rest = nir_scalar_chase_alu_src(index, 1 - i);
         if (!nir_scalar_is_const(k) || nir_scalar_is_const(rest))
            continue;

         const int64_t c = nir_scalar_as_int(k);
         if (c < 0 && stride < 2)
            break;

         byte_offset += c * (int64_t)stride * (INT64_C(1) << scale);
         index = nir_scalar_chase_movs(rest);
         sign_extend = true;
         index_folded = true;
         break;
      }
   }

   if (byte_offset < INT16_MIN || byte_offset > INT16_MAX)
      return;

   if (index_folded) {
      /* The array index lost its constant: rescale what is left. */
      if (!bytes && util_is_power_of_two_nonzero64(stride) &&
          scale + util_logbase2_64(stride) <= max_shift) {
         match->shift += util_logbase2_64(stride);
         match->offset = index;
      } else {
         nir_def *x = nir_channel(b, index.def, index.comp);
         match->offset = nir_get_scalar(nir_imul_imm(b, x, stride), 0);
      }
   } else if (stride && !sign_extend && !bytes &&
              util_is_power_of_two_nonzero64(stride) &&
              scale + util_logbase2_64(stride) <= max_shift) {
      /* An unchanged amul by a power of two: nonnegative, no wrap. */
      match->shift += util_logbase2_64(stride);
      match->offset = index;
   } else {
      match->offset = offset;
   }

   match->byte_offset = byte_offset;
   match->sign_extend = sign_extend;
}

static enum pipe_format
format_for_bitsize(unsigned bitsize)
{
   switch (bitsize) {
   case 8:
      return PIPE_FORMAT_R8_UINT;
   case 16:
      return PIPE_FORMAT_R16_UINT;
   case 32:
      return PIPE_FORMAT_R32_UINT;
   default:
      UNREACHABLE("should have been lowered");
   }
}

static bool
pass(struct nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_global &&
       intr->intrinsic != nir_intrinsic_load_global_constant &&
       intr->intrinsic != nir_intrinsic_global_atomic &&
       intr->intrinsic != nir_intrinsic_global_atomic_swap &&
       intr->intrinsic != nir_intrinsic_store_global)
      return false;

   const struct lower_address_options *options = data;
   const bool byte_indices = options->byte_indices;
   b->cursor = nir_before_instr(&intr->instr);

   unsigned bitsize = intr->intrinsic == nir_intrinsic_store_global
                         ? nir_src_bit_size(intr->src[0])
                         : intr->def.bit_size;
   enum pipe_format format = format_for_bitsize(bitsize);
   unsigned format_shift = util_logbase2(util_format_get_blocksize(format));

   nir_src *orig_offset = nir_get_io_offset_src(intr);
   nir_scalar base = nir_scalar_resolved(orig_offset->ssa, 0);
   struct match match = {.base = base};
   bool shift_must_match =
      (intr->intrinsic == nir_intrinsic_global_atomic) ||
      (intr->intrinsic == nir_intrinsic_global_atomic_swap);
   unsigned max_shift = format_shift + (shift_must_match ? 0 : 2);

   if (nir_scalar_is_alu(base)) {
      nir_op op = nir_scalar_alu_op(base);
      if (op == nir_op_ulea_agx || op == nir_op_ilea_agx) {
         unsigned shift = nir_scalar_as_uint(nir_scalar_chase_alu_src(base, 2));
         if (shift <= max_shift) {
            nir_scalar offset = nir_scalar_chase_alu_src(base, 1);
            uint8_t extra_shift = 0;
            if (op != nir_op_ulea_agx ||
                !peel_u2u64_index(offset, &offset, &extra_shift,
                                  max_shift - shift))
               peel_scaled_u32(&offset, &extra_shift, max_shift - shift);

            const unsigned total_shift = shift + extra_shift;
            if (total_shift >= format_shift ||
                (byte_indices && !shift_must_match && total_shift == 0 &&
                 op == nir_op_ulea_agx && offset.def->bit_size == 32)) {
               match = (struct match){
                  .base = nir_scalar_chase_alu_src(base, 0),
                  .offset = offset,
                  .shift = total_shift >= format_shift
                              ? total_shift - format_shift
                              : AGX_NIR_MEMORY_INDEX_BYTES,
                  .sign_extend = (op == nir_op_ilea_agx),
               };
            }
         }
      } else if (op == nir_op_iadd) {
         /* A load from base + u2u64(offset32 << shift) maps directly to AGX's
          * 64-bit base plus 32-bit register-index form when the scale covers
          * the element size. Keeping the add in 64-bit ALU form would
          * manufacture a carry chain that this address mode does not need.
          */
         for (unsigned i = 0; i < 2; ++i) {
            nir_scalar narrow;
            uint8_t shift = 0;
            if (!peel_u2u64_index(nir_scalar_chase_alu_src(base, i), &narrow,
                                  &shift, max_shift) ||
                (shift < format_shift &&
                 !(byte_indices && !shift_must_match && shift == 0)))
               continue;

            match = (struct match){
               .base = nir_scalar_chase_alu_src(base, 1 - i),
               .offset = narrow,
               .shift = shift >= format_shift
                           ? shift - format_shift
                           : AGX_NIR_MEMORY_INDEX_BYTES,
            };
            break;
         }

         if (match.offset.def != NULL)
            goto matched;

         for (unsigned i = 0; i < 2; ++i) {
            nir_scalar const_scalar = nir_scalar_chase_alu_src(base, i);
            if (!nir_scalar_is_const(const_scalar))
               continue;

            /* Put scalar into form (k*2^n), clamping n at the maximum hardware
             * shift.
             */
            int64_t raw_scalar = nir_scalar_as_uint(const_scalar);
            assert(raw_scalar != 0 && "must have been optimized out");

            uint32_t shift = MIN2(__builtin_ctz(raw_scalar), max_shift);
            int64_t k = raw_scalar >> shift;

            /* See if the reduced scalar is from a sign extension. We must have
             * at least the format shift to avoid underflowing.
             */
            if (k > INT32_MAX || k < INT32_MIN || shift < format_shift)
               break;

            /* Match the constant */
            match = (struct match){
               .base = nir_scalar_chase_alu_src(base, 1 - i),
               .offset = nir_get_scalar(nir_imm_int(b, k), 0),
               .shift = shift - format_shift,
               .sign_extend = true,
            };

            break;
         }
      }
   }

matched:

   if (options->byte_offsets && !shift_must_match &&
       intr->intrinsic != nir_intrinsic_global_atomic &&
       intr->intrinsic != nir_intrinsic_global_atomic_swap)
      fold_byte_offset(b, &match, format_shift, max_shift);

   const unsigned mem_base =
      match.shift | ((uint32_t)(uint16_t)match.byte_offset
                     << AGX_NIR_MEMORY_BYTE_OFFSET_SHIFT);

   nir_def *offset = match.offset.def != NULL
                        ? nir_channel(b, match.offset.def, match.offset.comp)
                        : nir_imm_int(b, 0);

   nir_def *new_base = nir_channel(b, match.base.def, match.base.comp);

   nir_def *repl = NULL;
   bool has_dest = (intr->intrinsic != nir_intrinsic_store_global);
   unsigned num_components = has_dest ? intr->def.num_components : 0;
   unsigned bit_size = has_dest ? intr->def.bit_size : 0;

   if (intr->intrinsic == nir_intrinsic_load_global) {
      repl =
         nir_load_agx(b, num_components, bit_size, new_base, offset,
                      .access = nir_intrinsic_access(intr), .base = mem_base,
                      .format = format, .sign_extend = match.sign_extend);

   } else if (intr->intrinsic == nir_intrinsic_load_global_constant) {
      repl = nir_load_constant_agx(b, num_components, bit_size, new_base,
                                   offset, .access = nir_intrinsic_access(intr),
                                   .base = mem_base, .format = format,
                                   .sign_extend = match.sign_extend);
   } else if (intr->intrinsic == nir_intrinsic_global_atomic) {
      repl =
         nir_global_atomic_agx(b, bit_size, new_base, offset, intr->src[1].ssa,
                               .access = nir_intrinsic_access(intr),
                               .atomic_op = nir_intrinsic_atomic_op(intr),
                               .sign_extend = match.sign_extend);
   } else if (intr->intrinsic == nir_intrinsic_global_atomic_swap) {
      repl = nir_global_atomic_swap_agx(
         b, bit_size, new_base, offset, intr->src[1].ssa, intr->src[2].ssa,
         .access = nir_intrinsic_access(intr),
         .atomic_op = nir_intrinsic_atomic_op(intr),
         .sign_extend = match.sign_extend);
   } else {
      nir_store_agx(b, intr->src[0].ssa, new_base, offset,
                    .access = nir_intrinsic_access(intr), .base = mem_base,
                    .format = format, .sign_extend = match.sign_extend);
   }

   if (repl)
      nir_def_rewrite_uses(&intr->def, repl);

   nir_instr_remove(&intr->instr);
   return true;
}

bool
agx_nir_lower_address(nir_shader *nir, bool byte_indices, bool byte_offsets)
{
   const struct lower_address_options options = {
      .byte_indices = byte_indices,
      .byte_offsets = byte_offsets,
   };
   bool progress = false;

   /* First, clean up as much as possible. This will make fusing more effective.
    */
   do {
      progress = false;
      NIR_PASS(progress, nir, agx_nir_cleanup_amul);
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      NIR_PASS(progress, nir, nir_opt_dce);
   } while (progress);

   /* Then, fuse as many lea as possible */
   NIR_PASS(progress, nir, agx_nir_fuse_lea);

   /* Next, lower load/store using the lea's */
   NIR_PASS(progress, nir, nir_shader_intrinsics_pass, pass,
            nir_metadata_control_flow, (void *)&options);

   /* Finally, lower any leftover lea instructions back to ALU to let
    * nir_opt_algebraic simplify them from here.
    */
   NIR_PASS(progress, nir, agx_nir_lower_lea);
   NIR_PASS(progress, nir, nir_opt_dce);

   return progress;
}
