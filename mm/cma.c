/*
 * Contiguous Memory Allocator
 *
 * Copyright (c) 2010-2011 by Samsung Electronics.
 * Copyright IBM Corporation, 2013
 * Copyright LG Electronics Inc., 2014
 * Written by:
 *	Marek Szyprowski <m.szyprowski@samsung.com>
 *	Michal Nazarewicz <mina86@mina86.com>
 *	Aneesh Kumar K.V <aneesh.kumar@linux.vnet.ibm.com>
 *	Joonsoo Kim <iamjoonsoo.kim@lge.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License or (at your optional) any later version of the license.
 */

#define pr_fmt(fmt) "cma: " fmt

#ifdef CONFIG_CMA_DEBUG
#ifndef DEBUG
#  define DEBUG
#endif
#endif

#include <linux/memblock.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/log2.h>
#include <linux/cma.h>
#include <linux/highmem.h>
#include <linux/io.h>

#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/swap.h>

struct cma {
	unsigned long	base_pfn;
	unsigned long	count;
	unsigned long	*bitmap;
	unsigned int order_per_bit; /* Order of pages represented by one bit */
	struct mutex	lock;
};

static struct cma cma_areas[MAX_CMA_AREAS];
static unsigned cma_area_count;
static DEFINE_MUTEX(cma_mutex);
static unsigned long cma_usage;

phys_addr_t cma_get_base(const struct cma *cma)
{
	return PFN_PHYS(cma->base_pfn);
}

unsigned long cma_get_size(const struct cma *cma)
{
	return cma->count << PAGE_SHIFT;
}

/* Get all cma range */
void cma_get_range(phys_addr_t *base, phys_addr_t *size)
{
	int i;
	unsigned long base_pfn = ULONG_MAX, max_pfn = 0;

	for (i = 0; i < cma_area_count; i++) {
		struct cma *cma = &cma_areas[i];

		if (cma->base_pfn < base_pfn)
			base_pfn = cma->base_pfn;

		if (cma->base_pfn + cma->count > max_pfn)
			max_pfn = cma->base_pfn + cma->count;
	}

	if (max_pfn) {
		*base = PFN_PHYS(base_pfn);
		*size = PFN_PHYS(max_pfn) - PFN_PHYS(base_pfn);
	} else {
		*base = *size = 0;
	}
}

void cma_resize_front(struct cma *cma, unsigned long nr_pfn)
{
	cma->base_pfn += nr_pfn;
	cma->count    -= nr_pfn;
}

static unsigned long cma_bitmap_aligned_mask(const struct cma *cma,
					     unsigned int align_order)
{
	if (align_order <= cma->order_per_bit)
		return 0;
	return (1UL << (align_order - cma->order_per_bit)) - 1;
}

/*
 * Find the offset of the base PFN from the specified align_order.
 * The value returned is represented in order_per_bits.
 */
static unsigned long cma_bitmap_aligned_offset(const struct cma *cma,
					       unsigned int align_order)
{
	return (cma->base_pfn & ((1UL << align_order) - 1))
		>> cma->order_per_bit;
}

static unsigned long cma_bitmap_maxno(struct cma *cma)
{
	return cma->count >> cma->order_per_bit;
}

static unsigned long cma_bitmap_pages_to_bits(const struct cma *cma,
					      unsigned long pages)
{
	return ALIGN(pages, 1UL << cma->order_per_bit) >> cma->order_per_bit;
}

static void cma_clear_bitmap(struct cma *cma, unsigned long pfn,
			     unsigned int count)
{
	unsigned long bitmap_no, bitmap_count;

	bitmap_no = (pfn - cma->base_pfn) >> cma->order_per_bit;
	bitmap_count = cma_bitmap_pages_to_bits(cma, count);

	mutex_lock(&cma->lock);
	bitmap_clear(cma->bitmap, bitmap_no, bitmap_count);
	mutex_unlock(&cma->lock);
}

