#ifndef __LINUX_GFP_H
#define __LINUX_GFP_H

#include <linux/mmzone.h>
#include <linux/stddef.h>
#include <linux/linkage.h>

struct vm_area_struct;

/*
 * GFP bitmasks..
 *
 * Zone modifiers (see linux/mmzone.h - low three bits)
 *
 * Do not put any conditional on these. If necessary modify the definitions
 * without the underscores and use the consistently. The definitions here may
 * be used in bit comparisons.
 */
/*没有__GFP_NORMAL这样的掩码,当没有指定__GFP_HIGHMEM及__GFP_DMA时,就相当于__GFP_NORMAL*/
/* 在ZONE_DMA标识的内存区域中查找空闲页 */
#define __GFP_DMA	((__force gfp_t)0x01u)
/* 在ZONE_HIGHMEM标识的内存区域中查找空闲页 */
#define __GFP_HIGHMEM	((__force gfp_t)0x02u)
/* 在ZONE_DMA32标识的内存区域中查找空闲页 */
#define __GFP_DMA32	((__force gfp_t)0x04u)

/*
 * Action modifiers - doesn't change the zoning
 *
 * __GFP_REPEAT: Try hard to allocate the memory, but the allocation attempt
 * _might_ fail.  This depends upon the particular VM implementation.
 *
 * __GFP_NOFAIL: The VM implementation _must_ retry infinitely: the caller
 * cannot handle allocation failures.
 *
 * __GFP_NORETRY: The VM implementation must not retry indefinitely.
 *
 * __GFP_MOVABLE: Flag that this page will be movable by the page migration
 * mechanism or reclaimed
 */
/* 当前正在向内核申请页分配的进程可以被阻塞,意味着调度器可以在此请求期间调度另外一个进程执行 */
#define __GFP_WAIT	((__force gfp_t)0x10u)	/* Can wait and reschedule? */
/* 内核允许使用紧急分配链表中的保留内存页,该请求必须以原子方式完成,意味着请求过程不允许被中断 */
#define __GFP_HIGH	((__force gfp_t)0x20u)	/* Should access emergency pools? */
/* 内核在查找空闲页的过程中可以进行io操作,如此内核可以将换出的页写到硬盘 */
#define __GFP_IO	((__force gfp_t)0x40u)	/* Can start physical IO? */
/* 查找空闲页的过程中允许执行文件系统相关操作 */
#define __GFP_FS	((__force gfp_t)0x80u)	/* Can call down to low-level FS? */
/* 从非缓存的冷页中分配 */
#define __GFP_COLD	((__force gfp_t)0x100u)	/* Cache-cold page required */
/* 禁止分配失败时的警告 */
#define __GFP_NOWARN	((__force gfp_t)0x200u)	/* Suppress page allocation failure warning */
/* 如果分配行为失败,可以自动尝试再次分配.尝试若干次后会终止 */
#define __GFP_REPEAT	((__force gfp_t)0x400u)	/* See above */
/* 分配失败后一直重试,直到分配成功为止,分配函数的调用者无法处理分配失败的情形 */
#define __GFP_NOFAIL	((__force gfp_t)0x800u)	/* See above */
/* 如果分配失败,不会进行重试操作 */
#define __GFP_NORETRY	((__force gfp_t)0x1000u)/* See above */
/* 增加复合页元素 */
#define __GFP_COMP	((__force gfp_t)0x4000u)/* Add compound page metadata */
/* 用0填充成功分配出来的物理页 */
#define __GFP_ZERO	((__force gfp_t)0x8000u)/* Return zeroed page on success */
/* 不要使用仅限紧急分配使用的保留分配链表 */
#define __GFP_NOMEMALLOC ((__force gfp_t)0x10000u) /* Don't use emergency reserves */
/* 只能在当前进程允许运行的各个CPU所关联的节点分配内存.该标志只有在NUMA系统上才有意义 */
#define __GFP_HARDWALL   ((__force gfp_t)0x20000u) /* Enforce hardwall cpuset memory allocs */
#define __GFP_THISNODE	((__force gfp_t)0x40000u)/* No fallback, no policies */
#define __GFP_RECLAIMABLE ((__force gfp_t)0x80000u) /* Page is reclaimable */
/* 将分配的物理页标记为可移动的 */
#define __GFP_MOVABLE	((__force gfp_t)0x100000u)  /* Page is movable */

