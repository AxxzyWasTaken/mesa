// SPDX-License-Identifier: MIT
//
// Compare and select shapes for the G17G (agx3c) packer, and through the
// transcoder for G18G. The glm_* kernels are the instruction shapes glamor's
// own shaders reach; cmp_* are branches
// on a float compared with a constant, a negated/absolute/half operand and a
// loaded value; sel_* are 16-bit compare-selects. Every branch guards an
// atomic so it cannot be if-converted, and compared values are ALU results
// unless the kernel says otherwise, so no wait rides on the compare. Compiled
// for applegpu_g17g and applegpu_g18g by ../g18g/build-reference.py.
#include <metal_stdlib>
using namespace metal;

#define BODY(k) atomic_fetch_add_explicit(&out[k], 1u, memory_order_relaxed)

kernel void cmp_fimm_lt(device const float4 *in [[buffer(0)]], device atomic_uint *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y; if (x < 0.5f) BODY(0); }
kernel void cmp_fimm_gt(device const float4 *in [[buffer(0)]], device atomic_uint *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y; if (x > 1.0f) BODY(0); }
kernel void cmp_fimm_ge_zero(device const float4 *in [[buffer(0)]], device atomic_uint *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y; if (x >= 0.0f) BODY(0); }
kernel void cmp_fimm_le(device const float4 *in [[buffer(0)]], device atomic_uint *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y; if (x <= 2.0f) BODY(0); }
kernel void cmp_fimm_eq_zero(device const float4 *in [[buffer(0)]], device atomic_uint *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y; if (x == 0.0f) BODY(0); }
kernel void cmp_fimm_ne(device const float4 *in [[buffer(0)]], device atomic_uint *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y; if (x != 0.25f) BODY(0); }
kernel void cmp_fimm_neg(device const float4 *in [[buffer(0)]], device atomic_uint *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y; if (x < -1.0f) BODY(0); }
kernel void cmp_fimm_half(device const half4 *in [[buffer(0)]], device atomic_uint *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{ half x = in[id].x * in[id].y; if (x < 1.0h) BODY(0); }
kernel void cmp_fimm_loaded(device const float *in [[buffer(0)]], device atomic_uint *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{ if (in[id] < 0.5f) BODY(0); }
kernel void cmp_freg_neg(device const float4 *in [[buffer(0)]], device atomic_uint *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y, y = in[id].z * in[id].w; if (-x < y) BODY(0); }
kernel void cmp_freg_abs0(device const float4 *in [[buffer(0)]], device atomic_uint *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y, y = in[id].z * in[id].w; if (fabs(x) < y) BODY(0); }
kernel void cmp_freg_abs1(device const float4 *in [[buffer(0)]], device atomic_uint *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y, y = in[id].z * in[id].w; if (x < fabs(y)) BODY(0); }
kernel void cmp_freg_abs2(device const float4 *in [[buffer(0)]], device atomic_uint *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y, y = in[id].z * in[id].w; if (fabs(x) == fabs(y)) BODY(0); }
kernel void cmp_freg16(device const half4 *in [[buffer(0)]], device atomic_uint *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{ half x = in[id].x * in[id].y, y = in[id].z * in[id].w; if (x < y) BODY(0); }
kernel void cmp_freg_ge(device const float4 *in [[buffer(0)]], device atomic_uint *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y, y = in[id].z * in[id].w; if (x >= y) BODY(0); }
kernel void cmp_freg_live(device const float4 *in [[buffer(0)]], device float *o2 [[buffer(1)]], device atomic_uint *out [[buffer(2)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y, y = in[id].z * in[id].w; if (x < y) BODY(0); o2[id] = x + y; }

kernel void sel_f16_imm(device const float4 *in [[buffer(0)]], device ushort *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y, y = in[id].z * in[id].w; o[id] = (x < y) ? ushort(255) : ushort(0); }
kernel void sel_f16_imm_zero(device const float4 *in [[buffer(0)]], device ushort *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y; o[id] = (x < 0.0f) ? ushort(255) : ushort(0); }
kernel void sel_f32(device const float4 *in [[buffer(0)]], device float *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y, y = in[id].z * in[id].w; o[id] = (x < y) ? x : y + 1.0f; }
kernel void sel_i16_imm(device const uint4 *in [[buffer(0)]], device ushort *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ uint i = in[id].x * in[id].y; ushort s = ushort(in[id].z * in[id].w); o[id] = (int(i) < 64) ? s : ushort(1); }

kernel void glm_fma_negimm_abs(device const float4 *in [[buffer(0)]], device float *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y, y = in[id].z * in[id].w; o[id] = fma(-2.0f, x, fabs(y)); }
kernel void glm_fma_imm_mul(device const float4 *in [[buffer(0)]], device float *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y, y = in[id].z * in[id].w; o[id] = fma(x, 2.0f, y); }
kernel void glm_fma_imm_add(device const float4 *in [[buffer(0)]], device float *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y, y = in[id].z * in[id].w; o[id] = fma(x, y, 1.0f); }
kernel void glm_fma_abs_add(device const float4 *in [[buffer(0)]], device float *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y, y = in[id].z * in[id].w, z = in[id].x + in[id].w; o[id] = fma(x, y, fabs(z)); }
kernel void glm_fma_neg_imm_reg(device const float4 *in [[buffer(0)]], device float *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y, y = in[id].z * in[id].w; o[id] = fma(-2.0f, x, y); }
kernel void glm_fsel16_regs(device const float4 *in [[buffer(0)]], device const half4 *h [[buffer(1)]], device half *o [[buffer(2)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y, y = in[id].z * in[id].w; half a = h[id].x * h[id].y, b = h[id].z * h[id].w; o[id] = (x < y) ? a : b; }
kernel void glm_fsel16_eqzero_imm(device const float4 *in [[buffer(0)]], device ushort *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y; o[id] = (x == 0.0f) ? ushort(255) : ushort(0); }
kernel void glm_isel16_gt_imm(device const int4 *in [[buffer(0)]], device ushort *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ int i = in[id].x * in[id].y; o[id] = (i > 1) ? ushort(0) : ushort(1); }
kernel void glm_isel16_lt_imm(device const int4 *in [[buffer(0)]], device ushort *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ int i = in[id].x * in[id].y; o[id] = (i < 1) ? ushort(0) : ushort(1); }

kernel void sel_f16_reg_zero(device const float4 *in [[buffer(0)]], device const ushort2 *s2 [[buffer(1)]], device ushort *o [[buffer(2)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y; ushort s = s2[id].x * s2[id].y; o[id] = (x > 0.0f) ? s : ushort(0); }
kernel void glm_fma_negimm_absload(device const float4 *in [[buffer(0)]], device float *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ float x = in[id].x * in[id].y; o[id] = fma(-2.0f, x, fabs(in[id].z)); }
kernel void glm_isel16_lt_reg(device const int4 *in [[buffer(0)]], device ushort *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ int i = in[id].x * in[id].y, j = in[id].z * in[id].w; o[id] = (i < j) ? ushort(0) : ushort(1); }
