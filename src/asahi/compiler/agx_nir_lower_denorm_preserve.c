/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */
#include "compiler/nir/nir_builder.h"
#include "agx_nir.h"

/*
 * FP32 arithmetic flushes denormal operands and results to zero. In a shader
 * that requests DenormPreserve for FP32, an addition or multiplication with a
 * denormal operand, or whose native result is zero or denormal, is computed
 * exactly in integer arithmetic instead. Vulkan derives the precision of fma
 * from a multiplication followed by an addition, so ffma is split into them.
 */

static nir_def *
exponent(nir_builder *b, nir_def *x)
{
   return nir_iand_imm(b, nir_ushr_imm(b, x, 23), 0xff);
}

static nir_def *
is_denorm(nir_builder *b, nir_def *x)
{
   return nir_iand(b, nir_ieq_imm(b, exponent(b, x), 0),
                   nir_ine_imm(b, nir_iand_imm(b, x, 0x7fffff), 0));
}

static nir_def *
is_zero(nir_builder *b, nir_def *x)
{
   return nir_ieq_imm(b, nir_iand_imm(b, x, 0x7fffffff), 0);
}

static nir_def *
is_finite(nir_builder *b, nir_def *x)
{
   return nir_ine_imm(b, exponent(b, x), 0xff);
}

/* x = sig * 2^(e - 150) with e >= 1, for finite x. */
static void
unpack(nir_builder *b, nir_def *x, nir_def **sig, nir_def **e)
{
   nir_def *biased = exponent(b, x);
   nir_def *m = nir_iand_imm(b, x, 0x7fffff);
   nir_def *denorm = nir_ieq_imm(b, biased, 0);
   *sig = nir_bcsel(b, denorm, m, nir_ior_imm(b, m, 0x800000));
   *e = nir_bcsel(b, denorm, nir_imm_int(b, 1), biased);
}

/* p * 2^e rounded to nearest even, as FP32 bits with the given sign bit.
 * Requires 0 < p < 2^63. */
static nir_def *
round_pack(nir_builder *b, nir_def *sign, nir_def *p, nir_def *e)
{
   nir_def *lo = nir_unpack_64_2x32_split_x(b, p);
   nir_def *hi = nir_unpack_64_2x32_split_y(b, p);
   nir_def *n = nir_bcsel(b, nir_ine_imm(b, hi, 0),
                          nir_iadd_imm(b, nir_ufind_msb(b, hi), 32),
                          nir_ufind_msb(b, lo));

   /* A normal result keeps 24 significant bits under its leading one; a
    * denormal one keeps the bits down to 2^-149. Its leading one, if
    * rounding carries into it, is then the exponent field's lowest bit. */
   nir_def *biased = nir_iadd_imm(b, nir_iadd(b, n, e), 127);
   nir_def *normal = nir_ige_imm(b, biased, 1);
   nir_def *shift = nir_bcsel(b, normal, nir_iadd_imm(b, n, -23),
                              nir_ineg(b, nir_iadd_imm(b, e, 149)));
   nir_def *base = nir_bcsel(
      b, normal, nir_ishl_imm(b, nir_iadd_imm(b, biased, -1), 23),
      nir_imm_int(b, 0));

   /* Fewer significant bits than kept: exact, and p < 2^24. */
   nir_def *q_left =
      nir_ishl(b, lo, nir_umin_imm(b, nir_ineg(b, shift), 31));

   nir_def *s = nir_umin_imm(b, shift, 63);
   nir_def *q = nir_ushr(b, p, s);
   nir_def *rem = nir_isub(b, p, nir_ishl(b, q, s));
   nir_def *half =
      nir_ishl(b, nir_imm_int64(b, 1), nir_iadd_imm(b, nir_umax_imm(b, s, 1), -1));
   nir_def *odd = nir_ine_imm(b, nir_iand_imm(b, nir_u2u32(b, q), 1), 0);
   nir_def *up = nir_ior(b, nir_ult(b, half, rem),
                         nir_iand(b, nir_ieq(b, rem, half), odd));
   up = nir_iand(b, up, nir_ine_imm(b, s, 0));
   nir_def *q_right = nir_iadd(b, nir_u2u32(b, q), nir_b2i32(b, up));
   /* Shifted out entirely: below half of 2^-149, since p < 2^63. */
   q_right = nir_bcsel(b, nir_ilt(b, nir_imm_int(b, 63), shift),
                       nir_imm_int(b, 0), q_right);

   nir_def *bits = nir_iadd(
      b, base, nir_bcsel(b, nir_ilt_imm(b, shift, 0), q_left, q_right));
   /* A carry out of the largest exponent also reaches infinity. */
   bits = nir_bcsel(b, nir_ilt(b, nir_imm_int(b, 254), biased),
                    nir_imm_int(b, 0x7f800000), bits);
   return nir_ior(b, bits, sign);
}

