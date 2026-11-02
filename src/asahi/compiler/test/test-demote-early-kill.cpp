/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */
#include <gtest/gtest.h>
#include "agx_test.h"
extern "C" {
#include "compiler/nir/nir_builder.h"
#include "agx_nir.h"
}

/* agx_nir_lower_sample_mask_deferred keeps demoted lanes alive as quad helpers
 * until the end of the shader unless it proves that demoted lanes never reach
 * a quad-dependent operation; then it kills and runs the depth/stencil test
 * right after the discard. The G17P form accumulates demotes the same way but
 * issues the update right after the last one, on every path. */
enum shape {
   GUARDED_NOT,
   GUARDED_ELSE,
   GUARDED_COMPLEMENT,
   UNGUARDED,
   OTHER_CONDITION,
   SAME_CONDITION,
   IF_DEMOTE_THEN_GUARDED,
   QUAD_ONLY_BEFORE,
   QUAD_IN_LOOP,
   GUARDED_PLAIN_LOOP,
   GUARDED_LOOP_AFTER,
   SUBGROUP_AFTER,
   NESTED_GUARDED,
   QUAD_BEFORE_SECOND_DEMOTE,
   ACTIVE_COUNT_AFTER,
   ACTIVE_INDEX_AFTER,
   LOWERED_QUAD_BALLOT_AFTER,
   DEMOTE_IN_LOOP_IN_ARM,
   DEMOTES_IN_NESTED_ARMS,
   MEMORY_WRITE_AFTER,
   SAMPLE_SHADING_AFTER,
   DEPTH_WRITE_AFTER,
   FORCED_EARLY_AFTER,
};

/* More work below a demote than splitting an if around the update costs. */
static nir_def *
lighting(nir_builder *b, nir_def *v)
{
   for (unsigned i = 0; i < 64; ++i)
      v = nir_fmul(b, v, nir_fadd_imm(b, v, i));
   return v;
}

