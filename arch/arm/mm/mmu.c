/*
 *  linux/arch/arm/mm/mmu.c
 *
 *  Copyright (C) 1995-2005 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/bootmem.h>
#include <linux/mman.h>
#include <linux/nodemask.h>

#include <asm/cputype.h>
#include <asm/mach-types.h>
#include <asm/setup.h>
#include <asm/sizes.h>
#include <asm/tlb.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#include "mm.h"

DEFINE_PER_CPU(struct mmu_gather, mmu_gathers);

/*
 * empty_zero_page is a special page that is used for
 * zero-initialized data and COW.
 */
struct page *empty_zero_page;
EXPORT_SYMBOL(empty_zero_page);

/*
 * The pmd table for the upper-most set of pages.
 */
pmd_t *top_pmd;

//不使用缓存
#define CPOLICY_UNCACHED	0
//向缓存和内存之间使用写缓冲
#define CPOLICY_BUFFERED	1
//缓存中发生写事件,直接反映到内存和内存下层.主要用于usb等经常插拔的存储设备
#define CPOLICY_WRITETHROUGH	2
//优先使用缓存,在缓存替换策略作用下,当数据需要从缓存区域脱离时反映到内存
#define CPOLICY_WRITEBACK	3
/*
 * 发送缓存失败时,缓存控制器分配cache line的方法有两种:
 * 1.read-allocate方式:从内存读取数据时分配cache line
 * 2.write-allocate方式:向内存写入数据时分配cache line
 */
#define CPOLICY_WRITEALLOC	4

static unsigned int cachepolicy __initdata = CPOLICY_WRITEBACK;
static unsigned int ecc_mask __initdata = 0;
pgprot_t pgprot_user;
pgprot_t pgprot_kernel;

EXPORT_SYMBOL(pgprot_user);
EXPORT_SYMBOL(pgprot_kernel);

struct cachepolicy {
	const char	policy[16];
	unsigned int	cr_mask;
	unsigned int	pmd;
	unsigned int	pte;
};

static struct cachepolicy cache_policies[] __initdata = {
	{
		.policy		= "uncached",
		.cr_mask	= CR_W|CR_C,
		.pmd		= PMD_SECT_UNCACHED,
		.pte		= L_PTE_MT_UNCACHED,
	}, {
		.policy		= "buffered",
		.cr_mask	= CR_C,
		.pmd		= PMD_SECT_BUFFERED,
		.pte		= L_PTE_MT_BUFFERABLE,
	}, {
		.policy		= "writethrough",
		.cr_mask	= 0,
		.pmd		= PMD_SECT_WT,
		.pte		= L_PTE_MT_WRITETHROUGH,
	}, {
		.policy		= "writeback",
		.cr_mask	= 0,
		.pmd		= PMD_SECT_WB,
		.pte		= L_PTE_MT_WRITEBACK,
	}, {
		.policy		= "writealloc",
		.cr_mask	= 0,
		.pmd		= PMD_SECT_WBWA,
		.pte		= L_PTE_MT_WRITEALLOC,
	}
};

/*
 * These are useful for identifying cache coherency
 * problems by allowing the cache or the cache and
 * writebuffer to be turned off.  (Note: the write
 * buffer should not be on and the cache off).
 */
static void __init early_cachepolicy(char **p)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(cache_policies); i++) {
		int len = strlen(cache_policies[i].policy);

		if (memcmp(*p, cache_policies[i].policy, len) == 0) {
			cachepolicy = i;
			cr_alignment &= ~cache_policies[i].cr_mask;
			cr_no_alignment &= ~cache_policies[i].cr_mask;
			*p += len;
			break;
		}
	}
	if (i == ARRAY_SIZE(cache_policies))
		printk(KERN_ERR "ERROR: unknown or unsupported cache policy\n");
	if (cpu_architecture() >= CPU_ARCH_ARMv6) {
		printk(KERN_WARNING "Only cachepolicy=writeback supported on ARMv6 and later\n");
		cachepolicy = CPOLICY_WRITEBACK;
	}
	flush_cache_all();
	set_cr(cr_alignment);
}
__early_param("cachepolicy=", early_cachepolicy);

