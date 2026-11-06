/*
 * Copyright 2021 Alyssa Rosenzweig
 * Copyright 2019 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/macros.h"
#include "layout.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

/* Z-order with rectangular (NxN or 2NxN) tiles, at most 128x128:
 *
 * 	[y6][x6][y5][x5][y4][x4]y3][x3][y2][x2][y1][x1][y0][x0]
 *
 * Efficient tiling algorithm described in
 * https://fgiesen.wordpress.com/2011/01/17/texture-tiling-and-swizzling/ but
 * for posterity, we split into X and Y parts, and are faced with the problem
 * of incrementing:
 *
 * 	0 [x6] 0 [x5] 0 [x4] 0 [x3] 0 [x2] 0 [x1] 0 [x0]
 *
 * To do so, we fill in the "holes" with 1's by adding the bitwise inverse of
 * the mask of bits we care about
 *
 * 	0 [x6] 0 [x5] 0 [x4] 0 [x3] 0 [x2] 0 [x1] 0 [x0]
 *    + 1  0   1  0   1  0   1  0   1  0   1  0   1  0
 *    ------------------------------------------------
 * 	1 [x6] 1 [x5] 1 [x4] 1 [x3] 1 [x2] 1 [x1] 1 [x0]
 *
 * Then when we add one, the holes are passed over by forcing carry bits high.
 * Finally, we need to zero out the holes, by ANDing with the mask of bits we
 * care about. In total, we get the expression (X + ~mask + 1) & mask, and
 * applying the two's complement identity, we are left with (X - mask) & mask
 */

typedef struct {
   uint64_t lo;
   uint64_t hi;
} __attribute__((packed)) ail_uint128_t;

/*
 * Given a power-of-two block width/height, construct the mask of "X" bits. This
 * is found by restricting the full mask of alternating 0s and 1s to only cover
 * the bottom 2 * log2(dim) bits. That's the same as modding by dim^2.
 */
static uint32_t
ail_space_mask(unsigned x)
{
   assert(util_is_power_of_two_nonzero(x));

   return MOD_POT(0x55555555, x * x);
}

template <typename T, bool is_store>
void
memcpy_small(void *_tiled, void *_linear, const struct ail_layout *tiled_layout,
             unsigned level, unsigned linear_pitch_B, unsigned sx_px,
             unsigned sy_px, unsigned swidth_px, unsigned sheight_px)
{
   enum pipe_format format = tiled_layout->format;
   unsigned linear_pitch_el = linear_pitch_B / sizeof(T);
   unsigned stride_el = tiled_layout->stride_el[level];
   unsigned sx_el = util_format_get_nblocksx(format, sx_px);
   unsigned sy_el = util_format_get_nblocksy(format, sy_px);
   unsigned swidth_el = util_format_get_nblocksx(format, swidth_px);
   unsigned sheight_el = util_format_get_nblocksy(format, sheight_px);
   unsigned sx_end_el = sx_el + swidth_el;
   unsigned sy_end_el = sy_el + sheight_el;

   struct ail_tile tile_size = tiled_layout->tilesize_el[level];
   unsigned tile_area_el = tile_size.width_el * tile_size.height_el;
   unsigned tiles_per_row = DIV_ROUND_UP(stride_el, tile_size.width_el);
   unsigned y_offs_el = ail_space_bits(MOD_POT(sy_el, tile_size.height_el))
                        << 1;
   unsigned x_offs_start_el =
      ail_space_bits(MOD_POT(sx_el, tile_size.width_el));
   unsigned space_mask_x = ail_space_mask(tile_size.width_el);
   unsigned space_mask_y = ail_space_mask(tile_size.height_el) << 1;
   unsigned log2_tile_width_el = util_logbase2(tile_size.width_el);
   unsigned log2_tile_height_el = util_logbase2(tile_size.height_el);

   T *linear = (T *)_linear;
   T *tiled = (T *)_tiled;

   for (unsigned y_el = sy_el; y_el < sy_end_el; ++y_el) {
      unsigned y_rowtile = y_el >> log2_tile_height_el;
      unsigned y_tile = y_rowtile * tiles_per_row;
      unsigned x_offs_el = x_offs_start_el;

      T *linear_row = linear;

      for (unsigned x_el = sx_el; x_el < sx_end_el; ++x_el) {
         unsigned tile_idx = (y_tile + (x_el >> log2_tile_width_el));
         unsigned tile_offset_el = tile_idx * tile_area_el;

         T *ptiled = &tiled[tile_offset_el + y_offs_el + x_offs_el];
         T *plinear = (linear_row++);

         if (is_store)
            *ptiled = *plinear;
         else
            *plinear = *ptiled;

         x_offs_el = (x_offs_el - space_mask_x) & space_mask_x;
      }

      y_offs_el = (y_offs_el - space_mask_y) & space_mask_y;
      linear += linear_pitch_el;
   }
}

