# G17P 4x MSAA Colour Attachment Compression

## Why 4x MSAA colour attachments are plain by default on G17P

G17P already has all packet and meta encodings required for compressed 4x MSAA
colour:
- Gen9 PBE (`agx_pbe_agx3_2d_multisample_pack`) encodes compressed 4x `GPU`
  surfaces;
- texture descriptors and `libagx_fast_clear` support 4x compressed colour
  (exercised by non-attachment 4x colour);
- `hk_agx3_g17p_direct_color_resolve_supported()` and
  `hk_agx3_g17p_multisample_store_supported()` handle 4x single-target stores.

4x colour *attachments* remain plain by default because of a **Vulkan
image-create vs render-pass mismatch** combined with the lack of a G17P
in-place decompressor:

1. **No G17P MSAA/1-8 B in-place decompressor (`hk_cmd_draw.c`):**
   `libagx_decompress` relies on AGX2 compute `image_store` (PBE from compute)
   and fails to compile for G17P (`src/asahi/clc/asahi_clc.c`), while
   `libagx_g17p_decompress_16b` handles only single-sample 16-byte targets.
   Consequently, `hk_cmd_draw.c` fails closed with `VK_ERROR_FEATURE_NOT_PRESENT`
   if any compressed MSAA attachment spills to memory.
2. **Two 4x MSAA resident tilebuffer budgets:**
   - **CLEAR / DONT_CARE non-multiview passes:** the 32 KiB G17P tilebuffer at
     16x16x4 holds **32 B/sample** (`32768 / (16 * 16 * 4)`), and
     `clear_wide_multisample` emits a full-area covering clear draw whenever
     `sample_size_B > 8`. Up to 8x 4-byte or 4x 8-byte targets fit; 5+ 8-byte
     targets spill.
   - **Stored `LOAD_OP_LOAD` or multiview passes:** on M4 Pro and M4 Max,
     `agx_chip_uncovered_tile_ms_reload_defect(G17P, chip_id, 4)` is `true`
     (40/102 runs at 12-16 B/sample lost upper bytes on uncovered tiles).
     `hk_cmd_draw.c` sets `g17p_wide_ms_to_memory = true`, shrinking the
     resident budget to `AGX_G17P_MAX_UNCOVERED_TILE_BYTES_PER_SAMPLE` =
     **8 B/sample**.

Because Vulkan chooses `ail_layout.compressed` immutably at `vkCreateImage`
time—before attachment slot count, neighbour formats, `loadOp`, or `viewMask`
are known—unconditional 4x colour attachment compression would fail closed on
arbitrary CTS MRT passes (>8 B/sample on `LOAD_OP_LOAD`, or >32 B/sample on
`LOAD_OP_CLEAR`).

## Why single-target 4x MSAA workloads (e.g. CS2, `aurora-silicon/mesa#454`) are safe

`hk_resident_attachments()` allocates compressed multisample targets **first**
in the tilebuffer. Therefore, a 4x MSAA pass never spills a compressed
attachment iff:

```
sum(compressed attachment bytes/texel) <= budget_B
```

where `budget_B = hk_g17p_msaa4x_render_pass_budget_B(view_mask != 0, has_stored_load)`:
- `8 B/sample` when `view_mask != 0 || has_stored_load`
- `32 B/sample` otherwise

For a single 4x MSAA colour target of 1-8 B/texel (e.g. RGBA8 = 4 B, BGRA8 = 4 B,
RGBA16F = 8 B), `sum <= 8 B/sample` ALWAYS holds—even across dependent
`LOAD_OP_LOAD` / `STORE_OP_STORE` passes and even when paired with uncompressed
MRT targets (which spill after the compressed prefix).

## Opt-in draft prototype (`ASAHI_MESA_DEBUG=msaa4xcompress`)

The opt-in prototype (draft-only until the G17P in-place decompressor lands)
provides:
- `AGX_DBG_MSAA4X_COMPRESS` (`ASAHI_MESA_DEBUG=msaa4xcompress`, default off) so
  G17P hardware can A/B benchmark 4x MSAA colour compression while developing
  the in-place decompressor; multiview-capable devices stay plain via
  `hk_g17p_multiview_spill_risk()` in `hk_image.c`;
- `hk_g17p_msaa4x_color_compression_eligible()` and the unit-tested residency
  model helpers `hk_g17p_msaa4x_render_pass_budget_B()` /
  `hk_g17p_msaa4x_render_pass_safe()` in `hk_g17p_compression.h`, tested in
  `test-hk-g16s-compression.c` (note: the model helpers are not enforced at
  render-pass begin — over-budget opt-in passes fail closed via the existing
  `hk_cmd_draw.c` error path);
- an explicit spill diagnostic in `hk_cmd_draw.c` reporting sample count,
  sample size, and max sample budget if an opt-in workload exceeds residency.

## Path to default-on upstream

To enable 4x MSAA colour attachment compression by default across G17P (and
remove `AGX_DBG_MSAA4X_COMPRESS` before leaving draft):
1. Measure real-world gain on J616C with `ASAHI_MESA_DEBUG=msaa4xcompress` (CS2
   `#454` and 4x MSAA CTS).
2. Add a G17P in-place MSAA decompression compute kernel in
   `src/asahi/libagx/compression.cl` modeled on `libagx_m5_decompress_rgba16f`
   (bindless compressed MSAA texture load -> direct twiddled `body` global
   store + `metadata[tile] = ail_tile_mode_uncompressed(format)`), avoiding
   AGX2 compute PBE `image_store`, covering 1/2/4/8-byte formats.
3. Wire it into `hk_cmd_draw.c` in place of the G17P fail-closed branch so
   rare >8 B `LOAD_OP_LOAD` / >32 B `CLEAR` MRT spills decompress cleanly while
   <=8 B single-target passes remain 100% resident and compressed.
