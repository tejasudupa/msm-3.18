/*
 * Contiguous Memory Allocator for DMA mapping framework
 * Copyright (c) 2010-2011 by Samsung Electronics.
 * Written by:
 *	Marek Szyprowski <m.szyprowski@samsung.com>
 *	Michal Nazarewicz <mina86@mina86.com>
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

#include <asm/page.h>
#include <asm/dma-contiguous.h>

#include <linux/bootmem.h>
#include <linux/memblock.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_platform.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/page-isolation.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/mm_types.h>
#include <linux/dma-contiguous.h>
#include <linux/dma-removed.h>
#include <linux/delay.h>
#include <trace/events/kmem.h>
#include <linux/memblock.h>
#include <linux/dma-contiguous.h>
#include <linux/cma.h>

struct cma {
	unsigned long	base_pfn;
	unsigned long	count;
	unsigned long	*bitmap;
	bool in_system;
	bool fixup;
	struct mutex lock;
};
struct cma *dma_contiguous_def_area;
phys_addr_t dma_contiguous_def_base;

#ifdef CONFIG_CMA_SIZE_MBYTES
#define CMA_SIZE_MBYTES CONFIG_CMA_SIZE_MBYTES
#else
#define CMA_SIZE_MBYTES 0
#endif

#ifdef CONFIG_CMA_RESERVE_DEFAULT_AREA
#define CMA_RESERVE_AREA 1
#else
#define CMA_RESERVE_AREA 0
#endif
static struct cma_area {
	phys_addr_t base;
	unsigned long size;
	struct cma *cma;
	const char *name;
	bool to_system;
	bool fixup;
	unsigned long alignment;
	unsigned long limit;
} cma_areas[MAX_CMA_AREAS];
static unsigned cma_area_count;

static struct cma_map {
	phys_addr_t base;
	struct device *dev;
} cma_maps[MAX_CMA_AREAS] __initdata;
static unsigned cma_map_count __initdata;
static bool allow_memblock_alloc __initdata;

static struct cma *cma_get_area(phys_addr_t base)
{
	int i;
	for (i = 0; i < cma_area_count; i++)
		if (cma_areas[i].base == base)
			return cma_areas[i].cma;
	return NULL;
}

static struct cma *cma_get_area_by_name(const char *name)
{
	int i;
	if (!name)
		return NULL;

	for (i = 0; i < cma_area_count; i++)
		if (cma_areas[i].name && strcmp(cma_areas[i].name, name) == 0)
			return cma_areas[i].cma;
	return NULL;
}

static __init int cma_activate_area(unsigned long base_pfn, unsigned long count)
{
	unsigned long pfn = base_pfn;
	unsigned i = count >> pageblock_order;
	struct zone *zone;

	WARN_ON_ONCE(!pfn_valid(pfn));
	zone = page_zone(pfn_to_page(pfn));

	do {
		unsigned j;
		base_pfn = pfn;
		for (j = pageblock_nr_pages; j; --j, pfn++) {
			WARN_ON_ONCE(!pfn_valid(pfn));
			if (page_zone(pfn_to_page(pfn)) != zone)
				return -EINVAL;
		}
		init_cma_reserved_pageblock(pfn_to_page(base_pfn));
	} while (--i);
	return 0;
}

static __init struct cma *cma_create_area(unsigned long base_pfn,
				     unsigned long count, bool system,
				     bool fixup)
						 {
						 	int bitmap_size = BITS_TO_LONGS(count) * sizeof(long);
						 	struct cma *cma;
						 	int ret = -ENOMEM;

						 	pr_debug("%s(base %08lx, count %lx)\n", __func__, base_pfn, count);

						 	cma = kmalloc(sizeof *cma, GFP_KERNEL);
						 	if (!cma)
						 		return ERR_PTR(-ENOMEM);

						 	cma->base_pfn = base_pfn;
						 	cma->count = count;
						 	cma->in_system = system;
						 	cma->fixup = fixup;
						 	cma->bitmap = kzalloc(bitmap_size, GFP_KERNEL);

						 	if (!cma->bitmap)
						 		goto no_mem;

						 	if (cma->in_system) {
						 		ret = cma_activate_area(base_pfn, count);
						 		if (ret)
						 			goto error;
						 	}
						 	mutex_init(&cma->lock);

						 	pr_debug("%s: returned %p\n", __func__, (void *)cma);
						 	return cma;

						 error:
						 	kfree(cma->bitmap);
						 no_mem:
						 	kfree(cma);
						 	return ERR_PTR(ret);
}

/*
 * Default global CMA area size can be defined in kernel's .config.
 * This is useful mainly for distro maintainers to create a kernel
 * that works correctly for most supported systems.
 * The size can be set in bytes or as a percentage of the total memory
 * in the system.
 *
 * Users, who want to set the size of global CMA area for their system
 * should use cma= kernel parameter.
 */
