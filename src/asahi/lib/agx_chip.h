/*
 * Copyright 2026 The Asahi Linux Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

/*
 * Apple GPU identity, instruction-set generation and binary encoding.
 *
 * These are three different questions and the driver has historically only
 * asked the first one, because every part it supported answered the other two
 * the same way. That stops being true at M3:
 *
 *   - G13 (M1) and G14 (M2) are the AGX2 instruction set. That is what
 *     agx_pack.c emits and what src/asahi/isa/AGX2.xml describes.
 *   - G15 (M3), G16 (M4) and G17 (M5) are AGX3, a different instruction set.
 *     Apple's own compiler routes them through a separate LLVM target, and
 *     the community AGX2 decoder consumes 0% of their code.
 *   - AGX3 is not one encoding. Two binary encodings exist, and which one a
 *     part uses is now known for every chip this driver can enumerate.
 *
 * AGX3 now has separate decoders and encoders. Device support remains a
 * stricter question: G17P is admitted only through its exact T8140 contract,
 * while other AGX3 identities stay outside the runnable Honeykrisp path until
 * their independently-versioned pieces are connected.
 */

enum agx_chip {
   /* AGX2. `G` is a single-cluster part, `X` is multi-cluster. */
   AGX_CHIP_G13G,
   AGX_CHIP_G13X,
   AGX_CHIP_G14G,
   AGX_CHIP_G14X,

   /* G15P is the A16 phone part and still executes AGX2. The Mac G15 parts
    * execute AGX3b; G15G additionally needs explicit scheduling nops. */
   AGX_CHIP_G15P,
   AGX_CHIP_G15G,
   AGX_CHIP_G15S,
   AGX_CHIP_G15C,
   AGX_CHIP_G15D,

   /* Every G16 part executes AGX3b. */
   AGX_CHIP_G16P,
   AGX_CHIP_G16G,
   AGX_CHIP_G16S,
   AGX_CHIP_G16C,

   /* G17P/G17A remain on the HAL200/AGX3b side of the boundary. The M5
    * G/S/C parts use AGX3c. */
   AGX_CHIP_G17P,
   AGX_CHIP_G17A,
   AGX_CHIP_G17G,
   AGX_CHIP_G17S,
   AGX_CHIP_G17C,

   AGX_CHIP_UNKNOWN,
};

/* Instruction set. Apple's compiler names these agx2 and agx3, and the low
 * nibble of an applegpu Mach-O cpusubtype is exactly this value. */
enum agx_isa {
   AGX_ISA_AGX2 = 2,
   AGX_ISA_AGX3 = 3,
   AGX_ISA_UNKNOWN = 0,
};

/*
 * Binary encoding.
 *
 * One instruction set, two encodings. Established by decoding a 274-kernel
 * corpus compiled for all ten AGX3 families with each of Apple's own encoder
 * tables, recovered from libapplegpu-nt.dylib. Every family decodes 100% under
 * exactly one table and 0-1.8% under any other -- no intermediate values:
 *
 *   agx3b (Apple variant 0): g15g, g15s, g15d, g16g, g16p, g16s, g17p
 *   agx3c (Apple variant 1): g17g, g17s, g18p
 *
 * An earlier partition by byte-identity read this as three groups, splitting
 * g15g off from the rest of variant 0. That was an artefact: g15g uses the same
 * encoder table, and differs only in that its scheduler pads with nops. See
 * agx_chip_needs_scheduling_stalls.
 *
 * The names match src/asahi/isa/AGX3b.xml and AGX3c.xml and the disassembler
 * entry points generated from them.
 */
enum agx_isa_encoding {
   AGX_ISA_ENCODING_AGX2,
   AGX_ISA_ENCODING_AGX3B,
   AGX_ISA_ENCODING_AGX3C,
   AGX_ISA_ENCODING_UNKNOWN,
};

