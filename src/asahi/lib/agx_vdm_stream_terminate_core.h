/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Bounded, target-aware AGX3 VDM Stream Terminate representation.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "agx_chip.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AGX_VDM_STREAM_TERMINATE_CORE_LENGTH 4u
#define AGX_VDM_STREAM_TERMINATE_CORE_WORD   UINT32_C(0xc0000000)

struct agx_vdm_stream_terminate_core {
   uint8_t bytes[AGX_VDM_STREAM_TERMINATE_CORE_LENGTH];
   uint8_t length;
};

/*
 * Pack/unpack the exact four-byte type-6 VDM Stream Terminate copied by the
 * inspected G15G/G15X, G16G/G16X, G17P, and G17G/G17X Apple Metal render
 * contexts.  Each matching context constructor copies the same constant pair
 * {0, 0xc0000000}; emitEndCommandStream advances four bytes and writes its
 * second word.
 *
 * The allowlist contains only identities directly served by those plugins,
 * plus G18G, whose J873 native VDM streams all end in 0xc0000000; G16P and
 * G17A remain closed.  The unlocated type-7 Stream Return form also
 * remains closed.  This helper does not allocate or emit a live command,
 * submit work, or admit a device.
 */
bool agx_vdm_stream_terminate_core_supported(enum agx_chip chip);

bool
agx_vdm_stream_terminate_core_pack(enum agx_chip chip,
                                   struct agx_vdm_stream_terminate_core *out);

bool agx_vdm_stream_terminate_core_unpack(enum agx_chip chip, const void *bytes,
                                          size_t size);

#ifdef __cplusplus
}
#endif
