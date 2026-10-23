/*
 * Copyright 2018 Alyssa Rosenzweig
 * Copyright 2019 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include "pool.h"
#include "agx_bo.h"
#include "agx_device.h"

/* Transient command stream pooling: command stream uploads try to simply copy
 * into wherever we left off. If there isn't space, we allocate a new entry
 * into the pool and copy there */

#define POOL_SLAB_SIZE (256 * 1024)

static struct agx_bo *
agx_pool_alloc_backing(struct agx_pool *pool, size_t bo_sz)
{
   struct agx_bo *bo =
      agx_bo_create(pool->dev, bo_sz, 0, pool->create_flags, pool->label);

   if (unlikely(bo == NULL))
      return NULL;

   util_dynarray_append(&pool->bos, bo);
   pool->transient_bo = bo;
   pool->transient_offset = 0;

   return bo;
}

void
agx_pool_init(struct agx_pool *pool, struct agx_device *dev, const char *label,
              unsigned create_flags, bool prealloc)
{
   memset(pool, 0, sizeof(*pool));
   pool->dev = dev;
   pool->create_flags = create_flags;
   pool->label = label;
   pool->bos = UTIL_DYNARRAY_INIT;

   if (prealloc)
      agx_pool_alloc_backing(pool, POOL_SLAB_SIZE);
}

void
agx_pool_cleanup(struct agx_pool *pool)
{
   util_dynarray_foreach(&pool->bos, struct agx_bo *, bo) {
      agx_bo_unreference(pool->dev, *bo);
   }

   util_dynarray_fini(&pool->bos);
}

struct agx_ptr
agx_pool_alloc_aligned_with_bo(struct agx_pool *pool, size_t sz,
                               unsigned alignment, struct agx_bo **out_bo)
{
   assert(alignment == util_next_power_of_two(alignment));

   /* Find or create a suitable BO */
   struct agx_bo *bo = pool->transient_bo;
   unsigned offset = ALIGN_POT(pool->transient_offset, alignment);

   /* If we don't fit, allocate a new backing */
   if (unlikely(bo == NULL || (offset + sz) >= POOL_SLAB_SIZE)) {
      struct agx_bo *fresh = agx_pool_alloc_backing(
         pool, ALIGN_POT(MAX2(POOL_SLAB_SIZE, sz), 16384));

      if (unlikely(fresh == NULL)) {
         /* BO creation fails once the kernel has failed this VM (a GPU
          * recovery blamed our work: the context already reports a reset) or
          * on real memory exhaustion. Recycle a backing that fits rather
          * than dereferencing NULL: what is written here is never executed
          * on a failed VM, and the application sees the reset through
          * GL_ARB_robustness and rebuilds its context.
          */
         util_dynarray_foreach(&pool->bos, struct agx_bo *, it) {
            if ((*it)->size >= sz) {
               fresh = *it;
               break;
            }
         }

         if (fresh == NULL) {
            fprintf(stderr, "agx: pool %s cannot allocate %zu bytes and has "
                            "no backing to recycle\n",
                    pool->label ? pool->label : "(unnamed)", sz);
            abort();
         }

         pool->transient_bo = fresh;
      }

      bo = fresh;
      offset = 0;
   }

   pool->transient_offset = offset + sz;

   struct agx_ptr ret = {
      .cpu = (uint8_t *)agx_bo_map(bo) + offset,
      .gpu = bo->va->addr + offset,
   };

   if (out_bo)
      *out_bo = bo;

   return ret;
}

uint64_t
agx_pool_upload(struct agx_pool *pool, const void *data, size_t sz)
{
   return agx_pool_upload_aligned(pool, data, sz, util_next_power_of_two(sz));
}

uint64_t
agx_pool_upload_aligned_with_bo(struct agx_pool *pool, const void *data,
                                size_t sz, unsigned alignment,
                                struct agx_bo **bo)
{
   struct agx_bo *local_bo = NULL;
   struct agx_bo **out_bo = bo != NULL ? bo : &local_bo;
   struct agx_ptr transfer =
      agx_pool_alloc_aligned_with_bo(pool, sz, alignment, out_bo);
   memcpy(transfer.cpu, data, sz);
   agx_bo_assert_range(*out_bo,
                              transfer.gpu - (*out_bo)->va->addr, sz);
   return transfer.gpu;
}
