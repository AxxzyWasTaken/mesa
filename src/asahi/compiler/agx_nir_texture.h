/*
 * Copyright 2023 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* Texture backend flags */
#define AGX_TEXTURE_FLAG_NO_CLAMP (1 << 0)

/* Indicates that the sampler should be overriden to clamp to 0 instead of 1 */
#define AGX_TEXTURE_FLAG_CLAMP_TO_0 (1 << 1)

/* Software border query for an unfiltered gather, rather than interpolation. */
#define AGX_TEXTURE_FLAG_BORDER_UNFILTERED (1 << 2)

/* G17P custom-border gather probe: non-comparing clamp-to-one sampler. */
#define AGX_TEXTURE_FLAG_BORDER_NONCOMPARE (1 << 3)

/* The texture handle serves the descriptor crawl only; the texture operation
 * itself names its texture state table entry by immediate index. G17G's
 * native fragment fetches are bindful this way while the level and layer
 * clamps still read the descriptor. */
#define AGX_TEXTURE_FLAG_TABLE_INDEX (1 << 4)

/* Bound buffer metadata in backend2: [texel count, RGB32, address lo, address
 * hi], all uint32. The address includes the view offset. Buffer lowering
 * consumes this before backend2 can represent offsets for a native 2D fetch. */
#define AGX_TEXTURE_FLAG_BUFFER_METADATA (1 << 5)

/* Texel buffers lowered to (at most) 16384x16384 2D textures */
#define AGX_TEXTURE_BUFFER_WIDTH      16384
#define AGX_TEXTURE_BUFFER_MAX_HEIGHT 16384
#define AGX_TEXTURE_BUFFER_MAX_SIZE                                            \
   (AGX_TEXTURE_BUFFER_WIDTH * AGX_TEXTURE_BUFFER_MAX_HEIGHT)
