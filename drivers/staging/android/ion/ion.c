/*

 * drivers/staging/android/ion/ion.c
 *
 * Copyright (C) 2011 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/freezer.h>
#include <linux/fs.h>
#include <linux/anon_inodes.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/memblock.h>
#include <linux/miscdevice.h>
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/rbtree.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/debugfs.h>
#include <linux/dma-buf.h>
#include <linux/idr.h>

#include "ion.h"
#include "ion_priv.h"
#include "compat_ion.h"
#include "ion_profile.h"
#include "mtk/mtk_ion.h"
#include "mtk/ion_drv_priv.h"

#define DEBUG_HEAP_SHRINKER

bool ion_buffer_fault_user_mappings(struct ion_buffer *buffer)
{
	return (buffer->flags & ION_FLAG_CACHED) &&
		!(buffer->flags & ION_FLAG_CACHED_NEEDS_SYNC);
}

bool ion_buffer_cached(struct ion_buffer *buffer)
{
	return !!(buffer->flags & ION_FLAG_CACHED);
}

static inline struct page *ion_buffer_page(struct page *page)
{
	return (struct page *)((unsigned long)page & ~(1UL));
}

static inline bool ion_buffer_page_is_dirty(struct page *page)
{
	return !!((unsigned long)page & 1UL);
}

static inline void ion_buffer_page_dirty(struct page **page)
{
	*page = (struct page *)((unsigned long)(*page) | 1UL);
}

static inline void ion_buffer_page_clean(struct page **page)
{
	*page = (struct page *)((unsigned long)(*page) & ~(1UL));
}

/* this function should only be called while dev->lock is held */
static void ion_buffer_add(struct ion_device *dev,
			   struct ion_buffer *buffer)
{
	struct rb_node **p = &dev->buffers.rb_node;
	struct rb_node *parent = NULL;
	struct ion_buffer *entry;

	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct ion_buffer, node);

		if (buffer < entry) {
			p = &(*p)->rb_left;
		} else if (buffer > entry) {
			p = &(*p)->rb_right;
		} else {
			pr_err("%s: buffer already found.", __func__);
			BUG();
		}
	}

	rb_link_node(&buffer->node, parent, p);
	rb_insert_color(&buffer->node, &dev->buffers);
}

/* this function should only be called while dev->lock is held */
static struct ion_buffer *ion_buffer_create(struct ion_heap *heap,
				     struct ion_device *dev,
				     unsigned long len,
				     unsigned long align,
				     unsigned long flags)
{
	struct ion_buffer *buffer;
	struct sg_table *table;
	struct scatterlist *sg;
	int i, ret;

	buffer = kzalloc(sizeof(struct ion_buffer), GFP_KERNEL);
	if (!buffer) {
		IONMSG("%s kzalloc failed, buffer is null.\n", __func__);
		return ERR_PTR(-ENOMEM);
	}

	buffer->heap = heap;
	buffer->flags = flags;

	/* log task pid for debug +by k.zhang */
	{
		struct task_struct *task;

		task = current->group_leader;
		get_task_comm(buffer->task_comm, task);
		buffer->pid = task_pid_nr(task);
	}

	kref_init(&buffer->ref);

	ret = heap->ops->allocate(heap, buffer, len, align, flags);

	if (ret) {
		if (!(heap->flags & ION_HEAP_FLAG_DEFER_FREE))
			goto err2;

		ion_heap_freelist_drain(heap, 0);
		ret = heap->ops->allocate(heap, buffer, len, align,
					  flags);
		if (ret)
			goto err2;
	}

	buffer->dev = dev;
	buffer->size = len;

	table = heap->ops->map_dma(heap, buffer);
	if (WARN_ONCE(table == NULL,
			"heap->ops->map_dma should return ERR_PTR on error"))
		table = ERR_PTR(-EINVAL);
	if (IS_ERR(table)) {
		ret = -EINVAL;
		goto err1;
	}

	buffer->sg_table = table;
	if (ion_buffer_fault_user_mappings(buffer)) {
		int num_pages = PAGE_ALIGN(buffer->size) / PAGE_SIZE;
		struct scatterlist *sg;
		int i, j, k = 0;

		buffer->pages = vmalloc(sizeof(struct page *) * num_pages);
		if (!buffer->pages) {
			IONMSG("%s vamlloc failed pages is null.\n", __func__);
			ret = -ENOMEM;
			goto err;
		}

		for_each_sg(table->sgl, sg, table->nents, i) {
			struct page *page = sg_page(sg);

			for (j = 0; j < sg->length / PAGE_SIZE; j++)
				buffer->pages[k++] = page++;
		}
	}

	buffer->dev = dev;
	buffer->size = len;
	INIT_LIST_HEAD(&buffer->vmas);


	mutex_init(&buffer->lock);
	/* this will set up dma addresses for the sglist -- it is not
	   technically correct as per the dma api -- a specific
	   device isn't really taking ownership here.  However, in practice on
	   our systems the only dma_address space is physical addresses.
	   Additionally, we can't afford the overhead of invalidating every
	   allocation via dma_map_sg. The implicit contract here is that
	   memory coming from the heaps is ready for dma, ie if it has a
	   cached mapping that mapping has been invalidated */
	for_each_sg(buffer->sg_table->sgl, sg, buffer->sg_table->nents, i) {
		if ((heap->id == ION_HEAP_TYPE_MULTIMEDIA_MAP_MVA) && (align < PAGE_OFFSET)) {
			if (align < VMALLOC_START || align > VMALLOC_END) {
				/*userspace va without vmalloc, has no page struct*/
				sg->length = sg_dma_len(sg);
				continue;
			}
		}
		sg_dma_address(sg) = sg_phys(sg);
		#ifdef CONFIG_NEED_SG_DMA_LENGTH
		sg->dma_length = sg->length;
		#endif
	}
	mutex_lock(&dev->buffer_lock);
	ion_buffer_add(dev, buffer);
	mutex_unlock(&dev->buffer_lock);
	return buffer;

err:
	heap->ops->unmap_dma(heap, buffer);
err1:
	heap->ops->free(buffer);
err2:
	kfree(buffer);
	return ERR_PTR(ret);
}

void ion_buffer_destroy(struct ion_buffer *buffer)
{
	if (WARN_ON(buffer->kmap_cnt > 0))
		buffer->heap->ops->unmap_kernel(buffer->heap, buffer);
	buffer->heap->ops->unmap_dma(buffer->heap, buffer);
	buffer->heap->ops->free(buffer);
	if (buffer->pages)
		vfree(buffer->pages);
	kfree(buffer);
}

static void _ion_buffer_destroy(struct kref *kref)
{
	struct ion_buffer *buffer = container_of(kref, struct ion_buffer, ref);
	struct ion_heap *heap = buffer->heap;
	struct ion_device *dev = buffer->dev;

	mutex_lock(&dev->buffer_lock);
	rb_erase(&buffer->node, &dev->buffers);
	mutex_unlock(&dev->buffer_lock);

	if (heap->flags & ION_HEAP_FLAG_DEFER_FREE)
		ion_heap_freelist_add(heap, buffer);
	else
		ion_buffer_destroy(buffer);
}

static void ion_buffer_get(struct ion_buffer *buffer)
{
	kref_get(&buffer->ref);
}

static int ion_buffer_put(struct ion_buffer *buffer)
{
	return kref_put(&buffer->ref, _ion_buffer_destroy);
}

static void ion_buffer_add_to_handle(struct ion_buffer *buffer)
{
	mutex_lock(&buffer->lock);
	buffer->handle_count++;
	mutex_unlock(&buffer->lock);
}

