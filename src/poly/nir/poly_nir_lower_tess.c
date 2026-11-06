/*
 * Copyright 2023 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#include "poly/cl/libpoly.h"
#include "poly/geometry.h"
#include "poly/nir/poly_nir.h"
#include "util/bitscan.h"
#include "util/macros.h"
#include "nir.h"
#include "nir_builder.h"
#include "nir_builder_opcodes.h"
#include "nir_intrinsics.h"
#include "nir_intrinsics_indices.h"
#include "shader_enums.h"

static nir_def *
tcs_unrolled_id(nir_builder *b)
{
   return poly_tcs_unrolled_id(b, nir_load_tess_param_buffer_poly(b),
                               nir_load_workgroup_id(b));
}

uint64_t
poly_tcs_per_vertex_outputs(const nir_shader *nir)
{
   return nir->info.outputs_written &
          ~(VARYING_BIT_TESS_LEVEL_INNER | VARYING_BIT_TESS_LEVEL_OUTER |
            VARYING_BIT_BOUNDING_BOX0 | VARYING_BIT_BOUNDING_BOX1);
}

unsigned
poly_tcs_output_stride(const nir_shader *nir)
{
   return poly_tcs_out_stride(util_last_bit(nir->info.patch_outputs_written),
                              nir->info.tess.tcs_vertices_out,
                              poly_tcs_per_vertex_outputs(nir));
}

static nir_def *
tcs_out_addr(nir_builder *b, nir_intrinsic_instr *intr, nir_def *vertex_id,
             bool compact_levels)
{
   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);

   nir_def *offset = nir_get_io_offset_src(intr)->ssa;
   /* Tessellation levels are compact scalar arrays. An indirect index left
    * by nir_lower_io is a component offset, not a varying-slot offset.
    * Adding it to the location turns outer[1] into inner[0], and outer[2:3]
    * into per-vertex outputs, leaving outer factors unwritten and culling the
    * patch before its evaluation shader can run. Constant indices have
    * already been folded into component by nir_lower_io.
    */
   bool tess_level = compact_levels &&
      (sem.location == VARYING_SLOT_TESS_LEVEL_OUTER ||
       sem.location == VARYING_SLOT_TESS_LEVEL_INNER);
   nir_def *addr = poly_tcs_out_address(
      b, nir_load_tess_param_buffer_poly(b), tcs_unrolled_id(b), vertex_id,
      tess_level ? nir_imm_int(b, sem.location)
                 : nir_iadd_imm(b, offset, sem.location),
      nir_imm_int(b, util_last_bit(b->shader->info.patch_outputs_written)),
      nir_imm_int(b, b->shader->info.tess.tcs_vertices_out),
      nir_imm_int64(b, poly_tcs_per_vertex_outputs(b->shader)));

   addr = nir_iadd_imm(b, addr, nir_intrinsic_component(intr) * 4);
   if (tess_level)
      addr = nir_iadd(b, addr, nir_u2u64(b, nir_ishl_imm(b, offset, 2)));

   return addr;
}

static nir_def *
lower_tes_load(nir_builder *b, nir_intrinsic_instr *intr)
{
   gl_varying_slot location = nir_intrinsic_io_semantics(intr).location;
   nir_src *offset_src = nir_get_io_offset_src(intr);

   nir_def *vertex = nir_imm_int(b, 0);
   nir_def *offset = offset_src ? offset_src->ssa : nir_imm_int(b, 0);

   if (intr->intrinsic == nir_intrinsic_load_per_vertex_input)
      vertex = intr->src[0].ssa;

   nir_def *addr = poly_tes_in_address(b, nir_load_tess_param_buffer_poly(b),
                                       nir_load_vertex_id(b), vertex,
                                       nir_iadd_imm(b, offset, location));

   if (nir_intrinsic_has_component(intr))
      addr = nir_iadd_imm(b, addr, nir_intrinsic_component(intr) * 4);

   /* Tessellation buffers have one raw word per component, including narrow
    * interfaces. */
   nir_def *value = nir_load_global_constant(b, intr->def.num_components, 32,
                                             addr, .align_mul = 4);
   return nir_u2uN(b, value, intr->def.bit_size);
}

static nir_def *
tcs_load_input(nir_builder *b, nir_intrinsic_instr *intr)
{
   nir_def *base = nir_imul(
      b, tcs_unrolled_id(b),
      poly_tcs_patch_vertices_in(b, nir_load_tess_param_buffer_poly(b)));
   nir_def *vertex = nir_iadd(b, base, intr->src[0].ssa);

   return poly_load_per_vertex_input(b, intr, vertex);
}

