# Two half comparands selecting a full word

`mixed-width-select.metal` supplies twelve independent native compute witnesses.
They were compiled with Apple's Metal toolchain27.1.266.1 for `applegpu_g17g`
and `applegpu_g18g`, using Metal3.0 and macOS27.0. This is compiler/encoding
evidence; these particular probes have not been executed on physical hardware.

The original `KHR-GL46.gpu_shader_fp64.fp64.conversions` case reaches an
`icmpsel` with two 16-bit register comparands and two 32-bit selected registers.
The old `encode_gpu1()` rejected it solely because the destination was wide.

The native instruction is `iseli32`. Fields24/39 stay1 for the narrow comparison;
fields10/54/68 are2 for the destination and full-width selected operands. Both
extension fields28/43 are1 for equality and signed relations, and0 for unsigned
relations. Equality is independent of sign. The live variants independently
exercise destination/comparand/selection cache and discard hints; waits retain
their ordinary fields74–81.

Representative G18G native bytes:

| Probe | Bytes |
| --- | --- |
| `u16_eq_word` | `da010a0a2c01412020022000` |
| `u16_ne_word` | `da010a0a2c03412020002000` |
| `u16_lt_word` | `da010a0a60012102` |
| `u16_gt_word` | `da010a0a220141402002` |
| `s16_lt_word` | `da010a0a240141a020022000` |
| `s16_gt_word` | `da010a0a240141e020022000` |
| `u16_eq_imm_true` | `da0302023c87412000002000` |
| `u16_eq_imm_false` | `da0302020c01412070fe2000` |

The production encoder reproduces the twelve G17G instructions byte-for-byte;
the same field vectors reproduce all twelve G18G instructions through their
native table forms. Existing half-result restrictions stay unchanged. Newly
admitted wide-result immediate selections require equality and one register
selection; signed/unsigned relations are admitted with two selected registers.

To regenerate, compile the `.metal` file to AIR and a metallib, then run
`metal-nt -arch applegpu_g17g` and `-arch applegpu_g18g` with the supplied pipeline
JSON. The J873 investigation273 receipt retains full commands, compiler hashes,
objects, decoded fields, before/after encoder comparisons and original CTS logs.
