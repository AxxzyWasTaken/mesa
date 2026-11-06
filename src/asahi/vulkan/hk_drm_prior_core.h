/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "drm-uapi/asahi_drm.h"

/* Prefixes belong to an occurrence in the ioctl payload, not the recorded
 * command buffer: simultaneous-use command buffers can appear more than once.
 * On exact-prior kernels the initial queue ordinals replace legacy header0.
 * Validate before mutating either output. */
static inline bool
hk_drm_serialize_prefix(struct drm_asahi_cmd_header *h, uint64_t prior[2],
                         uint32_t vdm_before, uint32_t cdm_before,
                         const uint64_t initial[2], bool exact)
{
   if (vdm_before >= DRM_ASAHI_BARRIER_NONE ||
       cdm_before >= DRM_ASAHI_BARRIER_NONE)
      return false;

   const uint32_t before[2] = {vdm_before, cdm_before};
   uint16_t barriers[2];
   for (unsigned sq = 0; sq < 2; ++sq) {
      bool historical = exact && before[sq] == 0;
      barriers[sq] = historical ? DRM_ASAHI_BARRIER_NONE : before[sq];
      prior[sq] = historical ? initial[sq] : 0;
   }
   h->vdm_barrier = barriers[0];
   h->cdm_barrier = barriers[1];
   return true;
}

/* Rebase one command when an API submission is split into ioctl slices.
 * Preserve an intentional header0 (all earlier ioctls), but an exact source
 * N in an earlier slice becomes initial_ordinal+N, not the broader header0.
 * The caller validates packet framing before passing its payload here. */
static inline bool
hk_drm_rebase_prior(struct drm_asahi_cmd_header *h, void *payload,
                    uint32_t vdm_before, uint32_t cdm_before,
                    const uint64_t initial[2], bool exact)
{
   uint16_t barriers[2] = {h->vdm_barrier, h->cdm_barrier};
   const uint32_t before[2] = {vdm_before, cdm_before};
   uint64_t prior[2] = {0, 0};
   struct drm_asahi_cmd_render_with_prior *r = payload;
   struct drm_asahi_cmd_compute_with_prior *c = payload;
   if (exact) {
      if (h->cmd_type == DRM_ASAHI_CMD_RENDER) {
         if (h->size < sizeof(*r))
            return false;
         prior[0] = r->prior_vdm;
         prior[1] = r->prior_cdm;
      } else if (h->cmd_type == DRM_ASAHI_CMD_COMPUTE) {
         if (h->size < sizeof(*c))
            return false;
         prior[0] = c->prior_vdm;
         prior[1] = c->prior_cdm;
      } else {
         return false;
      }
   }
   for (unsigned sq = 0; sq < 2; ++sq) {
      const uint16_t value = barriers[sq];
      if (value == DRM_ASAHI_BARRIER_NONE || value == 0)
         continue;
      if (value > before[sq]) {
         barriers[sq] = value - before[sq];
      } else if (exact) {
         if (initial[sq] > UINT64_MAX - value)
            return false;
         const uint64_t absolute = initial[sq] + value;
         if (absolute > prior[sq])
            prior[sq] = absolute;
         barriers[sq] = DRM_ASAHI_BARRIER_NONE;
      } else {
         barriers[sq] = 0;
      }
   }
   h->vdm_barrier = barriers[0];
   h->cdm_barrier = barriers[1];
   if (exact && h->cmd_type == DRM_ASAHI_CMD_RENDER &&
       (r->legacy.flags & DRM_ASAHI_RENDER_STAGE_PREFIXES)) {
      /* Both tiling prefixes are rebased like the header's. */
      struct drm_asahi_cmd_render_with_stage_prefixes *s = payload;
      if (h->size != sizeof(*s))
         return false;
      __u16 *ta[2] = {&s->ta_vdm_barrier, &s->ta_cdm_barrier};
      __u64 *ta_prior[2] = {&s->ta_prior_vdm, &s->ta_prior_cdm};
      for (unsigned sq = 0; sq < 2; ++sq) {
         const uint16_t value = *ta[sq];
         if (value == DRM_ASAHI_BARRIER_NONE || value == 0)
            continue;
         if (value > before[sq]) {
            *ta[sq] = value - before[sq];
         } else {
            if (initial[sq] > UINT64_MAX - value)
               return false;
            const uint64_t absolute = initial[sq] + value;
            if (absolute > *ta_prior[sq])
               *ta_prior[sq] = absolute;
            *ta[sq] = DRM_ASAHI_BARRIER_NONE;
         }
      }
   } else if (exact && h->cmd_type == DRM_ASAHI_CMD_RENDER &&
              (r->legacy.flags & DRM_ASAHI_RENDER_COMPUTE_STAGE_BARRIERS)) {
      /* The tiling half's compute prefix is rebased like the header's. */
      struct drm_asahi_cmd_render_with_stage_prior *s = payload;
      if (h->size != sizeof(*s))
         return false;
      const uint16_t value = s->ta_cdm_barrier;
      if (value != DRM_ASAHI_BARRIER_NONE && value != 0) {
         if (value > before[1]) {
            s->ta_cdm_barrier = value - before[1];
         } else {
            if (initial[1] > UINT64_MAX - value)
               return false;
            const uint64_t absolute = initial[1] + value;
            if (absolute > s->ta_prior_cdm)
               s->ta_prior_cdm = absolute;
            s->ta_cdm_barrier = DRM_ASAHI_BARRIER_NONE;
         }
      }
   }
   if (exact && h->cmd_type == DRM_ASAHI_CMD_RENDER) {
      r->prior_vdm = prior[0];
      r->prior_cdm = prior[1];
   } else if (exact) {
      c->prior_vdm = prior[0];
      c->prior_cdm = prior[1];
   }
   return true;
}