static void __init early_nocache(char **__unused)
{
	char *p = "buffered";
	printk(KERN_WARNING "nocache is deprecated; use cachepolicy=%s\n", p);
	early_cachepolicy(&p);
}
__early_param("nocache", early_nocache);

static void __init early_nowrite(char **__unused)
{
	char *p = "uncached";
	printk(KERN_WARNING "nowb is deprecated; use cachepolicy=%s\n", p);
	early_cachepolicy(&p);
}
__early_param("nowb", early_nowrite);

static void __init early_ecc(char **p)
{
	if (memcmp(*p, "on", 2) == 0) {
		ecc_mask = PMD_PROTECTION;
		*p += 2;
	} else if (memcmp(*p, "off", 3) == 0) {
		ecc_mask = 0;
		*p += 3;
	}
}
__early_param("ecc=", early_ecc);

static int __init noalign_setup(char *__unused)
{
	cr_alignment &= ~CR_A;
	cr_no_alignment &= ~CR_A;
	set_cr(cr_alignment);
	return 1;
}
__setup("noalign", noalign_setup);

#ifndef CONFIG_SMP
void adjust_cr(unsigned long mask, unsigned long set)
{
	unsigned long flags;

	mask &= ~CR_A;

	set &= mask;

	local_irq_save(flags);

	cr_no_alignment = (cr_no_alignment & ~mask) | set;
	cr_alignment = (cr_alignment & ~mask) | set;

	set_cr((get_cr() & ~mask) | set);

	local_irq_restore(flags);
}
#endif

//对应的page线性地址已经映射到物理内存,可以access也可以write
#define PROT_PTE_DEVICE		L_PTE_PRESENT|L_PTE_YOUNG|L_PTE_DIRTY|L_PTE_WRITE
#define PROT_SECT_DEVICE	PMD_TYPE_SECT|PMD_SECT_AP_WRITE

static struct mem_type mem_types[] = {
	//设备空间,对应其他io设备,应用于ioremap
	[MT_DEVICE] = {		  /* Strongly ordered / ARMv6 shared device */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_SHARED |
				  L_PTE_SHARED,
		.prot_l1	= PMD_TYPE_TABLE,
		//段页表,表示的页面设置share标志
		.prot_sect	= PROT_SECT_DEVICE | PMD_SECT_S,
		.domain		= DOMAIN_IO,
	},
	[MT_DEVICE_NONSHARED] = { /* ARMv6 non-shared device */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_NONSHARED,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PROT_SECT_DEVICE,
		.domain		= DOMAIN_IO,
	},
	[MT_DEVICE_CACHED] = {	  /* ioremap_cached */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_CACHED,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PROT_SECT_DEVICE | PMD_SECT_WB,
		.domain		= DOMAIN_IO,
	},	
	[MT_DEVICE_WC] = {	/* ioremap_wc */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_WC,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PROT_SECT_DEVICE,
		.domain		= DOMAIN_IO,
	},
	[MT_UNCACHED] = {
		.prot_pte	= PROT_PTE_DEVICE,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PMD_TYPE_SECT | PMD_SECT_XN,
		.domain		= DOMAIN_IO,
	},
	/* 内部高速SRAM空间 */
	[MT_CACHECLEAN] = {
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_XN,
		.domain    = DOMAIN_KERNEL,
	},
	/* 内部mini cache空间 */
	[MT_MINICLEAN] = {
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_XN | PMD_SECT_MINICACHE,
		.domain    = DOMAIN_KERNEL,
	},
	//低端中断向量,对应0地址开始的向量
	[MT_LOW_VECTORS] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_EXEC,
		.prot_l1   = PMD_TYPE_TABLE,
		.domain    = DOMAIN_USER,
	},
	//高端中断向量，它有vector_base宏决定
	[MT_HIGH_VECTORS] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_USER | L_PTE_EXEC,
		.prot_l1   = PMD_TYPE_TABLE,
		.domain    = DOMAIN_USER,
	},
	//RAM内存空间,段式映射,有读写权限,domain属于kernel
	[MT_MEMORY] = {
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_AP_WRITE,
		.domain    = DOMAIN_KERNEL,
	},
	/* ROM(flash)空间 */
	[MT_ROM] = {
		.prot_sect = PMD_TYPE_SECT,
		.domain    = DOMAIN_KERNEL,
	},
};

