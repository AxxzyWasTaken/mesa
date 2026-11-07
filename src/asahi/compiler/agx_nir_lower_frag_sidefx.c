/*
 * Copyright 2022 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "agx_compiler.h"

/* Fragment shaders with side effects require special handling to ensure the
 * side effects execute as intended. By default, they require late depth
 * testing, to ensure the side effects happen even for killed pixels. G17P and
 * G18 native late-memory shaders provide pass-through depth feedback after the
 * side effects. MOVMSK is its explicit early-test path and does not by itself
 * defer hardware depth rejection. Keep the existing all-live sample test on
 * other devices.
 *
 * For side effects with force early testing forced, the sample mask is written
 * at the *beginning* of the shader.
 */

#define ALL_SAMPLES (0xFF)

static void
insert_late_test(nir_builder *b, bool g17p)
{
   if (g17p) {
      nir_store_output(b, nir_load_frag_coord_z(b), nir_imm_int(b, 0),
                       .io_semantics.location = FRAG_RESULT_DEPTH,
                       .src_type = nir_type_float32);
      b->shader->info.outputs_written |= BITFIELD64_BIT(FRAG_RESULT_DEPTH);
      return;
   }
   nir_def *all_samples = nir_imm_intN_t(b, ALL_SAMPLES, 16);
   nir_sample_mask_agx(b, all_samples, all_samples);
}

/*
 * This pass runs from agx_preprocess_nir, which drivers may call more than once
 * on the same shader: Honeykrisp preprocesses at shader creation
 * (hk_preprocess_nir) and again while lowering (hk_lower_nir). The late test
 * must be inserted exactly once. Two sample_mask instructions on the same path
 * run the depth/stencil test twice per sample, which the sample_mask contract
 * documented in agx_nir_lower_sample_mask.c forbids.
 *
 * The FragDepth lowering this pass replaced was self-idempotent, because it
 * recorded FRAG_RESULT_DEPTH in outputs_written and the writes_zs check below
 * caught the second run. A sample mask leaves no such trace in shader_info, so
 * look for the instruction itself.
 */
static bool
has_sample_mask(nir_shader *s)
{
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;

            if (nir_instr_as_intrinsic(instr)->intrinsic ==
                nir_intrinsic_sample_mask_agx)
               return true;
         }
      }
   }

   return false;
}

/*
 * The hardware skips a fragment shader with side effects outright when the
 * depth test is NEVER, so the driver does not program NEVER at all: hk forces
 * the depth comparison to ALWAYS whenever a side-effecting shader is drawn
 * under VK_COMPARE_OP_NEVER (hk_cmd_draw.c, draw.force_never_in_shader, in
 * both the common FRAGMENT_FACE path and the native HAL200 depth words) and
 * expects the shader to emulate the test instead.
 *
 * 7fb60c4c813 emulated it by adding NaN to the synthesized FragDepth write.
 * 53aac78d1aa replaced that write with an all-live sample mask and dropped the
 * NaN with it; df3c8c45508 brought a pass-through FragDepth write back for
 * G17P but not the bias. Nothing has emulated the test since, so every
 * fragment passed a test the API asked to fail and the colour was written.
 *
 * Demote at the very end of the shader instead, after every side effect has
 * executed: the stores land, the colour write does not. This does not depend
 * on NaN comparison semantics in the depth unit, and it is a no-op for every
 * other draw, where load_depth_never_agx reads zero.
 */
static void
emulate_depth_never(nir_shader *s)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(s);
   nir_builder b = nir_builder_at(nir_after_impl(impl));

   nir_demote_if(&b, nir_ine_imm(&b, nir_load_depth_never_agx(&b), 0));
   s->info.fs.uses_discard = true;

   nir_progress(true, impl, nir_metadata_none);
}

struct sidefx_state {
   bool done;
   bool g17p;
};

static bool
pass(struct nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_store_output)
      return false;

   /* Only lower once */
   struct sidefx_state *state = data;
   if (state->done)
      return false;
   state->done = true;

   b->cursor = nir_before_instr(&intr->instr);
   insert_late_test(b, state->g17p);
   return true;
}


