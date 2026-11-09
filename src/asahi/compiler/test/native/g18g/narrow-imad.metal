#include <metal_stdlib>
using namespace metal;

kernel void mul_u16(device const ushort2 *input [[buffer(0)]],
                    device uint *output [[buffer(1)]],
                    uint id [[thread_position_in_grid]])
{
    ushort2 x = input[id];
    output[id] = uint(x.x) * uint(x.y);
}

kernel void mul_u16_live(device const ushort2 *input [[buffer(0)]],
                         device uint4 *output [[buffer(1)]],
                         uint id [[thread_position_in_grid]])
{
    ushort2 x = input[id];
    output[id] = uint4(uint(x.x) * uint(x.y), x.x, x.y, 0);
}

kernel void mul_s16(device const short2 *input [[buffer(0)]],
                    device int *output [[buffer(1)]],
                    uint id [[thread_position_in_grid]])
{
    short2 x = input[id];
    output[id] = int(x.x) * int(x.y);
}

kernel void mul_s16_u16(device const short *left [[buffer(0)]],
                        device const ushort *right [[buffer(1)]],
                        device int *output [[buffer(2)]],
                        uint id [[thread_position_in_grid]])
{
    output[id] = int(left[id]) * int(right[id]);
}

kernel void mad_u16(device const ushort2 *input [[buffer(0)]],
                    device const uint *addend [[buffer(1)]],
                    device uint *output [[buffer(2)]],
                    uint id [[thread_position_in_grid]])
{
    ushort2 x = input[id];
    output[id] = uint(x.x) * uint(x.y) + addend[id];
}

kernel void mul_u32_u16(device const uint *left [[buffer(0)]],
                        device const ushort *right [[buffer(1)]],
                        device uint *output [[buffer(2)]],
                        uint id [[thread_position_in_grid]])
{
    output[id] = left[id] * uint(right[id]);
}

kernel void mul_i32_i16(device const int *left [[buffer(0)]],
                        device const short *right [[buffer(1)]],
                        device int *output [[buffer(2)]],
                        uint id [[thread_position_in_grid]])
{
    output[id] = left[id] * int(right[id]);
}