const struct mem_type *get_mem_type(unsigned int type)
{
	return type < ARRAY_SIZE(mem_types) ? &mem_types[type] : NULL;
}

/*
 * Adjust the PMD section entries according to the CPU in use.
 */
//根据不同的arm版本,设置不同mem_type
static void __init build_mem_type_table(void)
{
	struct cachepolicy *cp;
	//cr = 0xc5387f
	unsigned int cr = get_cr();
	unsigned int user_pgprot, kern_pgprot, vecs_pgprot;
	/* cpu_arch ==9 */
	int cpu_arch = cpu_architecture();
	int i;
#ifdef CONFIG_SMP
	cachepolicy = CPOLICY_WRITEALLOC;
#endif

	/*
	 * Mark the device areas according to the CPU/architecture.
	 */
	/* mem_types已经静态初始化,但是根据不同的cpu/architecture需要微调 */
	if (cpu_is_xsc3() || (cpu_arch >= CPU_ARCH_ARMv6 && (cr & CR_XP))) {
		if (!cpu_is_xsc3()) {
			/*
			 * Mark device regions on ARMv6+ as execute-never
			 * to prevent speculative instruction fetches.
			 */
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_XN;
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_XN;
			mem_types[MT_DEVICE_CACHED].prot_sect |= PMD_SECT_XN;
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_XN;
		}
		if (cpu_arch >= CPU_ARCH_ARMv7 && (cr & CR_TRE)) {
			/*
			 * For ARMv7 with TEX remapping,
			 * - shared device is SXCB=1100
			 * - nonshared device is SXCB=0100
			 * - write combine device mem is SXCB=0001
			 * (Uncached Normal memory)
			 */
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_TEX(1);
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_TEX(1);
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_BUFFERABLE;
		} else if (cpu_is_xsc3()) {
			/*
			 * For Xscale3,
			 * - shared device is TEXCB=00101
			 * - nonshared device is TEXCB=01000
			 * - write combine device mem is TEXCB=00100
			 * (Inner/Outer Uncacheable in xsc3 parlance)
			 */
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_TEX(1) | PMD_SECT_BUFFERED;
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_TEX(2);
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_TEX(1);
		} else {
			/*
			 * For ARMv6 and ARMv7 without TEX remapping,
			 * - shared device is TEXCB=00001
			 * - nonshared device is TEXCB=01000
			 * - write combine device mem is TEXCB=00100
			 * (Uncached Normal in ARMv6 parlance).
			 */
			/*
 			 * 对于ARMv6 and ARMv7禁止tex remapping的情况:
 			 * shared device TEXCB=00001
 			 * nonshared device TEXCB=01000
			 */
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_BUFFERED;
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_TEX(2);
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_TEX(1);
		}
	} else {
		/*
		 * On others, write combining is "Uncached/Buffered"
		 */
		mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_BUFFERABLE;
	}

	/*
	 * Now deal with the memory-type mappings
	 */
	//cachepolicy是一个全局变量,初始值是CPOLICY_WRITEBACK
	cp = &cache_policies[cachepolicy];
	vecs_pgprot = kern_pgprot = user_pgprot = cp->pte;

#ifndef CONFIG_SMP
	/*
	 * Only use write-through for non-SMP systems
	 */
	if (cpu_arch >= CPU_ARCH_ARMv5 && cachepolicy > CPOLICY_WRITETHROUGH)
		vecs_pgprot = cache_policies[CPOLICY_WRITETHROUGH].pte;
#endif

	/*
	 * Enable CPU-specific coherency if supported.
	 * (Only available on XSC3 at the moment.)
	 */
	if (arch_is_coherent() && cpu_is_xsc3())
		mem_types[MT_MEMORY].prot_sect |= PMD_SECT_S;

	/*
	 * ARMv6 and above have extended page tables.
	 */
	if (cpu_arch >= CPU_ARCH_ARMv6 && (cr & CR_XP)) {
		/*
		 * Mark cache clean areas and XIP ROM read only
		 * from SVC mode and no access from userspace.
		 */
		//特权模式下只读,用户模式下禁止访问
		mem_types[MT_ROM].prot_sect |= PMD_SECT_APX|PMD_SECT_AP_WRITE;
		mem_types[MT_MINICLEAN].prot_sect |= PMD_SECT_APX|PMD_SECT_AP_WRITE;
		mem_types[MT_CACHECLEAN].prot_sect |= PMD_SECT_APX|PMD_SECT_AP_WRITE;

#ifdef CONFIG_SMP
		/*
		 * Mark memory with the "shared" attribute for SMP systems
		 */
		user_pgprot |= L_PTE_SHARED;
		kern_pgprot |= L_PTE_SHARED;
		vecs_pgprot |= L_PTE_SHARED;
		mem_types[MT_MEMORY].prot_sect |= PMD_SECT_S;
#endif
	}

	for (i = 0; i < 16; i++) {
		unsigned long v = pgprot_val(protection_map[i]);
		//为protection_map中的权限加上L_PTE_MT_WRITEBACK位
		protection_map[i] = __pgprot(v | user_pgprot);
	}

	mem_types[MT_LOW_VECTORS].prot_pte |= vecs_pgprot;
	mem_types[MT_HIGH_VECTORS].prot_pte |= vecs_pgprot;

	pgprot_user   = __pgprot(L_PTE_PRESENT | L_PTE_YOUNG | user_pgprot);
	pgprot_kernel = __pgprot(L_PTE_PRESENT | L_PTE_YOUNG |
				 L_PTE_DIRTY | L_PTE_WRITE |
				 L_PTE_EXEC | kern_pgprot);

	mem_types[MT_LOW_VECTORS].prot_l1 |= ecc_mask;
	mem_types[MT_HIGH_VECTORS].prot_l1 |= ecc_mask;
	mem_types[MT_MEMORY].prot_sect |= ecc_mask | cp->pmd;
	mem_types[MT_ROM].prot_sect |= cp->pmd;

	switch (cp->pmd) {
	case PMD_SECT_WT:
		mem_types[MT_CACHECLEAN].prot_sect |= PMD_SECT_WT;
		break;
	case PMD_SECT_WB:
	case PMD_SECT_WBWA:
		mem_types[MT_CACHECLEAN].prot_sect |= PMD_SECT_WB;
		break;
	}
	printk("Memory policy: ECC %sabled, Data cache %s\n",
		ecc_mask ? "en" : "dis", cp->policy);

	/* 为各种类型的内存构成l1/pte表项 */
	for (i = 0; i < ARRAY_SIZE(mem_types); i++) {
		struct mem_type *t = &mem_types[i];
		if (t->prot_l1)
			t->prot_l1 |= PMD_DOMAIN(t->domain);
		if (t->prot_sect)
			t->prot_sect |= PMD_DOMAIN(t->domain);
	}
}