/* Apple's independently-versioned command-stream/HAL axis. */
enum agx_command_stream_generation {
   AGX_CS_GENERATION_AGX2,
   AGX_CS_GENERATION_G15,
   AGX_CS_GENERATION_HAL200,
   AGX_CS_GENERATION_HAL300,
   AGX_CS_GENERATION_UNKNOWN,
};

/* PBE/texture descriptor encoder generation. The descriptor grows from 24 to
 * 32 bytes at Gen9 and is laid out again at Gen10. */
enum agx_descriptor_generation {
   AGX_DESCRIPTOR_GENERATION_UNKNOWN = 0,
   AGX_DESCRIPTOR_GENERATION_7 = 7,
   AGX_DESCRIPTOR_GENERATION_8 = 8,
   AGX_DESCRIPTOR_GENERATION_9 = 9,
   AGX_DESCRIPTOR_GENERATION_10 = 10,
};

/* Samplers keep the Gen4 class through M4, but HAL200 moves fields within the
 * word. Gen5 adds reduction mode and a wider LOD clamp on M5. */
enum agx_sampler_generation {
   AGX_SAMPLER_GENERATION_GEN4,
   AGX_SAMPLER_GENERATION_GEN4_HAL200,
   AGX_SAMPLER_GENERATION_GEN5,
   AGX_SAMPLER_GENERATION_UNKNOWN,
};

/* M4 Pro/Max and A18 Pro carry both queueing paths; M5 is SKSM-only. */
enum agx_submission_model {
   AGX_SUBMISSION_MODEL_CLASSIC,
   AGX_SUBMISSION_MODEL_DUAL,
   AGX_SUBMISSION_MODEL_SKSM,
   AGX_SUBMISSION_MODEL_UNKNOWN,
};

/* G17P keeps every colour target in the imageblock past 64 bytes a sample:
 * Apple's applegpu_g17p fragments store five RGBA32Float targets at tile
 * offsets 0..64 and eight at 0..112, and an eight-target mixed-format set at
 * 65 bytes, with no memory store, and its background builtins clear them at
 * the same offsets. */
#define AGX_G17P_MAX_BYTES_PER_SAMPLE (128)

/* On a G17P tile that no colour-writing geometry covered, the background
 * program -- clear or reload alike -- delivers only the low eight bytes of
 * each sample when the tilebuffer is multisampled. Measured on an A18 Pro:
 * with a sixteen-byte sample at 2x or 4x, such a texel reads back with its
 * upper eight bytes zero, while the same background program keeps the whole
 * sample on a tile a draw did cover -- including the pixels of that tile the
 * draw's scissor rejected. Eight-byte and narrower samples are unaffected at
 * every sample count, layer count and image size measured, and so is a
 * single-sample sixteen-byte target.
 *
 * Above this bound a load-op clear is emitted as a full-area draw instead,
 * which covers every tile and keeps the target in the tilebuffer. A reloaded
 * attachment has no such draw and spills. A multiview render has a second,
 * separate defect that the full-area draw does not cover --
 * dEQP-VK.multiview.multisample.no_queries.max_multi_view_view_count, 32
 * views of RGBA32F at 4x, whose clear value is all zeroes so the background
 * clear cannot be what it observes -- and spills too.
 */
#define AGX_G17P_MAX_UNCOVERED_TILE_BYTES_PER_SAMPLE (8)

/*
 * Whether Honeykrisp drives a chip through its native AGX3 path: AGX3b shaders
 * loaded by Ioto programs, VDM/PPP/CDM records in the HAL200 layout, Gen9
 * descriptors with heap-indexed textures and samplers, firmware-owned USC
 * private memory and no helper programs.
 *
 * Every native-path gate asks this question.  Where a chip on the path still
 * differs from G17P, a narrower capability predicate below answers for it;
 * the chip identity is never tested at the call site.
 */
#ifndef __OPENCL_VERSION__
#include <stdbool.h>
#endif

