# G17G and G18G 16-bit integer products

The line-culling fragment prolog reaches IMAD with two unsigned 16-bit
registers and a 32-bit destination. The old G18G encoder refuses this shape.
These native compiler probes distinguish the full product from a truncated
16-bit result, individual sign extension, input liveness and a register addend.

Reproduce the native references in an output directory with Metal toolchain
27.1.266.1, substituting the path to this directory for `$source`:

```sh
xcrun --sdk macosx metal -std=metal3.0 -mmacosx-version-min=27.0 \
  -c "$source/narrow-imad.metal" -o probe.air
xcrun --sdk macosx metallib probe.air -o probe.metallib
xcrun metal-nt -arch applegpu_g18g -platform_version macos 27.0 27.0 \
  -N "$source/narrow-imad.mtlp-json" probe.metallib -save-temps -o probe.gpubin
```

Repeat the final command with `-arch applegpu_g17g` and another output path
for the G17G reference. Mesa's G18G packer first emits G17G instructions and
then transcodes their field vectors, so both encodings must be grounded.

Each main program loads 16-bit operands and multiplies them directly, without
an intervening widening instruction. The retained `imadd32` bytes are:

| Function | Native bytes |
| --- | --- |
| `mul_u16` | `eb010000128041400000` |
| `mul_u16_live` | `2b018484128041400000` |
| `mul_s16` | `eb010000128041a000002000` |
| `mul_s16_u16` | `eb010000128041600000` |
| `mad_u16` | `eb010000220441400100` |
| `mul_u32_u16` | `eb010104128001400000` |
| `mul_i32_i16` | `eb010104128001600000` |

The corresponding G17G instructions are:

| Function | Native bytes |
| --- | --- |
| `mul_u16` | `2f00041a2100a102600020000102` |
| `mul_u16_live` | `2f80043a2100a102400110000102` |
| `mul_s16` | `2f00041a2100a102600020c00102` |
| `mul_s16_u16` | `2f00041a2100a102600020800102` |
| `mad_u16` | `2f00041a2100a1026000e0000400` |
| `mul_u32_u16` | `2f00041a2100a102280120000102` |
| `mul_i32_i16` | `2f00041a2100a102280120800102` |

In the first five probes fields 24 and 39 are both 1 (half-register inputs), while field 10 is 2
(32-bit output). Fields 28 and 43 independently sign-extend the inputs.
The live-input variant sets cache fields 25 and 40 in place of last-use
fields 27 and 42. A 32-bit register addend changes fields 53 and 54 to 2;
the pure product uses the immediate-zero endpoint with both fields 1.

Both generations use the same operand fields. These G17G programs wait on
slot 8; the corresponding G18G programs wait on slot 1.

The final two probes set field 24 to 2 for a 32-bit input in slot A, retaining
a half input in slot B. Only pure products are witnessed for this mixed-width
shape; a register addend remains admitted only with two half inputs.

This evidence admits G17G/G18G 16x16-to-32 and pure 32x16-to-32 products.
It does not establish immediate-addend, narrow-result or 64-bit-result forms,
nor hardware or CTS qualification from native byte equivalence alone.