static nir_shader *
build(enum shape s)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                  &agx_nir_options, "demote");
   b.shader->info.fs.uses_discard = true;
   b.shader->info.fs.needs_coarse_quad_helper_invocations = true;

   nir_def *xy = nir_load_frag_coord(&b);
   nir_def *x = nir_channel(&b, xy, 0), *y = nir_channel(&b, xy, 1);
   nir_def *c = nir_flt(&b, x, y);
   nir_def *d = nir_flt(&b, y, nir_imm_float(&b, 3.0));
   nir_def *ix = nir_f2i32(&b, x), *iy = nir_f2i32(&b, y);
   nir_def *out = nir_imm_float(&b, 0.0);
   nir_def *one = nir_imm_float(&b, 1.0);
   nir_def *two = nir_imm_float(&b, 2.0);

   if (s == QUAD_ONLY_BEFORE)
      out = nir_ddx(&b, x);

   if (s == IF_DEMOTE_THEN_GUARDED) {
      nir_push_if(&b, c);
      nir_demote(&b);
      nir_pop_if(&b, NULL);
   } else if (s != DEMOTE_IN_LOOP_IN_ARM && s != DEMOTES_IN_NESTED_ARMS) {
      nir_demote_if(&b, s == GUARDED_COMPLEMENT ? nir_ieq(&b, ix, iy) : c);
   }

   nir_def *v;
   switch (s) {
   case GUARDED_NOT:
   case IF_DEMOTE_THEN_GUARDED:
   case NESTED_GUARDED: {
      nir_if *nif = nir_push_if(&b, nir_inot(&b, c));
      if (s == NESTED_GUARDED) {
         nir_push_if(&b, d);
         v = nir_ddx(&b, y);
         nir_pop_if(&b, NULL);
         v = nir_if_phi(&b, v, one);
      } else {
         v = nir_ddy(&b, y);
      }
      nir_pop_if(&b, nif);
      out = nir_if_phi(&b, v, two);
      break;
   }
   case QUAD_BEFORE_SECOND_DEMOTE: {
      /* Split the top-level blocks with a guarded derivative. The next
       * block's derivative is reachable by lanes demoted in the first block,
       * even though it precedes this block's own demote. */
      nir_push_if(&b, nir_inot(&b, c));
      v = nir_ddx(&b, y);
      nir_pop_if(&b, NULL);
      out = nir_if_phi(&b, v, two);
      out = nir_fadd(&b, out, nir_ddy(&b, x));
      nir_demote_if(&b, c);
      break;
   }
   case GUARDED_ELSE: {
      nir_if *nif = nir_push_if(&b, c);
      nir_push_else(&b, nif);
      v = nir_ddx(&b, y);
      nir_pop_if(&b, nif);
      out = nir_if_phi(&b, two, v);
      break;
   }
   case GUARDED_COMPLEMENT: {
      nir_if *nif = nir_push_if(&b, nir_ine(&b, iy, ix));
      v = nir_ddx(&b, y);
      nir_pop_if(&b, nif);
      out = nir_if_phi(&b, v, two);
      break;
   }
   case UNGUARDED:
      out = nir_ddx(&b, y);
      break;
   case OTHER_CONDITION:
   case SAME_CONDITION: {
      nir_if *nif = nir_push_if(&b, s == OTHER_CONDITION ? d : c);
      v = nir_ddx(&b, y);
      nir_pop_if(&b, nif);
      out = nir_if_phi(&b, v, two);
      break;
   }
   case QUAD_IN_LOOP: {
      nir_loop *loop = nir_push_loop(&b);
      v = nir_ddx(&b, y);
      nir_break_if(&b, nir_flt(&b, v, x));
      nir_pop_loop(&b, loop);
      out = x;
      break;
   }
   case GUARDED_LOOP_AFTER: {
      nir_if *nif = nir_push_if(&b, nir_inot(&b, c));
      nir_tex_instr *tex = nir_tex_instr_create(b.shader, 2);
      tex->op = nir_texop_txl;
      tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
      tex->dest_type = nir_type_float32;
      tex->coord_components = 2;
      tex->texture_index = 0;
      tex->sampler_index = 0;
      tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, nir_vec2(&b, x, y));
      tex->src[1] =
         nir_tex_src_for_ssa(nir_tex_src_lod, nir_imm_float(&b, 0.0f));
      nir_def_init(&tex->instr, &tex->def, 4, 32);
      nir_builder_instr_insert(&b, &tex->instr);
      nir_loop *loop = nir_push_loop(&b);
      v = nir_fadd(&b, nir_channel(&b, &tex->def, 0), y);
      nir_break_if(&b, nir_flt(&b, v, x));
      nir_pop_loop(&b, loop);
      nir_pop_if(&b, nif);
      out = nir_if_phi(&b, v, two);
      break;
   }
   case GUARDED_PLAIN_LOOP: {
      nir_if *nif = nir_push_if(&b, nir_inot(&b, c));
      nir_loop *loop = nir_push_loop(&b);
      v = nir_fadd(&b, x, y);
      nir_break_if(&b, nir_flt(&b, v, x));
      nir_pop_loop(&b, loop);
      nir_pop_if(&b, nif);
      out = nir_if_phi(&b, v, two);
      break;
   }
   case SUBGROUP_AFTER:
      out = nir_u2f32(&b, nir_channel(&b, nir_ballot(&b, 1, 32, d), 0));
      break;
   case ACTIVE_COUNT_AFTER:
      out = nir_u2f32(&b, nir_load_active_subgroup_count_agx(&b, 32));
      break;
   case ACTIVE_INDEX_AFTER:
      out = nir_u2f32(&b, nir_load_active_subgroup_invocation_agx(&b, 32));
      break;
   case LOWERED_QUAD_BALLOT_AFTER:
      out = nir_u2f32(&b, nir_quad_ballot_agx(&b, 16, d));
      break;
   case QUAD_ONLY_BEFORE:
      out = nir_fadd(&b, out, x);
      break;
   case DEMOTE_IN_LOOP_IN_ARM: {
      /* TF3's forward shaders: a cheap arm, and a lit arm whose only discard
       * sits in the light loop ahead of the work that needs derivatives. */
      nir_if *nif = nir_push_if(&b, c);
      nir_push_else(&b, nif);
      nir_loop *loop = nir_push_loop(&b);
      nir_demote_if(&b, d);
      nir_break_if(&b, nir_flt(&b, y, x));
      nir_pop_loop(&b, loop);
      v = nir_ddx(&b, lighting(&b, y));
      nir_pop_if(&b, nif);
      out = nir_if_phi(&b, two, v);
      break;
   }
   case DEMOTES_IN_NESTED_ARMS: {
      /* Lit twice below one if, a discard in each: phys_transp_nrml_map_light.
       * Values and the inner condition cross the split. */
      nir_if *outer = nir_push_if(&b, c);
      nir_def *inner_cond = nir_flt(&b, nir_fadd(&b, x, y), two);
      nir_if *inner = nir_push_if(&b, inner_cond);
      nir_def *m = nir_fmul(&b, x, x);
      nir_demote_if(&b, d);
      v = nir_ddx(&b, lighting(&b, m));
      nir_push_else(&b, inner);
      nir_def *n = nir_fmul(&b, y, y);
      nir_demote_if(&b, d);
      nir_def *w = nir_ddy(&b, lighting(&b, n));
      nir_pop_if(&b, inner);
      v = nir_if_phi(&b, v, w);
      v = nir_fadd(&b, v, nir_ddx(&b, x));
      nir_push_else(&b, outer);
      nir_def *u = nir_fadd(&b, y, one);
      nir_pop_if(&b, outer);
      out = nir_if_phi(&b, v, u);
      break;
   }
   case MEMORY_WRITE_AFTER:
      out = nir_ddx(&b, y);
      b.shader->info.writes_memory = true;
      break;
   case SAMPLE_SHADING_AFTER:
      out = nir_ddx(&b, y);
      b.shader->info.fs.uses_sample_shading = true;
      break;
   case DEPTH_WRITE_AFTER:
      out = nir_ddx(&b, y);
      b.shader->info.outputs_written |= BITFIELD64_BIT(FRAG_RESULT_DEPTH);
      break;
   case FORCED_EARLY_AFTER:
      out = nir_ddx(&b, y);
      b.shader->info.fs.early_fragment_tests = true;
      break;
   }

   agx_test_store_output(&b, out, nir_imm_int(&b, 0), nir_type_float32,
                         FRAG_RESULT_DATA0);
   return b.shader;
}