static int __init cma_activate_area(struct cma *cma)
{
	int bitmap_size = BITS_TO_LONGS(cma_bitmap_maxno(cma)) * sizeof(long);
	unsigned long base_pfn = cma->base_pfn, pfn = base_pfn;
	unsigned i = cma->count >> pageblock_order;
	struct zone *zone;

	cma->bitmap = kzalloc(bitmap_size, GFP_KERNEL);

	if (!cma->bitmap)
		return -ENOMEM;

	WARN_ON_ONCE(!pfn_valid(pfn));
	zone = page_zone(pfn_to_page(pfn));

	do {
		unsigned j;

		base_pfn = pfn;
		for (j = pageblock_nr_pages; j; --j, pfn++) {
			WARN_ON_ONCE(!pfn_valid(pfn));
			/*
			 * alloc_contig_range requires the pfn range
			 * specified to be in the same zone. Make this
			 * simple by forcing the entire CMA resv range
			 * to be in the same zone.
			 */
			if (page_zone(pfn_to_page(pfn)) != zone)
				goto err;
		}
		init_cma_reserved_pageblock(pfn_to_page(base_pfn));
	} while (--i);

	mutex_init(&cma->lock);
	return 0;

err:
	kfree(cma->bitmap);
	cma->count = 0;
	return -EINVAL;
}

static int __init cma_init_reserved_areas(void)
{
	int i;

	for (i = 0; i < cma_area_count; i++) {
		int ret = cma_activate_area(&cma_areas[i]);

		if (ret)
			return ret;
	}

	return 0;
}
core_initcall(cma_init_reserved_areas);

/**
 * cma_init_reserved_mem() - create custom contiguous area from reserved memory
 * @base: Base address of the reserved area
 * @size: Size of the reserved area (in bytes),
 * @order_per_bit: Order of pages represented by one bit on bitmap.
 * @res_cma: Pointer to store the created cma region.
 *
 * This function creates custom contiguous area from already reserved memory.
 */
int __init cma_init_reserved_mem(phys_addr_t base, phys_addr_t size,
				 unsigned int order_per_bit,
				 struct cma **res_cma)
{
	struct cma *cma;
	phys_addr_t alignment;

	/* Sanity checks */
	if (cma_area_count == ARRAY_SIZE(cma_areas)) {
		pr_err("Not enough slots for CMA reserved regions!\n");
		return -ENOSPC;
	}

	if (!size || !memblock_is_region_reserved(base, size))
		return -EINVAL;

	/* ensure minimal alignment requied by mm core */
	alignment = PAGE_SIZE <<
			max_t(unsigned long, MAX_ORDER - 1, pageblock_order);

	/* alignment should be aligned with order_per_bit */
	if (!IS_ALIGNED(alignment >> PAGE_SHIFT, 1 << order_per_bit))
		return -EINVAL;

	if (ALIGN(base, alignment) != base || ALIGN(size, alignment) != size)
		return -EINVAL;

	/*
	 * Each reserved area must be initialised later, when more kernel
	 * subsystems (like slab allocator) are available.
	 */
	cma = &cma_areas[cma_area_count];
	cma->base_pfn = PFN_DOWN(base);
	cma->count = size >> PAGE_SHIFT;
	cma->order_per_bit = order_per_bit;
	*res_cma = cma;
	cma_area_count++;
	totalcma_pages += (size / PAGE_SIZE);

	return 0;
}

/**
 * cma_declare_contiguous() - reserve custom contiguous area
 * @base: Base address of the reserved area optional, use 0 for any
 * @size: Size of the reserved area (in bytes),
 * @limit: End address of the reserved memory (optional, 0 for any).
 * @alignment: Alignment for the CMA area, should be power of 2 or zero
 * @order_per_bit: Order of pages represented by one bit on bitmap.
 * @fixed: hint about where to place the reserved area
 * @res_cma: Pointer to store the created cma region.
 *
 * This function reserves memory from early allocator. It should be
 * called by arch specific code once the early allocator (memblock or bootmem)
 * has been activated and all other subsystems have already allocated/reserved
 * memory. This function allows to create custom reserved areas.
 *
 * If @fixed is true, reserve contiguous area at exactly @base.  If false,
 * reserve in range from @base to @limit.
 */
