/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#include "agx_ppp_hal300_depth_stencil_core.h"
#include "agx_bytes.h"

#include <string.h>

/*
 * Pack/unpack only the exact six-word HAL300 PPP block recovered from the
 * inspected G17G fixed-layout render-state path:
 *
 *   u32 header = bit 0
 *   u32 fragment control
 *   u32 fragment front face
 *   u32 fragment back face
 *   u32 fragment front stencil
 *   u32 fragment back stencil
 *
 * The G17G path copies the 24-byte constant block at 0x4e8180..0x4e81c0.
 * Its local rodata witness at file offset 0x952a40 begins with header bit 0
 * and five recognizable depth/stencil payload words.  Dynamic lowering at
 * 0x4ec5a4..0x4ec628 stages the same five slots in the order above.  The
 * homologous G16 path at 0x213e49834..0x213e498b8 instead stages front
 * stencil before back face, proving the HAL300-only adjacent-word swap.
 *
 * The three admitted chip identities are the HAL300 G17 GPU classes covered
 * by the G17G/G17X plugin witnesses.  This codec neither emits the surrounding
 * VDM PPP pointer nor admits a device or advertises a feature.
 */
bool
agx_ppp_hal300_depth_stencil_core_supported(enum agx_chip chip)
{
   switch (chip) {
   case AGX_CHIP_G17G:
   case AGX_CHIP_G17S:
   case AGX_CHIP_G17C:
   case AGX_CHIP_G18G:
      return true;

   default:
      return false;
   }
}

bool
agx_ppp_hal300_depth_stencil_core_pack(
   enum agx_chip chip,
   const struct agx_ppp_hal300_depth_stencil_fields *fields,
   struct agx_ppp_hal300_depth_stencil_core *out)
{
   if (out)
      memset(out, 0, sizeof(*out));

   if (!out || !fields ||
       !agx_ppp_hal300_depth_stencil_core_supported(chip))
      return false;

   agx_store_le32(
      &out->bytes[0], AGX_PPP_HAL300_DEPTH_STENCIL_HEADER);
   agx_store_le32(
      &out->bytes[4], fields->fragment_control_raw);
   agx_store_le32(
      &out->bytes[8], fields->fragment_front_face_raw);
   agx_store_le32(
      &out->bytes[12], fields->fragment_back_face_raw);
   agx_store_le32(
      &out->bytes[16], fields->fragment_front_stencil_raw);
   agx_store_le32(
      &out->bytes[20], fields->fragment_back_stencil_raw);
   out->length = AGX_PPP_HAL300_DEPTH_STENCIL_CORE_LENGTH;
   return true;
}

bool
agx_ppp_hal300_depth_stencil_core_unpack(
   enum agx_chip chip, const void *bytes, size_t size,
   struct agx_ppp_hal300_depth_stencil_fields *fields)
{
   if (fields)
      memset(fields, 0, sizeof(*fields));

   if (!bytes || !fields || size != AGX_PPP_HAL300_DEPTH_STENCIL_CORE_LENGTH ||
       !agx_ppp_hal300_depth_stencil_core_supported(chip))
      return false;

   const uint8_t *raw = bytes;
   if (agx_load_le32(raw) !=
       AGX_PPP_HAL300_DEPTH_STENCIL_HEADER)
      return false;

   fields->fragment_control_raw =
      agx_load_le32(&raw[4]);
   fields->fragment_front_face_raw =
      agx_load_le32(&raw[8]);
   fields->fragment_back_face_raw =
      agx_load_le32(&raw[12]);
   fields->fragment_front_stencil_raw =
      agx_load_le32(&raw[16]);
   fields->fragment_back_stencil_raw =
      agx_load_le32(&raw[20]);
   return true;
}
