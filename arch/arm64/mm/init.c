/*
 * Based on arch/arm/mm/init.c
 *
 * Copyright (C) 1995-2005 Russell King
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/errno.h>
#include <linux/swap.h>
#include <linux/init.h>
#include <linux/bootmem.h>
#include <linux/mman.h>
#include <linux/nodemask.h>
#include <linux/initrd.h>
#include <linux/gfp.h>
#include <linux/memblock.h>
#include <linux/sort.h>
#include <linux/of_fdt.h>
#include <linux/dma-mapping.h>
#include <linux/dma-contiguous.h>
#include <linux/efi.h>
#include <linux/cma.h>
#include <mt-plat/mtk_meminfo.h>

#include <asm/fixmap.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/sizes.h>
#include <asm/tlb.h>
#include <asm/alternative.h>
#include <mt-plat/mrdump.h>

#include "mm.h"

phys_addr_t memstart_addr __read_mostly = 0;

#ifdef CONFIG_BLK_DEV_INITRD
static int __init early_initrd(char *p)
{
	unsigned long start, size;
	char *endp;

	start = memparse(p, &endp);
	if (*endp == ',') {
		size = memparse(endp + 1, NULL);

		initrd_start = (unsigned long)__va(start);
		initrd_end = (unsigned long)__va(start + size);
	}
	return 0;
}
early_param("initrd", early_initrd);
#endif

/*
 * Return the maximum physical address for ZONE_DMA (DMA_BIT_MASK(32)). It
 * currently assumes that for memory starting above 4G, 32-bit devices will
 * use a DMA offset.
 */
static phys_addr_t __init max_zone_dma_phys(void)
{
	phys_addr_t offset = memblock_start_of_DRAM() & GENMASK_ULL(63, 32);
	return min(offset + (1ULL << 32), memblock_end_of_DRAM());
}

static void __init zone_sizes_init(unsigned long min, unsigned long max)
{
	struct memblock_region *reg;
	unsigned long zone_size[MAX_NR_ZONES], zhole_size[MAX_NR_ZONES];
	unsigned long max_dma = min;
#ifdef CONFIG_ZONE_MOVABLE_CMA
	phys_addr_t cma_base, cma_size;
	unsigned long cma_base_pfn = ULONG_MAX;

	cma_get_range(&cma_base, &cma_size);
	if (cma_size)
		cma_base_pfn = PFN_DOWN(cma_base);
#endif

	memset(zone_size, 0, sizeof(zone_size));

	/* 4GB maximum for 32-bit only capable devices */
	if (IS_ENABLED(CONFIG_ZONE_DMA)) {
		max_dma = PFN_DOWN(max_zone_dma_phys());
#ifdef CONFIG_ZONE_MOVABLE_CMA
		max_dma = min(max_dma, cma_base_pfn);
#endif
		zone_size[ZONE_DMA] = max_dma - min;
	}
#ifdef CONFIG_ZONE_MOVABLE_CMA
	if (cma_size) {
		zone_size[ZONE_NORMAL] = cma_base_pfn - max_dma;
		zone_size[ZONE_MOVABLE] = max - cma_base_pfn;
	} else {
		zone_size[ZONE_NORMAL] = max - max_dma;
	}
#else
	zone_size[ZONE_NORMAL] = max - max_dma;
#endif

	memcpy(zhole_size, zone_size, sizeof(zhole_size));

	for_each_memblock(memory, reg) {
		unsigned long start = memblock_region_memory_base_pfn(reg);
		unsigned long end = memblock_region_memory_end_pfn(reg);

		if (start >= max)
			continue;

		if (IS_ENABLED(CONFIG_ZONE_DMA) && start < max_dma) {
			unsigned long dma_end = min(end, max_dma);
			zhole_size[ZONE_DMA] -= dma_end - start;
		}

#ifdef CONFIG_ZONE_MOVABLE_CMA
		if (zone_size[ZONE_NORMAL] && end > max_dma && start < cma_base_pfn) {
			unsigned long normal_end = min(end, cma_base_pfn);
			unsigned long normal_start = max(start, max_dma);

			zhole_size[ZONE_NORMAL] -= normal_end - normal_start;
		}

		if (cma_size && end > cma_base_pfn) {
			unsigned long movable_end = min(end, max);
			unsigned long movable_start = max(start, cma_base_pfn);

			zhole_size[ZONE_MOVABLE] -= movable_end - movable_start;
		}

		if (!cma_size && end > max_dma) {
			unsigned long normal_end = min(end, max);
			unsigned long normal_start = max(start, max_dma);

			zhole_size[ZONE_NORMAL] -= normal_end - normal_start;
		}
#else
		if (end > max_dma) {
			unsigned long normal_end = min(end, max);
			unsigned long normal_start = max(start, max_dma);
			zhole_size[ZONE_NORMAL] -= normal_end - normal_start;
		}
#endif
	}

	free_area_init_node(0, zone_size, min, zhole_size);
}