static void ion_buffer_remove_from_handle(struct ion_buffer *buffer)
{
	/*
	 * when a buffer is removed from a handle, if it is not in
	 * any other handles, copy the taskcomm and the pid of the
	 * process it's being removed from into the buffer.  At this
	 * point there will be no way to track what processes this buffer is
	 * being used by, it only exists as a dma_buf file descriptor.
	 * The taskcomm and pid can provide a debug hint as to where this fd
	 * is in the system
	 */
	mutex_lock(&buffer->lock);
	buffer->handle_count--;
	BUG_ON(buffer->handle_count < 0);
	if (!buffer->handle_count) {
		struct task_struct *task;

		task = current->group_leader;
		get_task_comm(buffer->task_comm, task);
		buffer->pid = task_pid_nr(task);
	}
	mutex_unlock(&buffer->lock);
}

static struct ion_handle *ion_handle_create(struct ion_client *client,
				     struct ion_buffer *buffer)
{
	struct ion_handle *handle;

	handle = kzalloc(sizeof(struct ion_handle), GFP_KERNEL);
	if (!handle) {
		IONMSG("%s kzalloc failed handle is null.\n", __func__);
		return ERR_PTR(-ENOMEM);
	}
	kref_init(&handle->ref);
	RB_CLEAR_NODE(&handle->node);
	handle->client = client;
	ion_buffer_get(buffer);
	ion_buffer_add_to_handle(buffer);
	handle->buffer = buffer;

	return handle;
}

static void ion_handle_kmap_put(struct ion_handle *);

static void ion_handle_destroy(struct kref *kref)
{
	struct ion_handle *handle = container_of(kref, struct ion_handle, ref);
	struct ion_client *client = handle->client;
	struct ion_buffer *buffer = handle->buffer;

	mutex_lock(&buffer->lock);
	while (handle->kmap_cnt)
		ion_handle_kmap_put(handle);
	mutex_unlock(&buffer->lock);

	idr_remove(&client->idr, handle->id);
	if (!RB_EMPTY_NODE(&handle->node))
		rb_erase(&handle->node, &client->handles);

	ion_buffer_remove_from_handle(buffer);
	ion_buffer_put(buffer);

	handle->buffer = NULL;
	handle->client = NULL;

	kfree(handle);
}

struct ion_buffer *ion_handle_buffer(struct ion_handle *handle)
{
	return handle->buffer;
}

static void ion_handle_get(struct ion_handle *handle)
{
	kref_get(&handle->ref);
}

/* Must hold the client lock */
static struct ion_handle *ion_handle_get_check_overflow(
					struct ion_handle *handle)
{
	if (atomic_read(&handle->ref.refcount) + 1 == 0)
		return ERR_PTR(-EOVERFLOW);
	ion_handle_get(handle);
	return handle;
}

static int ion_handle_put_nolock(struct ion_handle *handle)
{
	int ret;

	ret = kref_put(&handle->ref, ion_handle_destroy);

	return ret;
}

int ion_handle_put(struct ion_handle *handle)
{
	struct ion_client *client = handle->client;
	int ret;

	mutex_lock(&client->lock);
	ret = ion_handle_put_nolock(handle);
	mutex_unlock(&client->lock);

	return ret;
}

/* Must hold the client lock */
static void user_ion_handle_get(struct ion_handle *handle)
{
	if (handle->user_ref_count++ == 0)
		kref_get(&handle->ref);

}

/* Must hold the client lock */
static struct ion_handle *user_ion_handle_get_check_overflow(struct ion_handle *handle)
{
	if (handle->user_ref_count + 1 == 0)
		return ERR_PTR(-EOVERFLOW);
	user_ion_handle_get(handle);
	return handle;
}

/* passes a kref to the user ref count.
 * We know we're holding a kref to the object before and
 * after this call, so no need to reverify handle.
 */
static struct ion_handle *pass_to_user(struct ion_handle *handle)
{
	struct ion_client *client = handle->client;
	struct ion_handle *ret;

	mutex_lock(&client->lock);
	ret = user_ion_handle_get_check_overflow(handle);
	ion_handle_put_nolock(handle);
	mutex_unlock(&client->lock);
	return ret;
}

/* Must hold the client lock */
static int user_ion_handle_put_nolock(struct ion_handle *handle)
{
	int ret = 0;

	if (--handle->user_ref_count == 0)
		ret = ion_handle_put_nolock(handle);

	return ret;
}

static struct ion_handle *ion_handle_lookup(struct ion_client *client,
					    struct ion_buffer *buffer)
{
	struct rb_node *n = client->handles.rb_node;

	while (n) {
		struct ion_handle *entry = rb_entry(n, struct ion_handle, node);

		if (buffer < entry->buffer)
			n = n->rb_left;
		else if (buffer > entry->buffer)
			n = n->rb_right;
		else
			return entry;
	}
	return ERR_PTR(-EINVAL);
}

static struct ion_handle *ion_handle_get_by_id_nolock(struct ion_client *client,
						int id)
{
	struct ion_handle *handle;

	handle = idr_find(&client->idr, id);
	if (handle)
		return ion_handle_get_check_overflow(handle);

	return ERR_PTR(-EINVAL);
}

struct ion_handle *ion_handle_get_by_id(struct ion_client *client,
						int id)
{
	struct ion_handle *handle;

	mutex_lock(&client->lock);
	handle = ion_handle_get_by_id_nolock(client, id);
	mutex_unlock(&client->lock);

	return handle;
}

static bool ion_handle_validate(struct ion_client *client,
				struct ion_handle *handle)
{
	WARN_ON(!mutex_is_locked(&client->lock));
	return idr_find(&client->idr, handle->id) == handle;
}

static int ion_handle_add(struct ion_client *client, struct ion_handle *handle)
{
	int id;
	struct rb_node **p = &client->handles.rb_node;
	struct rb_node *parent = NULL;
	struct ion_handle *entry;

	id = idr_alloc(&client->idr, handle, 1, 0, GFP_KERNEL);
	if (id < 0) {
		IONMSG("%s idr_alloc failed id = %d.\n", __func__, id);
		return id;
	}

	handle->id = id;

	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct ion_handle, node);

		if (handle->buffer < entry->buffer)
			p = &(*p)->rb_left;
		else if (handle->buffer > entry->buffer)
			p = &(*p)->rb_right;
		else
			WARN(1, "%s: buffer already found.", __func__);
	}

	rb_link_node(&handle->node, parent, p);
	rb_insert_color(&handle->node, &client->handles);

	return 0;
}

struct ion_handle *__ion_alloc(struct ion_client *client, size_t len,
			     size_t align, unsigned int heap_id_mask,
			     unsigned int flags, bool grab_handle)
{
	struct ion_handle *handle;
	struct ion_device *dev = client->dev;
	struct ion_buffer *buffer = NULL;
	struct ion_heap *heap;
	int ret;
	unsigned long long start, end;

	pr_debug("%s: len %zu align %zu heap_id_mask %u flags %x\n", __func__,
		 len, align, heap_id_mask, flags);
	/*
	 * traverse the list of heaps available in this system in priority
	 * order.  If the heap type is supported by the client, and matches the
	 * request of the caller allocate from it.  Repeat until allocate has
	 * succeeded or all heaps have been tried
	 */
	if (heap_id_mask != ION_HEAP_MAP_MVA_MASK)
		len = PAGE_ALIGN(len);

	if (!len) {
		IONMSG("%s len cannot be zero.\n", __func__);
		return ERR_PTR(-EINVAL);
	}

    /* add by k.zhang for sgtable_init KE bug */
	if ((len > 1024*1024*1024)) {
		IONMSG("%s error: size (%zu) is more than 1G !!\n", __func__, len);
		return ERR_PTR(-EINVAL);
	}
	MMProfileLogEx(ION_MMP_Events[PROFILE_ALLOC], MMProfileFlagStart, (unsigned long)client, len);
	start = sched_clock();

	down_read(&dev->lock);
	plist_for_each_entry(heap, &dev->heaps, node) {
		/* if the caller didn't specify this heap id */
		if (!((1 << heap->id) & heap_id_mask))
			continue;
		buffer = ion_buffer_create(heap, dev, len, align, flags);
		if (!IS_ERR(buffer))
			break;
	}
	up_read(&dev->lock);

	if (buffer == NULL) {
		IONMSG("%s buffer is null.\n", __func__);
		return ERR_PTR(-ENODEV);
	}

	if (IS_ERR(buffer)) {
		IONMSG("%s buffer is error 0x%p.\n", __func__, buffer);
		return ERR_CAST(buffer);
	}

	handle = ion_handle_create(client, buffer);

	/*
	 * ion_buffer_create will create a buffer with a ref_cnt of 1,
	 * and ion_handle_create will take a second reference, drop one here
	 */
	ion_buffer_put(buffer);

	if (IS_ERR(handle)) {
		IONMSG("%s handle is error 0x%p.\n", __func__, handle);
		return handle;
	}

	mutex_lock(&client->lock);
	if (grab_handle)
		ion_handle_get(handle);
	ret = ion_handle_add(client, handle);
	mutex_unlock(&client->lock);
	if (ret) {
		ion_handle_put(handle);
		handle = ERR_PTR(ret);
		IONMSG("%s ion handle add failed %d.\n", __func__, ret);
	}

	end = sched_clock();

	if (end-start > 100000000ULL) {/* unit is ns */
		IONMSG("warn: ion alloc buffer size: %zu time: %lld ns\n", buffer->size, end-start);
	}

	MMProfileLogEx(ION_MMP_Events[PROFILE_ALLOC], MMProfileFlagEnd, (unsigned long)client, (unsigned long)handle);

	ion_history_count_kick(true, len);

	handle->dbg.user_ts = end;
	do_div(handle->dbg.user_ts, 1000000);
	memcpy(buffer->alloc_dbg, client->dbg_name, ION_MM_DBG_NAME_LEN);

	return handle;
}

