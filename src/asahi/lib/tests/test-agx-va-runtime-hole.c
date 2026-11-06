/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#include "agx_va_runtime_hole.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define PAGE UINT64_C(0x4000)
#define RUNTIME_GRAPH_HOLE_SIZE                                             \
   (AGX_G17_RUNTIME_GRAPH_VA_END - AGX_G17_RUNTIME_GRAPH_VA_START)

static bool
overlaps_runtime_graph_hole(uint64_t addr, uint64_t size)
{
   return addr < AGX_G17_RUNTIME_GRAPH_VA_END &&
          addr + size > AGX_G17_RUNTIME_GRAPH_VA_START;
}

static void
test_exact_contract(void)
{
   static_assert(AGX_G17_RUNTIME_GRAPH_VA_START == UINT64_C(0x7000000000),
                 "kernel contract start");
   static_assert(AGX_G17_RUNTIME_GRAPH_VA_END == UINT64_C(0x7100000000),
                 "kernel contract end");
   static_assert(AGX_G17_RUNTIME_GRAPH_VA_END -
                       AGX_G17_RUNTIME_GRAPH_VA_START ==
                    UINT64_C(0x100000000),
                 "kernel contract size");

   assert(!agx_va_overlaps_runtime_graph_hole(
      AGX_CHIP_G17P, AGX_G17_RUNTIME_GRAPH_VA_START - PAGE, PAGE));
   assert(agx_va_overlaps_runtime_graph_hole(
      AGX_CHIP_G17P, AGX_G17_RUNTIME_GRAPH_VA_START - PAGE, PAGE + 1));
   assert(agx_va_overlaps_runtime_graph_hole(
      AGX_CHIP_G17P, AGX_G17_RUNTIME_GRAPH_VA_START, PAGE));
   assert(!agx_va_overlaps_runtime_graph_hole(
      AGX_CHIP_G17P, AGX_G17_RUNTIME_GRAPH_VA_END, PAGE));
   assert(!agx_va_overlaps_runtime_graph_hole(
      AGX_CHIP_G17P, AGX_G17_RUNTIME_GRAPH_VA_START, 0));
   assert(!agx_va_overlaps_runtime_graph_hole(
      AGX_CHIP_G17A, AGX_G17_RUNTIME_GRAPH_VA_START, PAGE));
}

static void
test_main_and_timestamp_allocations_skip_hole(void)
{
   struct util_vma_heap main, usc;
   const uint64_t main_start = AGX_G17_RUNTIME_GRAPH_VA_START - (2 * PAGE);
   const uint64_t main_size =
      (AGX_G17_RUNTIME_GRAPH_VA_END - AGX_G17_RUNTIME_GRAPH_VA_START) +
      (4 * PAGE);
   util_vma_heap_init(&main, main_start, main_size);
   util_vma_heap_init(&usc, UINT64_C(0x1000000000), UINT64_C(0x100000000));

   assert(agx_va_reserve_runtime_graph_hole(
      AGX_CHIP_G17P, &main, main_start, main_size, UINT64_C(0x8000000000),
      &usc, UINT64_C(0x1000000000), UINT64_C(0x100000000)));
   assert(main.free_size == main_size - RUNTIME_GRAPH_HOLE_SIZE);

   assert(!util_vma_heap_alloc_addr(&main,
                                    AGX_G17_RUNTIME_GRAPH_VA_START, PAGE));
   assert(!util_vma_heap_alloc_addr(
      &main, AGX_G17_RUNTIME_GRAPH_VA_END - PAGE, PAGE));
   assert(!util_vma_heap_alloc_addr(
      &main, AGX_G17_RUNTIME_GRAPH_VA_START - PAGE, 2 * PAGE));

   const uint64_t ordinary = util_vma_heap_alloc(&main, PAGE, PAGE);
   const uint64_t timestamp = util_vma_heap_alloc(&main, PAGE, PAGE);
   assert(ordinary != 0 && !overlaps_runtime_graph_hole(ordinary, PAGE));
   assert(timestamp != 0 && !overlaps_runtime_graph_hole(timestamp, PAGE));

   util_vma_heap_finish(&usc);
   util_vma_heap_finish(&main);
}