static inline bool
agx_chip_native_agx3(enum agx_chip chip)
{
   return chip == AGX_CHIP_G15S || chip == AGX_CHIP_G15C ||
          chip == AGX_CHIP_G16G || chip == AGX_CHIP_G17P;
}

/* The chip whose record encodings GPU-generated (libagx) command-stream
 * records use.  Every native chip shares G17P's VDM records, so naming that
 * one family keeps the precompiled kernels independent of the native chip. */
static inline enum agx_chip
agx_chip_record_family(enum agx_chip chip)
{
   return agx_chip_native_agx3(chip) ? AGX_CHIP_G17P : chip;
}

#ifndef __OPENCL_VERSION__

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "drm-uapi/asahi_drm.h"

/* The AGX3 ISA family: G17P (AGX3B) and G17S (AGX3C) share instruction forms
 * and the AGX3 backend. Encoder/packer capability stays per encoding; the
 * shared compiler policy gates on the family. */
static inline bool
agx_isa_is_agx3(enum agx_isa_encoding enc)
{
   return enc == AGX_ISA_ENCODING_AGX3B || enc == AGX_ISA_ENCODING_AGX3C;
}

#ifdef __cplusplus
extern "C" {
#endif

/* Return the exact command-stream architecture, or UNKNOWN.  Recognition does
 * not imply a complete Honeykrisp upload-to-submit path.  Callers opening a
 * device must check agx_chip_driver_supported, not only UNKNOWN. */
enum agx_chip
agx_chip_from_params(const struct drm_asahi_params_global *params);

/* The instruction set a chip executes. */
enum agx_isa agx_chip_isa(enum agx_chip chip);

/* The binary encoding of that instruction set. Known for every chip in the
 * enum; UNKNOWN only for AGX_CHIP_UNKNOWN. */
enum agx_isa_encoding agx_chip_isa_encoding(enum agx_chip chip);

/* These are queried several times per recorded draw and dispatch. Answer them
 * with one bounded table load instead of an out-of-line call through a jump
 * table: an indirect branch per query is especially costly when the x86
 * driver build runs under FEX. */
#define AGX_CHIP_TABLE(G13G, G13X, G14G, G14X, G15P, G15G, G15S, G15C, G15D,  \
                       G16P, G16G, G16S, G16C, G17P, G17A, G17G, G17S, G17C)  \
   {                                                                           \
      G13G, G13X, G14G, G14X, G15P, G15G, G15S, G15C, G15D, G16P, G16G, G16S,  \
      G16C, G17P, G17A, G17G, G17S, G17C,                                      \
   }

/* The table is positional (C++ translation units include this header and
 * reject array designators), so pin the enumerator order it relies on. */
static_assert(AGX_CHIP_G13G == 0 && AGX_CHIP_G13X == 1 && AGX_CHIP_G14G == 2 &&
                 AGX_CHIP_G14X == 3 && AGX_CHIP_G15P == 4 &&
                 AGX_CHIP_G15G == 5 && AGX_CHIP_G15S == 6 &&
                 AGX_CHIP_G15C == 7 && AGX_CHIP_G15D == 8 &&
                 AGX_CHIP_G16P == 9 && AGX_CHIP_G16G == 10 &&
                 AGX_CHIP_G16S == 11 && AGX_CHIP_G16C == 12 &&
                 AGX_CHIP_G17P == 13 && AGX_CHIP_G17A == 14 &&
                 AGX_CHIP_G17G == 15 && AGX_CHIP_G17S == 16 &&
                 AGX_CHIP_G17C == 17 && AGX_CHIP_UNKNOWN == 18,
              "AGX_CHIP_TABLE is ordered by enum agx_chip");