/* The deferred lowering appends the test-triggering update after all other
 * work; the immediate one places it where the discard was, ahead of the
 * output. */
static bool
kill_is_early(nir_shader *s)
{
   bool mask = false, after = false;
   nir_foreach_block(block, nir_shader_get_entrypoint(s)) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         /* The test-triggering coverage update; kill-only updates (dead
          * quads) have a constant zero live mask. */
         if (intr->intrinsic == nir_intrinsic_sample_mask_agx &&
             !(nir_src_is_const(intr->src[1]) &&
               nir_src_as_uint(intr->src[1]) == 0))
            mask = true;
         else if (mask && intr->intrinsic == nir_intrinsic_store_output)
            after = true;
      }
   }
   EXPECT_TRUE(mask);
   return after;
}

static bool
lower(enum shape s)
{
   nir_shader *nir = build(s);
   agx_nir_lower_discard_zs_emit(nir);
   agx_nir_lower_sample_mask_deferred(nir);
   nir_validate_shader(nir, "demote lowering");
   bool early = kill_is_early(nir);
   ralloc_free(nir);
   return early;
}

class DemoteEarlyKill : public ::testing::Test {
 protected:
   DemoteEarlyKill()
   {
      glsl_type_singleton_init_or_ref();
   }
   ~DemoteEarlyKill()
   {
      glsl_type_singleton_decref();
   }
};

