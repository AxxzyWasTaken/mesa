/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "asahi/compiler/agx_compile.h"
#include "util/format/u_formats.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bind the logical RT0 format to the exact native G17P EOT selector key. */
void agx_g17p_configure_rt0_eot_key(enum pipe_format format,
                                    struct agx_shader_key *key);

/* Return the IMGWBLK selector of one target in Apple's two-to-four-target
 * G17P EOT builtin, or zero if the format has no such target. The builtin
 * factory derives each target's selector from that target's format alone,
 * so it equals the selector of the format's single-target program. Formats
 * whose single-target program has a different shape are excluded. */
uint8_t agx_g17p_mrt_eot_selector(enum pipe_format format);

enum pipe_format agx_g17p_rt0_pbe_format(enum pipe_format format);

/* M5 keeps an R11G11B10 tile word in the packed-float tile format and stores
 * it through an R11G11B10 PBE, so the image keeps the compression class of
 * the textures that read it. G17P stores the raw word through R32_UINT. */
static inline bool
agx_rg11b10_tile_is_packed_float(enum agx_chip chip)
{
   return agx_chip_g17_m5(chip);
}

/* The end-of-tile PBE format of a render target on this chip. */
enum pipe_format agx_rt0_pbe_format(enum agx_chip chip,
                                    enum pipe_format format);

/* agx_g17p_mrt_eot_selector() for this chip. */
uint8_t agx_mrt_eot_selector(enum agx_chip chip, enum pipe_format format);

#ifdef __cplusplus
}
#endif