static void
test_sparse_shadow_preimage_is_reserved(void)
{
   struct util_vma_heap main, usc;
   const uint64_t main_start = UINT64_C(0x2000000000);
   const uint64_t main_size = UINT64_C(0x4000000000);
   const uint64_t sparse_ro_offset = UINT64_C(0x4000000000);
   const uint64_t preimage =
      AGX_G17_RUNTIME_GRAPH_VA_START - sparse_ro_offset;
   util_vma_heap_init(&main, main_start, main_size);
   util_vma_heap_init(&usc, UINT64_C(0x1000000000), UINT64_C(0x100000000));

   assert(agx_va_reserve_runtime_graph_hole(
      AGX_CHIP_G17P, &main, main_start, main_size, sparse_ro_offset, &usc,
      UINT64_C(0x1000000000), UINT64_C(0x100000000)));
   assert(main.free_size == main_size - RUNTIME_GRAPH_HOLE_SIZE);

   assert(!util_vma_heap_alloc_addr(&main, preimage, PAGE));
   assert(!util_vma_heap_alloc_addr(
      &main, preimage + RUNTIME_GRAPH_HOLE_SIZE - PAGE, PAGE));
   assert(!util_vma_heap_alloc_addr(&main, preimage - PAGE, 2 * PAGE));

   const uint64_t sparse = util_vma_heap_alloc(&main, PAGE, PAGE);
   assert(sparse != 0);
   assert(!overlaps_runtime_graph_hole(sparse + sparse_ro_offset, PAGE));

   util_vma_heap_finish(&usc);
   util_vma_heap_finish(&main);
}

static void
test_fixed_usc_allocation_skips_hole(void)
{
   struct util_vma_heap main, usc;
   const uint64_t usc_start = AGX_G17_RUNTIME_GRAPH_VA_START - PAGE;
   const uint64_t usc_size =
      (AGX_G17_RUNTIME_GRAPH_VA_END - AGX_G17_RUNTIME_GRAPH_VA_START) +
      (2 * PAGE);
   util_vma_heap_init(&main, UINT64_C(0x2000000000), UINT64_C(0x100000000));
   util_vma_heap_init(&usc, usc_start, usc_size);

   assert(agx_va_reserve_runtime_graph_hole(
      AGX_CHIP_G17P, &main, UINT64_C(0x2000000000), UINT64_C(0x100000000),
      UINT64_C(0x4000000000), &usc, usc_start, usc_size));
   assert(usc.free_size == usc_size - RUNTIME_GRAPH_HOLE_SIZE);

   assert(!util_vma_heap_alloc_addr(&usc,
                                    AGX_G17_RUNTIME_GRAPH_VA_START, PAGE));
   assert(!util_vma_heap_alloc_addr(
      &usc, AGX_G17_RUNTIME_GRAPH_VA_END - PAGE, PAGE));
   const uint64_t automatic = util_vma_heap_alloc(&usc, PAGE, PAGE);
   assert(automatic != 0 && !overlaps_runtime_graph_hole(automatic, PAGE));

   util_vma_heap_finish(&usc);
   util_vma_heap_finish(&main);
}

static void
test_g17p_axfb_alias_is_reserved_from_usc(void)
{
   struct util_vma_heap main, usc;
   const uint64_t aux_size =
      AGX_G17P_RENDER_AUX_ALIAS_END - AGX_G17P_RENDER_AUX_ALIAS_START;
   const uint64_t usc_start = AGX_G17P_RENDER_AUX_ALIAS_START - (2 * PAGE);
   const uint64_t usc_size = aux_size + (4 * PAGE);
   const uint64_t rejected_stk_va = UINT64_C(0x1000201c000);

   static_assert(AGX_G17P_RENDER_AUX_ALIAS_START ==
                    UINT64_C(0x10002000000),
                 "kernel AXFB alias start");
   static_assert(AGX_G17P_RENDER_AUX_ALIAS_END ==
                    UINT64_C(0x10002400000),
                 "kernel AXFB alias end");
   assert(rejected_stk_va >= AGX_G17P_RENDER_AUX_ALIAS_START);
   assert(rejected_stk_va + PAGE <= AGX_G17P_RENDER_AUX_ALIAS_END);

   util_vma_heap_init(&main, UINT64_C(0x2000000000),
                      UINT64_C(0x100000000));
   util_vma_heap_init(&usc, usc_start, usc_size);

   assert(agx_va_reserve_runtime_graph_hole(
      AGX_CHIP_G17P, &main, UINT64_C(0x2000000000),
      UINT64_C(0x100000000), UINT64_C(0x4000000000), &usc, usc_start,
      usc_size));
   assert(usc.free_size == usc_size - aux_size);

   /* This is the exact 16-KiB shader allocation that the kernel rejected in
    * SuperTuxKart.  It must be unavailable locally, while the pages directly
    * before and after the kernel-owned range remain usable. */
   assert(!util_vma_heap_alloc_addr(&usc, rejected_stk_va, PAGE));
   assert(util_vma_heap_alloc_addr(
      &usc, AGX_G17P_RENDER_AUX_ALIAS_START - PAGE, PAGE));
   assert(util_vma_heap_alloc_addr(&usc, AGX_G17P_RENDER_AUX_ALIAS_END,
                                   PAGE));

   util_vma_heap_finish(&usc);
   util_vma_heap_finish(&main);
}