static inline enum agx_command_stream_generation
agx_chip_command_stream_generation(enum agx_chip chip)
{
   static const uint8_t table[AGX_CHIP_UNKNOWN] = AGX_CHIP_TABLE(
      AGX_CS_GENERATION_AGX2, AGX_CS_GENERATION_AGX2, AGX_CS_GENERATION_AGX2,
      AGX_CS_GENERATION_AGX2,
      /* A16's command-stream class was not in the Mac corpus. */
      AGX_CS_GENERATION_UNKNOWN,
      AGX_CS_GENERATION_G15, AGX_CS_GENERATION_G15, AGX_CS_GENERATION_G15,
      AGX_CS_GENERATION_G15,
      AGX_CS_GENERATION_HAL200, AGX_CS_GENERATION_HAL200,
      AGX_CS_GENERATION_HAL200, AGX_CS_GENERATION_HAL200,
      AGX_CS_GENERATION_HAL200, AGX_CS_GENERATION_HAL200,
      AGX_CS_GENERATION_HAL300, AGX_CS_GENERATION_HAL300,
      AGX_CS_GENERATION_HAL300);
   return (unsigned)chip < AGX_CHIP_UNKNOWN
             ? (enum agx_command_stream_generation)table[chip]
             : AGX_CS_GENERATION_UNKNOWN;
}

static inline enum agx_descriptor_generation
agx_chip_descriptor_generation(enum agx_chip chip)
{
   static const uint8_t table[AGX_CHIP_UNKNOWN] = AGX_CHIP_TABLE(
      AGX_DESCRIPTOR_GENERATION_7, AGX_DESCRIPTOR_GENERATION_7,
      AGX_DESCRIPTOR_GENERATION_8, AGX_DESCRIPTOR_GENERATION_8,
      AGX_DESCRIPTOR_GENERATION_UNKNOWN,
      AGX_DESCRIPTOR_GENERATION_9, AGX_DESCRIPTOR_GENERATION_9,
      AGX_DESCRIPTOR_GENERATION_9, AGX_DESCRIPTOR_GENERATION_9,
      AGX_DESCRIPTOR_GENERATION_9, AGX_DESCRIPTOR_GENERATION_9,
      AGX_DESCRIPTOR_GENERATION_9, AGX_DESCRIPTOR_GENERATION_9,
      AGX_DESCRIPTOR_GENERATION_9, AGX_DESCRIPTOR_GENERATION_9,
      AGX_DESCRIPTOR_GENERATION_10, AGX_DESCRIPTOR_GENERATION_10,
      AGX_DESCRIPTOR_GENERATION_10);
   return (unsigned)chip < AGX_CHIP_UNKNOWN
             ? (enum agx_descriptor_generation)table[chip]
             : AGX_DESCRIPTOR_GENERATION_UNKNOWN;
}

static inline enum agx_sampler_generation
agx_chip_sampler_generation(enum agx_chip chip)
{
   static const uint8_t table[AGX_CHIP_UNKNOWN] = AGX_CHIP_TABLE(
      AGX_SAMPLER_GENERATION_GEN4, AGX_SAMPLER_GENERATION_GEN4,
      AGX_SAMPLER_GENERATION_GEN4, AGX_SAMPLER_GENERATION_GEN4,
      AGX_SAMPLER_GENERATION_UNKNOWN,
      AGX_SAMPLER_GENERATION_GEN4, AGX_SAMPLER_GENERATION_GEN4,
      AGX_SAMPLER_GENERATION_GEN4, AGX_SAMPLER_GENERATION_GEN4,
      AGX_SAMPLER_GENERATION_GEN4_HAL200, AGX_SAMPLER_GENERATION_GEN4_HAL200,
      AGX_SAMPLER_GENERATION_GEN4_HAL200, AGX_SAMPLER_GENERATION_GEN4_HAL200,
      AGX_SAMPLER_GENERATION_GEN4_HAL200, AGX_SAMPLER_GENERATION_GEN4_HAL200,
      AGX_SAMPLER_GENERATION_GEN5, AGX_SAMPLER_GENERATION_GEN5,
      AGX_SAMPLER_GENERATION_GEN5);
   return (unsigned)chip < AGX_CHIP_UNKNOWN
             ? (enum agx_sampler_generation)table[chip]
             : AGX_SAMPLER_GENERATION_UNKNOWN;
}