int __init cma_declare_contiguous(phys_addr_t base,
			phys_addr_t size, phys_addr_t limit,
			phys_addr_t alignment, unsigned int order_per_bit,
			bool fixed, struct cma **res_cma)
{
	phys_addr_t memblock_end = memblock_end_of_DRAM();
	phys_addr_t highmem_start;
	int ret = 0;

#ifdef CONFIG_X86
	/*
	 * high_memory isn't direct mapped memory so retrieving its physical
	 * address isn't appropriate.  But it would be useful to check the
	 * physical address of the highmem boundary so it's justfiable to get
	 * the physical address from it.  On x86 there is a validation check for
	 * this case, so the following workaround is needed to avoid it.
	 */
	highmem_start = __pa_nodebug(high_memory);
#else
	highmem_start = __pa(high_memory);
#endif
	pr_debug("%s(size %pa, base %pa, limit %pa alignment %pa)\n",
		__func__, &size, &base, &limit, &alignment);

	if (cma_area_count == ARRAY_SIZE(cma_areas)) {
		pr_err("Not enough slots for CMA reserved regions!\n");
		return -ENOSPC;
	}

	if (!size)
		return -EINVAL;

	if (alignment && !is_power_of_2(alignment))
		return -EINVAL;

	/*
	 * Sanitise input arguments.
	 * Pages both ends in CMA area could be merged into adjacent unmovable
	 * migratetype page by page allocator's buddy algorithm. In the case,
	 * you couldn't get a contiguous memory, which is not what we want.
	 */
	alignment = max(alignment,  (phys_addr_t)PAGE_SIZE <<
			  max_t(unsigned long, MAX_ORDER - 1, pageblock_order));
	base = ALIGN(base, alignment);
	size = ALIGN(size, alignment);
	limit &= ~(alignment - 1);

	if (!base)
		fixed = false;

	/* size should be aligned with order_per_bit */
	if (!IS_ALIGNED(size >> PAGE_SHIFT, 1 << order_per_bit))
		return -EINVAL;

	/*
	 * If allocating at a fixed base the request region must not cross the
	 * low/high memory boundary.
	 */
	if (fixed && base < highmem_start && base + size > highmem_start) {
		ret = -EINVAL;
		pr_err("Region at %pa defined on low/high memory boundary (%pa)\n",
			&base, &highmem_start);
		goto err;
	}

	/*
	 * If the limit is unspecified or above the memblock end, its effective
	 * value will be the memblock end. Set it explicitly to simplify further
	 * checks.
	 */
	if (limit == 0 || limit > memblock_end)
		limit = memblock_end;

	/* Reserve memory */
	if (fixed) {
		if (memblock_is_region_reserved(base, size) ||
		    memblock_reserve(base, size) < 0) {
			ret = -EBUSY;
			goto err;
		}
	} else {
		phys_addr_t addr = 0;

		/*
		 * All pages in the reserved area must come from the same zone.
		 * If the requested region crosses the low/high memory boundary,
		 * try allocating from high memory first and fall back to low
		 * memory in case of failure.
		 */
		if (base < highmem_start && limit > highmem_start) {
			addr = memblock_alloc_range(size, alignment,
						    highmem_start, limit);
			limit = highmem_start;
		}

		if (!addr) {
			addr = memblock_alloc_range(size, alignment, base,
						    limit);
			if (!addr) {
				ret = -ENOMEM;
				goto err;
			}
		}

		/*
		 * kmemleak scans/reads tracked objects for pointers to other
		 * objects but this address isn't mapped and accessible
		 */
		kmemleak_ignore(phys_to_virt(addr));
		base = addr;
	}

	ret = cma_init_reserved_mem(base, size, order_per_bit, res_cma);
	if (ret)
		goto free_mem;

	pr_info("Reserved %ld MiB at %pa\n", (unsigned long)size / SZ_1M,
		&base);
	return 0;

free_mem:
	memblock_free(base, size);
err:
	pr_err("Failed to reserve %ld MiB\n", (unsigned long)size / SZ_1M);
	return ret;
}

