/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#include "agx3_mem_core.h"

#include "agx3_binder.h"

#define KV(f, v) {(uint16_t)(f), (uint64_t)(v)}

static bool
common_ok(enum agx_isa_encoding enc, const struct agx3_mem_desc *d)
{
   if (d == NULL || d->data >= 256 || d->mask < 1 || d->mask > 15 ||
       d->shift > 4)
      return false;
   if (d->width != AGX3_MEM_W8 && d->width != AGX3_MEM_W16 &&
       d->width != AGX3_MEM_W32)
      return false;
   if (d->base_is_uniform) {
      if ((d->base & 3) != 0 || d->base > 508)
         return false;
   } else if ((d->base & 1) != 0 || d->base > (enc == AGX_ISA_ENCODING_AGX3C ? 252 : 188)) {
      return false;
   }
   if (d->idx_kind == AGX3_MEM_IDX_REG &&
       ((d->idx_reg & 1) != 0 || d->idx_reg > (enc == AGX_ISA_ENCODING_AGX3C ? 254 : 191)))
      return false;
   if (d->idx_kind == AGX3_MEM_IDX_IMM && d->idx_scaled) {
      if (d->elem_idx > 0xffff)
         return false;
   } else if (d->byte_off < -32768 || d->byte_off > 32767) {
      return false;
   }
   return true;
}

/* Build the operand overrides shared by loads and stores. `off_field`,
 * `shift_field` and `mask_field` differ between the two. Returns the number of
 * overrides written. */
static unsigned
common_overrides(const struct agx3_mem_desc *d, uint16_t off_field,
                 uint16_t shift_field, uint16_t mask_field,
                 struct agx3_field_kv *ov)
{
   unsigned n = 0;
   ov[n++] = (struct agx3_field_kv)KV(2, d->data);
   ov[n++] = (struct agx3_field_kv)KV(10, d->width);
   ov[n++] = (struct agx3_field_kv)KV(mask_field, d->mask);

   /* Index. */
   if (d->idx_kind == AGX3_MEM_IDX_IMM) {
      ov[n++] = (struct agx3_field_kv)KV(23, 1);
      ov[n++] = (struct agx3_field_kv)KV(24, 1);
      if (d->idx_scaled) {
         ov[n++] = (struct agx3_field_kv)KV(15, d->elem_idx);
         ov[n++] = (struct agx3_field_kv)KV(shift_field, d->shift);
      } else {
         ov[n++] =
            (struct agx3_field_kv)KV(off_field, (uint16_t)d->byte_off);
         ov[n++] = (struct agx3_field_kv)KV(shift_field, 0);
      }
   } else {
      ov[n++] = (struct agx3_field_kv)KV(23, 2);
      ov[n++] = (struct agx3_field_kv)KV(24, 2);
      ov[n++] = (struct agx3_field_kv)KV(16, d->idx_reg);
      ov[n++] = (struct agx3_field_kv)KV(27, d->idx_kill ? 1 : 0);
      ov[n++] = (struct agx3_field_kv)KV(28, d->idx_signed ? 1 : 0);
      ov[n++] = (struct agx3_field_kv)KV(shift_field, d->shift);
      /* The byte offset is added after the index is scaled. */
      ov[n++] = (struct agx3_field_kv)KV(off_field, (uint16_t)d->byte_off);
   }

   /* Base. */
   if (d->base_is_uniform) {
      ov[n++] = (struct agx3_field_kv)KV(38, 3);
      ov[n++] = (struct agx3_field_kv)KV(32, d->base);
   } else {
      ov[n++] = (struct agx3_field_kv)KV(38, 2);
      ov[n++] = (struct agx3_field_kv)KV(31, d->base);
      ov[n++] = (struct agx3_field_kv)KV(42, d->base_kill ? 1 : 0);
   }