static unsigned
count_quad_ballots(nir_shader *s)
{
   unsigned n = 0;
   nir_foreach_block(block, nir_shader_get_entrypoint(s)) {
      nir_foreach_instr(instr, block) {
         n += instr->type == nir_instr_type_intrinsic &&
              nir_instr_as_intrinsic(instr)->intrinsic ==
                 nir_intrinsic_quad_ballot_agx;
      }
   }
   return n;
}

/* G17P updates coverage right after the last demote. */
static bool
lower_g17p(enum shape s)
{
   nir_shader *nir = build(s);
   agx_nir_lower_discard_zs_emit(nir);
   agx_nir_lower_sample_mask_deferred_g17p(nir);
   nir_validate_shader(nir, "G17P demote lowering");
   bool early = kill_is_early(nir);
   ralloc_free(nir);
   return early;
}

struct updates {
   unsigned min, max;
};

/* Coverage updates over the paths through a list. */
static updates
count_updates(struct exec_list *list)
{
   updates u = {0, 0};
   foreach_list_typed(nir_cf_node, node, node, list) {
      updates n = {0, 0};
      if (node->type == nir_cf_node_block) {
         nir_foreach_instr(instr, nir_cf_node_as_block(node)) {
            if (instr->type == nir_instr_type_intrinsic &&
                nir_instr_as_intrinsic(instr)->intrinsic ==
                   nir_intrinsic_sample_mask_agx)
               n.min = ++n.max;
         }
      } else if (node->type == nir_cf_node_if) {
         nir_if *nif = nir_cf_node_as_if(node);
         updates a = count_updates(&nif->then_list);
         updates b = count_updates(&nif->else_list);
         n = {MIN2(a.min, b.min), MAX2(a.max, b.max)};
      } else {
         updates body = count_updates(&nir_cf_node_as_loop(node)->body);
         EXPECT_EQ(body.max, 0u);
      }
      u.min += n.min;
      u.max += n.max;
   }
   return u;
}

TEST_F(DemoteEarlyKill, G17PUpdatesOnceOnEveryPathAfterTheLastDemote)
{
   for (enum shape s :
        {UNGUARDED, OTHER_CONDITION, QUAD_IN_LOOP, GUARDED_LOOP_AFTER,
         QUAD_BEFORE_SECOND_DEMOTE, DEMOTE_IN_LOOP_IN_ARM,
         DEMOTES_IN_NESTED_ARMS}) {
      EXPECT_TRUE(lower_g17p(s)) << s;

      nir_shader *nir = build(s);
      agx_nir_lower_discard_zs_emit(nir);
      agx_nir_lower_sample_mask_deferred_g17p(nir);
      updates u = count_updates(&nir_shader_get_entrypoint(nir)->body);
      EXPECT_EQ(u.min, 1u) << s;
      EXPECT_EQ(u.max, 1u) << s;
      EXPECT_EQ(count_quad_ballots(nir), 0u) << s;
      ralloc_free(nir);
   }
}

/* One SIMD group must not run two testing updates, so the update sits in
 * top-level control flow, where every lane reaches it, and the derivatives
 * below the demotes come after it. */
TEST_F(DemoteEarlyKill, G17PSplitsIfsAroundASingleTopLevelUpdate)
{
   for (enum shape s : {DEMOTE_IN_LOOP_IN_ARM, DEMOTES_IN_NESTED_ARMS}) {
      nir_shader *nir = build(s);
      agx_nir_lower_discard_zs_emit(nir);
      agx_nir_lower_sample_mask_deferred_g17p(nir);
      nir_validate_shader(nir, "split if");
      nir_function_impl *impl = nir_shader_get_entrypoint(nir);

      unsigned top_level_updates = 0;
      bool deriv_after_update = false;
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_op op = nir_instr_as_intrinsic(instr)->intrinsic;
            if (op == nir_intrinsic_sample_mask_agx) {
               EXPECT_EQ(block->cf_node.parent, &impl->cf_node) << s;
               top_level_updates++;
            }
            deriv_after_update |= top_level_updates && op == nir_intrinsic_ddx;
         }
      }
      EXPECT_EQ(top_level_updates, 1u) << s;
      EXPECT_TRUE(deriv_after_update) << s;
      ralloc_free(nir);
   }
}

