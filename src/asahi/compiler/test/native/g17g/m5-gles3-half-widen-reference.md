# G17G half-to-float conversion reference

The adjacent Metal source reads dynamic half values and writes their float
conversions. Its modifier variant exercises negation, absolute value, and
negative absolute value. These are compiler references, not replacement Mesa
shaders or GLES test outputs.

Generated with Apple Metal Toolchain 27.1.5194.15.Wc8g1g:

```sh
xcrun -sdk macosx metal -c -target air64-apple-macos26.0 -std=metal3.2 \
  -fno-fast-math m5-gles3-half-widen-reference.metal -o reference.air
xcrun metallib reference.air -o reference.metallib
xcrun metal-nt -arch applegpu_g17g -platform_version macos 26.0 26.0 \
  -N m5-gles3-half-widen-reference.mtlp-json reference.metallib \
  -save-temps -o reference.gpubin
```

The captured binary SHA-256 is
`ea11312f7f64ef08db04279b8d479f0643c00420385e6f4a88c3fb28d0ddd1ca`.
The first two nested Mach-O text sections have SHA-256 values
`cfb9e75dbe59e8c4c4808460e556f790155fb426dfb63c061b62d2420dcd5780`
and `1f0d5ebebb11045c4898a90b40e7258dc48d8fc17639c50cec5397a1c9a7bc4f`.

`G17GHalfWidenMatchesNativeMetal` retains the exact conversion bytes at text
offsets 0x52, 0x5e, 0x64 and 0x6a in the first section, and 0x5e, 0x66 and
0x72 in the second. These independently match the AGX3C composer, including
its pipeline selector fields 50=3 and 51=48. The first instruction waits on
producer slot 8. No G17P modifier tail patch is applied to M5 encodings.

This establishes instruction encoding evidence. Hardware rendering and CTS
results must be reported separately.