struct ion_handle *ion_alloc(struct ion_client *client, size_t len,
			     size_t align, unsigned int heap_id_mask,
			     unsigned int flags)
{
	return __ion_alloc(client, len, align, heap_id_mask, flags, false);
}
EXPORT_SYMBOL(ion_alloc);

static void ion_free_nolock(struct ion_client *client, struct ion_handle *handle)
{
	bool valid_handle;

	BUG_ON(client != handle->client);

	valid_handle = ion_handle_validate(client, handle);

	if (!valid_handle) {
		WARN(1, "%s: invalid handle passed to free.\n", __func__);
		return;
	}
	ion_handle_put_nolock(handle);
}

static void user_ion_free_nolock(struct ion_client *client, struct ion_handle *handle)
{
	bool valid_handle;

	BUG_ON(client != handle->client);
	valid_handle = ion_handle_validate(client, handle);
	if (!valid_handle) {
		WARN(1, "%s: invalid handle passed to free.\n", __func__);
		return;
	}
	if (!handle->user_ref_count) {
		WARN(1, "%s: User does not have access!\n", __func__);
		return;
	}
	user_ion_handle_put_nolock(handle);
}

void ion_free(struct ion_client *client, struct ion_handle *handle)
{
	BUG_ON(client != handle->client);

	mutex_lock(&client->lock);
	ion_free_nolock(client, handle);
	mutex_unlock(&client->lock);
}
EXPORT_SYMBOL(ion_free);

int ion_phys(struct ion_client *client, struct ion_handle *handle,
	     ion_phys_addr_t *addr, size_t *len)
{
	struct ion_buffer *buffer;
	int ret;
	MMProfileLogEx(ION_MMP_Events[PROFILE_GET_PHYS], MMProfileFlagStart,
		(unsigned long)client, (unsigned long)handle);

	mutex_lock(&client->lock);
	if (!ion_handle_validate(client, handle)) {
		mutex_unlock(&client->lock);
		IONMSG("%s invalid handle pass to phys.\n", __func__);
		return -EINVAL;
	}

	buffer = handle->buffer;

	if (!buffer->heap->ops->phys) {
		pr_err("%s: ion_phys is not implemented by this heap (name=%s, type=%d).\n",
			__func__, buffer->heap->name, buffer->heap->type);
		mutex_unlock(&client->lock);
		return -ENODEV;
	}
	mutex_unlock(&client->lock);
	ret = buffer->heap->ops->phys(buffer->heap, buffer, addr, len);

	MMProfileLogEx(ION_MMP_Events[PROFILE_GET_PHYS], MMProfileFlagEnd, buffer->size, *addr);

	return ret;
}
EXPORT_SYMBOL(ion_phys);

static void *ion_buffer_kmap_get(struct ion_buffer *buffer)
{
	void *vaddr;

	if (buffer->kmap_cnt) {
		buffer->kmap_cnt++;
		return buffer->vaddr;
	}
	vaddr = buffer->heap->ops->map_kernel(buffer->heap, buffer);
	if (WARN_ONCE(vaddr == NULL,
			"heap->ops->map_kernel should return ERR_PTR on error"))
		return ERR_PTR(-EINVAL);
	if (IS_ERR(vaddr)) {
		IONMSG("%s map kernel is failed addr = 0x%p.\n", __func__, vaddr);
		return vaddr;
	}
	buffer->vaddr = vaddr;
	buffer->kmap_cnt++;
	return vaddr;
}

static void *ion_handle_kmap_get(struct ion_handle *handle)
{
	struct ion_buffer *buffer = handle->buffer;
	void *vaddr;

	if (handle->kmap_cnt) {
		handle->kmap_cnt++;
		return buffer->vaddr;
	}
	vaddr = ion_buffer_kmap_get(buffer);
	if (IS_ERR(vaddr)) {
		IONMSG("%s vadd is error 0x%p.\n", __func__, vaddr);
		return vaddr;
	}
	handle->kmap_cnt++;
	return vaddr;
}

static void ion_buffer_kmap_put(struct ion_buffer *buffer)
{
	buffer->kmap_cnt--;
	if (!buffer->kmap_cnt) {
		MMProfileLogEx(ION_MMP_Events[PROFILE_UNMAP_KERNEL], MMProfileFlagStart, buffer->size, 0);
		buffer->heap->ops->unmap_kernel(buffer->heap, buffer);
		MMProfileLogEx(ION_MMP_Events[PROFILE_UNMAP_KERNEL], MMProfileFlagEnd, buffer->size, 0);
		buffer->vaddr = NULL;
	}
}

static void ion_handle_kmap_put(struct ion_handle *handle)
{
	struct ion_buffer *buffer = handle->buffer;

	if (!handle->kmap_cnt) {
		WARN(1, "%s: Double unmap detected! bailing...\n", __func__);
		return;
	}
	handle->kmap_cnt--;
	if (!handle->kmap_cnt)
		ion_buffer_kmap_put(buffer);
}

void *ion_map_kernel(struct ion_client *client, struct ion_handle *handle)
{
	struct ion_buffer *buffer;
	void *vaddr;

	mutex_lock(&client->lock);
	if (!ion_handle_validate(client, handle)) {
		pr_err("%s: invalid handle passed to map_kernel.\n",
		       __func__);
		mutex_unlock(&client->lock);
		return ERR_PTR(-EINVAL);
	}

	buffer = handle->buffer;

	if (!handle->buffer->heap->ops->map_kernel) {
		pr_err("%s: map_kernel is not implemented by this heap.\n",
		       __func__);
		mutex_unlock(&client->lock);
		return ERR_PTR(-ENODEV);
	}

	mutex_lock(&buffer->lock);
	vaddr = ion_handle_kmap_get(handle);
	mutex_unlock(&buffer->lock);
	mutex_unlock(&client->lock);
	return vaddr;
}
EXPORT_SYMBOL(ion_map_kernel);

void ion_unmap_kernel(struct ion_client *client, struct ion_handle *handle)
{
	struct ion_buffer *buffer;

	mutex_lock(&client->lock);
	buffer = handle->buffer;
	mutex_lock(&buffer->lock);
	ion_handle_kmap_put(handle);
	mutex_unlock(&buffer->lock);
	mutex_unlock(&client->lock);
}
EXPORT_SYMBOL(ion_unmap_kernel);

static int ion_client_validate(struct ion_device *dev,
				struct ion_client *client)
{
	struct rb_node *n;