static void
test_g17p_production_heap_intersections(void)
{
   /* Exact geometry printed by the failing SuperTuxKart process.  Of every
    * fixed range retained by the kernel, the runtime-graph shadow preimage is
    * the sole main-heap intersection and AXFB is the sole USC intersection. */
   const uint64_t main_start = UINT64_C(0x4000000000);
   const uint64_t main_size = UINT64_C(0x2000000000);
   const uint64_t sparse_ro_offset = main_size;
   const uint64_t usc_start = UINT64_C(0x10000000000);
   const uint64_t usc_size = UINT64_C(0x100000000);
   const uint64_t runtime_shadow_start =
      AGX_G17_RUNTIME_GRAPH_VA_START - sparse_ro_offset;
   const uint64_t aux_size =
      AGX_G17P_RENDER_AUX_ALIAS_END - AGX_G17P_RENDER_AUX_ALIAS_START;
   struct util_vma_heap main, usc;

   util_vma_heap_init(&main, main_start, main_size);
   util_vma_heap_init(&usc, usc_start, usc_size);
   assert(agx_va_reserve_runtime_graph_hole(
      AGX_CHIP_G17P, &main, main_start, main_size, sparse_ro_offset, &usc,
      usc_start, usc_size));

   assert(main.free_size == main_size - RUNTIME_GRAPH_HOLE_SIZE);
   assert(usc.free_size == usc_size - aux_size);
   assert(!util_vma_heap_alloc_addr(&main, runtime_shadow_start, PAGE));
   assert(!util_vma_heap_alloc_addr(
      &main, runtime_shadow_start + RUNTIME_GRAPH_HOLE_SIZE - PAGE, PAGE));
   assert(!util_vma_heap_alloc_addr(
      &usc, AGX_G17P_RENDER_AUX_ALIAS_START, PAGE));
   assert(!util_vma_heap_alloc_addr(
      &usc, AGX_G17P_RENDER_AUX_ALIAS_END - PAGE, PAGE));

   util_vma_heap_finish(&usc);
   util_vma_heap_finish(&main);
}

static void
test_other_chips_keep_existing_layout(void)
{
   struct util_vma_heap main, usc;
   const uint64_t start = AGX_G17_RUNTIME_GRAPH_VA_START - PAGE;
   const uint64_t size = 3 * PAGE;
   util_vma_heap_init(&main, start, size);
   util_vma_heap_init(&usc, start, size);

   assert(agx_va_reserve_runtime_graph_hole(
      AGX_CHIP_G16C, &main, start, size, UINT64_C(0x8000000000), &usc,
      start, size));
   assert(util_vma_heap_alloc_addr(&main,
                                   AGX_G17_RUNTIME_GRAPH_VA_START, PAGE));
   assert(util_vma_heap_alloc_addr(&usc,
                                   AGX_G17_RUNTIME_GRAPH_VA_START, PAGE));

   util_vma_heap_finish(&usc);
   util_vma_heap_finish(&main);
}

/*
 * The user-window split, driven by the two kernel geometries that exist.
 *
 * Inputs are what agx_open_device() computes before the reduction:
 *   shader_base  = ALIGN_POT(MAX2(vm_start, 2^36 + 16384), 4 GiB) = 0x1100000000
 *   user_start   = shader_base + 4 GiB                            = 0x1200000000
 *   kernel_start = vm_end - MAX2(vm_kernel_min_size, 32 GiB)
 *   user_size    = kernel_start - user_start
 * with vm_start = UAT_PGSZ = 0x4000 on both geometries (kernel mmu.rs:145
 * IOVA_USER_BASE, and g17_drm.rs:24 T8140_USER_START), and vm_end two pages
 * below the root's translation top: 1 << 39 for AGX2 (mmu.rs:149), 1 << 42 for
 * AGX3 (g17_drm.rs:25).
 */