TEST_F(DemoteEarlyKill, G17PKeepsTheUpdateLateWhereItIsObservable)
{
   EXPECT_FALSE(lower_g17p(SUBGROUP_AFTER));
   EXPECT_FALSE(lower_g17p(ACTIVE_COUNT_AFTER));
   EXPECT_FALSE(lower_g17p(ACTIVE_INDEX_AFTER));
   EXPECT_FALSE(lower_g17p(LOWERED_QUAD_BALLOT_AFTER));
   EXPECT_FALSE(lower_g17p(MEMORY_WRITE_AFTER));
   EXPECT_FALSE(lower_g17p(SAMPLE_SHADING_AFTER));
}

/* These two have other reasons to keep a coverage update at the end: it is a
 * pure kill there, with the tests already run or left to zs_emit. */
TEST_F(DemoteEarlyKill, G17PDepthWritesAndForcedEarlyTestsStayLate)
{
   for (enum shape s : {DEPTH_WRITE_AFTER, FORCED_EARLY_AFTER}) {
      nir_shader *nir = build(s);
      agx_nir_lower_discard_zs_emit(nir);
      agx_nir_lower_sample_mask_deferred_g17p(nir);
      nir_validate_shader(nir, "late coverage");

      nir_block *last = nir_impl_last_block(nir_shader_get_entrypoint(nir));
      bool kill_in_last_block = false;
      nir_foreach_instr(instr, last) {
         kill_in_last_block |=
            instr->type == nir_instr_type_intrinsic &&
            nir_instr_as_intrinsic(instr)->intrinsic ==
               nir_intrinsic_sample_mask_agx;
      }
      EXPECT_TRUE(kill_in_last_block) << s;
      ralloc_free(nir);
   }
}

TEST_F(DemoteEarlyKill, GuardedQuadOperationsAllowEarlyKill)
{
   EXPECT_TRUE(lower(GUARDED_NOT));
   EXPECT_TRUE(lower(GUARDED_ELSE));
   EXPECT_TRUE(lower(GUARDED_COMPLEMENT));
   EXPECT_TRUE(lower(IF_DEMOTE_THEN_GUARDED));
   EXPECT_TRUE(lower(NESTED_GUARDED));
   EXPECT_TRUE(lower(QUAD_ONLY_BEFORE));
   EXPECT_TRUE(lower(GUARDED_PLAIN_LOOP));
}

TEST_F(DemoteEarlyKill, ReachableLaneDependentOperationsKeepDeferredKill)
{
   EXPECT_FALSE(lower(UNGUARDED));
   EXPECT_FALSE(lower(OTHER_CONDITION));
   EXPECT_FALSE(lower(SAME_CONDITION));
   EXPECT_FALSE(lower(QUAD_IN_LOOP));
   EXPECT_FALSE(lower(GUARDED_LOOP_AFTER));
   EXPECT_FALSE(lower(SUBGROUP_AFTER));
}

/* A fully demoted quad is killed right after the demote; a shader with a
 * non-quad subgroup operation keeps every demoted lane until the end. */
TEST_F(DemoteEarlyKill, FullyDemotedQuadsAreKilledEarly)
{
   for (enum shape s : {UNGUARDED, OTHER_CONDITION, SUBGROUP_AFTER}) {
      nir_shader *nir = build(s);
      agx_nir_lower_discard_zs_emit(nir);
      agx_nir_lower_sample_mask_deferred(nir);
      nir_validate_shader(nir, "dead quads");
      EXPECT_EQ(count_quad_ballots(nir), s == SUBGROUP_AFTER ? 0u : 1u);
      ralloc_free(nir);
   }
}

/* A kill-only coverage update ahead of a sampled loop is still a kill ahead
 * of that loop, so a shader with one keeps every update at the end. */