static const phys_addr_t size_bytes = CMA_SIZE_MBYTES * SZ_1M;
static phys_addr_t size_cmdline = -1;
static phys_addr_t base_cmdline;
static phys_addr_t limit_cmdline;

static int __init early_cma(char *p)
{
	pr_debug("%s(%s)\n", __func__, p);
	size_cmdline = memparse(p, &p);
	if (*p != '@')
		return 0;
	base_cmdline = memparse(p + 1, &p);
	if (*p != '-') {
		limit_cmdline = base_cmdline + size_cmdline;
		return 0;
	}
	limit_cmdline = memparse(p + 1, &p);

	return 0;
}
early_param("cma", early_cma);

#ifdef CONFIG_CMA_SIZE_PERCENTAGE

static phys_addr_t __init __maybe_unused cma_early_percent_memory(void)
{
	struct memblock_region *reg;
	unsigned long total_pages = 0;

	/*
	 * We cannot use memblock_phys_mem_size() here, because
	 * memblock_analyze() has not been called yet.
	 */
	for_each_memblock(memory, reg)
		total_pages += memblock_region_memory_end_pfn(reg) -
			       memblock_region_memory_base_pfn(reg);

	return (total_pages * CONFIG_CMA_SIZE_PERCENTAGE / 100) << PAGE_SHIFT;
}

#else

static inline __maybe_unused phys_addr_t cma_early_percent_memory(void)
{
	return 0;
}

#endif
#ifdef CONFIG_OF
int __init cma_fdt_scan(unsigned long node, const char *uname,
				int depth, void *data)
{
	phys_addr_t base, size;
	int len;
	const __be32 *prop;
	const char *name;
	bool in_system;
	bool remove, fixup;
	unsigned long size_cells = dt_root_size_cells;
	unsigned long addr_cells = dt_root_addr_cells;
	phys_addr_t limit = MEMBLOCK_ALLOC_ANYWHERE;
	const char *status;

	if (!of_get_flat_dt_prop(node, "linux,reserve-contiguous-region", NULL))
		return 0;

	status = of_get_flat_dt_prop(node, "status", NULL);
	/*
	 * Yes, we actually want strncmp here to check for a prefix
	 * ok vs. okay
	 */
	if (status && (strncmp(status, "ok", 2) != 0))
		return 0;

	prop = of_get_flat_dt_prop(node, "#size-cells", NULL);
	if (prop)
		size_cells = be32_to_cpup(prop);

	prop = of_get_flat_dt_prop(node, "#address-cells", NULL);
	if (prop)
		addr_cells = be32_to_cpup(prop);

	prop = of_get_flat_dt_prop(node, "reg", &len);
	if (!prop || depth != 2)
		return 0;

	base = dt_mem_next_cell(addr_cells, &prop);
	size = dt_mem_next_cell(size_cells, &prop);

	name = of_get_flat_dt_prop(node, "label", NULL);
	in_system =
		of_get_flat_dt_prop(node, "linux,reserve-region", NULL) ? 0 : 1;
	fixup = of_get_flat_dt_prop(node, "linux,fixup-reserve-region",
			NULL) ? 1 : 0;

	prop = of_get_flat_dt_prop(node, "linux,memory-limit", NULL);
	if (prop)
		limit = be32_to_cpu(prop[0]);

	remove =
	     of_get_flat_dt_prop(node, "linux,remove-completely", NULL) ? 1 : 0;

	/* fixup demands base and size to be PMD_SIZE aligned, if it doesn't
	 * then reduce fixup to remove
	 */
	if (fixup) {
		if (!IS_ALIGNED(base, PMD_SIZE) ||
				!IS_ALIGNED(size, PMD_SIZE)) {
			pr_info("fixup: fallback to remove region as base=%pa or size=%pa not aligned\n",
					&base, &size);
			fixup = 0;
			remove = 1;
		}
	}
	pr_info("Found %s, memory base %pa, size %ld MiB, limit %pa\n", uname,
			&base, (unsigned long)size / SZ_1M, &limit);
	dma_contiguous_reserve_area(size, &base, limit, name,
					in_system, remove, fixup);

	return 0;
}
#endif