static void
check_split(const char *what, uint64_t user_start, uint64_t user_size,
            uint64_t expect_start, uint64_t expect_size)
{
   uint64_t start = 0, size = 0;
   const bool ok = agx_va_split_user_window(user_start, user_size, &start, &size);

   if (!ok || start != expect_start || size != expect_size) {
      fprintf(stderr,
              "%s: split(%#" PRIx64 ", %#" PRIx64 ") -> ok=%d start=%#" PRIx64
              " size=%#" PRIx64 ", expected start=%#" PRIx64 " size=%#" PRIx64
              "\n",
              what, user_start, user_size, ok, start, size, expect_start,
              expect_size);
      abort();
   }

   /* The invariant the whole scheme rests on: a sparse buffer's read-only
    * alias is `addr + size`, so every address in the window must have that
    * bit clear or the alias carries out of its own half. */
   assert((start & size) == 0);
   assert(((start + (size - 1)) & size) == 0);
   assert(start > size);
}

static void
test_user_window_split(void)
{
   const uint64_t vm_start = PAGE;
   const uint64_t shader_base = UINT64_C(0x1100000000);
   const uint64_t user_start = shader_base + UINT64_C(0x100000000);
   const uint64_t kernel_reserve = UINT64_C(32) << 30;
   assert(user_start > vm_start);

   /* AGX2 / G13-G14: 39-bit root. Upstream's single "quarters" fixup is
    * exactly enough here, and this case pins that behaviour unchanged. */
   const uint64_t agx2_vm_end = (UINT64_C(1) << 39) - 2 * PAGE;
   check_split("AGX2 39-bit", user_start,
               (agx2_vm_end - kernel_reserve) - user_start,
               UINT64_C(0x2000000000), UINT64_C(0x1000000000));

   /* AGX3 / T8140 G17P: 42-bit root. The window is eight times the rounded
    * start, so one fixup leaves 2^40 -- which straddles the sparse bit and
    * used to trip the assert in agx_open_device. */
   const uint64_t agx3_vm_end = (UINT64_C(1) << 42) - 2 * PAGE;
   check_split("AGX3 42-bit", user_start,
               (agx3_vm_end - kernel_reserve) - user_start,
               UINT64_C(0x2000000000), UINT64_C(0x1000000000));

   /* The AGX3 window and its read-only alias must both stay below the kernel
    * heap the same open() reserves at the top of the address space. */
   const uint64_t split_start = UINT64_C(0x2000000000);
   const uint64_t split_size = UINT64_C(0x1000000000);
   assert(split_start + 2 * split_size <= agx3_vm_end - kernel_reserve);
   /* ...and above the USC/shader heap, which the split does not move. */
   assert(split_start >= shader_base + UINT64_C(0x100000000));

   /* Degenerate inputs are refused, not asserted on. */
   uint64_t o1, o2;
   assert(!agx_va_split_user_window(0, 1 << 20, &o1, &o2));
   assert(!agx_va_split_user_window(1 << 20, 0, &o1, &o2));
   assert(!agx_va_split_user_window(1 << 20, 1 << 20, NULL, &o2));

   /* A window smaller than one page of slack still splits or is refused --
    * never returns a bit that the window straddles. */
   for (unsigned k = 1; k < 48; ++k) {
      uint64_t start = UINT64_C(1) << k, size = 0, out = 0;
      if (agx_va_split_user_window(start, start, &size, &out)) {
         assert((size & out) == 0);
         assert(((size + (out - 1)) & out) == 0);
      }
   }
}


/*
 * G17P's user heap and its sparse read-only alias must both fit inside the
 * compact VDM aperture.
 *
 * The VDM PPP State Update carries its payload pointer in a 40-bit field
 * (agx_vdm_ppp_state_core.h: word 0 bits 0..7 are pointer bits 32..39, read
 * off Apple's own G15/HAL200/HAL300 emitters). G17P's USC window sits at
 * exactly 2^40, so a user heap placed AFTER the window -- which is what every
 * other chip does, and what G17P used to do -- puts every command-stream
 * allocation out of reach of that field. agx_ppp_fini() then refused every
 * payload and vkEndCommandBuffer() failed before a render command buffer
 * could be recorded.
 *
 * G17P interprets nested VDM payload pointers as offsets from 0x1000000000,
 * so merely remaining below 2^40 is insufficient: a compact offset with bit
 * 39 set selects the wrong address domain.
 * agx_device.c bounds the two allocation windows by 0x9000000000.
 */