/* AGX2 implementation retained from upstream ab0ab220 (MIT). */
static void
legacy_insert_z_write(nir_builder *b)
{
   /* When forcing depth test to NEVER, force Z to NaN.  This works around the
    * hardware discarding depth=NEVER.
    *
    * TODO: Check if Apple has a better way of handling this.
    */
   nir_def *bias = nir_bcsel(b, nir_ine_imm(b, nir_load_depth_never_agx(b), 0),
                             nir_imm_float(b, NAN), nir_imm_float(b, -0.0));

   nir_def *z = nir_fadd(b, bias, nir_load_frag_coord_z(b));

   nir_store_output(b, z, nir_imm_int(b, 0),
                    .io_semantics.location = FRAG_RESULT_DEPTH,
                    .src_type = nir_type_float32);

   b->shader->info.outputs_written |= BITFIELD64_BIT(FRAG_RESULT_DEPTH);
}

static bool
legacy_pass(struct nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_store_output)
      return false;

   /* Only lower once */
   bool *done = data;
   if (*done)
      return false;
   *done = true;

   b->cursor = nir_before_instr(&intr->instr);
   legacy_insert_z_write(b);
   return true;
}

static bool
legacy_frag_sidefx(nir_shader *s)
{
   assert(s->info.stage == MESA_SHADER_FRAGMENT);

   /* If there are no side effects, there's nothing to lower */
   if (!s->info.writes_memory)
      return false;

   /* Lower writes from helper invocations with the common legacy_pass. The hardware
    * will predicate simple writes for us, but that fails with sample shading so
    * we do that in software too.
    */
   NIR_PASS(_, s, nir_lower_helper_writes, s->info.fs.uses_sample_shading);

   bool writes_zs =
      s->info.outputs_written &
      (BITFIELD64_BIT(FRAG_RESULT_STENCIL) | BITFIELD64_BIT(FRAG_RESULT_DEPTH));

   /* If the shader wants early fragment tests, the sample mask lowering legacy_pass
    * will trigger an early test at the beginning of the shader. This lets us
    * use a Passthrough punch type, instead of Opaque which may result in the
    * shader getting skipped incorrectly and then the side effects not kicking
    * in. But this happens there to avoid it happening twice with a discard.
    */
   if (s->info.fs.early_fragment_tests)
      return false;

   /* If depth/stencil feedback is already used, we're done */
   if (writes_zs)
      return false;

   bool done = false;
   nir_shader_intrinsics_pass(s, legacy_pass, nir_metadata_control_flow, &done);

   /* If there's no render targets written, just put the write at the end */
   if (!done) {
      nir_function_impl *impl = nir_shader_get_entrypoint(s);
      nir_builder b = nir_builder_at(nir_after_impl(impl));

      legacy_insert_z_write(&b);
   }

   return true;
}


bool
agx_nir_lower_frag_sidefx(nir_shader *s, bool g17p)
{
   if (!g17p)
      return legacy_frag_sidefx(s);

   assert(s->info.stage == MESA_SHADER_FRAGMENT);

   /* If there are no side effects, there's nothing to lower */
   if (!s->info.writes_memory)
      return false;

   /* Lower writes from helper invocations with the common pass. The hardware
    * will predicate simple writes for us, but that fails with sample shading so
    * we do that in software too.
    */
   NIR_PASS(_, s, nir_lower_helper_writes,
            s->info.fs.uses_sample_shading || g17p);

   bool writes_zs =
      s->info.outputs_written &
      (BITFIELD64_BIT(FRAG_RESULT_STENCIL) | BITFIELD64_BIT(FRAG_RESULT_DEPTH));

   /* If the shader wants early fragment tests, the sample mask lowering pass
    * will trigger an early test at the beginning of the shader. This lets us
    * use a Passthrough punch type, instead of Opaque which may result in the
    * shader getting skipped incorrectly and then the side effects not kicking
    * in. But this happens there to avoid it happening twice with a discard.
    */
   if (s->info.fs.early_fragment_tests)
      return false;

   /* If depth/stencil feedback is already used, we're done */
   if (writes_zs)
      return false;

   /* If a previous run of this pass already inserted the late test, we're done.
    * Running it again would test every sample twice.
    */
   if (has_sample_mask(s))
      return false;

   struct sidefx_state state = {.g17p = g17p};
   nir_shader_intrinsics_pass(s, pass, nir_metadata_control_flow, &state);

   /* If there's no render targets written, just put the write at the end */
   if (!state.done) {
      nir_function_impl *impl = nir_shader_get_entrypoint(s);
      nir_builder b = nir_builder_at(nir_after_impl(impl));

      insert_late_test(&b, g17p);
   }

   if (g17p)
      emulate_depth_never(s);

   return true;
}