int __init __dma_contiguous_reserve_memory(size_t size, size_t alignment,
					size_t limit, phys_addr_t *base)
{	phys_addr_t addr;

	if (!allow_memblock_alloc) {
		*base = 0;
		return 0;
	}

	addr = __memblock_alloc_base(size, alignment, limit);
	if (!addr) {
		return -ENOMEM;
	} else {
		*base = addr;
		return 0;
	}
}
/**
 * dma_contiguous_reserve() - reserve area(s) for contiguous memory handling
 * @limit: End address of the reserved memory (optional, 0 for any).
 *
 * This function reserves memory from early allocator. It should be
 * called by arch specific code once the early allocator (memblock or bootmem)
 * has been activated and all other subsystems have already allocated/reserved
 * memory.
 */
 void __init dma_contiguous_reserve(phys_addr_t limit)
 {
 	phys_addr_t sel_size = 0;
 	int i;

 	of_scan_flat_dt(cma_fdt_scan, NULL);

 	pr_debug("%s(limit %pa)\n", __func__, &limit);

 	if (size_cmdline != -1) {
 		sel_size = size_cmdline;
 	} else {
 #ifdef CONFIG_CMA_SIZE_SEL_MBYTES
 		sel_size = size_bytes;
 #elif defined(CONFIG_CMA_SIZE_SEL_PERCENTAGE)
 		sel_size = cma_early_percent_memory();
 #elif defined(CONFIG_CMA_SIZE_SEL_MIN)
 		sel_size = min(size_bytes, cma_early_percent_memory());
 #elif defined(CONFIG_CMA_SIZE_SEL_MAX)
 		sel_size = max(size_bytes, cma_early_percent_memory());
 #endif
 	}

 	allow_memblock_alloc = true;

 	for (i = 0; i < cma_area_count; i++) {
 		if (cma_areas[i].base == 0) {
 			int ret;

 			ret = __dma_contiguous_reserve_memory(
 						cma_areas[i].size,
 						cma_areas[i].alignment,
 						cma_areas[i].limit,
 						&cma_areas[i].base);
 			if (ret) {
 				pr_err("CMA: failed to reserve %ld MiB for %s\n",
 				       (unsigned long)cma_areas[i].size / SZ_1M,
 				       cma_areas[i].name);
 				memmove(&cma_areas[i], &cma_areas[i+1],
 				   (cma_area_count - i)*sizeof(cma_areas[i]));
 				cma_area_count--;
 				i--;
 				continue;
 			}
 			dma_contiguous_early_fixup(cma_areas[i].base,
 							cma_areas[i].size);
 		}

 		pr_info("CMA: reserved %ld MiB at %pa for %s\n",
 			(unsigned long)cma_areas[i].size / SZ_1M,
 			&cma_areas[i].base, cma_areas[i].name);
 	}

 	if (sel_size) {
 		phys_addr_t base = 0;
 		pr_debug("%s: reserving %ld MiB for global area\n", __func__,
 			 (unsigned long)sel_size / SZ_1M);

 		if (dma_contiguous_reserve_area(sel_size, &base, limit, NULL,
 		    CMA_RESERVE_AREA ? 0 : 1, false, false) == 0) {
 			pr_info("CMA: reserved %ld MiB at %pa for default region\n",
 				(unsigned long)sel_size / SZ_1M, &base);
 			dma_contiguous_def_base = base;
 		}
 	}
 };