static void
test_g17p_user_window_fits_ppp_pointer_field(void)
{
   const uint64_t ppp_pointer_limit = UINT64_C(1) << 40;
   const uint64_t shader_base = UINT64_C(0x10000000000); /* AGX_G17P_USC_WINDOW_BASE */
   const uint64_t vdm_pointer_limit = AGX_G17P_VDM_USER_LIMIT;
   const uint64_t reservation = UINT64_C(1) << 36;

   assert(shader_base == ppp_pointer_limit &&
          "the USC window sits exactly at the PPP pointer limit");

   /* agx_device.c starts the low heap halfway through the effective user
    * aperture -- as high as it can go -- because the split needs size < start
    * and the alias needs a second window of the same size. The aperture is
    * the NARROWER of the compact VDM pointer and the absolute USC
    * texture/sampler pointer; see
    * test_g17p_user_window_fits_usc_resource_pointer below. */
   uint64_t start = AGX_G17P_USER_LIMIT / 2;
   assert(start >= reservation);
   assert(start < vdm_pointer_limit && vdm_pointer_limit < shader_base);
   uint64_t size = AGX_G17P_USER_LIMIT - start;
   uint64_t out_start = 0, out_size = 0;
   assert(agx_va_split_user_window(start, size, &out_start, &out_size));

   /* Whatever the split picks, halving until the alias fits must terminate
    * with a non-empty window -- the same loop agx_device.c runs. */
   while (out_size > 0 && out_start + 2 * out_size > AGX_G17P_USER_LIMIT)
      out_size /= 2;
   assert(out_size > 0);

   /* The heap itself, and the read-only alias above it, are both addressable
    * by the 39-bit base-relative VDM pointer. */
   assert(out_start >= reservation);
   assert(out_size == UINT64_C(0x2000000000)); /* 128 GiB */
   assert(out_start + out_size <= vdm_pointer_limit);
   assert(out_start + 2 * out_size <= vdm_pointer_limit);
   assert(out_start + out_size <= ppp_pointer_limit);
   assert(out_start + 2 * out_size <= ppp_pointer_limit);

   /* And it does not run into the USC window. */
   assert(out_start + 2 * out_size <= shader_base);
}

/*
 * The user window must clear the ABSOLUTE 36-bit shr(3) pointer that USC
 * Texture and USC Sampler carry, which is narrower than the compact VDM
 * pointer. Gallium binds graphics through those records on G17P; Honeykrisp
 * does not, which is why this only ever fired on the GL path.
 *
 * The bound is checked against the generated packer's own constant rather
 * than restated, so widening or narrowing cmdbuf.xml moves the test with it.
 */
static void
test_g17p_user_window_fits_usc_resource_pointer(void)
{
   static_assert(AGX_G17P_USC_RESOURCE_LIMIT == UINT64_C(0x8000000000),
                 "36 stored bits scaled by 8 is a 39-bit address");
   static_assert(AGX_G17P_USC_RESOURCE_LIMIT < AGX_G17P_VDM_USER_LIMIT,
                 "the USC resource pointer is the narrower of the two");
   static_assert(AGX_G17P_USER_LIMIT == AGX_G17P_USC_RESOURCE_LIMIT,
                 "so it is what bounds the G17P user window");

   /* The widest value the descriptor field can hold is exactly one scaled
    * step below the limit. This is the packer's contract, spelled out from
    * the field geometry rather than copied from the packer. */
   const uint64_t widest_encodable =
      ((UINT64_C(1) << AGX_G17P_USC_RESOURCE_FIELD_BITS) - 1)
      << AGX_G17P_USC_RESOURCE_SCALE_SHIFT;
   assert(widest_encodable ==
          AGX_G17P_USC_RESOURCE_LIMIT -
             (UINT64_C(1) << AGX_G17P_USC_RESOURCE_SCALE_SHIFT));
   assert(widest_encodable < AGX_G17P_USC_RESOURCE_LIMIT);

   /* Reproduce agx_device.c's placement exactly. */
   const uint64_t reservation = UINT64_C(1) << 36;
   uint64_t start = AGX_G17P_USER_LIMIT / 2;
   assert(start >= reservation);

   uint64_t out_start = 0, out_size = 0;
   assert(agx_va_split_user_window(start, AGX_G17P_USER_LIMIT - start,
                                   &out_start, &out_size));
   while (out_size > 0 && out_start + 2 * out_size > AGX_G17P_USER_LIMIT)
      out_size /= 2;
   assert(out_size > 0);

   /* 128 GiB of primary heap at 0x4000000000, whose read-only sparse alias
    * ends exactly on the limit: nothing the driver hands to USC Texture or
    * USC Sampler can be out of bounds, and nothing usable is wasted. */
   assert(out_start == UINT64_C(0x4000000000));
   assert(out_size == UINT64_C(0x2000000000));
   assert(out_start + 2 * out_size == AGX_G17P_USC_RESOURCE_LIMIT);

   /* The field stores an 8-byte-scaled address, so what has to fit is every
    * 8-aligned base in the primary heap and in its alias. The highest one is
    * the last scaled step below the end of the alias, which is exactly the
    * widest value the field can hold. */
   assert((out_start & 0x7) == 0);
   assert(out_start + 2 * out_size -
             (UINT64_C(1) << AGX_G17P_USC_RESOURCE_SCALE_SHIFT) ==
          widest_encodable);

   /* And it is still below the compact VDM pointer and the USC window. */
   assert(out_start + 2 * out_size <= AGX_G17P_VDM_USER_LIMIT);
   assert(out_start + 2 * out_size <= UINT64_C(0x10000000000));
}

