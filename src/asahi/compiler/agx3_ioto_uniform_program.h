/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Exact bounded AGX3 Ioto path for dword-aligned absolute uniform ranges.
 */

#pragma once

#include "asahi/lib/agx_ioto_program_core.h"
#include "agx3_ioto_load_packer.h"
#include "agx3_cprog_input.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* This is the exact subset of agx_usc_uniform whose destination and size are
 * dword aligned.  start_halfs and size_halfs retain Mesa's native 16-bit
 * uniform-file units. */
struct agx3_ioto_uniform_range {
   uint16_t start_halfs;
   uint16_t size_halfs;
   uint64_t address;
};

struct agx3_ioto_uniform_program_desc {
   uint64_t program_va;
   const struct agx3_ioto_uniform_range *ranges;
   size_t range_count;
   /* Optional known preamble inputs. G17P default allocator only; every
    * selected half must be uploaded exactly once by ranges. Ordinary uniform
    * uploads remain complete. This does not enable preamble execution. */
   const struct agx_ioto_cprog_inputs *preshader_inputs;

   uint8_t initial_gpr_dwords;
   bool fast_path_enabled;
   const struct agx3_cprog_inputs *cprog_inputs;

   /* Make the bytes independent of the range addresses: each range
    * materializes its own base with fixed-size LDIMMs, so rewriting the
    * recorded relocations retargets the program to any addresses. */
   bool patchable;

   /* Optional caller-owned 64-bit base, materialized before entering this
    * body.  Ranges at/above preloaded_base_address reuse it without emitting
    * LDIMM and the base survives every complete loader round. */
   bool preloaded_base_valid;
   uint16_t preloaded_base_half;
   uint64_t preloaded_base_address;

   struct agx_ioto_state_tail tail;
   uint16_t profile_payload0;
   uint16_t profile_payload1;
   struct agx_ioto_preshader_desc preshader;
};

/* All storage is caller owned.  The workspace spans must be mutually disjoint
 * and must not overlap output. They are scratch and may be changed on failure.
 * Output and *written are transactional. */
struct agx3_ioto_uniform_program_workspace {
   uint8_t *body;
   size_t body_capacity;

   struct agx_ioto_inflight_load *loads;
   size_t load_capacity;
   struct agx_ioto_inflight_load *sort;
   size_t sort_capacity;
   struct agx_ioto_load_cache_entry *cache;
   size_t cache_capacity;
   struct agx_ioto_load_relocation *relocations;
   size_t relocation_capacity;
   size_t *relocation_count;
};

/* Conservative body-scratch bound for the validated ranges.  The returned
 * value includes every address materialization, load, rollover pad and copy,
 * but excludes profile, state tail and terminal suffix. */
bool agx3_ioto_uniform_program_body_bound(
   enum agx_chip chip, const struct agx3_ioto_uniform_range *ranges,
   size_t range_count, size_t *bytes, size_t *chunks);

/* Includes native input publication and validates that each selected half
 * is supplied exactly once by these ranges. */
bool agx3_ioto_uniform_program_desc_body_bound(
   enum agx_chip chip, const struct agx3_ioto_uniform_program_desc *desc,
   size_t *bytes, size_t *chunks);

/* Also bounds the selected packed TPS input copies and validates their
 * coverage. NULL inputs preserves the ordinary body bound. */
bool agx3_ioto_uniform_program_body_bound_with_inputs(
   enum agx_chip chip, const struct agx3_ioto_uniform_range *ranges,
   size_t range_count, const struct agx_ioto_cprog_inputs *inputs,
   size_t *bytes, size_t *chunks);

/* Build only the scheduled resource-load body. copy_offset marks the first
 * trailing COPY record emitted by finishRound(), so a target-specific program
 * container can place its shader state and profile-loader prefix before those
 * copies. The workspace bytes may change on failure; the two outputs do not. */
bool agx3_ioto_uniform_program_body_generate(
   enum agx_chip chip, const struct agx3_ioto_uniform_program_desc *desc,
   struct agx3_ioto_uniform_program_workspace workspace, size_t *written,
   size_t *copy_offset);

bool agx3_ioto_uniform_program_generate(
   enum agx_chip chip, const struct agx3_ioto_uniform_program_desc *desc,
   struct agx3_ioto_uniform_program_workspace workspace, void *output,
   size_t capacity, size_t *written);

#ifdef __cplusplus
}
#endif