int cma_alloc_range_ok(struct cma *cma, int count, int align)
{
	unsigned long mask;
	unsigned long bitmap_maxno, bitmap_no, bitmap_count;

	mask = cma_bitmap_aligned_mask(cma, align);
	bitmap_maxno = cma_bitmap_maxno(cma);
	bitmap_count = cma_bitmap_pages_to_bits(cma, count);

	bitmap_no = bitmap_find_next_zero_area(cma->bitmap,
			bitmap_maxno, 0, bitmap_count, mask);

	if (bitmap_no >= bitmap_maxno)
		return false;
	return true;
}

/**
 * cma_alloc() - allocate pages from contiguous area
 * @cma:   Contiguous memory region for which the allocation is performed.
 * @count: Requested number of pages.
 * @align: Requested alignment of pages (in PAGE_SIZE order).
 *
 * This function allocates part of contiguous memory on specific
 * contiguous memory area.
 */
struct page *cma_alloc(struct cma *cma, size_t count, unsigned int align)
{
	unsigned long mask, offset, pfn, start = 0;
	unsigned long bitmap_maxno, bitmap_no, bitmap_count;
	struct page *page = NULL;
	int ret;

	if (!cma || !cma->count)
		return NULL;

	pr_debug("%s(cma %p, count %zu, align %d)\n", __func__, (void *)cma,
		 count, align);

	if (!count)
		return NULL;

	mask = cma_bitmap_aligned_mask(cma, align);
	offset = cma_bitmap_aligned_offset(cma, align);
	bitmap_maxno = cma_bitmap_maxno(cma);
	bitmap_count = cma_bitmap_pages_to_bits(cma, count);

	for (;;) {
		mutex_lock(&cma->lock);
		bitmap_no = bitmap_find_next_zero_area_off(cma->bitmap,
				bitmap_maxno, start, bitmap_count, mask,
				offset);
		if (bitmap_no >= bitmap_maxno) {
			mutex_unlock(&cma->lock);
			break;
		}
		bitmap_set(cma->bitmap, bitmap_no, bitmap_count);
		/*
		 * It's safe to drop the lock here. We've marked this region for
		 * our exclusive use. If the migration fails we will take the
		 * lock again and unmark it.
		 */
		mutex_unlock(&cma->lock);

		pfn = cma->base_pfn + (bitmap_no << cma->order_per_bit);
		mutex_lock(&cma_mutex);
		ret = alloc_contig_range(pfn, pfn + count, MIGRATE_CMA);
		mutex_unlock(&cma_mutex);
		if (ret == 0) {

			mutex_lock(&cma_mutex);
			cma_usage += count;
			mutex_unlock(&cma_mutex);

			page = pfn_to_page(pfn);
			break;
		}

		cma_clear_bitmap(cma, pfn, count);
		if (ret != -EBUSY)
			break;

		pr_debug("%s(): memory range at %p is busy, retrying\n",
			 __func__, pfn_to_page(pfn));
		/* try again with a bit different memory target */
		start = bitmap_no + mask + 1;
	}

	pr_debug("%s(): returned %p\n", __func__, page);
	return page;
}

/**
 * cma_release() - release allocated pages
 * @cma:   Contiguous memory region for which the allocation is performed.
 * @pages: Allocated pages.
 * @count: Number of allocated pages.
 *
 * This function releases memory allocated by alloc_cma().
 * It returns false when provided pages do not belong to contiguous area and
 * true otherwise.
 */
bool cma_release(struct cma *cma, const struct page *pages, unsigned int count)
{
	unsigned long pfn;

	if (!cma || !pages)
		return false;

	pr_debug("%s(page %p)\n", __func__, (void *)pages);

	pfn = page_to_pfn(pages);

	if (pfn < cma->base_pfn || pfn >= cma->base_pfn + cma->count)
		return false;

	VM_BUG_ON(pfn + count > cma->base_pfn + cma->count);

	free_contig_range(pfn, count);
	cma_clear_bitmap(cma, pfn, count);

	mutex_lock(&cma_mutex);
	cma_usage -= count;
	mutex_unlock(&cma_mutex);

	return true;
}