static void
test_g17p_vdm_command_alias_heap(void)
{
   struct util_vma_heap heap;
   assert(agx_g17p_vdm_alias_heap_init(&heap));

   const uint64_t command_bo_size = UINT64_C(0x20000);
   const uint64_t first =
      util_vma_heap_alloc(&heap, command_bo_size, PAGE);
   assert(first == AGX_G17P_VDM_ALIAS_BASE);
   assert(agx_g17p_vdm_alias_range_valid(first, command_bo_size));
   assert(first - AGX_G17P_VDM_USER_BASE <
          AGX_G17P_VDM_ALIAS_OFFSET_END);

   /* Kernel g17_resources.rs, native 092506 page-list capture. Each sparse
    * four-page block must remain disjoint from the entire command aperture. */
   const uint32_t native_tvb_pages[] = {
      0x11, 0x16, 0x1b, 0x20, 0x25, 0x2a, 0x4a, 0x4f,
      0x54, 0x59, 0x5e, 0x63, 0x68, 0x6d, 0x72, 0x77,
      0x7c, 0x81, 0x86, 0x8b, 0x90, 0x95, 0x9a, 0x9f,
      0xa4, 0xa9, 0xae, 0xb3, 0xb8, 0xbd, 0xc2, 0xc7,
   };
   for (unsigned i = 0; i < sizeof(native_tvb_pages) / sizeof(native_tvb_pages[0]); ++i) {
      const uint64_t end = ((uint64_t)native_tvb_pages[i] + 4) * 0x8000;
      assert(end <= AGX_G17P_VDM_ALIAS_OFFSET_START);
   }
   /* The old third command BO collided with block six. */
   assert(0x200000 + 3 * command_bo_size > (uint64_t)native_tvb_pages[6] * 0x8000);
   assert(0x200000 + 2 * command_bo_size < ((uint64_t)native_tvb_pages[6] + 4) * 0x8000);
   assert(AGX_G17P_VDM_ALIAS_OFFSET_START == 0x800000);

   /* Kernel g17_resources.rs reserves its own arenas in every client root
    * and refuses (EINVAL) a user bind that overlaps one. The heap is the
    * window minus those arenas, never the whole window. */
   const uint64_t capacity_bytes = agx_g17p_vdm_alias_heap_capacity();
   assert(capacity_bytes < AGX_G17P_VDM_ALIAS_SIZE);
   assert(capacity_bytes == AGX_G17P_VDM_ALIAS_AVAILABLE_SIZE);
   assert(heap.free_size == capacity_bytes - command_bo_size);

   /* Firefox (WebRender + WebGL) holds 32 batches x (512-KiB encoder +
    * 256-KiB pool alias) = exactly the 24 MiB below the render-state alias
    * arena; the next command BO must continue above the arena, not into it. */
   const uint64_t arena_start = AGX_G17P_VDM_ALIAS_RESERVED_START;
   const uint64_t arena_end = AGX_G17P_VDM_ALIAS_RESERVED_END;
   assert(agx_g17p_vdm_alias_kernel_reserved(arena_start, PAGE));
   assert(agx_g17p_vdm_alias_kernel_reserved(arena_start - PAGE, PAGE + 1));
   assert(!agx_g17p_vdm_alias_kernel_reserved(arena_start - PAGE, PAGE));
   assert(!agx_g17p_vdm_alias_kernel_reserved(arena_end, PAGE));
   assert(!agx_g17p_vdm_alias_kernel_reserved(arena_start, 0));

   /* Exhaust both free regions, including the jump over all kernel arenas. */
   const uint64_t free_regions[][2] = {
      {AGX_G17P_VDM_ALIAS_BASE, arena_start},
      {arena_end,
       AGX_G17P_VDM_USER_BASE + AGX_G17P_VDM_ALIAS_OFFSET_END},
   };
   unsigned capacity = 0;
   uint64_t unusable = 0;
   for (unsigned r = 0; r < sizeof(free_regions) / sizeof(free_regions[0]); ++r) {
      const uint64_t bytes = free_regions[r][1] - free_regions[r][0];
      capacity += bytes / command_bo_size;
      unusable += bytes % command_bo_size;
   }
   assert(capacity_bytes == UINT64_C(0x1800000) + UINT64_C(0x80000000));
   assert(capacity == 192 + 16384);

   const uint64_t below_arena = (arena_start - AGX_G17P_VDM_ALIAS_BASE) / command_bo_size;
   uint64_t last = first;
   for (unsigned i = 1; i < capacity; ++i) {
      last = util_vma_heap_alloc(&heap, command_bo_size, PAGE);
      assert(last != 0);
      assert(agx_g17p_vdm_alias_range_valid(last, command_bo_size));
      assert(!agx_g17p_vdm_alias_kernel_reserved(last, command_bo_size));
      if (i < below_arena)
         assert(last + command_bo_size <= arena_start);
      else if (i == below_arena)
         assert(last == arena_end);
   }
   assert(last + command_bo_size ==
          AGX_G17P_VDM_USER_BASE + AGX_G17P_VDM_ALIAS_OFFSET_END);
   assert(heap.free_size == unusable);
   assert(util_vma_heap_alloc(&heap, command_bo_size, PAGE) == 0);

   util_vma_heap_free(&heap, first, command_bo_size);
   assert(util_vma_heap_alloc(&heap, command_bo_size, PAGE) == first);

   assert(!agx_g17p_vdm_alias_range_valid(
      AGX_G17P_VDM_ALIAS_BASE - PAGE, PAGE));
   assert(!agx_g17p_vdm_alias_range_valid(
      AGX_G17P_VDM_USER_BASE + AGX_G17P_VDM_ALIAS_OFFSET_END, PAGE));
   assert(!agx_g17p_vdm_alias_range_valid(AGX_G17P_VDM_ALIAS_BASE, 0));

   uint32_t compact = 0;
   assert(agx_g17p_vdm_alias_compact(first, &compact));
   assert(compact == AGX_G17P_VDM_ALIAS_OFFSET_START);
   /* More than 192 live 128-KiB command BOs must skip the driver's compact
    * arenas, not bind over its render-state aliases and TVB growth backing. */
   assert(!agx_g17p_vdm_alias_compact(
      AGX_G17P_VDM_USER_BASE + UINT64_C(0x02000000), &compact));
   assert(!agx_g17p_vdm_alias_range_valid(
      AGX_G17P_VDM_ALIAS_RESERVED_START - PAGE, 2 * PAGE));
   assert(!agx_g17p_vdm_alias_range_valid(
      AGX_G17P_VDM_USER_BASE + UINT64_C(0x04000000), PAGE));
   assert(!agx_g17p_vdm_alias_range_valid(
      AGX_G17P_VDM_USER_BASE + UINT64_C(0x50000000), PAGE));
   assert(!agx_g17p_vdm_alias_range_valid(
      AGX_G17P_VDM_ALIAS_RESERVED_END - PAGE, 2 * PAGE));
   assert(agx_g17p_vdm_alias_compact(
      AGX_G17P_VDM_ALIAS_RESERVED_END, &compact));
   assert(compact == UINT32_C(0x80000000));
   assert(agx_g17p_vdm_alias_compact(last + command_bo_size - 1, &compact));
   assert(compact == UINT32_MAX);
   assert(!agx_g17p_vdm_alias_compact(
      AGX_G17P_VDM_USER_BASE + (UINT64_C(1) << 32), &compact));
   assert(!agx_g17p_vdm_alias_compact(AGX_G17P_VDM_ALIAS_BASE - 1,
                                      &compact));
   assert(!agx_g17p_vdm_alias_compact(first, NULL));
   util_vma_heap_finish(&heap);
}

