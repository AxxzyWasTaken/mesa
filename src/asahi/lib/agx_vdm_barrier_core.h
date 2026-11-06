/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Bounded, target-aware AGX3 VDM memory-barrier representation.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "agx_chip.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AGX_VDM_BARRIER_CORE_LENGTH 4u
#define AGX_VDM_BARRIER_CORE_WORD   UINT32_C(0x21000960)

struct agx_vdm_barrier_core {
   uint8_t bytes[AGX_VDM_BARRIER_CORE_LENGTH];
   uint8_t length;
};

bool agx_vdm_barrier_core_supported(enum agx_chip chip);

bool agx_vdm_barrier_core_pack(enum agx_chip chip,
                               struct agx_vdm_barrier_core *out);

bool agx_vdm_barrier_core_unpack(enum agx_chip chip, const void *bytes,
                                 size_t size);

#ifdef __cplusplus
}
#endif
