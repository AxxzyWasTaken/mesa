// SPDX-License-Identifier: MIT
#include <metal_stdlib>
using namespace metal;

// Full-width selections are deliberately independent of narrow comparands.
#define PROBE(NAME, TYPE, OP) \
kernel void NAME(device const ushort4 *comparison [[buffer(0)]], \
                 device const uint2 *choices [[buffer(1)]], \
                 device uint *output [[buffer(2)]], \
                 uint id [[thread_position_in_grid]]) \
{ \
   TYPE a = TYPE(comparison[id].y), b = TYPE(comparison[id].z); \
   uint2 v = choices[id]; \
   output[id] = (a OP b) ? v.x : v.y; \
}

PROBE(u16_eq_word, ushort, ==)
PROBE(u16_ne_word, ushort, !=)
PROBE(u16_lt_word, ushort, <)
PROBE(u16_gt_word, ushort, >)
PROBE(s16_eq_word, short, ==)
PROBE(s16_lt_word, short, <)
PROBE(s16_gt_word, short, >)

#define LIVE_PROBE(NAME, TYPE, OP) \
kernel void NAME(device const ushort4 *comparison [[buffer(0)]], \
                 device const uint2 *choices [[buffer(1)]], \
                 device uint4 *output [[buffer(2)]], \
                 uint id [[thread_position_in_grid]]) \
{ \
   TYPE a = TYPE(comparison[id].y), b = TYPE(comparison[id].z); \
   uint2 v = choices[id]; \
   uint selected = (a OP b) ? v.x : v.y; \
   output[id] = uint4(selected + 0x1234567u, uint(a), uint(b), v.x ^ v.y); \
}

LIVE_PROBE(u16_eq_word_live, ushort, ==)
LIVE_PROBE(u16_lt_word_live, ushort, <)
LIVE_PROBE(s16_lt_word_live, short, <)

kernel void u16_eq_imm_true(device const ushort2 *comparison [[buffer(0)]],
                            device const uint *choices [[buffer(1)]],
                            device uint2 *output [[buffer(2)]],
                            uint id [[thread_position_in_grid]])
{
   ushort2 c = comparison[id];
   uint v = choices[id];
   output[id] = uint2(v, select(v, 7u, c.x == c.y));
}

kernel void u16_eq_imm_false(device const ushort2 *comparison [[buffer(0)]],
                             device const uint *choices [[buffer(1)]],
                             device uint2 *output [[buffer(2)]],
                             uint id [[thread_position_in_grid]])
{
   ushort2 c = comparison[id];
   uint v = choices[id];
   output[id] = uint2(v, select(255u, v, c.x == c.y));
}