/* Gather the bits of v at the even positions (Morton decode) */
static inline unsigned
ail_compact_bits(unsigned v)
{
   v &= 0x55555555;
   v = (v | (v >> 1)) & 0x33333333;
   v = (v | (v >> 2)) & 0x0f0f0f0f;
   v = (v | (v >> 4)) & 0x00ff00ff;
   v = (v | (v >> 8)) & 0x0000ffff;
   return v;
}

/*
 * Detile 32-bit elements tile by tile. The Z-order puts every 4x4 block of a
 * tile in 16 consecutive elements (one 64-byte line), rows two pixel pairs at
 * offsets {0,1,4,5} + {0,2,8,10}[row], so whole tiles are read sequentially
 * and written as 8-byte pairs. Reading the tiled memory in order and in whole
 * lines matters: it is usually write-combined, where small reads are slow. Tiles
 * the box only partially covers fall back to memcpy_small.
 */
static void
ail_detile_32bpp(void *_tiled, void *_linear,
                 const struct ail_layout *tiled_layout, unsigned level,
                 unsigned linear_pitch_B, unsigned sx, unsigned sy,
                 unsigned width, unsigned height)
{
   static const unsigned row_offs[4] = {0, 2, 8, 10};
   struct ail_tile tile_size = tiled_layout->tilesize_el[level];
   unsigned tw = tile_size.width_el, th = tile_size.height_el;
   unsigned area = tw * th;
   unsigned tiles_per_row = DIV_ROUND_UP(tiled_layout->stride_el[level], tw);
   unsigned space_mask_x = ail_space_mask(tw);
   unsigned space_mask_y = ail_space_mask(th) << 1;
   unsigned ex = sx + width, ey = sy + height;
   const uint32_t *tiled = (const uint32_t *)_tiled;
   uint8_t *linear = (uint8_t *)_linear;

   for (unsigned ty = sy / th; ty <= (ey - 1) / th; ++ty) {
      unsigned y0 = ty * th;

      for (unsigned tx = sx / tw; tx <= (ex - 1) / tw; ++tx) {
         unsigned x0 = tx * tw;

         if (x0 < sx || x0 + tw > ex || y0 < sy || y0 + th > ey) {
            unsigned px = MAX2(x0, sx), py = MAX2(y0, sy);

            memcpy_small<uint32_t, false>(
               _tiled, linear + (py - sy) * linear_pitch_B + (px - sx) * 4,
               tiled_layout, level, linear_pitch_B, px, py,
               MIN2(x0 + tw, ex) - px, MIN2(y0 + th, ey) - py);
            continue;
         }

         const uint32_t *tile = tiled + (ty * tiles_per_row + tx) * area;
         uint8_t *dst = linear + (y0 - sy) * linear_pitch_B + (x0 - sx) * 4;

         for (unsigned i = 0; i < area; i += 16) {
            unsigned bx = ail_compact_bits(i & space_mask_x);
            unsigned by = ail_compact_bits((i & space_mask_y) >> 1);
            uint32_t src[16];

            /* One 64-byte read: wide loads, which uncached memory needs */
            memcpy(src, tile + i, sizeof(src));

            for (unsigned r = 0; r < 4; ++r) {
               uint8_t *d = dst + (by + r) * linear_pitch_B + bx * 4;

               memcpy(d, src + row_offs[r], 8);
               memcpy(d + 8, src + row_offs[r] + 4, 8);
            }
         }
      }
   }
}