#ifdef CONFIG_HAVE_ARCH_PFN_VALID
#define PFN_MASK ((1UL << (64 - PAGE_SHIFT)) - 1)

int pfn_valid(unsigned long pfn)
{
	phys_addr_t addr = pfn << PAGE_SHIFT;

	if ((addr >> PAGE_SHIFT) != pfn)
		return 0;
	return (pfn & PFN_MASK) == pfn && memblock_is_memory(addr);
}
EXPORT_SYMBOL(pfn_valid);
#endif

#ifndef CONFIG_SPARSEMEM
static void __init arm64_memory_present(void)
{
}
#else
static void __init arm64_memory_present(void)
{
	struct memblock_region *reg;

	for_each_memblock(memory, reg)
		memory_present(0, memblock_region_memory_base_pfn(reg),
			       memblock_region_memory_end_pfn(reg));
}
#endif

void __init arm64_memblock_init(void)
{
	phys_addr_t dma_phys_limit = 0;

	/*
	 * Register the kernel text, kernel data, initrd, and initial
	 * pagetables with memblock.
	 */
	memblock_reserve(__pa(_text), _end - _text);
#ifdef CONFIG_BLK_DEV_INITRD
	if (initrd_start)
		memblock_reserve(__virt_to_phys(initrd_start), initrd_end - initrd_start);
#endif

	early_init_fdt_scan_reserved_mem();

	/* 4GB maximum for 32-bit only capable devices */
	if (IS_ENABLED(CONFIG_ZONE_DMA))
		dma_phys_limit = max_zone_dma_phys();

	high_memory = __va(memblock_end_of_DRAM() - 1) + 1;
	dma_contiguous_reserve(dma_phys_limit);

	mrdump_rsvmem();
	memblock_allow_resize();
	memblock_dump_all();
}

void __init bootmem_init(void)
{
	unsigned long min, max;

	min = PFN_UP(memblock_start_of_DRAM());
	max = PFN_DOWN(memblock_end_of_DRAM());

	/*
	 * Sparsemem tries to allocate bootmem in memory_present(), so must be
	 * done after the fixed reservations.
	 */
	arm64_memory_present();

	sparse_init();
	zone_sizes_init(min, max);

	max_pfn = max_low_pfn = max;
}

#ifndef CONFIG_SPARSEMEM_VMEMMAP
static inline void free_memmap(unsigned long start_pfn, unsigned long end_pfn)
{
	struct page *start_pg, *end_pg;
	unsigned long pg, pgend;

	/*
	 * Convert start_pfn/end_pfn to a struct page pointer.
	 */
	start_pg = pfn_to_page(start_pfn - 1) + 1;
	end_pg = pfn_to_page(end_pfn - 1) + 1;

	/*
	 * Convert to physical addresses, and round start upwards and end
	 * downwards.
	 */
	pg = (unsigned long)PAGE_ALIGN(__pa(start_pg));
	pgend = (unsigned long)__pa(end_pg) & PAGE_MASK;

	/*
	 * If there are free pages between these, free the section of the
	 * memmap array.
	 */
	if (pg < pgend)
		free_bootmem(pg, pgend - pg);
}

/*
 * The mem_map array can get very big. Free the unused area of the memory map.
 */
static void __init free_unused_memmap(void)
{
	unsigned long start, prev_end = 0;
	struct memblock_region *reg;

	for_each_memblock(memory, reg) {
		start = __phys_to_pfn(reg->base);

#ifdef CONFIG_SPARSEMEM
		/*
		 * Take care not to free memmap entries that don't exist due
		 * to SPARSEMEM sections which aren't present.
		 */
		start = min(start, ALIGN(prev_end, PAGES_PER_SECTION));
#endif
		/*
		 * If we had a previous bank, and there is a space between the
		 * current bank and the previous, free it.
		 */
		if (prev_end && prev_end < start)
			free_memmap(prev_end, start);

		/*
		 * Align up here since the VM subsystem insists that the
		 * memmap entries are valid from the bank end aligned to
		 * MAX_ORDER_NR_PAGES.
		 */
		prev_end = ALIGN(__phys_to_pfn(reg->base + reg->size),
				 MAX_ORDER_NR_PAGES);
	}

#ifdef CONFIG_SPARSEMEM
	if (!IS_ALIGNED(prev_end, PAGES_PER_SECTION))
		free_memmap(prev_end, ALIGN(prev_end, PAGES_PER_SECTION));
#endif
}
#endif	/* !CONFIG_SPARSEMEM_VMEMMAP */