static nir_def *
exact_mul(nir_builder *b, nir_def *x, nir_def *y)
{
   nir_def *sign = nir_iand_imm(b, nir_ixor(b, x, y), 0x80000000u);
   nir_def *sx, *ex, *sy, *ey;
   unpack(b, x, &sx, &ex);
   unpack(b, y, &sy, &ey);
   nir_def *p = nir_imul(b, nir_u2u64(b, sx), nir_u2u64(b, sy));
   nir_def *e = nir_iadd_imm(b, nir_iadd(b, ex, ey), -300);
   return nir_bcsel(b, nir_ieq_imm(b, p, 0), sign,
                    round_pack(b, sign, nir_bcsel(b, nir_ieq_imm(b, p, 0),
                                                  nir_imm_int64(b, 1), p),
                               e));
}

static nir_def *
exact_add(nir_builder *b, nir_def *x, nir_def *y, nir_def *native)
{
   nir_def *sx, *ex, *sy, *ey;
   unpack(b, x, &sx, &ex);
   unpack(b, y, &sy, &ey);
   nir_def *emin = nir_imin(b, ex, ey);
   nir_def *d = nir_isub(b, nir_imax(b, ex, ey), emin);

   /* Both fit in 62 bits once aligned to the smaller exponent. */
   nir_def *dx = nir_umin_imm(b, nir_isub(b, ex, emin), 38);
   nir_def *dy = nir_umin_imm(b, nir_isub(b, ey, emin), 38);
   nir_def *ax = nir_ishl(b, nir_u2u64(b, sx), dx);
   nir_def *ay = nir_ishl(b, nir_u2u64(b, sy), dy);

   nir_def *nx = nir_ilt_imm(b, x, 0);
   nir_def *ny = nir_ilt_imm(b, y, 0);
   nir_def *same = nir_ieq(b, nx, ny);
   nir_def *x_ge = nir_uge(b, ax, ay);
   nir_def *mag = nir_bcsel(b, same, nir_iadd(b, ax, ay),
                            nir_bcsel(b, x_ge, nir_isub(b, ax, ay),
                                      nir_isub(b, ay, ax)));
   nir_def *sign = nir_iand_imm(b, nir_bcsel(b, nir_ior(b, same, x_ge), x, y),
                                0x80000000u);

   nir_def *zero = nir_ieq_imm(b, mag, 0);
   nir_def *sum = round_pack(
      b, sign, nir_bcsel(b, zero, nir_imm_int64(b, 1), mag),
      nir_iadd_imm(b, emin, -150));
   /* Cancellation to zero is +0 when rounding to nearest. */
   sum = nir_bcsel(b, zero, nir_imm_int(b, 0), sum);

   /* Past 38 exponents apart the smaller one is below half an ulp of the
    * larger, which is normal and which the native sum already returns. */
   return nir_bcsel(b, nir_ilt(b, nir_imm_int(b, 38), d), native, sum);
}

static nir_def *
lower_add(nir_builder *b, nir_def *x, nir_def *y)
{
   nir_def *native = nir_fadd(b, x, y);

   nir_def *slow = nir_ior(
      b, nir_ior(b, is_denorm(b, x), is_denorm(b, y)),
      nir_iand(b, nir_ieq_imm(b, exponent(b, native), 0),
               nir_inot(b, nir_iand(b, is_zero(b, x), is_zero(b, y)))));
   slow = nir_iand(b, slow, nir_iand(b, is_finite(b, x), is_finite(b, y)));

   nir_if *nif = nir_push_if(b, slow);
   nir_def *exact = exact_add(b, x, y, native);
   nir_pop_if(b, nif);
   return nir_if_phi(b, exact, native);
}

static nir_def *
lower_mul(nir_builder *b, nir_def *x, nir_def *y)
{
   nir_def *native = nir_fmul(b, x, y);

   nir_def *slow = nir_ior(
      b, nir_ior(b, is_denorm(b, x), is_denorm(b, y)),
      nir_iand(b, nir_ieq_imm(b, exponent(b, native), 0),
               nir_inot(b, nir_ior(b, is_zero(b, x), is_zero(b, y)))));
   nir_def *finite = nir_iand(b, is_finite(b, x), is_finite(b, y));

   nir_if *nif = nir_push_if(b, nir_iand(b, slow, finite));
   nir_def *exact = exact_mul(b, x, y);
   nir_pop_if(b, nif);
   nir_def *result = nir_if_phi(b, exact, native);

   /* The native product flushes the denormal of inf * denormal to zero and
    * returns NaN. */
   nir_def *inf_x = nir_ieq_imm(b, nir_iand_imm(b, x, 0x7fffffff), 0x7f800000);
   nir_def *inf_y = nir_ieq_imm(b, nir_iand_imm(b, y, 0x7fffffff), 0x7f800000);
   nir_def *inf_denorm = nir_ior(b, nir_iand(b, inf_x, is_denorm(b, y)),
                                 nir_iand(b, inf_y, is_denorm(b, x)));
   return nir_bcsel(
      b, inf_denorm,
      nir_ior_imm(b, nir_iand_imm(b, nir_ixor(b, x, y), 0x80000000u),
                  0x7f800000),
      result);
}