/*
 * The store direction of ail_detile_32bpp: gather each 4x4 block of a fully
 * covered tile from four linear rows and write it as one sequential 64-byte
 * line. The tiled side is usually write-combined, where whole in-order lines
 * are fast and scattered 4-byte stores are not. Tiles the box only partially
 * covers fall back to memcpy_small.
 */
static void
ail_tile_32bpp(void *_tiled, void *_linear,
               const struct ail_layout *tiled_layout, unsigned level,
               unsigned linear_pitch_B, unsigned sx, unsigned sy,
               unsigned width, unsigned height)
{
   static const unsigned row_offs[4] = {0, 2, 8, 10};
   struct ail_tile tile_size = tiled_layout->tilesize_el[level];
   unsigned tw = tile_size.width_el, th = tile_size.height_el;
   unsigned area = tw * th;
   unsigned tiles_per_row = DIV_ROUND_UP(tiled_layout->stride_el[level], tw);
   unsigned space_mask_x = ail_space_mask(tw);
   unsigned space_mask_y = ail_space_mask(th) << 1;
   unsigned ex = sx + width, ey = sy + height;
   uint32_t *tiled = (uint32_t *)_tiled;
   const uint8_t *linear = (const uint8_t *)_linear;

   for (unsigned ty = sy / th; ty <= (ey - 1) / th; ++ty) {
      unsigned y0 = ty * th;

      for (unsigned tx = sx / tw; tx <= (ex - 1) / tw; ++tx) {
         unsigned x0 = tx * tw;

         if (x0 < sx || x0 + tw > ex || y0 < sy || y0 + th > ey) {
            unsigned px = MAX2(x0, sx), py = MAX2(y0, sy);

            memcpy_small<uint32_t, true>(
               _tiled,
               (void *)(linear + (py - sy) * linear_pitch_B + (px - sx) * 4),
               tiled_layout, level, linear_pitch_B, px, py,
               MIN2(x0 + tw, ex) - px, MIN2(y0 + th, ey) - py);
            continue;
         }

         uint32_t *tile = tiled + (ty * tiles_per_row + tx) * area;
         const uint8_t *src =
            linear + (y0 - sy) * linear_pitch_B + (x0 - sx) * 4;

         for (unsigned i = 0; i < area; i += 16) {
            unsigned bx = ail_compact_bits(i & space_mask_x);
            unsigned by = ail_compact_bits((i & space_mask_y) >> 1);
            uint32_t blk[16];

            for (unsigned r = 0; r < 4; ++r) {
               const uint8_t *s = src + (by + r) * linear_pitch_B + bx * 4;

               memcpy(blk + row_offs[r], s, 8);
               memcpy(blk + row_offs[r] + 4, s + 8, 8);
            }

            /* One 64-byte write: whole lines, which WC memory needs */
            memcpy(tile + i, blk, sizeof(blk));
         }
      }
   }
}

/*
 * ail_tile_32bpp/ail_detile_32bpp for 8- and 16-bit elements: each 64-byte
 * line of a fully covered tile holds an 8x8 (8-bit) or 8x4 (16-bit) Z-order
 * block, gathered from or scattered to the linear rows through a table of
 * offsets, while the tiled side is written or read as one sequential line.
 * Video planes (R8, R8G8) take this path; one store per element to
 * write-combined memory made a 4K frame upload take ~15 ms. Tiles the box
 * only partially covers fall back to memcpy_small.
 */