	for (n = rb_first(&dev->clients); n; n = rb_next(n)) {
		struct ion_client *valid_client = rb_entry(n, struct ion_client, node);

		if (client == valid_client)
			return 1;
	}

	return 0;
}

static int ion_debug_client_show(struct seq_file *s, void *unused)
{
	struct ion_client *client = s->private;
	struct ion_device *dev = g_ion_device;
	struct rb_node *n;
	/*size_t sizes[ION_NUM_HEAP_IDS] = {0};
	const char *names[ION_NUM_HEAP_IDS] = {NULL};*/
	size_t *sizes;
	const char **names;
	int i;

	sizes = kcalloc(ION_NUM_HEAP_IDS, sizeof(size_t), GFP_ATOMIC);

	if (!sizes)
		return -ENOMEM;

	names = kcalloc(ION_NUM_HEAP_IDS, sizeof(char *), GFP_ATOMIC);

	if (!names)
		return -ENOMEM;

	down_read(&dev->lock);
	if (!ion_client_validate(dev, client)) {
		pr_err("%s: client is invlaid.\n", __func__);
		up_read(&dev->lock);
		kfree(sizes);
		kfree(names);
		return -1;
	}

	seq_printf(s, "%16.s %8.s %8.s %8.s %8.s %8.s\n",
			"heap_name", "pid", "size", "handle_count", "handle", "buffer");

	mutex_lock(&client->lock);
	for (n = rb_first(&client->handles); n; n = rb_next(n)) {
		struct ion_handle *handle = rb_entry(n, struct ion_handle,
						     node);
		struct ion_buffer *buffer = handle->buffer;
		unsigned int id = buffer->heap->id;

		if (!names[id])
			names[id] = buffer->heap->name;
		sizes[id] += buffer->size;

		seq_printf(s, "%16.s %3d %8zu %3d %pK %pK.\n", buffer->heap->name,
						client->pid, buffer->size, buffer->handle_count, handle, buffer);
	}
	mutex_unlock(&client->lock);

	seq_puts(s, "----------------------------------------------------\n");

	seq_printf(s, "%16.16s: %16.16s\n", "heap_name", "size_in_bytes");
	for (i = 0; i < ION_NUM_HEAP_IDS; i++) {
		if (!names[i])
			continue;
		seq_printf(s, "%16.16s: %16zu\n", names[i], sizes[i]);
	}

	up_read(&dev->lock);

	kfree(sizes);
	kfree(names);

	return 0;
}

static int ion_debug_client_open(struct inode *inode, struct file *file)
{
	return single_open(file, ion_debug_client_show, inode->i_private);
}

static const struct file_operations debug_client_fops = {
	.open = ion_debug_client_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ion_get_client_serial(const struct rb_root *root,
					const unsigned char *name)
{
	int serial = -1;
	struct rb_node *node;

	for (node = rb_first(root); node; node = rb_next(node)) {
		struct ion_client *client = rb_entry(node, struct ion_client,
						node);

		if (strcmp(client->name, name))
			continue;
		serial = max(serial, client->display_serial);
	}
	return serial + 1;
}

struct ion_client *ion_client_create(struct ion_device *dev,
				     const char *name)
{
	struct ion_client *client;
	struct task_struct *task;
	struct rb_node **p;
	struct rb_node *parent = NULL;
	struct ion_client *entry;
	pid_t pid;

	if (!name) {
		pr_err("%s: Name cannot be null\n", __func__);
		return ERR_PTR(-EINVAL);
	}

	get_task_struct(current->group_leader);
	task_lock(current->group_leader);
	pid = task_pid_nr(current->group_leader);
	/* don't bother to store task struct for kernel threads,
	   they can't be killed anyway */
	if (current->group_leader->flags & PF_KTHREAD) {
		put_task_struct(current->group_leader);
		task = NULL;
	} else {
		task = current->group_leader;
	}
	task_unlock(current->group_leader);

	client = kzalloc(sizeof(struct ion_client), GFP_KERNEL);
	if (!client)
		goto err_put_task_struct;

	client->dev = dev;
	client->handles = RB_ROOT;
	idr_init(&client->idr);
	mutex_init(&client->lock);
	client->task = task;
	client->pid = pid;
	client->name = kstrdup(name, GFP_KERNEL);
	if (!client->name)
		goto err_free_client;

	down_write(&dev->lock);
	client->display_serial = ion_get_client_serial(&dev->clients, name);
	client->display_name = kasprintf(
		GFP_KERNEL, "%s-%d", name, client->display_serial);
	if (!client->display_name) {
		up_write(&dev->lock);
		goto err_free_client_name;
	}
	p = &dev->clients.rb_node;
	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct ion_client, node);

		if (client < entry)
			p = &(*p)->rb_left;
		else if (client > entry)
			p = &(*p)->rb_right;
	}
	rb_link_node(&client->node, parent, p);
	rb_insert_color(&client->node, &dev->clients);

	client->debug_root = debugfs_create_file(client->display_name, 0664,
						dev->clients_debug_root,
						client, &debug_client_fops);
	if (!client->debug_root) {
		char buf[256], *path;

		path = dentry_path(dev->clients_debug_root, buf, 256);
		pr_err("Failed to create client debugfs at %s/%s\n",
			path, client->display_name);
	}

	up_write(&dev->lock);

	return client;

err_free_client_name:
	kfree(client->name);
err_free_client:
	kfree(client);
err_put_task_struct:
	if (task)
		put_task_struct(current->group_leader);
	return ERR_PTR(-ENOMEM);
}
EXPORT_SYMBOL(ion_client_create);

void ion_client_destroy(struct ion_client *client)
{
	struct ion_device *dev = client->dev;
	struct rb_node *n;

	pr_debug("%s: %d\n", __func__, __LINE__);
	while ((n = rb_first(&client->handles))) {
		struct ion_handle *handle = rb_entry(n, struct ion_handle,
						     node);

		mutex_lock(&client->lock);
		IONMSG("warn destroy: hdl=%p, buf=%p, ref=%d, sz=%zu, kmp=%d, client %s, disp %s, dbg %s\n",
		       handle, handle->buffer, atomic_read(&handle->buffer->ref.refcount),
				handle->buffer->size,  handle->buffer->kmap_cnt,
				client->name ? client->name : NULL,
				client->display_name ? client->display_name : NULL,
				client->dbg_name ? client->dbg_name : NULL);
		ion_handle_destroy(&handle->ref);
		mutex_unlock(&client->lock);
	}

	idr_destroy(&client->idr);

	down_write(&dev->lock);
	if (client->task)
		put_task_struct(client->task);
	rb_erase(&client->node, &dev->clients);
	debugfs_remove_recursive(client->debug_root);
	up_write(&dev->lock);

	kfree(client->display_name);
	kfree(client->name);
	kfree(client);
}
EXPORT_SYMBOL(ion_client_destroy);

struct sg_table *ion_sg_table(struct ion_client *client,
			      struct ion_handle *handle)
{
	struct ion_buffer *buffer;
	struct sg_table *table;

	mutex_lock(&client->lock);
	if (!ion_handle_validate(client, handle)) {
		pr_err("%s: invalid handle passed to map_dma.\n",
		       __func__);
		mutex_unlock(&client->lock);
		return ERR_PTR(-EINVAL);
	}
	buffer = handle->buffer;
	table = buffer->sg_table;
	mutex_unlock(&client->lock);
	return table;
}
EXPORT_SYMBOL(ion_sg_table);

static void ion_buffer_sync_for_device(struct ion_buffer *buffer,
				       struct device *dev,
				       enum dma_data_direction direction);

static struct sg_table *ion_map_dma_buf(struct dma_buf_attachment *attachment,
					enum dma_data_direction direction)
{
	struct dma_buf *dmabuf = attachment->dmabuf;
	struct ion_buffer *buffer = dmabuf->priv;

	ion_buffer_sync_for_device(buffer, attachment->dev, direction);
	return buffer->sg_table;
}

static void ion_unmap_dma_buf(struct dma_buf_attachment *attachment,
			      struct sg_table *table,
			      enum dma_data_direction direction)
{
}