/**
 * dma_contiguous_reserve_area() - reserve custom contiguous area
 * @size: Size of the reserved area (in bytes),
 * @base: Base address of the reserved area optional, use 0 for any
 * @limit: End address of the reserved memory (optional, 0 for any).
 * @res_cma: Pointer to store the created cma region.
 * @fixed: hint about where to place the reserved area
 *
 * This function reserves memory from early allocator. It should be
 * called by arch specific code once the early allocator (memblock or bootmem)
 * has been activated and all other subsystems have already allocated/reserved
 * memory. This function allows to create custom reserved areas for specific
 * devices.
 *
 * If @fixed is true, reserve contiguous area at exactly @base.  If false,
 * reserve in range from @base to @limit.
 */
 int __init dma_contiguous_reserve_area(phys_addr_t size, phys_addr_t *res_base,
 				       phys_addr_t limit, const char *name,
 				       bool to_system, bool remove, bool fixup)
 {
 	phys_addr_t base = *res_base;
 	phys_addr_t alignment = PAGE_SIZE;
 	int ret = 0;

 	pr_debug("%s(size %lx, base %pa, limit %pa)\n", __func__,
 		 (unsigned long)size, &base,
 		 &limit);

 	/* Sanity checks */
 	if (cma_area_count == ARRAY_SIZE(cma_areas)) {
 		pr_err("Not enough slots for CMA reserved regions!\n");
 		return -ENOSPC;
 	}

 	if (!size)
 		return -EINVAL;

 	/* fixup allowed only if !to_system and !remove */
 	if (fixup && (to_system || remove)) {
 		pr_err("cannot fixup cma region\n");
 		return -EINVAL;
 	}

 	/* Sanitise input arguments */
 	if (fixup)
 		alignment = PMD_SIZE;
 	else if (!remove)
 		alignment = PAGE_SIZE << max(MAX_ORDER - 1, pageblock_order);

 	base = ALIGN(base, alignment);
 	size = ALIGN(size, alignment);
 	limit &= ~(alignment - 1);

 	/* Reserve memory */
 	if (base) {
 		if (memblock_is_region_reserved(base, size) ||
 		    memblock_reserve(base, size) < 0) {
 			ret = -EBUSY;
 			goto err;
 		}
 	} else {
 		ret = __dma_contiguous_reserve_memory(size, alignment, limit,
 							&base);
 		if (ret)
 			goto err;
 	}

 	if (base && remove) {
 		if (!to_system) {
 			memblock_free(base, size);
 			memblock_remove(base, size);
 		} else {
 			WARN(1, "Removing is incompatible with staying in the system\n");
 		}
 	}

 	/*
 	 * Each reserved area must be initialised later, when more kernel
 	 * subsystems (like slab allocator) are available.
 	 */
 	cma_areas[cma_area_count].base = base;
 	cma_areas[cma_area_count].size = size;
 	cma_areas[cma_area_count].name = name;
 	cma_areas[cma_area_count].alignment = alignment;
 	cma_areas[cma_area_count].limit = limit;
 	cma_areas[cma_area_count].to_system = to_system;
 	cma_areas[cma_area_count].fixup = fixup;
 	cma_area_count++;
 	*res_base = base;


 	/* Architecture specific contiguous memory fixup. */
 	if (!remove && base)
 		dma_contiguous_early_fixup(base, size);
 	return 0;
 err:
 	pr_err("CMA: failed to reserve %ld MiB\n", (unsigned long)size / SZ_1M);
 	return ret;
 }

/**
 * dma_alloc_from_contiguous() - allocate pages from contiguous area
 * @dev:   Pointer to device for which the allocation is performed.
 * @count: Requested number of pages.
 * @align: Requested alignment of pages (in PAGE_SIZE order).
 *
 * This function allocates memory buffer for specified device. It uses
 * device specific contiguous memory area if available or the default
 * global one. Requires architecture specific dev_get_cma_area() helper
 * function.
 */
struct page *dma_alloc_from_contiguous(struct device *dev, size_t count,
				       unsigned int align)
{
	if (align > CONFIG_CMA_ALIGNMENT)
		align = CONFIG_CMA_ALIGNMENT;

	return cma_alloc(dev_get_cma_area(dev), count, align);
}
/**
 * dma_contiguous_add_device() - add device to custom contiguous reserved area
 * @dev:   Pointer to device structure.
 * @base: Pointer to the base address of the reserved area returned by
 *        dma_contiguous_reserve_area() function, also used to return
 *
 * This function assigns the given device to the contiguous memory area
 * reserved earlier by dma_contiguous_reserve_area() function.
 */
int __init dma_contiguous_add_device(struct device *dev, phys_addr_t base)
{
	if (cma_map_count == ARRAY_SIZE(cma_maps)) {
		pr_err("Not enough slots for CMA reserved regions!\n");
		return -ENOSPC;
	}
	cma_maps[cma_map_count].dev = dev;
	cma_maps[cma_map_count].base = base;
	cma_map_count++;
	return 0;
}