static nir_def *
lower_tcs_impl(nir_builder *b, nir_intrinsic_instr *intr,
               bool can_ignore_shader_out_barriers, bool compact_levels)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_barrier: {
      /* We lowered the TCS outputs from shader_out to global memory,
       * preserve barriers if they don't target shader outputs or modify their
       * execution/memory scopes and modes accordingly to match subgroups since
       * a patch fits in a subgroup. */
      nir_variable_mode modes = nir_intrinsic_memory_modes(intr);

      if (!can_ignore_shader_out_barriers) {
         /* A patch fits in a subgroup, so a pure execution rendezvous is
          * redundant.  Memory barriers still order the global TCS output
          * buffer between the control-point and patch-constant phases. */
         if (nir_intrinsic_memory_scope(intr) == SCOPE_NONE)
            return NIR_LOWER_INSTR_PROGRESS_REPLACE;

         /* Skip barriers that don't handle shader outputs */
         if (!(modes & nir_var_shader_out))
            return NULL;

         /* Output loads/stores below become global accesses.  Their barriers
          * must follow that address-space change while preserving scopes,
          * acquire/release semantics and ordering for any other memory
          * modes. */
         nir_barrier(b, .execution_scope = nir_intrinsic_execution_scope(intr),
                     .memory_scope = nir_intrinsic_memory_scope(intr),
                     .memory_semantics = nir_intrinsic_memory_semantics(intr),
                     .memory_modes =
                        (modes & ~nir_var_shader_out) | nir_var_mem_global);
         return NIR_LOWER_INSTR_PROGRESS_REPLACE;
      }

      /* Skip barriers that don't handle shader outputs */
      if (!(modes & nir_var_shader_out))
         return NULL;

      modes &= ~nir_var_shader_out;

      /* A barrier that only targeted shader outputs can be dropped entirely by
       * drivers that don't need it. */
      if (!modes)
         return NIR_LOWER_INSTR_PROGRESS_REPLACE;

      /* Keep the original scopes while the barrier still targets other modes.
       */
      nir_barrier(b, .execution_scope = nir_intrinsic_execution_scope(intr),
                  .memory_scope = nir_intrinsic_memory_scope(intr),
                  .memory_semantics = nir_intrinsic_memory_semantics(intr),
                  .memory_modes = modes);
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;
   }

   case nir_intrinsic_load_primitive_id:
      return nir_channel(b, nir_load_workgroup_id(b), 0);

   case nir_intrinsic_load_instance_id:
      return nir_channel(b, nir_load_workgroup_id(b), 1);

   case nir_intrinsic_load_invocation_id:
      /* A DXBC hull shader may have one output control point but wider
       * fork/join phases. In that case the workgroup still needs distinct
       * phase invocation IDs even though the control-point phase is scalar. */
      if (b->shader->info.workgroup_size[0] == 1)
         return nir_imm_int(b, 0);
      else
         return nir_channel(b, nir_load_local_invocation_id(b), 0);

   case nir_intrinsic_load_per_vertex_input:
      return tcs_load_input(b, intr);

   case nir_intrinsic_load_patch_vertices_in:
      return poly_tcs_patch_vertices_in(b, nir_load_tess_param_buffer_poly(b));

   case nir_intrinsic_load_tess_level_outer_default:
      return poly_tess_level_outer_default(b,
                                           nir_load_tess_param_buffer_poly(b));

   case nir_intrinsic_load_tess_level_inner_default:
      return poly_tess_level_inner_default(b,
                                           nir_load_tess_param_buffer_poly(b));

   case nir_intrinsic_load_output: {
      nir_def *addr = tcs_out_addr(b, intr, nir_undef(b, 1, 32), compact_levels);
      nir_def *value = nir_load_global(b, intr->def.num_components, 32, addr,
                                       .align_mul = 4);
      return nir_u2uN(b, value, intr->def.bit_size);
   }

   case nir_intrinsic_load_per_vertex_output: {
      nir_def *addr = tcs_out_addr(b, intr, intr->src[0].ssa, compact_levels);
      nir_def *value = nir_load_global(b, intr->def.num_components, 32, addr,
                                       .align_mul = 4);
      return nir_u2uN(b, value, intr->def.bit_size);
   }

   case nir_intrinsic_store_output: {
      /* Only vec2, make sure we can't overwrite */
      assert(intr->src[0].ssa->num_components <= 2 ||
             nir_intrinsic_io_semantics(intr).location !=
                VARYING_SLOT_TESS_LEVEL_INNER);

      nir_store_global(b, nir_u2u32(b, intr->src[0].ssa),
                       tcs_out_addr(b, intr, nir_undef(b, 1, 32), compact_levels),
                       .write_mask = nir_intrinsic_write_mask(intr));
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;
   }

   case nir_intrinsic_store_per_vertex_output: {
      nir_store_global(b, nir_u2u32(b, intr->src[0].ssa),
                       tcs_out_addr(b, intr, intr->src[1].ssa, compact_levels),
                       .write_mask = nir_intrinsic_write_mask(intr));
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;
   }

   default:
      return NULL;
   }
}