void ion_pages_sync_for_device(struct device *dev, struct page *page,
		size_t size, enum dma_data_direction dir)
{
	struct scatterlist sg;

	sg_init_table(&sg, 1);
	sg_set_page(&sg, page, size, 0);
	/*
	 * This is not correct - sg_dma_address needs a dma_addr_t that is valid
	 * for the targeted device, but this works on the currently targeted
	 * hardware.
	 */
	sg_dma_address(&sg) = page_to_phys(page);
	dma_sync_sg_for_device(dev, &sg, 1, dir);
}

struct ion_vma_list {
	struct list_head list;
	struct vm_area_struct *vma;
};

static void ion_buffer_sync_for_device(struct ion_buffer *buffer,
				       struct device *dev,
				       enum dma_data_direction dir)
{
	struct ion_vma_list *vma_list;
	int pages = PAGE_ALIGN(buffer->size) / PAGE_SIZE;
	int i;

	pr_debug("%s: syncing for device %s\n", __func__,
		 dev ? dev_name(dev) : "null");

	if (!ion_buffer_fault_user_mappings(buffer))
		return;

	mutex_lock(&buffer->lock);
	for (i = 0; i < pages; i++) {
		struct page *page = buffer->pages[i];

		if (ion_buffer_page_is_dirty(page))
			ion_pages_sync_for_device(dev, ion_buffer_page(page),
							PAGE_SIZE, dir);

		ion_buffer_page_clean(buffer->pages + i);
	}
	list_for_each_entry(vma_list, &buffer->vmas, list) {
		struct vm_area_struct *vma = vma_list->vma;

		zap_page_range(vma, vma->vm_start, vma->vm_end - vma->vm_start,
			       NULL);
	}
	mutex_unlock(&buffer->lock);
}

static int ion_vm_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct ion_buffer *buffer = vma->vm_private_data;
	unsigned long pfn;
	int ret;

	mutex_lock(&buffer->lock);
	ion_buffer_page_dirty(buffer->pages + vmf->pgoff);
	BUG_ON(!buffer->pages || !buffer->pages[vmf->pgoff]);

	pfn = page_to_pfn(ion_buffer_page(buffer->pages[vmf->pgoff]));
	ret = vm_insert_pfn(vma, (unsigned long)vmf->virtual_address, pfn);
	mutex_unlock(&buffer->lock);
	if (ret) {
		IONMSG("%s vm insert pfn failed, vma = 0x%p, addr = 0x%p, pfn = %lu.\n",
				__func__, vma, vmf->virtual_address, pfn);
		return VM_FAULT_ERROR;
	}

	return VM_FAULT_NOPAGE;
}

static void ion_vm_open(struct vm_area_struct *vma)
{
	struct ion_buffer *buffer = vma->vm_private_data;
	struct ion_vma_list *vma_list;

	vma_list = kmalloc(sizeof(struct ion_vma_list), GFP_KERNEL);
	if (!vma_list) {
		IONMSG("%s kmalloc failed, vma_list is null.\n", __func__);
		return;
	}
	vma_list->vma = vma;
	mutex_lock(&buffer->lock);
	list_add(&vma_list->list, &buffer->vmas);
	mutex_unlock(&buffer->lock);
	pr_debug("%s: adding %pK\n", __func__, vma);
}

static void ion_vm_close(struct vm_area_struct *vma)
{
	struct ion_buffer *buffer = vma->vm_private_data;
	struct ion_vma_list *vma_list, *tmp;

	pr_debug("%s\n", __func__);
	mutex_lock(&buffer->lock);
	list_for_each_entry_safe(vma_list, tmp, &buffer->vmas, list) {
		if (vma_list->vma != vma)
			continue;
		list_del(&vma_list->list);
		kfree(vma_list);
		pr_debug("%s: deleting %pK\n", __func__, vma);
		break;
	}
	mutex_unlock(&buffer->lock);
}

static struct vm_operations_struct ion_vma_ops = {
	.open = ion_vm_open,
	.close = ion_vm_close,
	.fault = ion_vm_fault,
};

static int ion_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct ion_buffer *buffer = dmabuf->priv;
	int ret = 0;

	MMProfileLogEx(ION_MMP_Events[PROFILE_MAP_USER], MMProfileFlagStart, buffer->size, vma->vm_start);

	if (!buffer->heap->ops->map_user) {
		pr_err("%s: this heap does not define a method for mapping to userspace\n",
			__func__);
		return -EINVAL;
	}

	if (ion_buffer_fault_user_mappings(buffer)) {
		vma->vm_flags |= VM_IO | VM_PFNMAP | VM_DONTEXPAND |
							VM_DONTDUMP;
		vma->vm_private_data = buffer;
		vma->vm_ops = &ion_vma_ops;
		ion_vm_open(vma);
		return 0;
	}

	if (!(buffer->flags & ION_FLAG_CACHED))
		/* vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot); */
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	mutex_lock(&buffer->lock);
	/* now map it to userspace */
	ret = buffer->heap->ops->map_user(buffer->heap, buffer, vma);
	mutex_unlock(&buffer->lock);

	if (ret)
		pr_err("%s: failure mapping buffer to userspace\n",
		       __func__);
	MMProfileLogEx(ION_MMP_Events[PROFILE_MAP_USER], MMProfileFlagEnd, buffer->size, vma->vm_start);

	return ret;
}

static void ion_dma_buf_release(struct dma_buf *dmabuf)
{
	struct ion_buffer *buffer = dmabuf->priv;

	ion_buffer_put(buffer);
}

static void *ion_dma_buf_kmap(struct dma_buf *dmabuf, unsigned long offset)
{
	struct ion_buffer *buffer = dmabuf->priv;

	return buffer->vaddr + offset * PAGE_SIZE;
}

static void ion_dma_buf_kunmap(struct dma_buf *dmabuf, unsigned long offset,
			       void *ptr)
{
}

static int ion_dma_buf_begin_cpu_access(struct dma_buf *dmabuf, size_t start,
					size_t len,
					enum dma_data_direction direction)
{
	struct ion_buffer *buffer = dmabuf->priv;
	void *vaddr;

	if (!buffer->heap->ops->map_kernel) {
		pr_err("%s: map kernel is not implemented by this heap.\n",
		       __func__);
		return -ENODEV;
	}

	mutex_lock(&buffer->lock);
	vaddr = ion_buffer_kmap_get(buffer);
	mutex_unlock(&buffer->lock);
	return PTR_ERR_OR_ZERO(vaddr);
}

static void ion_dma_buf_end_cpu_access(struct dma_buf *dmabuf, size_t start,
				       size_t len,
				       enum dma_data_direction direction)
{
	struct ion_buffer *buffer = dmabuf->priv;

	mutex_lock(&buffer->lock);
	ion_buffer_kmap_put(buffer);
	mutex_unlock(&buffer->lock);
}

static struct dma_buf_ops dma_buf_ops = {
	.map_dma_buf = ion_map_dma_buf,
	.unmap_dma_buf = ion_unmap_dma_buf,
	.mmap = ion_mmap,
	.release = ion_dma_buf_release,
	.begin_cpu_access = ion_dma_buf_begin_cpu_access,
	.end_cpu_access = ion_dma_buf_end_cpu_access,
	.kmap_atomic = ion_dma_buf_kmap,
	.kunmap_atomic = ion_dma_buf_kunmap,
	.kmap = ion_dma_buf_kmap,
	.kunmap = ion_dma_buf_kunmap,
};

struct dma_buf *ion_share_dma_buf(struct ion_client *client,
						struct ion_handle *handle)
{
	struct ion_buffer *buffer;
	struct dma_buf *dmabuf;
	bool valid_handle;

	mutex_lock(&client->lock);
	valid_handle = ion_handle_validate(client, handle);
	if (!valid_handle) {
		WARN(1, "%s: invalid handle passed to share.\n", __func__);
		mutex_unlock(&client->lock);
		return ERR_PTR(-EINVAL);
	}
	buffer = handle->buffer;
	ion_buffer_get(buffer);
	mutex_unlock(&client->lock);