/* The order of x as an unsigned integer, for x not NaN; -0 sorts below +0. */
static nir_def *
order_key(nir_builder *b, nir_def *x)
{
   return nir_bcsel(b, nir_ilt_imm(b, x, 0), nir_inot(b, x),
                    nir_ixor(b, x, nir_imm_int(b, 0x80000000u)));
}

/* Float comparisons flush denormal operands, so compare them as integers. */
static nir_def *
lower_compare(nir_builder *b, nir_op op, nir_def *x, nir_def *y)
{
   nir_def *native = nir_build_alu2(b, op, x, y);
   nir_def *nan = nir_ior(b, nir_fneu(b, x, x), nir_fneu(b, y, y));
   nir_def *zeros = nir_iand(b, is_zero(b, x), is_zero(b, y));
   nir_def *lt = nir_iand(b, nir_inot(b, zeros),
                          nir_ult(b, order_key(b, x), order_key(b, y)));
   nir_def *eq = nir_ior(b, zeros, nir_ieq(b, x, y));
   nir_def *exact;

   switch (op) {
   case nir_op_flt:
      exact = nir_iand(b, nir_inot(b, nan), lt);
      break;
   case nir_op_fge:
      exact = nir_iand(b, nir_inot(b, nan), nir_inot(b, lt));
      break;
   case nir_op_feq:
      exact = nir_iand(b, nir_inot(b, nan), eq);
      break;
   default: /* fneu */
      exact = nir_ior(b, nan, nir_inot(b, eq));
      break;
   }

   return nir_bcsel(b, nir_ior(b, is_denorm(b, x), is_denorm(b, y)), exact,
                    native);
}

static bool
lower(nir_builder *b, nir_alu_instr *alu, UNUSED void *data)
{
   if (nir_src_bit_size(alu->src[0].src) != 32 ||
       alu->def.num_components != 1)
      return false;

   b->cursor = nir_before_instr(&alu->instr);
   /* Keep the native operations apart: refusing contraction also keeps the
    * fma split. */
   b->fp_math_ctrl = alu->fp_math_ctrl | nir_fp_exact;
   nir_def *r;

   switch (alu->op) {
   case nir_op_fadd:
      r = lower_add(b, nir_ssa_for_alu_src(b, alu, 0),
                    nir_ssa_for_alu_src(b, alu, 1));
      break;
   case nir_op_fmul:
      r = lower_mul(b, nir_ssa_for_alu_src(b, alu, 0),
                    nir_ssa_for_alu_src(b, alu, 1));
      break;
   case nir_op_ffma:
      r = lower_add(b,
                    lower_mul(b, nir_ssa_for_alu_src(b, alu, 0),
                              nir_ssa_for_alu_src(b, alu, 1)),
                    nir_ssa_for_alu_src(b, alu, 2));
      break;
   case nir_op_fneg:
      /* A move with a modifier flushes; the sign bit is the whole operation. */
      r = nir_ixor(b, nir_ssa_for_alu_src(b, alu, 0), nir_imm_int(b, 0x80000000u));
      break;
   case nir_op_fabs:
      r = nir_iand_imm(b, nir_ssa_for_alu_src(b, alu, 0), 0x7fffffffu);
      break;
   case nir_op_flt:
   case nir_op_fge:
   case nir_op_feq:
   case nir_op_fneu:
      r = lower_compare(b, alu->op, nir_ssa_for_alu_src(b, alu, 0),
                        nir_ssa_for_alu_src(b, alu, 1));
      break;
   case nir_op_fsign: {
      /* A denormal has the sign of a nonzero value. */
      nir_def *x = nir_ssa_for_alu_src(b, alu, 0);
      nir_def *native = nir_fsign(b, x);
      r = nir_bcsel(b, is_denorm(b, x),
                    nir_ior_imm(b, nir_iand_imm(b, x, 0x80000000u),
                                0x3f800000),
                    native);
      break;
   }
   default:
      return false;
   }

   nir_def_replace(&alu->def, r);
   return true;
}

bool
agx_nir_lower_denorm_preserve(nir_shader *shader)
{
   if (!(shader->info.float_controls_execution_mode &
         FLOAT_CONTROLS_DENORM_PRESERVE_FP32))
      return false;

   /* Lowering creates the native operations it keeps; visit each original
    * instruction once. */
   bool progress = false;
   nir_foreach_function_impl(impl, shader) {
      struct util_dynarray alus = UTIL_DYNARRAY_INIT;
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_alu)
               util_dynarray_append(&alus, nir_instr_as_alu(instr));
         }
      }

      nir_builder b = nir_builder_create(impl);
      bool impl_progress = false;
      util_dynarray_foreach(&alus, nir_alu_instr *, alu)
         impl_progress |= lower(&b, *alu, NULL);
      util_dynarray_fini(&alus);

      progress |= nir_progress(impl_progress, impl, nir_metadata_none);
   }
   return progress;
}
