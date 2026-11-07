/* SPDX-License-Identifier: MIT */
#pragma once

#include "nir_builder.h"
#include "nir_builtin_builder.h"

/* HK-only flag, disjoint from the shared AGX texture flags. */
#define HK_TEXTURE_FLAG_COMBINED_BIAS (1u << 3)

static inline bool
hk_lower_g17p_sampler_bias(nir_builder *b, nir_tex_instr *tex, void *data)
{
   (void)data;
   if ((tex->op != nir_texop_txb && tex->op != nir_texop_txl) ||
       tex->is_shadow || (tex->backend_flags & HK_TEXTURE_FLAG_COMBINED_BIAS))
      return false;

   /* The sampler and instruction each truncate to signed 5:6 fixed point.
    * Adding those separately rounded values can exceed the filtering error
    * bound. Combine in float before the single instruction quantization,
    * selecting an otherwise identical zero-bias sampler for this sample. */
   b->cursor = nir_before_instr(&tex->instr);
   nir_def *bias = nir_build_texture_query(b, tex, nir_texop_lod_bias, 1,
                                          nir_type_float32, false, false);
   nir_instr_as_tex(nir_def_instr(bias))->backend_flags |=
      HK_TEXTURE_FLAG_COMBINED_BIAS;
   /* An explicit LOD takes the sampler's bias too, and the sum has to clamp as
    * one value: the hardware would clamp a negative LOD before biasing it. */
   nir_src *shader_bias =
      &tex->src[nir_tex_instr_src_index(tex, tex->op == nir_texop_txl
                                                ? nir_tex_src_lod
                                                : nir_tex_src_bias)].src;
   nir_src_rewrite(shader_bias,
                   nir_fadd(b, nir_f2f32(b, shader_bias->ssa),
                            nir_f2f32(b, bias)));
   tex->backend_flags |= HK_TEXTURE_FLAG_COMBINED_BIAS;
   return true;
}