	dmabuf = dma_buf_export(buffer, &dma_buf_ops, buffer->size, O_RDWR,
				NULL);
	if (IS_ERR(dmabuf)) {
		IONMSG("%s dma buf export failed dmabuf is error 0x%p.\n", __func__, dmabuf);
		ion_buffer_put(buffer);
		return dmabuf;
	}

	return dmabuf;
}
EXPORT_SYMBOL(ion_share_dma_buf);

int ion_share_dma_buf_fd(struct ion_client *client, struct ion_handle *handle)
{
	struct dma_buf *dmabuf;
	int fd;

	dmabuf = ion_share_dma_buf(client, handle);
	if (IS_ERR(dmabuf)) {
		IONMSG("%s dmabuf is err 0x%p.\n", __func__, dmabuf);
		return PTR_ERR(dmabuf);
	}

	fd = dma_buf_fd(dmabuf, O_CLOEXEC);
	if (fd < 0) {
		IONMSG("%s dma_buf_fd failed %d.\n", __func__, fd);
		dma_buf_put(dmabuf);
	}
	handle->dbg.fd = fd;
	return fd;
}
EXPORT_SYMBOL(ion_share_dma_buf_fd);

struct ion_handle *ion_import_dma_buf(struct ion_client *client, int fd)
{
	struct dma_buf *dmabuf;
	struct ion_buffer *buffer;
	struct ion_handle *handle;
	int ret;

	MMProfileLogEx(ION_MMP_Events[PROFILE_IMPORT], MMProfileFlagStart, 1, 1);

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf)) {
		IONMSG("%s dma_buf_get fail fd=%d ret=0x%p\n", __func__, fd, dmabuf);
		return ERR_CAST(dmabuf);
	}
	/* if this memory came from ion */

	if (dmabuf->ops != &dma_buf_ops) {
		pr_err("%s: can not import dmabuf from another exporter\n",
		       __func__);
		dma_buf_put(dmabuf);
		return ERR_PTR(-EINVAL);
	}
	buffer = dmabuf->priv;

	mutex_lock(&client->lock);
	/* if a handle exists for this buffer just take a reference to it */
	handle = ion_handle_lookup(client, buffer);
	if (!IS_ERR(handle)) {
		handle = ion_handle_get_check_overflow(handle);
		mutex_unlock(&client->lock);
		goto end;
	}

	handle = ion_handle_create(client, buffer);
	if (IS_ERR(handle)) {
		mutex_unlock(&client->lock);
		IONMSG("%s handle is error 0x%p.\n", __func__, handle);
		goto end;
	}

	ret = ion_handle_add(client, handle);
	mutex_unlock(&client->lock);

	if (ret) {
		ion_handle_put(handle);
		handle = ERR_PTR(ret);
		IONMSG("ion_import: ion_handle_add fail %d\n", ret);
	}

end:
	dma_buf_put(dmabuf);

	MMProfileLogEx(ION_MMP_Events[PROFILE_IMPORT], MMProfileFlagEnd, 1, 1);
	handle->dbg.fd = fd;
	handle->dbg.user_ts = sched_clock();
	do_div(handle->dbg.user_ts, 1000000);
	return handle;
}
EXPORT_SYMBOL(ion_import_dma_buf);

static int ion_sync_for_device(struct ion_client *client, int fd)
{
	struct dma_buf *dmabuf;
	struct ion_buffer *buffer;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf)) {
		IONMSG("%s dma_buf_get failed dmabuf is err %d, 0x%p.\n", __func__, fd, dmabuf);
		return PTR_ERR(dmabuf);
	}

	/* if this memory came from ion */
	if (dmabuf->ops != &dma_buf_ops) {
		pr_err("%s: can not sync dmabuf from another exporter\n",
		       __func__);
		dma_buf_put(dmabuf);
		return -EINVAL;
	}
	buffer = dmabuf->priv;

	if (buffer->heap->type != (int)ION_HEAP_TYPE_FB)
		dma_sync_sg_for_device(NULL, buffer->sg_table->sgl,
			buffer->sg_table->nents, DMA_BIDIRECTIONAL);
	else
		pr_err("%s: can not sync support heap type(%d) to sync\n",
		       __func__, buffer->heap->type);

	dma_buf_put(dmabuf);
	return 0;
}

/* fix up the cases where the ioctl direction bits are incorrect */
static unsigned int ion_ioctl_dir(unsigned int cmd)
{
	switch (cmd) {
	case ION_IOC_SYNC:
	case ION_IOC_FREE:
	case ION_IOC_CUSTOM:
		return _IOC_WRITE;
	default:
		return _IOC_DIR(cmd);
	}
}

static long ion_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct ion_client *client = filp->private_data;
	struct ion_device *dev = client->dev;
	struct ion_handle *cleanup_handle = NULL;
	int ret = 0;
	unsigned int dir;

	union {
		struct ion_fd_data fd;
		struct ion_allocation_data allocation;
		struct ion_handle_data handle;
		struct ion_custom_data custom;
	} data;

	dir = ion_ioctl_dir(cmd);

	if (_IOC_SIZE(cmd) > sizeof(data)) {
		IONMSG("ion_ioctl cmd = %d, _IOC_SIZE(cmd) = %d, sizeof(data) = %zd.\n",
				cmd, _IOC_SIZE(cmd), sizeof(data));
		return -EINVAL;
	}

	if (dir & _IOC_WRITE) {
		if (copy_from_user(&data, (void __user *)arg, _IOC_SIZE(cmd))) {
			IONMSG("ion_ioctl copy_from_user fail!. cmd = %d, n = %d.\n", cmd, _IOC_SIZE(cmd));
			return -EFAULT;
		}
	}

	switch (cmd) {
	case ION_IOC_ALLOC:
	{
		struct ion_handle *handle;

		handle = __ion_alloc(client, data.allocation.len,
						data.allocation.align,
						data.allocation.heap_id_mask,
						data.allocation.flags, true);
		if (IS_ERR(handle)) {
			ret = PTR_ERR(handle);
			IONMSG("ION_IOC_ALLOC handle is invalid. ret = %d.\n", ret);
			return ret;
		}

		pass_to_user(handle);
		data.allocation.handle = handle->id;

		cleanup_handle = handle;
		break;
	}
	case ION_IOC_FREE:
	{
		struct ion_handle *handle;

		mutex_lock(&client->lock);
		handle = ion_handle_get_by_id_nolock(client, data.handle.handle);
		if (IS_ERR(handle)) {
			mutex_unlock(&client->lock);
			IONMSG("ION_IOC_FREE handle is invalid. handle = %d, ret = %d.\n", data.handle.handle, ret);
			return PTR_ERR(handle);
		}
		user_ion_free_nolock(client, handle);
		ion_handle_put_nolock(handle);
		mutex_unlock(&client->lock);
		break;
	}
	case ION_IOC_SHARE:
	case ION_IOC_MAP:
	{
		struct ion_handle *handle;

		handle = ion_handle_get_by_id(client, data.handle.handle);
		if (IS_ERR(handle)) {
			ret = PTR_ERR(handle);
			IONMSG("ION_IOC_SHARE handle is invalid. handle = %d, ret = %d.\n", data.handle.handle, ret);
			return ret;
		}
		data.fd.fd = ion_share_dma_buf_fd(client, handle);
		ion_handle_put(handle);
		if (data.fd.fd < 0) {
			IONMSG("ION_IOC_SHARE fd = %d.\n", data.fd.fd);
			ret = data.fd.fd;
		}
		break;
	}
	case ION_IOC_IMPORT:
	{
		struct ion_handle *handle;

		handle = ion_import_dma_buf(client, data.fd.fd);
		if (IS_ERR(handle)) {
			ret = PTR_ERR(handle);
			IONMSG("ion_import fail: fd=%d, ret=%d\n", data.fd.fd, ret);
		} else {
			handle = pass_to_user(handle);
			if (IS_ERR(handle))
				ret = PTR_ERR(handle);
			else
				data.handle.handle = handle->id;
		}
		break;
	}
	case ION_IOC_SYNC:
	{
		ret = ion_sync_for_device(client, data.fd.fd);
		break;
	}
	case ION_IOC_CUSTOM:
	{
		if (!dev->custom_ioctl) {
			IONMSG("ION_IOC_CUSTOM dev has no custom ioctl!.\n");
			return -ENOTTY;
		}
		ret = dev->custom_ioctl(client, data.custom.cmd,
						data.custom.arg);
		break;
	}
	default:
	{
		/*IONMSG("ion_ioctl : No such command!! 0x%x\n", cmd);
		ion_aee_print("ion_ioctl: No such command!! 0x%x.\n", cmd);*/
		return -ENOTTY;
	}
	}

	if (dir & _IOC_READ) {
		if (copy_to_user((void __user *)arg, &data, _IOC_SIZE(cmd))) {
			if (cleanup_handle) {
				mutex_lock(&client->lock);
				user_ion_free_nolock(client, cleanup_handle);
				ion_handle_put_nolock(cleanup_handle);
				mutex_unlock(&client->lock);
			}
			IONMSG("ion_ioctl copy_to_user fail! cmd = %d, n = %d.\n", cmd, _IOC_SIZE(cmd));
			return -EFAULT;
		}
	}
	if (cleanup_handle)
		ion_handle_put(cleanup_handle);
	return ret;
}