#define MAX_SHRINK_PAGES  ((unsigned long)40*1024*1024/4096)

/*
 * Make sure we have at least @pages of free memory
 * return number of pages freed
 */
static unsigned long try_shrink_memory(unsigned long pages)
{
	unsigned long free = nr_free_pages(), tfree = 0, ofree, alloc;
	int max_retries;
	unsigned long start, end;

	start = sched_clock();

	ofree = free;
	max_retries = pages / MAX_SHRINK_PAGES + 5;
	while (pages > free && max_retries--) {
		/*
		 * When shrinking large numbers of pages at once, vmscan
		 * tends to free too much pages and using lots of time.
		 *
		 * Only shrink a maximum number of page at a time.
		 */
		alloc = min(MAX_SHRINK_PAGES, pages - free);
		tfree += shrink_all_memory(alloc);
		free = nr_free_pages();
	}

	end = sched_clock();
	pr_info("%s: free pages %lu, current free %lu, takes %lu us\n",
		__func__, tfree, nr_free_pages(), end-start);

	return tfree;
}

/**
 * cma_alloc_large() - Allocate large chunk of memory from the cma zone.
 * @cma:   Contiguous memory region for which the allocation is performed.
 * @count: Requested number of pages.
 * @align: Requested alignment of pages (in PAGE_SIZE order).
 *
 * Normal cma_alloc will easily to fail and/or take lots of time when user
 * trying to allocate large chunk of memory from it. Add helper function
 * to improve this usage.
 *
 * return first page of allocated memory.
 */
struct page *cma_alloc_large(struct cma *cma, int count, unsigned int align)
{
	struct page *page;
	struct zone *zone;
	unsigned long wmark_low = 0;
	struct zone *zones = NODE_DATA(numa_node_id())->node_zones;
	int retries = 0, org_swappiness;

	/*
	 * We are going to make lots of free spaces. Pages swap out during
	 * the process might be freed soon. Temporary set swappiness to 0 to
	 * reduce this waste and accelerate freeing.
	 */
	org_swappiness = vm_swappiness;
	vm_swappiness = 0;

	for_each_zone(zone)
		if (zone != &zones[ZONE_MOVABLE])
			wmark_low += zone->watermark[WMARK_LOW];

	/*
	 * Make sure we have enough free memory to fullfil this request.
	 *
	 * This helps to trigger memory shrinker faster and make sure cma_alloc
	 * below will success.
	 * Without this, pages migrate out from CMA ares might be freed when
	 * CMA tries to make room to migrate more pages. This also reduces
	 * pages CMA need to handle by freeing them first.
	 *
	 * Kernel will start memory reclaim when free memory is lower than
	 * low watermark. To avoid this while we do cma_alloc, we should make
	 * sure free memory is more than count + wmark_low
	 */
	try_shrink_memory(count + wmark_low);

	for (retries = 0; retries < 3; retries++) {
		page = cma_alloc(cma, count, align);
		if (page)
			break;
	}

	vm_swappiness = org_swappiness;
	return page;
}

static int cma_usage_show(struct seq_file *m, void *v)
{
	unsigned char *fmt = "%-10s: %10lu kB\n";

	seq_printf(m, fmt, "CMA usage", cma_usage*4);

	return 0;
}

static int cma_usage_open(struct inode *inode, struct file *file)
{
	return single_open(file, &cma_usage_show, NULL);
}

static const struct file_operations memory_ssvp_fops = {
	.open		= cma_usage_open,
	.read		= seq_read,
	.release	= single_release,
};

/**
 * Provide CMA memory allocation usage
 * cat /sys/kernel/debug/cmainfo
 */
static int __init cma_debug_init(void)
{
	int ret = 0;

	struct dentry *dentry;

	dentry = debugfs_create_file("cmainfo", S_IRUGO, NULL, NULL,
					&memory_ssvp_fops);
	if (!dentry)
		pr_warn("Failed to create debugfs cmainfo file\n");
	else
		pr_info("cma usage create success.");

	return ret;
}

late_initcall(cma_debug_init);
