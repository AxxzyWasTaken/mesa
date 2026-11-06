/* SPDX-License-Identifier: MIT
 * Exercise the production range allocator with failing Vulkan/host allocation.
 */
#include "zink_context.h"
#include "zink_screen.h"
#include "util/u_memory.h"

static unsigned alloc_count, fail_alloc;
static void *
query_test_calloc(size_t size)
{
   if (++alloc_count == fail_alloc)
      return NULL;
   return calloc(1, size);
}
#undef CALLOC_STRUCT
#define CALLOC_STRUCT(T) ((struct T *)query_test_calloc(sizeof(struct T)))
#include "zink_query.c"

static unsigned pool_creates, fail_pool;
static VKAPI_ATTR VkResult VKAPI_CALL
create_pool(VkDevice dev, const VkQueryPoolCreateInfo *info,
            const VkAllocationCallbacks *allocator, VkQueryPool *pool)
{
   if (++pool_creates == fail_pool)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   *pool = (VkQueryPool)(uintptr_t)pool_creates;
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
destroy_pool(VkDevice dev, VkQueryPool pool, const VkAllocationCallbacks *allocator)
{
}

static struct zink_screen screen;
static struct zink_context ctx;
static struct zink_batch_state bs;
static struct zink_query q;

static void
setup(enum pipe_query_type type, VkQueryType vk_type)
{
   memset(&screen, 0, sizeof(screen));
   memset(&ctx, 0, sizeof(ctx));
   memset(&bs, 0, sizeof(bs));
   memset(&q, 0, sizeof(q));
   alloc_count = fail_alloc = pool_creates = fail_pool = 0;
   NUM_QUERIES = 16;
   screen.vk.CreateQueryPool = create_pool;
   screen.vk.DestroyQueryPool = destroy_pool;
   ctx.base.screen = &screen.base;
   ctx.bs = &bs;
   list_inithead(&ctx.query_pools);
   q.type = type;
   q.vkqtype = vk_type;
}

static void
teardown(void)
{
   struct zink_query_start *starts = q.starts.data;
   for (unsigned j = 0; j < q.starts.capacity / sizeof(*starts); j++)
      for (unsigned i = 0; i < PIPE_MAX_VERTEX_STREAMS; i++)
         unref_vk_query(&ctx, starts[j].vkq[i]);
   util_dynarray_fini(&q.starts);
   util_dynarray_fini(&bs.dead_querypools);
   zink_context_destroy_query_pools(&ctx);
}

int
main(void)
{
   /* Failed initial vkCreateQueryPool: no range is published. */
   setup(PIPE_QUERY_OCCLUSION_COUNTER, VK_QUERY_TYPE_OCCLUSION);
   fail_pool = 1;
   assert(!query_pool_get_range(&ctx, &q));
   assert(q.failed && !q.active && q.starts.size == 0);
   teardown();

   /* Failed host pool/vkq allocation. */
   for (unsigned i = 1; i <= 2; i++) {
      setup(PIPE_QUERY_OCCLUSION_COUNTER, VK_QUERY_TYPE_OCCLUSION);
      fail_alloc = i;
      assert(!query_pool_get_range(&ctx, &q));
      assert(q.failed && q.starts.size == 0);
      teardown();
   }

   /* Rollover preserves all earlier, complete starts. */
   setup(PIPE_QUERY_OCCLUSION_COUNTER, VK_QUERY_TYPE_OCCLUSION);
   const unsigned first_pool_size = NUM_QUERIES;
   for (unsigned i = 0; i < first_pool_size; i++)
      assert(query_pool_get_range(&ctx, &q));
   fail_pool = 2;
   assert(!query_pool_get_range(&ctx, &q));
   assert(q.failed && get_num_starts(&q) == first_pool_size);
   struct zink_query_start *first = q.starts.data;
   assert(first->vkq[0]->pool->refcount == first_pool_size);
   teardown();

   /* Multi-pool failure releases the partial allocation. */
   setup(PIPE_QUERY_PRIMITIVES_GENERATED, VK_QUERY_TYPE_PIPELINE_STATISTICS);
   fail_pool = 2;
   assert(!query_pool_get_range(&ctx, &q));
   assert(q.failed && q.starts.size == 0 && list_is_empty(&ctx.query_pools));
   teardown();

   /* Shared XFB references are balanced when a later stream fails. */
   setup(PIPE_QUERY_SO_OVERFLOW_ANY_PREDICATE, VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT);
   q.type = PIPE_QUERY_PRIMITIVES_EMITTED;
   assert(query_pool_get_range(&ctx, &q));
   first = q.starts.data;
   struct zink_vk_query *shared = first->vkq[0];
   ctx.curr_xfb_queries[0] = shared;
   q.type = PIPE_QUERY_SO_OVERFLOW_ANY_PREDICATE;
   fail_alloc = alloc_count + 1;
   assert(!query_pool_get_range(&ctx, &q));
   assert(shared->refcount == 1 && shared->pool->refcount == 1);
   assert(get_num_starts(&q) == 1);
   teardown();

   /* Timestamp replacement failure leaves the previous start intact. */
   setup(PIPE_QUERY_TIMESTAMP, VK_QUERY_TYPE_TIMESTAMP);
   assert(query_pool_get_range(&ctx, &q));
   first = q.starts.data;
   struct zink_vk_query *previous = first->vkq[0];
   previous->pool->last_range = previous->pool->num_results;
   fail_pool = 2;
   assert(!query_pool_get_range(&ctx, &q));
   assert(get_num_starts(&q) == 1 && first->vkq[0] == previous);
   teardown();

   /* One QBO still needs aggregation when a flush splits an active query. */
   setup(PIPE_QUERY_OCCLUSION_COUNTER, VK_QUERY_TYPE_OCCLUSION);
   q.buffer_count = 1;
   struct zink_query_start split = {0};
   util_dynarray_append(&q.starts, split);
   assert(can_direct_poll_query_result(&q));
   util_dynarray_append(&q.starts, split);
   assert(!can_direct_poll_query_result(&q));
   /* Teardown also visits spare slots, as the production allocator permits. */
   memset(q.starts.data, 0, q.starts.capacity);
   uint64_t values[] = {2048, 2048};
   union pipe_query_result result;
   util_query_clear_result(&result, q.type);
   check_query_results(&q, &result, 2, values, NULL);
   assert(result.u64 == 4096);
   teardown();

   puts("zink query allocation and aggregation: PASS");
   return 0;
}