static int ion_release(struct inode *inode, struct file *file)
{
	struct ion_client *client = file->private_data;

	pr_debug("%s: %d\n", __func__, __LINE__);
	ion_client_destroy(client);
	return 0;
}

static int ion_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct ion_device *dev = container_of(miscdev, struct ion_device, dev);
	struct ion_client *client;
	char debug_name[64];

	pr_debug("%s: %d\n", __func__, __LINE__);
	snprintf(debug_name, 64, "%u", task_pid_nr(current->group_leader));
	client = ion_client_create(dev, debug_name);
	if (IS_ERR(client)) {
		IONMSG("%s ion client create failed 0x%p.\n", __func__, client);
		return PTR_ERR(client);
	}
	file->private_data = client;

	return 0;
}

static const struct file_operations ion_fops = {
	.owner          = THIS_MODULE,
	.open           = ion_open,
	.release        = ion_release,
	.unlocked_ioctl = ion_ioctl,
	.compat_ioctl   = compat_ion_ioctl,
};

static size_t ion_debug_heap_total(struct ion_client *client,
				   unsigned int id)
{
	size_t size = 0;
	struct rb_node *n;

	mutex_lock(&client->lock);
	for (n = rb_first(&client->handles); n; n = rb_next(n)) {
		struct ion_handle *handle = rb_entry(n,
						     struct ion_handle,
						     node);
		if ((handle->buffer->heap->id == id)
			|| ((id == ION_HEAP_TYPE_MULTIMEDIA)
			&& ((handle->buffer->heap->id == ION_HEAP_TYPE_MULTIMEDIA_FOR_CAMERA) ||
				(handle->buffer->heap->id == ION_HEAP_TYPE_MULTIMEDIA_MAP_MVA))))
			size += handle->buffer->size;
	}
	mutex_unlock(&client->lock);
	return size;
}

static int ion_debug_heap_show(struct seq_file *s, void *unused)
{
	struct ion_heap *heap = s->private;
	struct ion_device *dev = heap->dev;
	struct rb_node *n;
	size_t total_size = 0;
	size_t camera_total_size = 0;
	size_t total_orphaned_size = 0;

	seq_printf(s, "%16.s(%16.s) %16.s %16.s %s\n", "client", "dbg_name", "pid", "size", "address");
	seq_puts(s, "----------------------------------------------------\n");

	down_read(&dev->lock);
	for (n = rb_first(&dev->clients); n; n = rb_next(n)) {
		struct ion_client *client = rb_entry(n, struct ion_client,
						     node);
		size_t size = ion_debug_heap_total(client, heap->id);

		if (!size)
			continue;
		if (client->task) {
			char task_comm[TASK_COMM_LEN];

			get_task_comm(task_comm, client->task);
			seq_printf(s, "%16.s(%16.s) %16u %16zu 0x%p\n", task_comm,
					client->dbg_name, client->pid, size, client);
		} else {
			seq_printf(s, "%16.s(%16.s) %16u %16zu 0x%p\n", client->name,
					"from_kernel", client->pid, size, client);
		}
	}
	up_read(&dev->lock);

	seq_puts(s, "----------------------------------------------------\n");
	seq_puts(s, "orphaned allocations (info is from last known client):\n");
	mutex_lock(&dev->buffer_lock);
	for (n = rb_first(&dev->buffers); n; n = rb_next(n)) {
		struct ion_buffer *buffer = rb_entry(n, struct ion_buffer,
						     node);
		if (buffer->heap->id != heap->id) {
			if ((heap->id == ION_HEAP_TYPE_MULTIMEDIA)
				&& (buffer->heap->id == ION_HEAP_TYPE_MULTIMEDIA_FOR_CAMERA))
				camera_total_size += buffer->size;
			else
				continue;
		}
		total_size += buffer->size;
		if (!buffer->handle_count) {
			seq_printf(s, "%16.s %16u %16zu %d %d\n",
				   buffer->task_comm, buffer->pid,
				   buffer->size, buffer->kmap_cnt,
				   atomic_read(&buffer->ref.refcount));
			total_orphaned_size += buffer->size;
		}
	}
	mutex_unlock(&dev->buffer_lock);
	seq_puts(s, "----------------------------------------------------\n");
	seq_printf(s, "%16.s %16zu\n", "total orphaned",
		   total_orphaned_size);
	seq_printf(s, "%16.s %16zu\n", "total ", total_size);
	if (heap->id == ION_HEAP_TYPE_MULTIMEDIA)
		seq_printf(s, "%16.s %16zu\n", "camera total ", camera_total_size);
	if (heap->flags & ION_HEAP_FLAG_DEFER_FREE)
		seq_printf(s, "%16.s %16zu\n", "deferred free",
				heap->free_list_size);
	seq_puts(s, "----------------------------------------------------\n");

	if (heap->debug_show)
		heap->debug_show(heap, s, unused);

	return 0;
}

static int ion_debug_heap_open(struct inode *inode, struct file *file)
{
	return single_open(file, ion_debug_heap_show, inode->i_private);
}