template <typename T, bool is_store>
static void
ail_lines_small(void *_tiled, void *_linear,
                const struct ail_layout *tiled_layout, unsigned level,
                unsigned linear_pitch_B, unsigned sx, unsigned sy,
                unsigned width, unsigned height)
{
   const unsigned n = 64 / sizeof(T);
   struct ail_tile tile_size = tiled_layout->tilesize_el[level];
   unsigned tw = tile_size.width_el, th = tile_size.height_el;
   unsigned area = tw * th;
   unsigned tiles_per_row = DIV_ROUND_UP(tiled_layout->stride_el[level], tw);
   unsigned space_mask_x = ail_space_mask(tw);
   unsigned space_mask_y = ail_space_mask(th) << 1;
   unsigned ex = sx + width, ey = sy + height;
   T *tiled = (T *)_tiled;
   uint8_t *linear = (uint8_t *)_linear;
   unsigned offs[64];

   for (unsigned e = 0; e < n; ++e)
      offs[e] = ail_compact_bits(e & space_mask_x) * sizeof(T) +
                ail_compact_bits((e & space_mask_y) >> 1) * linear_pitch_B;

#if defined(__aarch64__)
   /* A line is an 8-element-wide block: 8 rows of 8 bytes (8-bit) or 4 rows
    * of 16 bytes (16-bit). Load the rows into four q registers and permute
    * them into Z-order with one 64-byte table lookup.
    */
   const unsigned row_B = 8 * sizeof(T), rows = 64 / row_B;
   uint8_t fwd[64], inv[64];
   bool neon = true;

   for (unsigned k = 0; k < 64; ++k) {
      unsigned e = k / sizeof(T);
      unsigned dx = ail_compact_bits(e & space_mask_x);
      unsigned dy = ail_compact_bits((e & space_mask_y) >> 1);
      unsigned p = dy * row_B + dx * sizeof(T) + k % sizeof(T);

      neon &= dx < 8 && dy < rows;
      fwd[k] = p & 63;
      inv[p & 63] = k;
   }

   uint8x16x4_t to_tiled = vld1q_u8_x4(fwd), to_linear = vld1q_u8_x4(inv);
#endif

   for (unsigned ty = sy / th; ty <= (ey - 1) / th; ++ty) {
      unsigned y0 = ty * th;

      for (unsigned tx = sx / tw; tx <= (ex - 1) / tw; ++tx) {
         unsigned x0 = tx * tw;

         if (x0 < sx || x0 + tw > ex || y0 < sy || y0 + th > ey) {
            unsigned px = MAX2(x0, sx), py = MAX2(y0, sy);

            memcpy_small<T, is_store>(
               _tiled,
               linear + (py - sy) * linear_pitch_B + (px - sx) * sizeof(T),
               tiled_layout, level, linear_pitch_B, px, py,
               MIN2(x0 + tw, ex) - px, MIN2(y0 + th, ey) - py);
            continue;
         }

         T *tile = tiled + (ty * tiles_per_row + tx) * area;
         uint8_t *base =
            linear + (y0 - sy) * linear_pitch_B + (x0 - sx) * sizeof(T);

         for (unsigned i = 0; i < area; i += n) {
            unsigned bx = ail_compact_bits(i & space_mask_x);
            unsigned by = ail_compact_bits((i & space_mask_y) >> 1);
            uint8_t *l = base + by * linear_pitch_B + bx * sizeof(T);
            T blk[64 / sizeof(T)];

#if defined(__aarch64__)
            if (neon) {
               uint8x16x4_t q;

               if (is_store) {
                  for (unsigned r = 0; r < 4; ++r) {
                     if (rows == 8)
                        q.val[r] = vcombine_u8(
                           vld1_u8(l + 2 * r * linear_pitch_B),
                           vld1_u8(l + (2 * r + 1) * linear_pitch_B));
                     else
                        q.val[r] = vld1q_u8(l + r * linear_pitch_B);
                  }
                  uint8x16x4_t t = {{vqtbl4q_u8(q, to_tiled.val[0]),
                                     vqtbl4q_u8(q, to_tiled.val[1]),
                                     vqtbl4q_u8(q, to_tiled.val[2]),
                                     vqtbl4q_u8(q, to_tiled.val[3])}};
                  vst1q_u8_x4((uint8_t *)(tile + i), t);
               } else {
                  q = vld1q_u8_x4((const uint8_t *)(tile + i));
                  for (unsigned r = 0; r < 4; ++r) {
                     uint8x16_t v = vqtbl4q_u8(q, to_linear.val[r]);

                     if (rows == 8) {
                        vst1_u8(l + 2 * r * linear_pitch_B, vget_low_u8(v));
                        vst1_u8(l + (2 * r + 1) * linear_pitch_B,
                                vget_high_u8(v));
                     } else {
                        vst1q_u8(l + r * linear_pitch_B, v);
                     }
                  }
               }
               continue;
            }
#endif

            if (is_store) {
               for (unsigned e = 0; e < n; ++e)
                  memcpy(&blk[e], l + offs[e], sizeof(T));
               memcpy(tile + i, blk, sizeof(blk));
            } else {
               memcpy(blk, tile + i, sizeof(blk));
               for (unsigned e = 0; e < n; ++e)
                  memcpy(l + offs[e], &blk[e], sizeof(T));
            }
         }
      }
   }
}