static void cma_assign_device_from_dt(struct device *dev)
{
	struct device_node *node;
	struct cma *cma;
	const char *name;
	u32 value;

	node = of_parse_phandle(dev->of_node, "linux,contiguous-region", 0);
	if (!node)
		return;
	if (of_property_read_u32(node, "reg", &value) && !value)
		return;

	if (of_property_read_string(node, "label", &name))
		return;

	cma = cma_get_area_by_name(name);
	if (!cma)
		return;

	dev_set_cma_area(dev, cma);

	if (of_property_read_bool(node, "linux,remove-completely"))
		set_dma_ops(dev, &removed_dma_ops);

	/* see if fixup was over-ruled and we fallback to removed region
	 * because of non aligned base/size
	 */
	if (of_property_read_bool(node, "linux,fixup-reserve-region") &&
			!cma->fixup)
		set_dma_ops(dev, &removed_dma_ops);

	pr_info("Assigned CMA region at %lx to %s device\n", (unsigned long)value, dev_name(dev));
}
static int cma_device_init_notifier_call(struct notifier_block *nb,
					 unsigned long event, void *data)
{
	struct device *dev = data;
	if (event == BUS_NOTIFY_ADD_DEVICE && dev->of_node)
		cma_assign_device_from_dt(dev);
	return NOTIFY_DONE;
}

static struct notifier_block cma_dev_init_nb = {
	.notifier_call = cma_device_init_notifier_call,
};
static int __init cma_init_reserved_areas(void)
{
	struct cma *cma;
	int i;

	for (i = 0; i < cma_area_count; i++) {
		phys_addr_t base = PFN_DOWN(cma_areas[i].base);
		unsigned int count = cma_areas[i].size >> PAGE_SHIFT;
		bool system = cma_areas[i].to_system;
		bool fixup = cma_areas[i].fixup;

		cma = cma_create_area(base, count, system, fixup);
		if (!IS_ERR(cma))
			cma_areas[i].cma = cma;
	}

	dma_contiguous_def_area = cma_get_area(dma_contiguous_def_base);

	for (i = 0; i < cma_map_count; i++) {
		cma = cma_get_area(cma_maps[i].base);
		dev_set_cma_area(cma_maps[i].dev, cma);
	}

	bus_register_notifier(&platform_bus_type, &cma_dev_init_nb);
	return 0;
}
core_initcall(cma_init_reserved_areas);
/**
 * dma_release_from_contiguous() - release allocated pages
 * @dev:   Pointer to device for which the pages were allocated.
 * @pages: Allocated pages.
 * @count: Number of allocated pages.
 *
 * This function releases memory allocated by dma_alloc_from_contiguous().
 * It returns false when provided pages do not belong to contiguous area and
 * true otherwise.
 */
bool dma_release_from_contiguous(struct device *dev, struct page *pages,
				 int count)
{
	return cma_release(dev_get_cma_area(dev), pages, count);
}

/*
 * Support for reserved memory regions defined in device tree
 */
#ifdef CONFIG_OF_RESERVED_MEM
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>

#undef pr_fmt
#define pr_fmt(fmt) fmt

static int rmem_cma_device_init(struct reserved_mem *rmem, struct device *dev)
{
	dev_set_cma_area(dev, rmem->priv);
	return 0;
}

static void rmem_cma_device_release(struct reserved_mem *rmem,
				    struct device *dev)
{
	dev_set_cma_area(dev, NULL);
}

static const struct reserved_mem_ops rmem_cma_ops = {
	.device_init	= rmem_cma_device_init,
	.device_release = rmem_cma_device_release,
};

static int __init rmem_cma_setup(struct reserved_mem *rmem)
{
	phys_addr_t align = PAGE_SIZE << max(MAX_ORDER - 1, pageblock_order);
	phys_addr_t mask = align - 1;
	unsigned long node = rmem->fdt_node;
	struct cma *cma;
	int err;

	if (!of_get_flat_dt_prop(node, "reusable", NULL) ||
	    of_get_flat_dt_prop(node, "no-map", NULL))
		return -EINVAL;

	if ((rmem->base & mask) || (rmem->size & mask)) {
		pr_err("Reserved memory: incorrect alignment of CMA region\n");
		return -EINVAL;
	}

	err = cma_init_reserved_mem(rmem->base, rmem->size, 0, &cma);
	if (err) {
		pr_err("Reserved memory: unable to setup CMA region\n");
		return err;
	}
	/* Architecture specific contiguous memory fixup. */
	dma_contiguous_early_fixup(rmem->base, rmem->size);

	//if (of_get_flat_dt_prop(node, "linux,cma-default", NULL))
	//	dma_contiguous_set_default(cma);

	rmem->ops = &rmem_cma_ops;
	rmem->priv = cma;

	pr_info("Reserved memory: created CMA memory pool at %pa, size %ld MiB\n",
		&rmem->base, (unsigned long)rmem->size / SZ_1M);

	return 0;
}
RESERVEDMEM_OF_DECLARE(cma, "shared-dma-pool", rmem_cma_setup);
#endif