static const struct file_operations debug_heap_fops = {
	.open = ion_debug_heap_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ion_debug_heap_pool_show(struct seq_file *s, void *unused)
{
	struct ion_heap *heap = s->private;
	size_t total_size;

	if (!heap->ops->page_pool_total) {
		pr_err("%s: ion page pool total is not implemented by heap(%s).\n",
		       __func__, heap->name);
		return -ENODEV;
	}

	total_size = heap->ops->page_pool_total(heap);

	return 0;
}

static int ion_debug_heap_pool_open(struct inode *inode, struct file *file)
{
	return single_open(file, ion_debug_heap_pool_show, inode->i_private);
}

static const struct file_operations debug_heap_pool_fops = {
	.open = ion_debug_heap_pool_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

#ifdef DEBUG_HEAP_SHRINKER
static int debug_shrink_set(void *data, u64 val)
{
	struct ion_heap *heap = data;
	struct shrink_control sc;
	int objs;

	sc.gfp_mask = -1;
	sc.nr_to_scan = 0;

	if (!val) {
		IONMSG("%s val cannot be zero.\n", __func__);
		return 0;
	}

	objs = heap->ops->shrink(heap, sc.gfp_mask, sc.nr_to_scan);
	sc.nr_to_scan = objs;

	heap->ops->shrink(heap, sc.gfp_mask, sc.nr_to_scan);
	return 0;
}

static int debug_shrink_get(void *data, u64 *val)
{
	struct ion_heap *heap = data;
	struct shrink_control sc;
	int objs;

	sc.gfp_mask = -1;
	sc.nr_to_scan = 0;

	objs = heap->ops->shrink(heap, sc.gfp_mask, sc.nr_to_scan);
	*val = objs;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(debug_shrink_fops, debug_shrink_get,
			debug_shrink_set, "%llu\n");
#endif

void ion_device_add_heap(struct ion_device *dev, struct ion_heap *heap)
{
	struct dentry *debug_file;
	char tmp_name[64];

	if (!heap->ops->allocate || !heap->ops->free || !heap->ops->map_dma ||
	    !heap->ops->unmap_dma)
		pr_err("%s: can not add heap with invalid ops struct.\n",
		       __func__);

	if (heap->flags & ION_HEAP_FLAG_DEFER_FREE)
		ion_heap_init_deferred_free(heap);

	if ((heap->flags & ION_HEAP_FLAG_DEFER_FREE) || heap->ops->shrink)
		ion_heap_init_shrinker(heap);

	heap->dev = dev;
	down_write(&dev->lock);
	/* use negative heap->id to reverse the priority -- when traversing
	   the list later attempt higher id numbers first */
	plist_node_init(&heap->node, -heap->id);
	plist_add(&heap->node, &dev->heaps);
	debug_file = debugfs_create_file(heap->name, 0664,
					dev->heaps_debug_root, heap,
					&debug_heap_fops);

	if (!debug_file) {
		char buf[256], *path;

		path = dentry_path(dev->heaps_debug_root, buf, 256);
		pr_err("Failed to create heap debugfs at %s/%s\n",
			path, heap->name);
	}

#ifdef DEBUG_HEAP_SHRINKER
	if (heap->ops->shrink) {
		char debug_name[64];

		snprintf(debug_name, 64, "%s_shrink", heap->name);
		debug_file = debugfs_create_file(
			debug_name, 0644, dev->heaps_debug_root, heap,
			&debug_shrink_fops);
		if (!debug_file) {
			char buf[256], *path;

			path = dentry_path(dev->heaps_debug_root, buf, 256);
			pr_err("Failed to create heap shrinker debugfs at %s/%s\n",
				path, debug_name);
		}
	}
#endif

	snprintf(tmp_name, 64, "%s_total_in_pool", heap->name);
	debug_file = debugfs_create_file(
			tmp_name, 0644, dev->heaps_debug_root, heap,
				     &debug_heap_pool_fops);
	if (!debug_file) {
		char buf[256], *path;

		path = dentry_path(dev->heaps_debug_root, buf, 256);
		pr_err("Failed to create heap page pool debugfs at %s/%s\n", path, tmp_name);
	}

	up_write(&dev->lock);
}

struct ion_device *ion_device_create(long (*custom_ioctl)
				     (struct ion_client *client,
				      unsigned int cmd,
				      unsigned long arg))
{
	struct ion_device *idev;
	int ret;

	idev = kzalloc(sizeof(struct ion_device), GFP_KERNEL);
	if (!idev) {
		IONMSG("%s kzalloc failed idev is null.\n", __func__);
		return ERR_PTR(-ENOMEM);
	}

	idev->dev.minor = MISC_DYNAMIC_MINOR;
	idev->dev.name = "ion";
	idev->dev.fops = &ion_fops;
	idev->dev.parent = NULL;
	ret = misc_register(&idev->dev);
	if (ret) {
		pr_err("ion: failed to register misc device.\n");
		kfree(idev);
		return ERR_PTR(ret);
	}

	idev->debug_root = debugfs_create_dir("ion", NULL);
	if (!idev->debug_root) {
		pr_err("ion: failed to create debugfs root directory.\n");
		goto debugfs_done;
	}
	idev->heaps_debug_root = debugfs_create_dir("heaps", idev->debug_root);
	if (!idev->heaps_debug_root) {
		pr_err("ion: failed to create debugfs heaps directory.\n");
		goto debugfs_done;
	}
	idev->clients_debug_root = debugfs_create_dir("clients",
						idev->debug_root);
	if (!idev->clients_debug_root)
		pr_err("ion: failed to create debugfs clients directory.\n");

debugfs_done:

	idev->custom_ioctl = custom_ioctl;
	idev->buffers = RB_ROOT;
	mutex_init(&idev->buffer_lock);
	init_rwsem(&idev->lock);
	plist_head_init(&idev->heaps);
	idev->clients = RB_ROOT;
	return idev;
}

void ion_device_destroy(struct ion_device *dev)
{
	misc_deregister(&dev->dev);
	debugfs_remove_recursive(dev->debug_root);
	/* XXX need to free the heaps and clients ? */
	kfree(dev);
}

void __init ion_reserve(struct ion_platform_data *data)
{
	int i;

	for (i = 0; i < data->nr; i++) {
		if (data->heaps[i].size == 0)
			continue;

		if (data->heaps[i].base == 0) {
			phys_addr_t paddr;

			paddr = memblock_alloc_base(data->heaps[i].size,
						    data->heaps[i].align,
						    MEMBLOCK_ALLOC_ANYWHERE);
			if (!paddr) {
				pr_err("%s: error allocating memblock for heap %d\n",
					__func__, i);
				continue;
			}
			data->heaps[i].base = paddr;
		} else {
			int ret = memblock_reserve(data->heaps[i].base,
					       data->heaps[i].size);
			if (ret)
				pr_err("memblock reserve of %zx@%lx failed\n",
				       data->heaps[i].size,
				       data->heaps[i].base);
		}
		pr_info("%s: %s reserved base %lx size %zu\n", __func__,
			data->heaps[i].name,
			data->heaps[i].base,
			data->heaps[i].size);
	}
}

/* ============================================================================ */
/* helper functions */
/* ============================================================================ */

struct ion_handle *ion_drv_get_handle(struct ion_client *client, int user_handle,
		struct ion_handle *kernel_handle, int from_kernel)
{
	struct ion_handle *handle;

	if (from_kernel) {
		handle = kernel_handle;

		if (IS_ERR_OR_NULL(handle)) {
			IONMSG("%s handle invalid, handle = 0x%p.\n", __func__, handle);
			return ERR_PTR(-EINVAL);
		}

		mutex_lock(&client->lock);
		if (!ion_handle_validate(client, handle)) {
			IONMSG("%s handle invalid, handle=0x%p\n", __func__, handle);
			mutex_unlock(&client->lock);
			return ERR_PTR(-EINVAL);
		}
		ion_handle_get(handle);
		mutex_unlock(&client->lock);
	} else {
		handle = ion_handle_get_by_id(client, user_handle);
		if (!handle) {
			IONMSG("%s handle invalid, handle_id=%d\n", __func__, user_handle);
			return ERR_PTR(-EINVAL);
		}
	}
	return handle;
}

int ion_drv_put_kernel_handle(void *kernel_handle)
{
	return ion_handle_put(kernel_handle);
}

struct ion_heap *ion_drv_get_heap(struct ion_device *dev, int heap_id, int need_lock)
{
	struct ion_heap *_heap, *heap = NULL, *tmp;

	if (need_lock)
		down_write(&dev->lock);

	plist_for_each_entry_safe(_heap, tmp, &dev->heaps, node) {
		if (_heap->id == heap_id) {
			heap = _heap;
			break;
		}
	}

	if (need_lock)
		up_write(&dev->lock);

	return heap;
}

struct ion_buffer *ion_drv_file_to_buffer(struct file *file)
{
	struct dma_buf *dmabuf;
	struct ion_buffer *buffer = NULL;
	const char *pathname = NULL;

	if (!file)
		goto file2buf_exit;
	if (!(file->f_path.dentry))
		goto file2buf_exit;

	pathname = file->f_path.dentry->d_name.name;
	if (!pathname)
		goto file2buf_exit;

	if (strstr(pathname, "dmabuf")) {
		dmabuf = file->private_data;
		if (dmabuf->ops == &dma_buf_ops)
			buffer = dmabuf->priv;
	}

file2buf_exit:

	if (buffer)
		return buffer;
	else
		return ERR_PTR(-EINVAL);
}

/* ============================================================================================= */