static void
test_g17p_vdm_payload_context_address(void)
{
   /* The first native Gallium draw mapped its PPP payload at 0x5fffee0000.
    * Encoding that absolute VA made G17P add its 0x1000000000 render-context
    * base and fault on the exact unmapped address 0x6fffee0000. */
   const uint64_t payload = UINT64_C(0x5fffee0000);
   const uint64_t observed_fault = UINT64_C(0x6fffee0000);
   uint64_t compact = UINT64_MAX;

   assert(agx_g17p_vdm_payload_compact(payload, &compact));
   assert(compact == UINT64_C(0x4fffee0000));
   assert(AGX_G17P_VDM_USER_BASE + compact == payload);
   assert(AGX_G17P_VDM_USER_BASE + payload == observed_fault);

   uint64_t expanded = UINT64_MAX;
   assert(agx_g17p_vdm_payload_expand(compact, 16, &expanded));
   assert(expanded == payload);

   /* Serializing the full VA into a context-relative field reproduces the
    * hardware fault exactly: validation must add the base unconditionally,
    * not accept an already-large value as an absolute address. */
   assert(agx_g17p_vdm_payload_expand(payload, 16, &expanded));
   assert(expanded == observed_fault);

   assert(agx_g17p_vdm_payload_compact(AGX_G17P_VDM_USER_BASE, &compact));
   assert(compact == 0);
   assert(agx_g17p_vdm_payload_compact(AGX_G17P_VDM_USER_LIMIT - 4,
                                      &compact));
   assert(compact == AGX_G17P_VDM_USER_SIZE - 4);

   compact = UINT64_C(0xdeadbeef);
   assert(!agx_g17p_vdm_payload_compact(AGX_G17P_VDM_USER_BASE - 4,
                                        &compact));
   assert(compact == UINT64_C(0xdeadbeef));
   assert(!agx_g17p_vdm_payload_compact(AGX_G17P_VDM_USER_LIMIT, &compact));
   assert(compact == UINT64_C(0xdeadbeef));
   assert(!agx_g17p_vdm_payload_compact(payload + 1, &compact));
   assert(compact == UINT64_C(0xdeadbeef));
   assert(!agx_g17p_vdm_payload_compact(payload, NULL));

   expanded = UINT64_C(0xdeadbeef);
   assert(!agx_g17p_vdm_payload_expand(AGX_G17P_VDM_USER_SIZE, 1,
                                       &expanded));
   assert(expanded == UINT64_C(0xdeadbeef));
   assert(!agx_g17p_vdm_payload_expand(AGX_G17P_VDM_USER_SIZE - 4, 8,
                                       &expanded));
   assert(expanded == UINT64_C(0xdeadbeef));
   assert(!agx_g17p_vdm_payload_expand(compact, 0, &expanded));
   assert(!agx_g17p_vdm_payload_expand(compact, 4, NULL));
}

