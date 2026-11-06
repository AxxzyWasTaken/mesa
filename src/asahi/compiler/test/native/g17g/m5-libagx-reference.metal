// SPDX-License-Identifier: MIT
//
// Instruction shapes libagx's precompiled kernels reach on G17G (agx3c), and
// through the transcoder on G18G: asahi_clc's g18g target compiles them as
// G17G. Compared and computed values
// are ALU results unless a kernel says otherwise, so no load wait rides on
// the instruction under test. Compiled for applegpu_g17g and applegpu_g18g
// by ../g18g/build-reference.py.
#include <metal_stdlib>
using namespace metal;

// Immediate minus a register (isub32, immediate in slot A): libagx's ineg,
// 4 - x and 31 - x, with the source dead, live, narrow and loaded.
kernel void isub_imm0_dead(device const uint4 *in [[buffer(0)]], device uint *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ uint x = in[id].x * in[id].y; o[id] = 0u - x; }
kernel void isub_imm0_live(device const uint4 *in [[buffer(0)]], device uint *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ uint x = in[id].x * in[id].y; o[id] = 0u - x; o[id + 64] = x ^ in[id].z; }
kernel void isub_imm4_dead(device const uint4 *in [[buffer(0)]], device uint *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ uint x = in[id].x * in[id].y; o[id] = 4u - x; }
kernel void isub_imm31_dead(device const uint4 *in [[buffer(0)]], device uint *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ uint x = in[id].x * in[id].y; o[id] = 31u - x; }
kernel void isub_imm31_live(device const uint4 *in [[buffer(0)]], device uint *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ uint x = in[id].x * in[id].y; o[id] = 31u - x; o[id + 64] = x ^ in[id].z; }
kernel void isub_imm255_dead(device const uint4 *in [[buffer(0)]], device uint *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ uint x = in[id].x * in[id].y; o[id] = 255u - x; }
kernel void isub_imm0_int(device const int4 *in [[buffer(0)]], device int *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ int x = in[id].x * in[id].y; o[id] = -x; }
kernel void isub_imm_short(device const short4 *in [[buffer(0)]], device int *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ short x = in[id].x * in[id].y; o[id] = 16 - int(x); }
kernel void isub_imm_ushort(device const ushort4 *in [[buffer(0)]], device uint *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ ushort x = in[id].x * in[id].y; o[id] = 16u - uint(x); }
kernel void isub_imm_loaded(device const uint *in [[buffer(0)]], device uint *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ o[id] = 31u - in[id]; }

// Scalar byte loads: a uchar array indexed by a 32-bit id, strided, into a
// ushort, through a loaded pointer, signed, and two in flight.
kernel void ld_u8(device const uchar *p [[buffer(0)]], device uint *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ o[id] = uint(p[id]) * 3u; }
kernel void ld_u8_stride4(device const uchar *p [[buffer(0)]], device uint *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ o[id] = uint(p[id * 4u]) + 1u; }
kernel void ld_u8_half(device const uchar *p [[buffer(0)]], device ushort *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ o[id] = ushort(p[id]) + ushort(5); }
struct u8_ptr { device const uchar *p; };
kernel void ld_u8_ptr(device const u8_ptr *pp [[buffer(0)]], device uint *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ device const uchar *p = pp[id & 7u].p; o[id] = uint(p[id]) ^ 9u; }
kernel void ld_i8(device const char *p [[buffer(0)]], device int *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ o[id] = int(p[id]) * 3; }
kernel void ld_u8_two(device const uchar *p [[buffer(0)]], device uint *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ o[id] = uint(p[id]) + uint(p[id + 1000u]) * 7u; }

// popcount (cbs) of ALU results, a live source, a half, and loaded words
// waiting on load slots 8 and 1.
kernel void popcnt32(device const uint4 *in [[buffer(0)]], device uint *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ uint x = in[id].x * in[id].y; o[id] = popcount(x); }
kernel void popcnt32_live(device const uint4 *in [[buffer(0)]], device uint *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ uint x = in[id].x * in[id].y; o[id] = popcount(x) + x; }
kernel void popcnt16(device const ushort4 *in [[buffer(0)]], device ushort *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ ushort x = in[id].x * in[id].y; o[id] = popcount(x); }
kernel void popcnt_loaded(device const uint *in [[buffer(0)]], device uint *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ o[id] = popcount(in[id]); }
kernel void popcnt_two_loads(device const uint *a [[buffer(0)]], device const uint *b [[buffer(1)]], device uint *o [[buffer(2)]], uint id [[thread_position_in_grid]])
{ uint x = a[id], y = b[id * 3]; o[id] = popcount(x) + popcount(y); }

// 32-bit and-not, and a 16-bit AND reading a live word's low half.
kernel void andc32_dead(device const uint4 *in [[buffer(0)]], device uint *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ uint x = in[id].x * in[id].y, y = in[id].z * in[id].w; o[id] = x & ~y; }
kernel void andc32_live(device const uint4 *in [[buffer(0)]], device uint *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ uint x = in[id].x * in[id].y, y = in[id].z * in[id].w; o[id] = x & ~y; o[id + 64] = x + y; }
kernel void andc32_swap(device const uint4 *in [[buffer(0)]], device uint *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ uint x = in[id].x * in[id].y, y = in[id].z * in[id].w; o[id] = ~x & y; o[id + 64] = x; }
kernel void and16_of32(device const uint4 *in [[buffer(0)]], device const ushort4 *h [[buffer(1)]], device ushort *o [[buffer(2)]], uint id [[thread_position_in_grid]])
{ uint x = in[id].x * in[id].y; ushort k = h[id].x * h[id].y; o[id] = ushort(x) & k; }
kernel void and16_of32_live(device const uint4 *in [[buffer(0)]], device const ushort4 *h [[buffer(1)]], device ushort *o [[buffer(2)]], device uint *o2 [[buffer(3)]], uint id [[thread_position_in_grid]])
{ uint x = in[id].x * in[id].y; ushort k = h[id].x * h[id].y; o[id] = ushort(x) & k; o2[id] = x; }

// Selects on a half compared for equality with 3, 4 and 255.
kernel void sel_heq4_w(device const ushort4 *h [[buffer(0)]], device const uint4 *in [[buffer(1)]], device uint *o [[buffer(2)]], uint id [[thread_position_in_grid]])
{ ushort k = h[id].x * h[id].y; uint w = in[id].x * in[id].y; o[id] = (k == 4) ? 0u : w; }
kernel void sel_heq255_w(device const ushort4 *h [[buffer(0)]], device const uint4 *in [[buffer(1)]], device uint *o [[buffer(2)]], uint id [[thread_position_in_grid]])
{ ushort k = h[id].x * h[id].y; uint w = in[id].x * in[id].y; o[id] = (k == 255) ? w : 7u; }
kernel void sel_heq3_h(device const ushort4 *h [[buffer(0)]], device ushort *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ ushort k = h[id].x * h[id].y; ushort j = h[id].z * h[id].w; o[id] = (k == 3) ? ushort(0) : j; }
kernel void sel_heq4_ww(device const ushort4 *h [[buffer(0)]], device const uint4 *in [[buffer(1)]], device uint *o [[buffer(2)]], uint id [[thread_position_in_grid]])
{ ushort k = h[id].x * h[id].y; uint w = in[id].x * in[id].y, v = in[id].z * in[id].w; o[id] = (k == 4) ? v : w; }
