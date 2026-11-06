/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Exact AGX3 device/threadgroup atomic encoders.
 *
 * The 32-bit operation selector and every operand below were isolated in
 * Apple's G15G/G16G/G17S compiler output.  AGX3b device atomics are in-place;
 * AGX3c device atomics split value and return registers.  Threadgroup atomics
 * split them on both encodings.  G17P additionally grounds no-result
 * threadgroup atomics with immediate and register byte addresses.  The float
 * add selector was established on A18 Pro hardware for device memory.  The
 * one witnessed 64-bit form (Apple's G17P atomic_max on atomic_ulong, the
 * f90=3 min/max family) returns a changed flag rather than the old value on
 * hardware and is not encoded here, so the interface has no width selector.
 * G17G/G18G ground returned and no-result threadgroup atomics with immediate
 * and register byte addresses; G18G (agx3d) additionally kills the register
 * address of a returning form (agx3_atomic_encode_local_g18g).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "asahi/lib/agx_chip.h"

#ifdef __cplusplus
extern "C" {
#endif
#define AGX3_ATOMIC_MAX_LENGTH 18u

enum agx3_atomic_op {
   AGX3_ATOMIC_ADD = 0,
   AGX3_ATOMIC_SUB = 1,
   AGX3_ATOMIC_XCHG = 2,
   AGX3_ATOMIC_CMPXCHG = 3,
   AGX3_ATOMIC_UMIN = 4,
   AGX3_ATOMIC_IMIN = 5,
   AGX3_ATOMIC_UMAX = 6,
   AGX3_ATOMIC_IMAX = 7,
   AGX3_ATOMIC_AND = 8,
   AGX3_ATOMIC_OR = 9,
   AGX3_ATOMIC_XOR = 10,
   AGX3_ATOMIC_FADD = 11,
};

enum agx3_atomic_base_kind {
   AGX3_ATOMIC_BASE_REGISTER = 2,
   AGX3_ATOMIC_BASE_UNIFORM = 3,
};

enum agx3_atomic_index_kind {
   AGX3_ATOMIC_INDEX_IMMEDIATE = 1,
   AGX3_ATOMIC_INDEX_REGISTER = 2,
   AGX3_ATOMIC_INDEX_UNIFORM = 3,
};

struct agx3_device_atomic_desc {
   enum agx3_atomic_op op;
   uint16_t value; /* GPR half index */
   bool value_kill;
   bool exec_masked; /* instruction is inside partial/elected execution */

   bool returns_value;
   uint16_t result; /* GPR half index; equals value on AGX3b */
   uint8_t slot;    /* 1..6 (AGX3b), 1..8 (AGX3c) when returning */
   /* AGX3b: thread-private storage half the old value is published to, for a
    * following reload into the result register. Even. */
   uint32_t result_tps_half;

   enum agx3_atomic_base_kind base_kind;
   uint16_t base; /* uniform half (4-aligned) or GPR half (2-aligned) */

   enum agx3_atomic_index_kind index_kind;
   uint16_t index; /* immediate element index, GPR half, or uniform half */
   bool index_kill;
   uint8_t index_shift; /* address contribution = index << shift */
   int32_t byte_offset; /* signed 16-bit byte displacement */

   uint16_t wait_mask; /* bit i waits scoreboard slot i+1 */
};

struct agx3_local_atomic_desc {
   enum agx3_atomic_op op;
   uint16_t value;  /* GPR half index */
   bool value_kill;

   bool returns_value;
   uint16_t result; /* GPR half index */
   uint8_t slot;       /* returned-value producer: 1..6 / 1..8 */

   /* A register is a 16-bit GPR containing the byte address.  An immediate
    * is a byte displacement from the start of threadgroup memory. */
   enum agx3_atomic_index_kind address_kind;
   uint16_t address;
   bool address_kill;

   uint16_t wait_mask; /* bit i waits scoreboard slot i+1 */
};

bool agx3_atomic_encode_device(enum agx_isa_encoding enc,
                               const struct agx3_device_atomic_desc *d,
                               uint8_t out[AGX3_ATOMIC_MAX_LENGTH],
                               uint8_t *len);

bool agx3_atomic_encode_local(enum agx_isa_encoding enc,
                              const struct agx3_local_atomic_desc *d,
                              uint8_t out[AGX3_ATOMIC_MAX_LENGTH],
                              uint8_t *len);

/* G18G (M6) compiles as agx3c: as agx3_atomic_encode_local, plus the returning
 * register-address form with address_kill. */
bool agx3_atomic_encode_local_g18g(enum agx_isa_encoding enc,
                                   const struct agx3_local_atomic_desc *d,
                                   uint8_t out[AGX3_ATOMIC_MAX_LENGTH],
                                   uint8_t *len);

#ifdef __cplusplus
}
#endif
