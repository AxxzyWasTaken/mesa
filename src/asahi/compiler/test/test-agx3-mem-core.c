/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * The AGX3 memory encoders must reproduce real Apple lddev/stdev instructions.
 * Each vector is a device load/store decoded from the G16G/G17S corpus; the
 * encoder rebuilds its bytes from the normalized descriptor.
 */

#include "agx3_mem_core.h"
#include "agx3_mem_core_vectors.c"

#include <stdio.h>
#include <string.h>

int
main(void)
{
   unsigned pass = 0, fail = 0;

   /* Native M5 m5_copy_indexed_buffers: the first store keeps its value
    * and base live for the next store, discards its index, and waits slot8. */
   {
      const uint8_t expected[] = {
         0x0f,0x08,0x03,0x0e,0xe1,0x0e,0x00,0x80,0x00,0x20,
      };
      const struct agx3_mem_desc d = {
         .data = 0, .width = AGX3_MEM_W32, .mask = 15,
         .base = 8, .idx_kind = AGX3_MEM_IDX_REG,
         .idx_reg = 14, .idx_kill = true, .shift = 4, .wait_mask = 128,
      };
      uint8_t out[AGX3_MEM_MAX_LENGTH], len = 0;
      if (!agx3_mem_encode_store(AGX_ISA_ENCODING_AGX3C, &d, out, &len) ||
          len != sizeof(expected) || memcmp(out, expected, sizeof(expected))) {
         fprintf(stderr, "FAIL M5 register-base store extended wait\n");
         fail++;
      } else {
         pass++;
      }
   }

   /* AGX3c table composition of a descriptor load at uniform12+244,
    * preserving four words and both producer waits1+4. The compact ep0
    * cannot represent that dependency set; native form order selects ep1. */
   {
      const uint8_t expected[] = {0x4f,0x0c,0x03,0x00,0x78,0xac,0x80,0xbe,0x00,0x20};
      const struct agx3_mem_desc d = {
         .data = 8, .width = AGX3_MEM_W32, .mask = 15,
         .base_is_uniform = true, .base = 12,
         .idx_kind = AGX3_MEM_IDX_IMM, .byte_off = 244,
         .slot = 4, .wait_mask = 9,
      };
      uint8_t out[AGX3_MEM_MAX_LENGTH], len = 0;
      if (agx3_mem_encode_load_form(AGX_ISA_ENCODING_AGX3C, &d, 0, out, &len) ||
          !agx3_mem_encode_load(AGX_ISA_ENCODING_AGX3C, &d, out, &len) ||
          len != sizeof(expected) || memcmp(out, expected, sizeof(expected))) {
         fprintf(stderr, "FAIL M5 descriptor load extended waits\n");
         fail++;
      } else {
         pass++;
      }
   }

   for (size_t i = 0; i < mem_vec_count; ++i) {
      const struct mem_vec *v = &mem_vecs[i];
      const enum agx_isa_encoding e =
         v->enc == 0 ? AGX_ISA_ENCODING_AGX3B : AGX_ISA_ENCODING_AGX3C;

      struct agx3_mem_desc d = {
         .data = (uint16_t)v->data,
         .width = (enum agx3_mem_width)v->width,
         .mask = (uint8_t)v->mask,
         .shift = (uint8_t)v->shift,
         .base_is_uniform = v->base_uni != 0,
         .base = (uint16_t)v->base,
         .base_kill = v->base_kill != 0,
         .idx_kind = v->idx_reg ? AGX3_MEM_IDX_REG : AGX3_MEM_IDX_IMM,
         .idx_scaled = v->scaled != 0,
         .byte_off = v->byte_off,
         .elem_idx = v->elem_idx,
         .idx_reg = (uint16_t)v->idx_reg_v,
         .idx_signed = v->idx_signed != 0,
         .idx_kill = v->idx_kill != 0,
         .slot = (uint8_t)v->slot,
         .wait_mask = (uint16_t)v->wait,
      };

      uint8_t out[AGX3_MEM_MAX_LENGTH];
      uint8_t len = 0;
      bool ok = v->is_load ? agx3_mem_encode_load(e, &d, out, &len)
                           : agx3_mem_encode_store(e, &d, out, &len);

      if (!ok || len != v->nbytes || memcmp(out, v->bytes, v->nbytes) != 0) {
         if (fail < 10)
            fprintf(stderr, "FAIL %s i=%zu enc=%d\n",
                    v->is_load ? "lddev" : "stdev", i, v->enc);
         fail++;
      } else {
         pass++;
      }
   }

   /* Apple's complete G17P float2-position VS: its sole/final LDDEV uses
    * normalized f79=2, signals slot 6, and waits for vertex_id in slot 1. */
   {
      static const uint8_t expected[] = {
         0x67, 0x10, 0x44, 0x00, 0x00, 0x80, 0x20,
         0x00, 0x59, 0x01, 0x00, 0x40, 0x48, 0x00,
      };
      struct agx3_mem_desc d = {
         .data = 0,
         .width = AGX3_MEM_W32,
         .mask = 3,
         .shift = 3,
         .base_is_uniform = true,
         .base = 0,
         .idx_kind = AGX3_MEM_IDX_REG,
         .idx_reg = 0,
         .idx_kill = true,
         .slot = 6,
         .wait_mask = 1,
         .last_load = true,
      };
      uint8_t out[AGX3_MEM_MAX_LENGTH];
      uint8_t len = 0;
      const bool ok = agx3_mem_encode_load(AGX_ISA_ENCODING_AGX3B, &d, out,
                                           &len);
      if (!ok || len != sizeof(expected) || memcmp(out, expected, len)) {
         fprintf(stderr, "FAIL G17P final vertex LDDEV\n");
         fail++;
      } else {
         pass++;
      }

      if (agx3_mem_encode_store(AGX_ISA_ENCODING_AGX3B, &d, out, &len) ||
          agx3_mem_encode_store(AGX_ISA_ENCODING_AGX3C, &d, out, &len)) {
         fprintf(stderr, "FAIL final-load store guard\n");
         fail++;
      } else {
         pass += 2;
      }
   }

   /* Apple Metal, applegpu_g17g (gpu/m5-xfb-alu-reference.metal). The final
    * LDDEV of a program is the 14-byte ep2 form with f82=2; every earlier
    * load keeps the 8-byte ep0 form. Same descriptor fields otherwise. */
   {
      static const struct {
         const char *name;
         uint16_t data, base, idx_reg;
         uint8_t width, mask, shift, slot;
         uint16_t wait;
         bool last;
         uint8_t nbytes;
         uint8_t bytes[14];
      } vecs[] = {
         /* m5_xfb_load_uint4_indexed: out[id] = table[index[id]] */
         {"index[id]", 0, 4, 8, AGX3_MEM_W32, 1, 2, 8, 1, false, 8,
          {0x0f, 0x04, 0x03, 0x08, 0x18, 0x22, 0x10, 0x40}},
         {"table[index]", 0, 0, 0, AGX3_MEM_W32, 15, 4, 8, 0x80, true, 14,
          {0x0f, 0x00, 0x03, 0x00, 0x78, 0x0a, 0x10, 0x80, 0x41, 0x20, 0x80,
           0x00, 0x00, 0x00}},
         /* m5_xfb_imad64: a[id] then b[id] */
         {"a[id]", 2, 0, 4, AGX3_MEM_W32, 1, 2, 8, 1, false, 8,
          {0x1f, 0x00, 0x03, 0x04, 0x18, 0x22, 0x10, 0x40}},
         {"b[id]", 0, 4, 4, AGX3_MEM_W32, 1, 2, 8, 0, true, 14,
          {0x0f, 0x04, 0x03, 0x04, 0x18, 0x02, 0x10, 0xc0, 0x41, 0x00, 0x80,
           0x00, 0x00, 0x00}},
         /* m5_xfb_ctz: a 16-bit element, sole load */
         {"ushort a[id]", 2, 0, 0, AGX3_MEM_W16, 1, 1, 8, 1, true, 14,
          {0x17, 0x00, 0x03, 0x00, 0x18, 0x22, 0x10, 0x80, 0xc1, 0x00, 0x80,
           0x00, 0x00, 0x00}},
      };
      for (size_t i = 0; i < sizeof(vecs) / sizeof(vecs[0]); ++i) {
         struct agx3_mem_desc d = {
            .data = vecs[i].data,
            .width = (enum agx3_mem_width)vecs[i].width,
            .mask = vecs[i].mask,
            .shift = vecs[i].shift,
            .base_is_uniform = true,
            .base = vecs[i].base,
            .idx_kind = AGX3_MEM_IDX_REG,
            .idx_reg = vecs[i].idx_reg,
            .slot = vecs[i].slot,
            .wait_mask = vecs[i].wait,
            .last_load = vecs[i].last,
         };
         uint8_t out[AGX3_MEM_MAX_LENGTH];
         uint8_t len = 0;
         if (!agx3_mem_encode_load(AGX_ISA_ENCODING_AGX3C, &d, out, &len) ||
             len != vecs[i].nbytes || memcmp(out, vecs[i].bytes, len)) {
            fprintf(stderr, "FAIL G17G %s load\n", vecs[i].name);
            fail++;
         } else {
            pass++;
         }
      }
   }

   /* Fresh G17P: *(device uint *)(byte_base + offsets[id]). The
    * 32-bit access uses an unscaled byte index, not an element index. */
   {
      const uint8_t expected[]={0x67,0x00,0x46,0x00,0x01,0x80,0x20,
                                0x00,0x51,0x01,0x00,0x40,0x42,0x00};
      struct agx3_mem_desc d={.data=0,.width=AGX3_MEM_W32,.mask=1,
         .base_is_uniform=true,.base=4,.idx_kind=AGX3_MEM_IDX_REG,
         .idx_reg=0,.idx_kill=true,.shift=0,.slot=6,.wait_mask=32,
         .last_load=true};
      uint8_t out[AGX3_MEM_MAX_LENGTH],len=0;
      if(!agx3_mem_encode_load(AGX_ISA_ENCODING_AGX3B,&d,out,&len)||
         len!=sizeof(expected)||memcmp(out,expected,len)){
         fprintf(stderr,"FAIL G17P byte-indexed u32 load\n");fail++;
      }else pass++;
   }

   /* Apple Metal 32023.917, applegpu_g17p, exact `device uchar *` scalar
    * load from buffer(1). The byte result occupies the low half of an
    * aligned GPR; the sole/final load carries f79=2. */
   {
      static const uint8_t expected[] = {
         0x67, 0x10, 0x44, 0x02, 0x01, 0x00, 0x20,
         0x00, 0x61, 0x01, 0x00, 0x40, 0x42, 0x00,
      };
      struct agx3_mem_desc d = {
         .data = 2,
         .width = AGX3_MEM_W8,
         .mask = 1,
         .shift = 0,
         .base_is_uniform = true,
         .base = 4,
         .idx_kind = AGX3_MEM_IDX_REG,
         .idx_reg = 0,
         .slot = 6,
         .wait_mask = 1,
         .last_load = true,
      };
      uint8_t out[AGX3_MEM_MAX_LENGTH], len = 0;
      if (!agx3_mem_encode_load(AGX_ISA_ENCODING_AGX3B, &d, out, &len) ||
          len != sizeof(expected) || memcmp(out, expected, len)) {
         fprintf(stderr, "FAIL G17P scalar-byte LDDEV\n");
         fail++;
      } else {
         pass++;
      }
   }

   /* Apple Metal 32023.917, applegpu_g17p. A relaxed atomic store/load uses
    * the ordinary STDEV/LDDEV family with one coherency field set: f75 for a
    * store and f77 for a load. The plain uint variants differ in exactly the
    * corresponding bit. */
   {
      static const uint8_t expected_store[] = {
         0xe7, 0x00, 0x54, 0x0c, 0x05, 0x02, 0x20,
         0x00, 0x11, 0x00, 0x00, 0xa0, 0x11, 0x00,
      };
      static const uint8_t expected_load[] = {
         0x67, 0x00, 0x44, 0x0a, 0x05, 0x85, 0x20,
         0x00, 0x51, 0x01, 0x00, 0x80, 0x46, 0x00,
      };
      struct agx3_mem_desc store = {
         .data = 12,
         .width = AGX3_MEM_W32,
         .mask = 1,
         .shift = 2,
         .base_is_uniform = true,
         .base = 20,
         .idx_kind = AGX3_MEM_IDX_REG,
         .idx_reg = 4,
         .coherent = true,
      };
      struct agx3_mem_desc load = {
         .data = 10,
         .width = AGX3_MEM_W32,
         .mask = 1,
         .shift = 2,
         .base_is_uniform = true,
         .base = 20,
         .idx_kind = AGX3_MEM_IDX_REG,
         .idx_reg = 10,
         .idx_kill = true,
         .slot = 6,
         .coherent = true,
         .agx3b_last_load = true,
      };
      uint8_t out[AGX3_MEM_MAX_LENGTH], len = 0;
      if (!agx3_mem_encode_store(AGX_ISA_ENCODING_AGX3B, &store, out, &len) ||
          len != sizeof(expected_store) || memcmp(out, expected_store, len)) {
         fprintf(stderr, "FAIL G17P coherent STDEV\n");
         fail++;
      } else {
         pass++;
      }
      if (!agx3_mem_encode_load(AGX_ISA_ENCODING_AGX3B, &load, out, &len) ||
          len != sizeof(expected_load) || memcmp(out, expected_load, len)) {
         fprintf(stderr, "FAIL G17P coherent LDDEV\n");
         fail++;
      } else {
         pass++;
      }
   }

   if (fail) {
      fprintf(stderr, "agx3_mem_core: %u of %zu failed\n", fail, mem_vec_count);
      return 1;
   }
   printf("agx3_mem_core: all %u corpus memory ops reproduced\n", pass);
   return 0;
}