struct tcs_options {
   bool ignore_barriers;
   bool compact_levels;
};

static bool
lower_tcs(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   b->cursor = nir_before_instr(&intr->instr);

   const struct tcs_options *options = data;
   nir_def *repl = lower_tcs_impl(b, intr, options->ignore_barriers,
                                 options->compact_levels);
   if (!repl)
      return false;

   if (repl != NIR_LOWER_INSTR_PROGRESS_REPLACE)
      nir_def_rewrite_uses(&intr->def, repl);

   nir_instr_remove(&intr->instr);
   return true;
}

bool
poly_nir_lower_tcs_with_options(nir_shader *tcs,
                                bool can_ignore_shader_out_barriers,
                                bool compact_levels)
{
   struct tcs_options options = {can_ignore_shader_out_barriers, compact_levels};
   return nir_shader_intrinsics_pass(tcs, lower_tcs, nir_metadata_control_flow,
                                     &options);
}

bool
poly_nir_lower_tcs(nir_shader *tcs, bool can_ignore_shader_out_barriers)
{
   return poly_nir_lower_tcs_with_options(tcs, can_ignore_shader_out_barriers,
                                         false);
}

static nir_def *
lower_tes_impl(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_tess_coord_xy:
      return poly_load_tess_coord(b, nir_load_tess_param_buffer_poly(b),
                                  nir_load_vertex_id(b));

   case nir_intrinsic_load_primitive_id:
      return poly_tes_patch_id(b, nir_load_tess_param_buffer_poly(b),
                               nir_load_vertex_id(b));

   case nir_intrinsic_load_input:
   case nir_intrinsic_load_per_vertex_input:
   case nir_intrinsic_load_tess_level_inner:
   case nir_intrinsic_load_tess_level_outer:
      return lower_tes_load(b, intr);

   case nir_intrinsic_load_patch_vertices_in:
      return poly_tes_patch_vertices_in(b, nir_load_tess_param_buffer_poly(b));

   default:
      return NULL;
   }
}

static bool
lower_tes(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *repl = lower_tes_impl(b, intr, data);

   if (repl) {
      nir_def_replace(&intr->def, repl);
      return true;
   } else {
      return false;
   }
}

static bool
lower_tes_indexing(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_vertex_id)
      return false;

   b->cursor = nir_before_instr(&intr->instr);
   nir_def *p = nir_load_tess_param_buffer_poly(b);
   nir_def *id = nir_channel(b, nir_load_global_invocation_id(b, 32), 0);
   nir_def_replace(&intr->def, poly_load_tes_index(b, p, id));
   return true;
}

bool
poly_nir_lower_tes(nir_shader *tes, bool to_hw_vs)
{
   nir_lower_tess_coord_z(
      tes, tes->info.tess._primitive_mode == TESS_PRIMITIVE_TRIANGLES);

   /* A geometry shader after tessellation reads the patch's primitive ID, not
    * the index of the tessellated primitive. Hand it the patch ID with each
    * vertex; see gs_api_primitive_id(). */
   if (!to_hw_vs) {
      nir_function_impl *impl = nir_shader_get_entrypoint(tes);
      nir_builder b = nir_builder_at(nir_after_impl(impl));
      nir_store_output(&b, nir_load_primitive_id(&b), nir_imm_int(&b, 0),
                       .write_mask = 1, .src_type = nir_type_uint32,
                       .io_semantics.location = VARYING_SLOT_PRIMITIVE_ID,
                       .io_semantics.num_slots = 1);
      tes->info.outputs_written |= VARYING_BIT_PRIMITIVE_ID;
   }

   nir_shader_intrinsics_pass(tes, lower_tes, nir_metadata_control_flow, NULL);

   /* Points mode renders as points, make sure we write point size for the HW */
   if (tes->info.tess.point_mode && to_hw_vs) {
      nir_lower_default_point_size(tes);
   }

   if (to_hw_vs) {
      /* We lower to a HW VS, so update the shader info so the compiler does the
       * right thing.
       */
      tes->info.stage = MESA_SHADER_VERTEX;
      memset(&tes->info.vs, 0, sizeof(tes->info.vs));
      tes->info.vs.tes_poly = true;
   } else {
      /* If we're running as a compute shader, we need to load from the index
       * buffer manually. Fortunately, this doesn't require a shader key:
       * tess-as-compute always use U32 index buffers.
       */
      nir_shader_intrinsics_pass(tes, lower_tes_indexing,
                                 nir_metadata_control_flow, NULL);
   }

   nir_lower_idiv(tes, &(nir_lower_idiv_options){.allow_fp16 = true});
   return nir_progress(true, nir_shader_get_entrypoint(tes), nir_metadata_none);
}