/* 用于hight == 0, radix tree root中直接保存数据的情况 */
#define __GFP_BITS_SHIFT 21	/* Room for 21 __GFP_FOO bits */
#define __GFP_BITS_MASK ((__force gfp_t)((1 << __GFP_BITS_SHIFT) - 1))

/* This equals 0, but use constants in case they ever change */
#define GFP_NOWAIT	(GFP_ATOMIC & ~__GFP_HIGH)
/* GFP_ATOMIC means both !wait (__GFP_WAIT not set) and use emergency pool */
/* 用于原子分配,也是下面几个掩码中唯一不带__GFP_WAIT的 */
#define GFP_ATOMIC	(__GFP_HIGH)
/* 禁止io操作,是下面几个掩码中唯一不带__GFP_IO的 */
#define GFP_NOIO	(__GFP_WAIT)
/* 禁止文件系统相关调用 */
#define GFP_NOFS	(__GFP_WAIT | __GFP_IO)
#define GFP_KERNEL	(__GFP_WAIT | __GFP_IO | __GFP_FS)
#define GFP_TEMPORARY	(__GFP_WAIT | __GFP_IO | __GFP_FS | \
			 __GFP_RECLAIMABLE)
/* 为用户空间分配内存页 */
#define GFP_USER	(__GFP_WAIT | __GFP_IO | __GFP_FS | __GFP_HARDWALL)
/* 对GFP_USER的一个扩展,可以使用非线性映射的高端内存 */
#define GFP_HIGHUSER	(__GFP_WAIT | __GFP_IO | __GFP_FS | __GFP_HARDWALL | \
			 __GFP_HIGHMEM)
/* 表示高端内存区域的可移动页 */
#define GFP_HIGHUSER_MOVABLE	(__GFP_WAIT | __GFP_IO | __GFP_FS | \
				 __GFP_HARDWALL | __GFP_HIGHMEM | \
				 __GFP_MOVABLE)
#define GFP_NOFS_PAGECACHE	(__GFP_WAIT | __GFP_IO | __GFP_MOVABLE)
#define GFP_USER_PAGECACHE	(__GFP_WAIT | __GFP_IO | __GFP_FS | \
				 __GFP_HARDWALL | __GFP_MOVABLE)
#define GFP_HIGHUSER_PAGECACHE	(__GFP_WAIT | __GFP_IO | __GFP_FS | \
				 __GFP_HARDWALL | __GFP_HIGHMEM | \
				 __GFP_MOVABLE)

#define GFP_THISNODE	((__force gfp_t)0)

/* This mask makes up all the page movable related flags */
#define GFP_MOVABLE_MASK (__GFP_RECLAIMABLE|__GFP_MOVABLE)

/* Control page allocator reclaim behavior */
#define GFP_RECLAIM_MASK (__GFP_WAIT|__GFP_HIGH|__GFP_IO|__GFP_FS|\
			__GFP_NOWARN|__GFP_REPEAT|__GFP_NOFAIL|\
			__GFP_NORETRY|__GFP_NOMEMALLOC)

/* Control allocation constraints */
#define GFP_CONSTRAINT_MASK (__GFP_HARDWALL|__GFP_THISNODE)

/* Do not use these with a slab allocator */
#define GFP_SLAB_BUG_MASK (__GFP_DMA32|__GFP_HIGHMEM|~__GFP_BITS_MASK)

/* Flag - indicates that the buffer will be suitable for DMA.  Ignored on some
   platforms, used as appropriate on others */

//限制页面分配器只能在ZONE_DMA域中分配空闲物理页面,用于分配适用于DMA缓冲区的内存
#define GFP_DMA		__GFP_DMA

/* 4GB DMA on some platforms */
#define GFP_DMA32	__GFP_DMA32

/* Convert GFP flags to their corresponding migrate type */
/* 将分配时用到的flag(GFP_)转换成migrate type */
static inline int allocflags_to_migratetype(gfp_t gfp_flags)
{
	WARN_ON((gfp_flags & GFP_MOVABLE_MASK) == GFP_MOVABLE_MASK);

	if (unlikely(page_group_by_mobility_disabled))
		return MIGRATE_UNMOVABLE;

	/* Group based on mobility */
	//返回free_area.free_list数组索引
	return (((gfp_flags & __GFP_MOVABLE) != 0) << 1) |
		((gfp_flags & __GFP_RECLAIMABLE) != 0);
}