/* Whether ail_lines_small handles the box: 8/16-bit, a tile of whole lines */
static bool
ail_lines_small_ok(const struct ail_layout *tiled_layout, unsigned level,
                   unsigned blocksize_B, unsigned swidth_px,
                   unsigned sheight_px)
{
   struct ail_tile tile_size = tiled_layout->tilesize_el[level];

   return (blocksize_B == 1 || blocksize_B == 2) && swidth_px && sheight_px &&
          util_format_get_blockwidth(tiled_layout->format) == 1 &&
          util_format_get_blockheight(tiled_layout->format) == 1 &&
          tile_size.width_el * tile_size.height_el >= 64 / blocksize_B;
}

#define TILED_UNALIGNED_TYPES(blocksize_B, store)                              \
   if (blocksize_B == 1) {                                                     \
      memcpy_small<uint8_t, store>(_tiled, _linear, tiled_layout, level,       \
                                   linear_pitch_B, sx_px, sy_px, swidth_px,    \
                                   sheight_px);                                \
   } else if (blocksize_B == 2) {                                              \
      memcpy_small<uint16_t, store>(_tiled, _linear, tiled_layout, level,      \
                                    linear_pitch_B, sx_px, sy_px, swidth_px,   \
                                    sheight_px);                               \
   } else if (blocksize_B == 4) {                                              \
      memcpy_small<uint32_t, store>(_tiled, _linear, tiled_layout, level,      \
                                    linear_pitch_B, sx_px, sy_px, swidth_px,   \
                                    sheight_px);                               \
   } else if (blocksize_B == 8) {                                              \
      memcpy_small<uint64_t, store>(_tiled, _linear, tiled_layout, level,      \
                                    linear_pitch_B, sx_px, sy_px, swidth_px,   \
                                    sheight_px);                               \
   } else if (blocksize_B == 16) {                                             \
      memcpy_small<ail_uint128_t, store>(_tiled, _linear, tiled_layout, level, \
                                         linear_pitch_B, sx_px, sy_px,         \
                                         swidth_px, sheight_px);               \
   } else {                                                                    \
      UNREACHABLE("Invalid block size");                                       \
   }