   /* Wait mask -> f60+i. */
   for (unsigned i = 0; i < 8; ++i) {
      if (d->wait_mask & (1u << i))
         ov[n++] = (struct agx3_field_kv)KV(60 + i, 1);
   }
   return n;
}

static void
mem_baseline(enum agx_isa_encoding enc, bool is_load,
             struct agx3_field_kv *base, unsigned *n)
{
   unsigned i = 0;
   base[i++] = (struct agx3_field_kv)KV(9, 2);
   base[i++] = (struct agx3_field_kv)KV(39, 3);
   base[i++] = (struct agx3_field_kv)KV(54, 1);
   if (enc == AGX_ISA_ENCODING_AGX3C) {
      base[i++] = (struct agx3_field_kv)KV(45, 15);
      base[i++] = (struct agx3_field_kv)KV(53, 1);
      base[i++] = (struct agx3_field_kv)KV(59, is_load ? 104 : 187);
   }
   *n = i;
}

bool
agx3_mem_encode_load_form(enum agx_isa_encoding enc,
                          const struct agx3_mem_desc *d, uint8_t ep,
                          uint8_t out[AGX3_MEM_MAX_LENGTH], uint8_t *len)
{
   if (!common_ok(enc, d) || d->slot < 1 ||
       d->slot > (enc == AGX_ISA_ENCODING_AGX3C ? 8 : 6) ||
       (enc == AGX_ISA_ENCODING_AGX3B ? ep != 0 : ep > 5))
      return false;

   struct agx3_field_kv base[7];
   unsigned nbase;
   mem_baseline(enc, true, base, &nbase);
   if (d->agx3b_last_load) {
      if (enc != AGX_ISA_ENCODING_AGX3B)
         return false;
      base[nbase++] = (struct agx3_field_kv)KV(79, 2);
   }

   struct agx3_field_kv ov[32];
   unsigned n = common_overrides(d, /*off*/ 76, /*shift*/ 75, /*mask*/ 73, ov);
   ov[n++] = (struct agx3_field_kv)KV(12, d->slot);
   ov[n++] = (struct agx3_field_kv)KV(77, d->coherent ? 1 : 0);

   return agx3_bind_compose(enc, "lddev", ep, base, nbase, ov, n, out, len);
}

bool
agx3_mem_encode_load(enum agx_isa_encoding enc, const struct agx3_mem_desc *d,
                     uint8_t out[AGX3_MEM_MAX_LENGTH], uint8_t *len)
{
   if (enc == AGX_ISA_ENCODING_AGX3C) {
      static const uint8_t forms[] = {0, 1, 3, 4, 2, 5};
      for (unsigned i = 0; i < sizeof(forms); ++i) {
         if (agx3_mem_encode_load_form(enc, d, forms[i], out, len))
            return true;
      }
      return false;
   }
   return agx3_mem_encode_load_form(enc, d, 0, out, len);
}

bool
agx3_mem_encode_store(enum agx_isa_encoding enc, const struct agx3_mem_desc *d,
                      uint8_t out[AGX3_MEM_MAX_LENGTH], uint8_t *len)
{
   if (!common_ok(enc, d) || d->agx3b_last_load)
      return false;

   struct agx3_field_kv base[6];
   unsigned nbase;
   mem_baseline(enc, false, base, &nbase);

   struct agx3_field_kv ov[32];
   unsigned n = common_overrides(d, /*off*/ 74, /*shift*/ 73, /*mask*/ 71, ov);
   ov[n++] = (struct agx3_field_kv)KV(75, d->coherent ? 1 : 0);

   if (enc == AGX_ISA_ENCODING_AGX3C) {
      static const uint8_t forms[] = {0, 1, 3, 4, 2, 5};
      for (unsigned i = 0; i < sizeof(forms); ++i) {
         if (agx3_bind_compose(enc, "stdev", forms[i], base, nbase,
                               ov, n, out, len))
            return true;
      }
      return false;
   }
   return agx3_bind_compose(enc, "stdev", 0, base, nbase, ov, n, out, len);
}
