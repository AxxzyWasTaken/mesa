/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */
#include "hk_nir_forward_image_stores.h"
#include "nir_builder.h"
#include "compiler/glsl_types.h"
#include <stdio.h>

enum event { NONE, ALIAS_STORE, IMAGE_BARRIER, SHARED_BARRIER, ATOMIC, VOLATILE,
             ROBUST_ALIAS_STORE, ROBUST_ALIAS_OOB, ROBUST_ATOMIC_OOB, INDEPENDENT_STORE,
             VOLATILE_STORE };

static bool
check(enum event event, bool expect_forwarded)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options,
                                                 "image-store-forward");
   const struct glsl_type *type =
      glsl_image_type(GLSL_SAMPLER_DIM_2D, false, GLSL_TYPE_INT);
   nir_variable *a = nir_variable_create(b.shader, nir_var_image, type, "a");
   nir_variable *alias = nir_variable_create(b.shader, nir_var_image, type, "alias");
   if (event == INDEPENDENT_STORE) {
      a->data.access = ACCESS_RESTRICT;
      alias->data.access = ACCESS_RESTRICT;
   }
   a->data.binding = 0;
   alias->data.binding = 1;
   nir_def *da = &nir_build_deref_var(&b, a)->def;
   nir_def *db = &nir_build_deref_var(&b, alias)->def;
   nir_def *coord = nir_imm_ivec4(&b, 0, 0, 0, 0);
   nir_def *zero = nir_imm_int(&b, 0);
   nir_image_deref_store(&b, da, coord, zero, nir_imm_ivec4(&b, 456, 0, 0, 0), zero,
                        .image_dim = GLSL_SAMPLER_DIM_2D,
                        .format = PIPE_FORMAT_R32_SINT, .src_type = nir_type_int32,
                        .access = event == VOLATILE_STORE ? ACCESS_VOLATILE : 0);
   nir_def *other_coord = (event == ROBUST_ALIAS_OOB || event == ROBUST_ATOMIC_OOB)
      ? nir_imm_ivec4(&b, -1, 0, 0, 0) : coord;
   if (event == ALIAS_STORE || event == ROBUST_ALIAS_STORE || event == ROBUST_ALIAS_OOB || event == INDEPENDENT_STORE)
      nir_image_deref_store(&b, db, other_coord, zero, nir_imm_ivec4(&b, 7, 0, 0, 0), zero,
                           .image_dim = GLSL_SAMPLER_DIM_2D,
                           .format = PIPE_FORMAT_R32_SINT, .src_type = nir_type_int32);
   if (event == IMAGE_BARRIER || event == SHARED_BARRIER)
      nir_barrier(&b, .execution_scope = SCOPE_WORKGROUP,
                  .memory_scope = SCOPE_WORKGROUP,
                  .memory_semantics = NIR_MEMORY_ACQ_REL,
                  .memory_modes = event == IMAGE_BARRIER ? nir_var_image : nir_var_mem_shared);
   if (event == ATOMIC || event == ROBUST_ATOMIC_OOB)
      nir_image_deref_atomic(&b, 32, db, other_coord, zero, nir_imm_int(&b, 7),
                             .image_dim = GLSL_SAMPLER_DIM_2D,
                             .format = PIPE_FORMAT_R32_SINT,
                             .atomic_op = nir_atomic_op_iadd);
   /* The alias overwrite tests publish the write through the second
    * binding before reading through the first one. */
   if (event == ALIAS_STORE || event == ROBUST_ALIAS_STORE || event == ATOMIC)
      nir_barrier(&b, .execution_scope = SCOPE_WORKGROUP,
                  .memory_scope = SCOPE_WORKGROUP,
                  .memory_semantics = NIR_MEMORY_ACQ_REL,
                  .memory_modes = nir_var_image);
   nir_def *value = nir_image_deref_load(&b, 4, 32, da, coord, zero, zero,
                                        .image_dim = GLSL_SAMPLER_DIM_2D,
                                        .format = PIPE_FORMAT_R32_SINT,
                                        .dest_type = nir_type_int32,
                                        .access = event == VOLATILE ? ACCESS_VOLATILE : 0);
   nir_intrinsic_instr *out = nir_store_ssbo(&b, value, zero, zero,
                                            .align_mul = 4, .write_mask = 0xf);
   nir_validate_shader(b.shader, "before image forwarding");
   hk_nir_forward_image_stores(b.shader, event == ROBUST_ALIAS_STORE ||
                                event == ROBUST_ALIAS_OOB || event == ROBUST_ATOMIC_OOB);
   /* Resolve the fixture's image dimensions so the test checks the value
    * selected by robust in/out-of-bounds writes, not a particular IR shape. */
   nir_foreach_function_impl(impl, b.shader) {
      nir_builder sizes = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_image_deref_size) {
               sizes.cursor = nir_after_instr(instr);
               nir_def_rewrite_uses(&intr->def, nir_imm_ivec2(&sizes, 4, 4));
               nir_instr_remove(instr);
            }
         }
      }
   }
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, b.shader, nir_opt_constant_folding);
      NIR_PASS(progress, b.shader, nir_opt_algebraic);
      NIR_PASS(progress, b.shader, nir_opt_dce);
   } while (progress);
   nir_validate_shader(b.shader, "after image forwarding");
   bool forwarded = nir_src_is_const(out->src[0]) && nir_src_comp_as_int(out->src[0], 0) == 456;
   bool ok = forwarded == expect_forwarded;
   if (event == VOLATILE_STORE) {
      bool kept = false;
      nir_foreach_function_impl(impl, b.shader) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type != nir_instr_type_intrinsic)
                  continue;
               nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
               kept |= intr->intrinsic == nir_intrinsic_image_deref_store &&
                       (nir_intrinsic_access(intr) & ACCESS_VOLATILE);
            }
         }
      }
      ok &= kept;
   }
   printf("%s event=%u forwarded=%u expected=%u\n", ok ? "PASS" : "FAIL", event,
          forwarded, expect_forwarded);
   ralloc_free(b.shader);
   return ok;
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();
   unsigned failed = 0;
   failed += !check(NONE, true);
   failed += !check(ALIAS_STORE, false);
   failed += !check(IMAGE_BARRIER, false);
   failed += !check(SHARED_BARRIER, true);
   failed += !check(ATOMIC, false);
   failed += !check(VOLATILE, false);
   failed += !check(ROBUST_ALIAS_STORE, false);
   failed += !check(ROBUST_ALIAS_OOB, true);
   failed += !check(ROBUST_ATOMIC_OOB, true);
   failed += !check(INDEPENDENT_STORE, true);
   failed += !check(VOLATILE_STORE, true);
   glsl_type_singleton_decref();
   printf("image-store forwarding: %u failures\n", failed);
   return failed != 0;
}