/* Native sampler heap element size. The packed state word remains eight
 * bytes, but G15 and later place it at offset zero of a 32-byte sparse heap
 * element. Zero means the heap ABI is not proven for this identity. */
static inline unsigned
agx_chip_sampler_heap_element_size(enum agx_chip chip)
{
   switch (agx_chip_command_stream_generation(chip)) {
   case AGX_CS_GENERATION_AGX2:
      return 8;
   case AGX_CS_GENERATION_G15:
   case AGX_CS_GENERATION_HAL200:
   case AGX_CS_GENERATION_HAL300:
      return 32;
   default:
      return 0;
   }
}

/*
 * Whether the native path runs on Apple's G15 command-stream generation
 * (M3, RTKit 2419) rather than HAL200.  The record layouts are shared; the
 * firmware-facing differences are the direct CDM launch pointer, the CDM
 * visibility barrier and the render-command resource specifiers.
 */
static inline bool
agx_chip_native_g15(enum agx_chip chip)
{
   return agx_chip_native_agx3(chip) &&
          agx_chip_command_stream_generation(chip) == AGX_CS_GENERATION_G15;
}

/* Whether the kernel presents the T8140 native render context: a fixed Bind0
 * and seven low context views at 0x1000000000 that every render job needs.
 * This is a HAL200 firmware construct; the G15 firmware has none. */
static inline bool
agx_chip_has_native_render_context(enum agx_chip chip)
{
   return chip == AGX_CHIP_G17P;
}

/* Whether the native indirect compute pair (grid helper plus the 24-byte
 * mode-2 launch) is qualified.  G15 reads the same mode-2 record and the same
 * six-word geometry (three global thread counts, three local sizes); other
 * native chips refuse indirect dispatches rather than run an unqualified
 * record. */
static inline bool
agx_chip_native_indirect_compute(enum agx_chip chip)
{
   return chip == AGX_CHIP_G17P || chip == AGX_CHIP_G15S ||
          chip == AGX_CHIP_G15C || chip == AGX_CHIP_G16G;
}

/* Whether the CDM's fetch of an indirect geometry block needs a cache flush
 * after the dispatch that wrote it.  On G15 the ordinary launch tail is not
 * established for a shader-store to CDM-fetch edge, so the grid helper's tail
 * is followed by the wide flush before the mode-2 launch. */
static inline bool
agx_chip_indirect_geometry_flush(enum agx_chip chip)
{
   return agx_chip_native_g15(chip);
}

/*
 * Whether a CDM cache flush must use the wide-visibility barrier.  Apple's G15
 * encoder publishes a memory dependency between dispatches of one compute
 * stream with 0x600215fe, not the ordinary flags-5 word that suffices on
 * HAL200; the kernel's end-of-command flush does not cover dependencies
 * inside a stream.
 */
static inline bool
agx_chip_cdm_wide_visibility(enum agx_chip chip)
{
   return agx_chip_native_g15(chip);
}

/* Whether consecutive application dispatches of one CDM stream may omit the
 * ordering barrier between them.  Vulkan orders dispatches only through
 * explicit barriers, and Apple's G15 encoder emits none between independent
 * dispatches; every other record (internal or indirect launch, cache flush,
 * link, stream end) still follows a barrier. */
static inline bool
agx_chip_elide_app_dispatch_barrier(enum agx_chip chip)
{
   return agx_chip_native_g15(chip);
}