#define vectors_base()	(vectors_high() ? 0xffff0000 : 0)

static void __init alloc_init_pte(pmd_t *pmd, unsigned long addr,
				  unsigned long end, unsigned long pfn,
				  const struct mem_type *type)
{
	pte_t *pte;

	if (pmd_none(*pmd)) {
		/* *pmd为0,分配1024个pte指针,刚好占用一个page */
		pte = alloc_bootmem_low_pages(2 * PTRS_PER_PTE * sizeof(pte_t));
		/* 根据pte生成pmd */
		__pmd_populate(pmd, __pa(pte) | type->prot_l1);
	}

	pte = pte_offset_kernel(pmd, addr);
	do {
		set_pte_ext(pte, pfn_pte(pfn, __pgprot(type->prot_pte)), 0);
		pfn++;
	} while (pte++, addr += PAGE_SIZE, addr != end);
}

/* 分配[addr,end)这段内存的段页表项 */
static void __init alloc_init_section(pgd_t *pgd, unsigned long addr,
				      unsigned long end, unsigned long phys,
				      const struct mem_type *type)
{
	/* 从pgd中获取pmd,pgd是逻辑的,实际工作由pmd完成 */
	pmd_t *pmd = pmd_offset(pgd, addr);

	/*
	 * Try a section mapping - end, addr and phys must all be aligned
	 * to a section boundary.  Note that PMDs refer to the individual
	 * L1 entries, whereas PGDs refer to a group of L1 entries making
	 * up one logical pointer to an L2 table.
	 */
	if (((addr | end | phys) & ~SECTION_MASK) == 0) {
		/* addr | end | phys以段大小对齐,属于段式映射 */
		pmd_t *p = pmd;

		/* addr是一个pgd的起始地址,必须以一个pgd的大小(2MB)对齐 */
		if (addr & SECTION_SIZE)
			pmd++;

		do {
			/* 写入段页表项,段页表项的是由段基地址和一些控制位组成 */
			*pmd = __pmd(phys | type->prot_sect);
			phys += SECTION_SIZE;
			/* pmd自加后每次偏移4字节 */
		} while (pmd++, addr += SECTION_SIZE, addr != end);

		/* 确保pmd写入内存中,而不是cache */
		flush_pmd_entry(p);
	} else {
		/*
		 * No need to loop; pte's aren't interested in the
		 * individual L1 entries.
		 */
		/* 页式映射 */
		alloc_init_pte(pmd, addr, end, __phys_to_pfn(phys), type);
	}
}

