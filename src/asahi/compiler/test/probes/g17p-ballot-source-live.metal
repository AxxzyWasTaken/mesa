// SPDX-License-Identifier: MIT
#include <metal_stdlib>
using namespace metal;

kernel void ballot_dead(device uint *out [[buffer(0)]],
                        uint tid [[thread_position_in_grid]])
{
   int x = as_type<int>(tid * tid);
   uint b = uint(simd_vote::vote_t(simd_ballot(x < 0)));
   out[tid] = b;
}

kernel void ballot_live(device uint *out [[buffer(0)]],
                        uint tid [[thread_position_in_grid]])
{
   int x = as_type<int>(tid * tid);
   uint b = uint(simd_vote::vote_t(simd_ballot(x < 0)));
   out[tid] = b + x;
}