/* Number of helper-table slots in the USC code-container lead block.  G15
 * and G16 parts have ten; with six their fragment programs of 256x256 and
 * larger walk past the lead into the guard page.  On G17P the state loader's
 * profile prefix calls the slot at 0x100 + 16 * core id (id = 16 * cluster +
 * core), so each table needs a slot per core of its cluster: six on the A18
 * Pro, ten on the M4 Pro, whose cluster 1 cores 8 and 9 otherwise run past
 * the table and never draw their tiles. */
static inline unsigned
agx_chip_usc_helper_table_slots(enum agx_chip chip, unsigned cores_per_cluster)
{
   if (chip != AGX_CHIP_G17P)
      return 10;
   return cores_per_cluster > 6 ? cores_per_cluster : 6;
}

/* Whether the USC window base page needs a return in every slot past its
 * helper tables.  On G15C the profile prefix's computed helper call can
 * index past both ten-slot tables on some shader cores; with zeroes there
 * the call never returns and runs into the unbound page after the base
 * page. */
static inline bool
agx_chip_usc_base_page_fill_returns(enum agx_chip chip)
{
   return chip == AGX_CHIP_G15C;
}

static inline bool
agx_chip_usc_base_page_fill_returns_for(enum agx_chip chip,
                                        unsigned num_clusters)
{
   return agx_chip_usc_base_page_fill_returns(chip) ||
          (agx_chip_native_agx3(chip) && num_clusters > 2);
}

/* Robust image accesses on native chips need explicit Vulkan view bounds:
 * native 2D accesses outside the view alias other texels of the image. G17P
 * lowers them together with its own hardware defects; G15 and G16 parts
 * bound the coordinates in NIR and take the out-of-bounds alpha from the
 * view swizzle. */
static inline bool
agx_chip_explicit_robust_image_bounds(enum agx_chip chip)
{
   return chip == AGX_CHIP_G16G || agx_chip_native_g15(chip);
}

/*
 * Whether the Apple9 imageblock applies: up to 128 bytes a sample in a full
 * 32 KiB (32768-byte) tile. Apple's Metal feature set tables give G15 (M3)
 * and G17P (A18 Pro) the same Apple9 limits.
 */
static inline bool
agx_chip_native_imageblock(enum agx_chip chip)
{
   return chip == AGX_CHIP_G17P || chip == AGX_CHIP_G15S ||
          chip == AGX_CHIP_G15C || chip == AGX_CHIP_G16G;
}

/*
 * Whether the defect applies that drops the upper bytes of a wide multisample
 * sample on a tile no geometry covered
 * (AGX_G17P_MAX_UNCOVERED_TILE_BYTES_PER_SAMPLE). Measured on A18 Pro, and on
 * G15 (M3), where a layered 4x clear of a 16-byte format keeps only the low
 * eight bytes of each sample.
 */
static inline bool
agx_chip_uncovered_tile_ms_defect(enum agx_chip chip)
{
   return chip == AGX_CHIP_G17P || chip == AGX_CHIP_G16G ||
          agx_chip_native_g15(chip);
}

/* Whether a native fragment program may merge triangles across the
 * linker's other merge-disabling cases: memory writes and wide subgroup
 * operations. Apple's G17P metadata leaves the bit clear for both; on other
 * chips it is unverified, so they keep the linker's conservative bit. */
static inline bool
agx_chip_tri_merge_across_side_effects(enum agx_chip chip)
{
   return chip == AGX_CHIP_G17P;
}

/* Whether an image load misses a store made earlier in the same invocation,
 * so Honeykrisp forwards the stored value to the load. A18 Pro's loads
 * behave this way; G15 loads observe the invocation's own stores. */
static inline bool
agx_chip_image_load_misses_own_store(enum agx_chip chip)
{
   return chip == AGX_CHIP_G17P;
}

/* Whether native FP32 FFMA meets the exact rounding contract required by
 * VK_KHR_shader_fma. G15 and G16G remain unqualified for that contract;
 * G16G probes also fail near the minimum normal result. */