static void __init create_36bit_mapping(struct map_desc *md,
					const struct mem_type *type)
{
	unsigned long phys, addr, length, end;
	pgd_t *pgd;

	addr = md->virtual;
	phys = (unsigned long)__pfn_to_phys(md->pfn);
	length = PAGE_ALIGN(md->length);

	if (!(cpu_architecture() >= CPU_ARCH_ARMv6 || cpu_is_xsc3())) {
		printk(KERN_ERR "MM: CPU does not support supersection "
		       "mapping for 0x%08llx at 0x%08lx\n",
		       __pfn_to_phys((u64)md->pfn), addr);
		return;
	}

	/* N.B.	ARMv6 supersections are only defined to work with domain 0.
	 *	Since domain assignments can in fact be arbitrary, the
	 *	'domain == 0' check below is required to insure that ARMv6
	 *	supersections are only allocated for domain 0 regardless
	 *	of the actual domain assignments in use.
	 */
	if (type->domain) {
		printk(KERN_ERR "MM: invalid domain in supersection "
		       "mapping for 0x%08llx at 0x%08lx\n",
		       __pfn_to_phys((u64)md->pfn), addr);
		return;
	}

	if ((addr | length | __pfn_to_phys(md->pfn)) & ~SUPERSECTION_MASK) {
		printk(KERN_ERR "MM: cannot create mapping for "
		       "0x%08llx at 0x%08lx invalid alignment\n",
		       __pfn_to_phys((u64)md->pfn), addr);
		return;
	}

	/*
	 * Shift bits [35:32] of address into bits [23:20] of PMD
	 * (See ARMv6 spec).
	 */
	phys |= (((md->pfn >> (32 - PAGE_SHIFT)) & 0xF) << 20);

	pgd = pgd_offset_k(addr);
	end = addr + length;
	do {
		pmd_t *pmd = pmd_offset(pgd, addr);
		int i;

		for (i = 0; i < 16; i++)
			*pmd++ = __pmd(phys | type->prot_sect | PMD_SECT_SUPER);

		addr += SUPERSECTION_SIZE;
		phys += SUPERSECTION_SIZE;
		pgd += SUPERSECTION_SIZE >> PGDIR_SHIFT;
	} while (addr != end);
}

/*
 * Create the page directory entries and any necessary
 * page tables for the mapping specified by `md'.  We
 * are able to cope here with varying sizes and address
 * offsets, and we take full advantage of sections and
 * supersections.
 */