static inline enum zone_type gfp_zone(gfp_t flags)
{
#ifdef CONFIG_ZONE_DMA
	if (flags & __GFP_DMA)
		return ZONE_DMA;
#endif
	if ((flags & (__GFP_HIGHMEM | __GFP_MOVABLE)) ==
			(__GFP_HIGHMEM | __GFP_MOVABLE))
		return ZONE_MOVABLE;
#ifdef CONFIG_HIGHMEM
	if (flags & __GFP_HIGHMEM)
		return ZONE_HIGHMEM;
#endif
	/* 既不在ZONE_DMA也不在ZONE_HIGHMEM就返回ZONE_NORMAL */
	return ZONE_NORMAL;
}

/*
 * There is only one page-allocator function, and two main namespaces to
 * it. The alloc_page*() variants return 'struct page *' and as such
 * can allocate highmem pages, the *get*page*() variants return
 * virtual kernel addresses to the allocated page(s).
 */

static inline int gfp_zonelist(gfp_t flags)
{
	if (NUMA_BUILD && unlikely(flags & __GFP_THISNODE))
		return 1;

	return 0;
}

/*
 * We get the zone list from the current node and the gfp_mask.
 * This zone list contains a maximum of MAXNODES*MAX_NR_ZONES zones.
 * There are two zonelists per node, one for all zones with memory and
 * one containing just zones from the node the zonelist belongs to.
 *
 * For the normal case of non-DISCONTIGMEM systems the NODE_DATA() gets
 * optimized to &contig_page_data at compile-time.
 */
static inline struct zonelist *node_zonelist(int nid, gfp_t flags)
{
	return NODE_DATA(nid)->node_zonelists + gfp_zonelist(flags);
}

#ifndef HAVE_ARCH_FREE_PAGE
static inline void arch_free_page(struct page *page, int order) { }
#endif
#ifndef HAVE_ARCH_ALLOC_PAGE
static inline void arch_alloc_page(struct page *page, int order) { }
#endif

struct page *
__alloc_pages_internal(gfp_t gfp_mask, unsigned int order,
		       struct zonelist *zonelist, nodemask_t *nodemask);

/* 分配2的order次方个连续的物理页面并返回起始页的struct page * */
static inline struct page *
__alloc_pages(gfp_t gfp_mask, unsigned int order,
		struct zonelist *zonelist)
{
	return __alloc_pages_internal(gfp_mask, order, zonelist, NULL);
}

static inline struct page *
__alloc_pages_nodemask(gfp_t gfp_mask, unsigned int order,
		struct zonelist *zonelist, nodemask_t *nodemask)
{
	return __alloc_pages_internal(gfp_mask, order, zonelist, nodemask);
}


static inline struct page *alloc_pages_node(int nid, gfp_t gfp_mask,
						unsigned int order)
{
	if (unlikely(order >= MAX_ORDER))
		return NULL;

	/* Unknown node is current node */
	if (nid < 0)
		nid = numa_node_id();

	return __alloc_pages(gfp_mask, order, node_zonelist(nid, gfp_mask));
}

#define alloc_pages(gfp_mask, order) \
		alloc_pages_node(numa_node_id(), gfp_mask, order)
#define alloc_page_vma(gfp_mask, vma, addr) alloc_pages(gfp_mask, 0)
#define alloc_page(gfp_mask) alloc_pages(gfp_mask, 0)

extern unsigned long __get_free_pages(gfp_t gfp_mask, unsigned int order);
extern unsigned long get_zeroed_page(gfp_t gfp_mask);

void *alloc_pages_exact(size_t size, gfp_t gfp_mask);
void free_pages_exact(void *virt, size_t size);

#define __get_free_page(gfp_mask) \
		__get_free_pages((gfp_mask),0)

#define __get_dma_pages(gfp_mask, order) \
		__get_free_pages((gfp_mask) | GFP_DMA,(order))

extern void __free_pages(struct page *page, unsigned int order);
extern void free_pages(unsigned long addr, unsigned int order);
extern void free_hot_page(struct page *page);
extern void free_cold_page(struct page *page);

#define __free_page(page) __free_pages((page), 0)
#define free_page(addr) free_pages((addr),0)

void page_alloc_init(void);
void drain_zone_pages(struct zone *zone, struct per_cpu_pages *pcp);
void drain_all_pages(void);
void drain_local_pages(void *dummy);

#endif /* __LINUX_GFP_H */