void
ail_detile(void *_tiled, void *_linear, const struct ail_layout *tiled_layout,
           unsigned level, unsigned linear_pitch_B, unsigned sx_px,
           unsigned sy_px, unsigned swidth_px, unsigned sheight_px)
{
   unsigned width_px = u_minify(tiled_layout->width_px, level);
   unsigned height_px = u_minify(tiled_layout->height_px, level);
   unsigned blocksize_B = util_format_get_blocksize(tiled_layout->format);

   assert(level < tiled_layout->levels && "Mip level out of bounds");
   assert(ail_is_level_twiddled_uncompressed(tiled_layout, level) &&
          "Invalid usage");
   assert((sx_px + swidth_px) <= width_px && "Invalid usage");
   assert((sy_px + sheight_px) <= height_px && "Invalid usage");

   struct ail_tile tile_size = tiled_layout->tilesize_el[level];
   if (blocksize_B == 4 && swidth_px && sheight_px &&
       util_format_get_blockwidth(tiled_layout->format) == 1 &&
       util_format_get_blockheight(tiled_layout->format) == 1 &&
       tile_size.width_el >= 4 && tile_size.height_el >= 4) {
      ail_detile_32bpp(_tiled, _linear, tiled_layout, level, linear_pitch_B,
                       sx_px, sy_px, swidth_px, sheight_px);
      return;
   }

   if (ail_lines_small_ok(tiled_layout, level, blocksize_B, swidth_px,
                          sheight_px)) {
      if (blocksize_B == 1)
         ail_lines_small<uint8_t, false>(_tiled, _linear, tiled_layout, level,
                                         linear_pitch_B, sx_px, sy_px,
                                         swidth_px, sheight_px);
      else
         ail_lines_small<uint16_t, false>(_tiled, _linear, tiled_layout,
                                          level, linear_pitch_B, sx_px, sy_px,
                                          swidth_px, sheight_px);
      return;
   }

   TILED_UNALIGNED_TYPES(blocksize_B, false);
}

void
ail_tile(void *_tiled, void *_linear, const struct ail_layout *tiled_layout,
         unsigned level, unsigned linear_pitch_B, unsigned sx_px,
         unsigned sy_px, unsigned swidth_px, unsigned sheight_px)
{
   unsigned width_px = u_minify(tiled_layout->width_px, level);
   unsigned height_px = u_minify(tiled_layout->height_px, level);
   unsigned blocksize_B = ail_get_blocksize_B(tiled_layout);

   assert(level < tiled_layout->levels && "Mip level out of bounds");
   assert(ail_is_level_twiddled_uncompressed(tiled_layout, level) &&
          "Invalid usage");
   assert((sx_px + swidth_px) <= width_px && "Invalid usage");
   assert((sy_px + sheight_px) <= height_px && "Invalid usage");

   struct ail_tile tile_size = tiled_layout->tilesize_el[level];
   if (blocksize_B == 4 && swidth_px && sheight_px &&
       util_format_get_blockwidth(tiled_layout->format) == 1 &&
       util_format_get_blockheight(tiled_layout->format) == 1 &&
       tile_size.width_el >= 4 && tile_size.height_el >= 4) {
      ail_tile_32bpp(_tiled, _linear, tiled_layout, level, linear_pitch_B,
                     sx_px, sy_px, swidth_px, sheight_px);
      return;
   }

   if (ail_lines_small_ok(tiled_layout, level, blocksize_B, swidth_px,
                          sheight_px)) {
      if (blocksize_B == 1)
         ail_lines_small<uint8_t, true>(_tiled, _linear, tiled_layout, level,
                                        linear_pitch_B, sx_px, sy_px,
                                        swidth_px, sheight_px);
      else
         ail_lines_small<uint16_t, true>(_tiled, _linear, tiled_layout, level,
                                         linear_pitch_B, sx_px, sy_px,
                                         swidth_px, sheight_px);
      return;
   }

   TILED_UNALIGNED_TYPES(blocksize_B, true);
}
