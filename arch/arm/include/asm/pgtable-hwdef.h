/*
 *  arch/arm/include/asm/pgtable-hwdef.h
 *
 *  Copyright (C) 1995-2002 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef _ASMARM_PGTABLE_HWDEF_H
#define _ASMARM_PGTABLE_HWDEF_H

/*
 * Hardware page table definitions.
 *
 * + Level 1 descriptor (PMD)
 *   - common
 */
/* 一级页表项的类型,根据页表项的bit[0,1]判断 */
#define PMD_TYPE_MASK		(3 << 0)
#define PMD_TYPE_FAULT		(0 << 0)
/* L1为coarse page table, L2只能是large page(64KB)/small page(4KB) */
#define PMD_TYPE_TABLE		(1 << 0)
#define PMD_TYPE_SECT		(2 << 0)
#define PMD_BIT4		(1 << 4)
#define PMD_DOMAIN(x)		((x) << 5)
#define PMD_PROTECTION		(1 << 9)	/* v5 */
/*
 *   - section
 */
/* 段页表描述符的各个bit */
/* 写缓冲使能 */
#define PMD_SECT_BUFFERABLE	(1 << 2)
/* cache使能 */
#define PMD_SECT_CACHEABLE	(1 << 3)
/* 禁止执行标志,1:禁止执行 0:可执行 */
#define PMD_SECT_XN		(1 << 4)	/* v6 */
/* 页表指示的页面可写 */
#define PMD_SECT_AP_WRITE	(1 << 10)
/* 页表指示的页面可读 */
#define PMD_SECT_AP_READ	(1 << 11)
/* 扩展类型,与B,C标志协同控制内存访问类型 */
#define PMD_SECT_TEX(x)		((x) << 12)	/* v5 */
/* 扩展访问权限位与APX协同控制内存访问权限 */
#define PMD_SECT_APX		(1 << 15)	/* v6 */
/* 共享访问 */
#define PMD_SECT_S		(1 << 16)	/* v6 */
/* 全局访问 */
#define PMD_SECT_nG		(1 << 17)	/* v6 */
/* 段页表和超级段页表开关 */
#define PMD_SECT_SUPER		(1 << 18)	/* v6 */

/* 见ARM1176JZF,Table 6-2 */
#define PMD_SECT_UNCACHED	(0)
#define PMD_SECT_BUFFERED	(PMD_SECT_BUFFERABLE)
/* write through */
#define PMD_SECT_WT		(PMD_SECT_CACHEABLE)
/* write back */
#define PMD_SECT_WB		(PMD_SECT_CACHEABLE | PMD_SECT_BUFFERABLE)
#define PMD_SECT_MINICACHE	(PMD_SECT_TEX(1) | PMD_SECT_CACHEABLE)
#define PMD_SECT_WBWA		(PMD_SECT_TEX(1) | PMD_SECT_CACHEABLE | PMD_SECT_BUFFERABLE)
#define PMD_SECT_NONSHARED_DEV	(PMD_SECT_TEX(2))

/*
 *   - coarse table (not used)
 */

/*
 * + Level 2 descriptor (PTE)
 *   - common
 */
#define PTE_TYPE_MASK		(3 << 0)
#define PTE_TYPE_FAULT		(0 << 0)
/* large page */
#define PTE_TYPE_LARGE		(1 << 0)
/* 对arm1176来说,不区分PTE_TYPE_SMALL和PTE_TYPE_EXT,它们都属于Extended small page */
#define PTE_TYPE_SMALL		(2 << 0)
#define PTE_TYPE_EXT		(3 << 0)	/* v5 */
/* 指向的页面开启buffer */
#define PTE_BUFFERABLE		(1 << 2)
/* 指向的页面开启cache */
#define PTE_CACHEABLE		(1 << 3)

/*
 *   - extended small page/tiny page
 */
/* 下面就是extended small page的页表项中的bit */
#define PTE_EXT_XN		(1 << 0)	/* v6 */
/* extended small page的ap */
#define PTE_EXT_AP_MASK		(3 << 4)
#define PTE_EXT_AP0		(1 << 4)
#define PTE_EXT_AP1		(2 << 4)
#define PTE_EXT_AP_UNO_SRO	(0 << 4)
#define PTE_EXT_AP_UNO_SRW	(PTE_EXT_AP0)
#define PTE_EXT_AP_URO_SRW	(PTE_EXT_AP1)
#define PTE_EXT_AP_URW_SRW	(PTE_EXT_AP1|PTE_EXT_AP0)
#define PTE_EXT_TEX(x)		((x) << 6)	/* v5 */
#define PTE_EXT_APX		(1 << 9)	/* v6 */
#define PTE_EXT_COHERENT	(1 << 9)	/* XScale3 */
#define PTE_EXT_SHARED		(1 << 10)	/* v6 */
#define PTE_EXT_NG		(1 << 11)	/* v6 */

/*
 *   - small page
 */
#define PTE_SMALL_AP_MASK	(0xff << 4)
#define PTE_SMALL_AP_UNO_SRO	(0x00 << 4)
#define PTE_SMALL_AP_UNO_SRW	(0x55 << 4)
#define PTE_SMALL_AP_URO_SRW	(0xaa << 4)
#define PTE_SMALL_AP_URW_SRW	(0xff << 4)

#endif