void __init create_mapping(struct map_desc *md)
{
	unsigned long phys, addr, length, end;
	const struct mem_type *type;
	pgd_t *pgd;

	/* vectors_base()为0xffff0000 */
	if (md->virtual != vectors_base() && md->virtual < TASK_SIZE) {
		/* 不能为用户空间建立映射 */
		printk(KERN_WARNING "BUG: not creating mapping for "
		       "0x%08llx at 0x%08lx in user region\n",
		       __pfn_to_phys((u64)md->pfn), md->virtual);
		return;
	}

	/* type为MT_DEVICE或者MT_ROM的memory不能映射到[PAGE_OFFSET,VMALLOC_END)这块区域 */
	if ((md->type == MT_DEVICE || md->type == MT_ROM) &&
	    md->virtual >= PAGE_OFFSET && md->virtual < VMALLOC_END) {
		printk(KERN_WARNING "BUG: mapping for 0x%08llx at 0x%08lx "
		       "overlaps vmalloc space\n",
		       __pfn_to_phys((u64)md->pfn), md->virtual);
	}

	type = &mem_types[md->type];

	/*
	 * Catch 36-bit addresses
	 */
	if (md->pfn >= 0x100000) {
		create_36bit_mapping(md, type);
		return;
	}

	/* 虚拟起始地址必须以page size对齐 */
	addr = md->virtual & PAGE_MASK;
	phys = (unsigned long)__pfn_to_phys(md->pfn);
	/* 长度以page对齐 */
	length = PAGE_ALIGN(md->length + (md->virtual & ~PAGE_MASK));

	if (type->prot_l1 == 0 && ((addr | phys | length) & ~SECTION_MASK)) {
		/* type->prot_l1 == 0表示段式映射,addr/phys/length都必须以段大小对齐 */
		printk(KERN_WARNING "BUG: map for 0x%08lx at 0x%08lx can not "
		       "be mapped using pages, ignoring.\n",
		       __pfn_to_phys(md->pfn), addr);
		return;
	}

	pgd = pgd_offset_k(addr);
	end = addr + length;
	/* length可能跨越了多个pgd,所以要对pgd中的表项进行遍历 */
	do {
		/* 以2MB为单位进行循环,取下个pgd指向的虚拟地址 */
		unsigned long next = pgd_addr_end(addr, end);

		/* 每个pgd条目中处理2个pmd */
		alloc_init_section(pgd, addr, next, phys, type);

		phys += next - addr;
		addr = next;
	} while (pgd++, addr != end);
}

/*
 * Create the architecture specific mappings
 */
void __init iotable_init(struct map_desc *io_desc, int nr)
{
	int i;

	for (i = 0; i < nr; i++)
		create_mapping(io_desc + i);
}

static unsigned long __initdata vmalloc_reserve = SZ_128M;

/*
 * vmalloc=size forces the vmalloc area to be exactly 'size'
 * bytes. This can be used to increase (or decrease) the vmalloc
 * area - the default is 128m.
 */
static void __init early_vmalloc(char **arg)
{
	vmalloc_reserve = memparse(*arg, arg);

	if (vmalloc_reserve < SZ_16M) {
		vmalloc_reserve = SZ_16M;
		printk(KERN_WARNING
			"vmalloc area too small, limiting to %luMB\n",
			vmalloc_reserve >> 20);
	}
}
__early_param("vmalloc=", early_vmalloc);

#define VMALLOC_MIN	(void *)(VMALLOC_END - vmalloc_reserve)