static inline bool
agx_chip_exact_fma32(enum agx_chip chip)
{
   return chip != AGX_CHIP_G16G && !agx_chip_native_g15(chip);
}

/* Whether the sampler state word carries the LOD bias.  The HAL200 sampler
 * layout has a bias field; Gen4 does not, so the shader applies the bias. */
static inline bool
agx_chip_sampler_has_lod_bias(enum agx_chip chip)
{
   return agx_chip_sampler_generation(chip) ==
          AGX_SAMPLER_GENERATION_GEN4_HAL200;
}

/*
 * Whether native shaders may carry an Ioto constant program (CProg) and a
 * shader preamble.  Only the G17P Ioto preamble sequence is qualified; other
 * native chips compile every stage with no_preamble.
 */
static inline bool
agx_chip_native_preamble(enum agx_chip chip)
{
   return chip == AGX_CHIP_G17P;
}

/*
 * Whether a resource-info query reserves thread-invariant (TI) spill storage
 * in a native compute launch.  Apple's compiler stages a uniform query's
 * inputs for its constant program in TI storage, and the launch template
 * follows that witness (agx3_cdm_direct_template_sources_compute()).  Mesa
 * computes every query from descriptor memory, so only a constant program
 * could consume the storage: a chip without native preambles needs none, and
 * G15's direct launch has no TI field to carry it.
 */
static inline bool
agx_chip_query_ti_spill(enum agx_chip chip)
{
   return agx_chip_native_preamble(chip);
}

/*
 * Whether this driver can generate code for a chip.
 *
 * True for every known encoding: AGX2 through agx_pack.c, AGX3b/AGX3c through
 * agx3_pack_binary.c. False only for an unrecognised part, where no packer can
 * be chosen at all.
 *
 * This is a statement about the compiler, not about the driver. It says a
 * shader compiled for this chip is encoded by the right packer, and that an
 * ungrounded shader produces no binary rather than a mis-encoded one -- not
 * that the driver can submit the result. Device open must gate on
 * agx_chip_driver_supported.
 */
bool agx_chip_codegen_supported(enum agx_chip chip);

/* Whether every currently-required userspace axis is implemented. This is the
 * device-open gate; recognising a chip or having its ISA packer is not enough.
 */
bool agx_chip_driver_supported(enum agx_chip chip);

/* Validate the kernel-published identity as well as the userspace chip table.
 * G17P is the first identity whose runnable path depends on explicit USC and
 * HAL fields added to drm_asahi_params_global. */
bool agx_params_driver_supported(
   const struct drm_asahi_params_global *params);

bool agx_params_fixup_upstream_uapi(struct drm_asahi_params_global *params);

/* Exact product name when the chip identity maps to one unambiguous SoC.
 * Variant letters are not marketing tiers: notably, G17P is A18 Pro rather
 * than an M5 variant. Returns NULL when one chip identity spans products. */
const char *agx_chip_exact_product_name(enum agx_chip chip);

/* libagx_g13{g,x} contains AGX2 machine code and USC records. It must never be
 * selected for an AGX3 device, even when no current draw is expected to call a
 * precompiled helper. */
bool agx_params_m5_bringup_supported(
   const struct drm_asahi_params_global *params);

bool agx_chip_uses_legacy_precompiled_programs(enum agx_chip chip);

/* M5 variants sharing the AGX3C instruction and HAL300 command formats. */
static inline bool
agx_chip_g17_m5(enum agx_chip chip)
{
   return chip == AGX_CHIP_G17G || chip == AGX_CHIP_G17S;
}

/* Stable name for diagnostics, e.g. "G15X". Never NULL. */
const char *agx_chip_name(enum agx_chip chip);

/* Stable name for the instruction set, e.g. "AGX3". Never NULL. */
const char *agx_isa_name(enum agx_isa isa);

const char *agx_command_stream_generation_name(
   enum agx_command_stream_generation generation);

#ifdef __cplusplus
}
#endif

#endif