/*
 * mem_init() marks the free areas in the mem_map and tells us how much memory
 * is free.  This is done after various parts of the system have claimed their
 * memory after the kernel image.
 */
void __init mem_init(void)
{
	set_max_mapnr(pfn_to_page(max_pfn) - mem_map);

#ifndef CONFIG_SPARSEMEM_VMEMMAP
	free_unused_memmap();
#endif
	/* this will put all unused low memory onto the freelists */
	free_all_bootmem();

	mem_init_print_info(NULL);

#define MLK(b, t) b, t, ((t) - (b)) >> 10
#define MLM(b, t) b, t, ((t) - (b)) >> 20
#define MLG(b, t) b, t, ((t) - (b)) >> 30
#define MLK_ROUNDUP(b, t) b, t, DIV_ROUND_UP(((t) - (b)), SZ_1K)

	pr_notice("Virtual kernel memory layout:\n"
		  "    vmalloc : 0x%16lx - 0x%16lx   (%6ld GB)\n"
#ifdef CONFIG_SPARSEMEM_VMEMMAP
		  "    vmemmap : 0x%16lx - 0x%16lx   (%6ld GB maximum)\n"
		  "              0x%16lx - 0x%16lx   (%6ld MB actual)\n"
#endif
		  "    PCI I/O : 0x%16lx - 0x%16lx   (%6ld MB)\n"
		  "    fixed   : 0x%16lx - 0x%16lx   (%6ld KB)\n"
		  "    modules : 0x%16lx - 0x%16lx   (%6ld MB)\n"
		  "    memory  : 0x%16lx - 0x%16lx   (%6ld MB)\n"
		  "      .init : 0x%p" " - 0x%p" "   (%6ld KB)\n"
		  "      .text : 0x%p" " - 0x%p" "   (%6ld KB)\n"
		  "      .data : 0x%p" " - 0x%p" "   (%6ld KB)\n",
		  MLG(VMALLOC_START, VMALLOC_END),
#ifdef CONFIG_SPARSEMEM_VMEMMAP
		  MLG(VMEMMAP_START,
		      VMEMMAP_START + VMEMMAP_SIZE),
		  MLM((unsigned long)virt_to_page(PAGE_OFFSET),
		      (unsigned long)virt_to_page(high_memory)),
#endif
		  MLM((unsigned long)PCI_IOBASE, (unsigned long)PCI_IOBASE + SZ_16M),
		  MLK(FIXADDR_START, FIXADDR_TOP),
		  MLM(MODULES_VADDR, MODULES_END),
		  MLM(PAGE_OFFSET, (unsigned long)high_memory),
		  MLK_ROUNDUP(__init_begin, __init_end),
		  MLK_ROUNDUP(_text, _etext),
		  MLK_ROUNDUP(_sdata, _edata));

#undef MLK
#undef MLM
#undef MLK_ROUNDUP

	/*
	 * Check boundaries twice: Some fundamental inconsistencies can be
	 * detected at build time already.
	 */
#ifdef CONFIG_COMPAT
	BUILD_BUG_ON(TASK_SIZE_32			> TASK_SIZE_64);
#endif
	BUILD_BUG_ON(TASK_SIZE_64			> MODULES_VADDR);
	BUG_ON(TASK_SIZE_64				> MODULES_VADDR);

	if (PAGE_SIZE >= 16384 && get_num_physpages() <= 128) {
		extern int sysctl_overcommit_memory;
		/*
		 * On a machine this small we won't get anywhere without
		 * overcommit, so turn it on by default.
		 */
		sysctl_overcommit_memory = OVERCOMMIT_ALWAYS;
	}
}

void free_initmem(void)
{
	fixup_init();
	free_initmem_default(0);
	free_alternatives_memory();
}

#ifdef CONFIG_BLK_DEV_INITRD

static int keep_initrd;

void free_initrd_mem(unsigned long start, unsigned long end)
{
	if (!keep_initrd)
		free_reserved_area((void *)start, (void *)end, 0, "initrd");
}

static int __init keepinitrd_setup(char *__unused)
{
	keep_initrd = 1;
	return 1;
}

__setup("keepinitrd", keepinitrd_setup);
#endif