//mb->start + mb->size这片区域的虚拟地址是不能覆盖vmalloc区域的
static int __init check_membank_valid(struct membank *mb)
{
	/*
	 * Check whether this memory region has non-zero size or
	 * invalid node number.
	 */
	if (mb->size == 0 || mb->node >= MAX_NUMNODES)
		return 0;

	/*
	 * Check whether this memory region would entirely overlap
	 * the vmalloc area.
	 */
	if (phys_to_virt(mb->start) >= VMALLOC_MIN) {
		printk(KERN_NOTICE "Ignoring RAM at %.8lx-%.8lx "
			"(vmalloc region overlap).\n",
			mb->start, mb->start + mb->size - 1);
		return 0;
	}

	/*
	 * Check whether this memory region would partially overlap
	 * the vmalloc area.
	 */
	if (phys_to_virt(mb->start + mb->size) < phys_to_virt(mb->start) ||
	    phys_to_virt(mb->start + mb->size) > VMALLOC_MIN) {
		unsigned long newsize = VMALLOC_MIN - phys_to_virt(mb->start);

		printk(KERN_NOTICE "Truncating RAM at %.8lx-%.8lx "
			"to -%.8lx (vmalloc region overlap).\n",
			mb->start, mb->start + mb->size - 1,
			mb->start + newsize - 1);
		mb->size = newsize;
	}

	return 1;
}

/* 获取有效的bank数量 */
static void __init sanity_check_meminfo(struct meminfo *mi)
{
	int i, j;

	for (i = 0, j = 0; i < mi->nr_banks; i++) {
		if (check_membank_valid(&mi->bank[i]))
			mi->bank[j++] = mi->bank[i];
	}
	mi->nr_banks = j;
}

/* 
 * 将[0,0xC0000000)及[0xD0000000,0xE0000000)这段内存的段页表清空.
 * [0xC0000000,0xD0000000)这段虚拟内存对应的是DRAM,不做清除.DRAM
 * 的段页表在bootmem_init中设置
 */
static inline void prepare_page_table(struct meminfo *mi)
{
	unsigned long addr;
	int i;

	/*
	 * Clear out all the mappings below the kernel image.
	 */
	/*
	 * 在head.S的__create_page_tables中创建过段页表,其位置在0xc0004000,
	 * 现在我们要清除[0,0xC0000000)这段内存的段页表.
	 */
	for (addr = 0; addr < PAGE_OFFSET; addr += PGDIR_SIZE)
		pmd_clear(pmd_off_k(addr));
	/*
	 * Clear out all the kernel space mappings, except for the first
	 * memory bank, up to the end of the vmalloc region.
	 */
	for (addr = __phys_to_virt(mi->bank[0].start + mi->bank[0].size);
	     addr < VMALLOC_END; addr += PGDIR_SIZE)
		pmd_clear(pmd_off_k(addr));
}

/*
 * Reserve the various regions of node 0
 */
/* 在bitmap中保留node 0的kernel image所在page和段页表所在的page */
void __init reserve_node_zero(pg_data_t *pgdat)
{
	unsigned long res_size = 0;

	/*
	 * Register the kernel text and data with bootmem.
	 * Note that this can only be in node 0.
	 */
	/* 保留kernel镜像所在的page */
	reserve_bootmem_node(pgdat, __pa(&_stext), &_end - &_stext,
			BOOTMEM_DEFAULT);

	/*
	 * Reserve the page tables.  These are already in use,
	 * and can only be in node 0.
	 */
	//保留段页表所在的page
	reserve_bootmem_node(pgdat, __pa(swapper_pg_dir),
			     PTRS_PER_PGD * sizeof(pgd_t), BOOTMEM_DEFAULT);
}

/*
 * Set up device the mappings.  Since we clear out the page tables for all
 * mappings above VMALLOC_END, we will remove any debug device mappings.
 * This means you have to be careful how you debug this function, or any
 * called function.  This means you can't use any function or debugging
 * method which may touch any device, otherwise the kernel _will_ crash.
 */
