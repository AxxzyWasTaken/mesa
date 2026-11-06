/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * AGX3 pixel writeout barriers: wait_pix -> pixwait and signal_pix -> pixrel.
 *
 * Every Apple fragment shader brackets its tile traffic with these two:
 *
 *   pixwait(gate) ... [movmsk] ... lstcol x (RT x sample) ... pixrel(release)
 *
 * Both carry a writeout-class mask that is the same bitmask AGX2 spells as the
 * wait_pix / signal_pix `writeout` immediate -- 1 for the depth/coverage class,
 * 4/8/12 for the colour classes -- so the IR immediate passes straight through.
 * Only the masks Apple's own compiler emitted are accepted; anything else fails
 * closed rather than guessing a class the hardware may interpret differently.
 *
 * Composer-driven; reproduces Apple's witnessed G15G/G16G/G17S bytes exactly.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "asahi/lib/agx_chip.h"

#ifdef __cplusplus
extern "C" {
#endif

/* pixwait and pixrel are both six bytes on AGX3b and AGX3c. */
#define AGX3_PIXEL_MAX_LENGTH 6u

/* Complete native G17P colour-partition class corpus. */
#define AGX3_G17P_MAX_COLOUR_PARTITIONS 3u

/*
 * Open the colour-tile protocol. This is the f14=512, f15=1 pixwait form
 * that precedes the ordinary class-12 colour wait. It is byte-exactly
 * witnessed on G17P (AGX3b) and the M5 native flat-colour fragment (AGX3c).
 * AGX2 is refused. Encoding this gate does not qualify other M5 transactions.
 */
bool agx3_pixel_colour_entry(enum agx_isa_encoding enc,
                             uint8_t out[AGX3_PIXEL_MAX_LENGTH], uint8_t *len);
/*
 * The same entry gate with f34 set: 077000050000 on AGX3c. Every native G17G
 * fragment that writes depth or stencil alone at several samples opens with
 * it, and exactly those never release the depth/coverage class explicitly
 * (no pixrel 1); every other witnessed fragment uses the plain gate. What
 * the bit selects in hardware is not decoded. AGX3c only.
 */
bool agx3_pixel_colour_entry_zs_unreleased(enum agx_isa_encoding enc,
                                           uint8_t out[AGX3_PIXEL_MAX_LENGTH],
                                           uint8_t *len);

/*
 * Bracket a colourless fragment's returning memory atomic. Apple's G17P
 * fragments with no colour writes acquire pixel class 4 without the colour
 * selector (f14=4, no f34) immediately before the atomic, and follow the
 * reload of its result with a barrier. A fragment returning a colour instead
 * acquires colour partition 0's write class (870254040800) there, and its
 * store later acquires only class 8. Voiding atomics and plain stores take
 * neither. AGX3b only.
 */
bool agx3_pixel_atomic_acquire(enum agx_isa_encoding enc,
                               uint8_t out[AGX3_PIXEL_MAX_LENGTH],
                               uint8_t *len);
bool agx3_pixel_atomic_barrier(enum agx_isa_encoding enc,
                               uint8_t out[AGX3_PIXEL_MAX_LENGTH],
                               uint8_t *len);

/*
 * Return the read, write, and combined pixel classes for one colour-tile
 * partition.  G17P assigns two class bits per partition: RT0 uses 8/4/12,
 * RT1 uses 32/16/48, and RT2 uses 128/64/192.  These are the complete
 * partition classes witnessed by native multi-RT shaders; higher partitions
 * fail closed.
 */
bool agx3_pixel_colour_partition_classes(unsigned partition,
                                         uint32_t *read_class,
                                         uint32_t *write_class,
                                         uint32_t *combined_class);

/*
 * Wait for the writeout classes named by `writeout` before continuing.
 *
 * `writeout` is the AGX2 wait_pix immediate. Accepted values are exactly those
 * witnessed in Apple's fragment corpus: 1 (depth/coverage) and 4, 8, 12, 16,
 * 32, 48, 60, 64, 128, 192 (colour classes; the multi-RT shaders widen the
 * mask). Mesa emits 1, 4, 8 and 12.
 *
 * The shader-entry gate (`f14=512, f15=1`) is exposed separately through
 * agx3_pixel_colour_entry(): it is a prologue Apple's compiler inserts, not a
 * writeout-class wait described by Mesa IR.
 *
 * `wait_mask` is the scoreboard wait set, one bit per slot starting at slot 1
 * (six slots on AGX3b, eight on AGX3c), matching the tile and memory cores.
 */
bool agx3_pixel_wait(enum agx_isa_encoding enc, uint32_t writeout,
                     uint16_t wait_mask, uint8_t out[AGX3_PIXEL_MAX_LENGTH],
                     uint8_t *len);

/*
 * Release the writeout classes named by `writeout`.
 *
 * The corpus descriptor factors exactly as `(last ? 0x200 : 0) | writeout`
 * across all nine witnessed values, so `last` -- set on the release that ends
 * the shader's writeout sequence -- is a separate argument. Accepted
 * `writeout` values are the witnessed set {0, 1, 12, 48, 60, 192}; the
 * colour-class singletons that only ever appear on pixwait (4, 8, 16, ...) are
 * refused here because no pixrel witness carries them.
 */
bool agx3_pixel_release(enum agx_isa_encoding enc, uint32_t writeout, bool last,
                        uint16_t wait_mask, uint8_t out[AGX3_PIXEL_MAX_LENGTH],
                        uint8_t *len);

#ifdef __cplusplus
}
#endif