TEST_F(DemoteEarlyKill, NoDeadQuadKillBeforeSampledLoop)
{
   nir_shader *nir = build(GUARDED_LOOP_AFTER);
   agx_nir_lower_discard_zs_emit(nir);
   agx_nir_lower_sample_mask_deferred(nir);
   nir_validate_shader(nir, "sampled loop");
   EXPECT_EQ(count_quad_ballots(nir), 0u);
   ralloc_free(nir);
}

TEST_F(DemoteEarlyKill, EarlierDemoteReachesQuadBeforeLaterDemote)
{
   EXPECT_FALSE(lower(QUAD_BEFORE_SECOND_DEMOTE));
}

TEST_F(DemoteEarlyKill, LoweredLaneOperationsKeepDeferredKill)
{
   EXPECT_FALSE(lower(ACTIVE_COUNT_AFTER));
   EXPECT_FALSE(lower(ACTIVE_INDEX_AFTER));
   EXPECT_FALSE(lower(LOWERED_QUAD_BALLOT_AFTER));

   for (enum shape s : {ACTIVE_COUNT_AFTER, ACTIVE_INDEX_AFTER}) {
      nir_shader *nir = build(s);
      agx_nir_lower_discard_zs_emit(nir);
      agx_nir_lower_sample_mask_deferred(nir);
      nir_validate_shader(nir, "lowered subgroup operations");
      EXPECT_EQ(count_quad_ballots(nir), 0u);
      ralloc_free(nir);
   }
}

/* M5 final coverage update: the testing form (all samples targeted, the
 * surviving set live) runs in the then-arm of an if on the shader part's
 * tests_zs uniform, and the pure kill in its else-arm. No coverage update
 * takes its target from tests_zs | killed, which the per-lane test split
 * would otherwise turn into a divergent testing arm. */
static void
expect_m5_final_update(nir_shader *s)
{
   unsigned testing = 0, fused = 0;
   nir_foreach_block(block, nir_shader_get_entrypoint(s)) {
      nir_cf_node *parent = block->cf_node.parent;
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic != nir_intrinsic_sample_mask_agx)
            continue;
         const bool live_zero = nir_src_is_const(intr->src[1]) &&
                                nir_src_as_uint(intr->src[1]) == 0;
         if (!nir_src_is_const(intr->src[0])) {
            nir_instr *t = nir_def_instr(intr->src[0].ssa);
            if (t->type == nir_instr_type_alu &&
                nir_instr_as_alu(t)->op == nir_op_ior)
               fused++;
         }
         if (live_zero)
            continue;
         testing++;
         EXPECT_TRUE(nir_src_is_const(intr->src[0]) &&
                     (nir_src_as_uint(intr->src[0]) & 0xff) == 0xff);
         ASSERT_EQ(parent->type, nir_cf_node_if);
         nir_if *nif = nir_cf_node_as_if(parent);
         nir_def *cond = nif->condition.ssa;
         nir_instr *ci = nir_def_instr(cond);
         ASSERT_EQ(ci->type, nir_instr_type_alu);
         nir_instr *src = nir_def_instr(nir_instr_as_alu(ci)->src[0].src.ssa);
         ASSERT_EQ(src->type, nir_instr_type_intrinsic);
         EXPECT_EQ(nir_instr_as_intrinsic(src)->intrinsic,
                   nir_intrinsic_load_shader_part_tests_zs_agx);
      }
   }
   EXPECT_EQ(testing, 1u);
   EXPECT_EQ(fused, 0u);
}

TEST_F(DemoteEarlyKill, M5FinalCoverageUpdateIsUniform)
{
   for (enum shape s : {UNGUARDED, GUARDED_NOT, QUAD_IN_LOOP}) {
      nir_shader *nir = build(s);
      agx_nir_lower_discard_zs_emit(nir);
      agx_nir_lower_sample_mask_deferred_m5(nir);
      nir_validate_shader(nir, "M5 coverage update");
      expect_m5_final_update(nir);
      ralloc_free(nir);
   }
}