static void __init devicemaps_init(struct machine_desc *mdesc)
{
	struct map_desc map;
	unsigned long addr;
	void *vectors;

	/*
	 * Allocate the vector page early.
	 */
	vectors = alloc_bootmem_low_pages(PAGE_SIZE);
	BUG_ON(!vectors);

	for (addr = VMALLOC_END; addr; addr += PGDIR_SIZE)
		pmd_clear(pmd_off_k(addr));

	/*
	 * Map the kernel if it is XIP.
	 * It is always first in the modulearea.
	 */
#ifdef CONFIG_XIP_KERNEL
	map.pfn = __phys_to_pfn(CONFIG_XIP_PHYS_ADDR & SECTION_MASK);
	map.virtual = MODULES_VADDR;
	map.length = ((unsigned long)&_etext - map.virtual + ~SECTION_MASK) & SECTION_MASK;
	map.type = MT_ROM;
	create_mapping(&map);
#endif

	/*
	 * Map the cache flushing regions.
	 */
#ifdef FLUSH_BASE
	map.pfn = __phys_to_pfn(FLUSH_BASE_PHYS);
	map.virtual = FLUSH_BASE;
	map.length = SZ_1M;
	map.type = MT_CACHECLEAN;
	create_mapping(&map);
#endif
#ifdef FLUSH_BASE_MINICACHE
	map.pfn = __phys_to_pfn(FLUSH_BASE_PHYS + SZ_1M);
	map.virtual = FLUSH_BASE_MINICACHE;
	map.length = SZ_1M;
	map.type = MT_MINICLEAN;
	create_mapping(&map);
#endif

	/*
	 * Create a mapping for the machine vectors at the high-vectors
	 * location (0xffff0000).  If we aren't using high-vectors, also
	 * create a mapping at the low-vectors virtual address.
	 */
	map.pfn = __phys_to_pfn(virt_to_phys(vectors));
	map.virtual = 0xffff0000;
	map.length = PAGE_SIZE;
	map.type = MT_HIGH_VECTORS;
	create_mapping(&map);

	if (!vectors_high()) {
		map.virtual = 0;
		map.type = MT_LOW_VECTORS;
		create_mapping(&map);
	}

	/*
	 * Ask the machine support to map in the statically mapped devices.
	 */
	if (mdesc->map_io)
		mdesc->map_io();

	/*
	 * Finally flush the caches and tlb to ensure that we're in a
	 * consistent state wrt the writebuffer.  This also ensures that
	 * any write-allocated cache lines in the vector page are written
	 * back.  After this point, we can start to touch devices again.
	 */
	local_flush_tlb_all();
	flush_cache_all();
}

/*
 * paging_init() sets up the page tables, initialises the zone memory
 * maps, and sets up the zero page, bad page and bad page tables.
 */
void __init paging_init(struct meminfo *mi, struct machine_desc *mdesc)
{
	void *zero_page;

	build_mem_type_table();
	sanity_check_meminfo(mi);
	prepare_page_table(mi);
	bootmem_init(mi);
	devicemaps_init(mdesc);

	top_pmd = pmd_off_k(0xffff0000);

	/*
	 * allocate the zero page.  Note that we count on this going ok.
	 */
	zero_page = alloc_bootmem_low_pages(PAGE_SIZE);
	memzero(zero_page, PAGE_SIZE);
	empty_zero_page = virt_to_page(zero_page);
	flush_dcache_page(empty_zero_page);
}

/*
 * In order to soft-boot, we need to insert a 1:1 mapping in place of
 * the user-mode pages.  This will then ensure that we have predictable
 * results when turning the mmu off
 */
void setup_mm_for_reboot(char mode)
{
	unsigned long base_pmdval;
	pgd_t *pgd;
	int i;

	if (current->mm && current->mm->pgd)
		pgd = current->mm->pgd;
	else
		pgd = init_mm.pgd;

	base_pmdval = PMD_SECT_AP_WRITE | PMD_SECT_AP_READ | PMD_TYPE_SECT;
	if (cpu_architecture() <= CPU_ARCH_ARMv5TEJ && !cpu_is_xscale())
		base_pmdval |= PMD_BIT4;

	for (i = 0; i < FIRST_USER_PGD_NR + USER_PTRS_PER_PGD; i++, pgd++) {
		unsigned long pmdval = (i << PGDIR_SHIFT) | base_pmdval;
		pmd_t *pmd;

		pmd = pmd_off(pgd, i << PGDIR_SHIFT);
		pmd[0] = __pmd(pmdval);
		pmd[1] = __pmd(pmdval + (1 << (PGDIR_SHIFT - 1)));
		flush_pmd_entry(pmd);
	}
}