static void
test_capture_replay_region_isolated_even_when_full(void)
{
   const uint64_t user_start = UINT64_C(0x4000000000);
   const uint64_t user_size = UINT64_C(0x2000000000);
   const uint64_t replay_size = user_size / 8;
   struct util_vma_heap main_heap, replay_heap;
   util_vma_heap_init(&main_heap, user_start, user_size);
   assert(agx_capture_replay_heap_init(&main_heap, &replay_heap, user_size));
   assert(main_heap.free_size == user_size - replay_size);
   assert(replay_heap.free_size == replay_size);

   const uint64_t captured = util_vma_heap_alloc(&replay_heap, replay_size, PAGE);
   assert(captured && replay_heap.free_size == 0);
   assert(!util_vma_heap_alloc(&replay_heap, PAGE, PAGE));
   assert(!util_vma_heap_alloc_addr(&main_heap, captured, PAGE));

   /* Freeing the last replay allocation returns it to that same heap. */
   util_vma_heap_free(&replay_heap, captured, replay_size);
   const uint64_t ordinary = util_vma_heap_alloc(&main_heap, PAGE, PAGE);
   assert(ordinary && (ordinary < captured || ordinary >= captured + replay_size));
   assert(util_vma_heap_alloc_addr(&replay_heap, captured, replay_size));
   assert(!util_vma_heap_alloc_addr(&main_heap, captured, PAGE));
   util_vma_heap_free(&replay_heap, captured, replay_size);
   util_vma_heap_free(&main_heap, ordinary, PAGE);
   util_vma_heap_finish(&replay_heap);
   util_vma_heap_finish(&main_heap);
}

int
main(void)
{
   test_exact_contract();
   test_user_window_split();
   test_main_and_timestamp_allocations_skip_hole();
   test_sparse_shadow_preimage_is_reserved();
   test_fixed_usc_allocation_skips_hole();
   test_g17p_axfb_alias_is_reserved_from_usc();
   test_g17p_production_heap_intersections();
   test_other_chips_keep_existing_layout();
   test_g17p_user_window_fits_ppp_pointer_field();
   test_g17p_user_window_fits_usc_resource_pointer();
   test_g17p_vdm_command_alias_heap();
   test_g17p_vdm_payload_context_address();
   test_capture_replay_region_isolated_even_when_full();
   puts("AGX VA layout: user-window split and G17 kernel reservations passed");
   return 0;
}
