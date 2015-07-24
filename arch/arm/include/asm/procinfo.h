/*
 *  arch/arm/include/asm/procinfo.h
 *
 *  Copyright (C) 1996-1999 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef __ASM_PROCINFO_H
#define __ASM_PROCINFO_H

#ifdef __KERNEL__

struct cpu_tlb_fns;
struct cpu_user_fns;
struct cpu_cache_fns;
struct processor;

/*
 * Note!  struct processor is always defined if we're
 * using MULTI_CPU, otherwise this entry is unused,
 * but still exists.
 *
 * NOTE! The following structure is defined by assembly
 * language, NOT C code.  For more information, check:
 *  arch/arm/mm/proc-*.S and arch/arm/kernel/head.S
 */
struct proc_info_list {
	//0x0007b000
	unsigned int		cpu_val;
	//0x0007f000
	unsigned int		cpu_mask;
	//PMD_TYPE_SECT|PMD_SECT_BUFFERABLE|PMD_SECT_CACHEABLE|PMD_SECT_AP_WRITE|PMD_SECT_AP_READ
	unsigned long		__cpu_mm_mmu_flags;	/* used by head.S */
	//PMD_TYPE_SECT|PMD_SECT_XN|PMD_SECT_AP_WRITE|PMD_SECT_AP_READ  
	unsigned long		__cpu_io_mmu_flags;	/* used by head.S */
	//__v6_setup
	unsigned long		__cpu_flush;		/* used by head.S */
	//"armv6"
	const char		*arch_name;
	//"v6" 
	const char		*elf_name;
	//HWCAP_SWP|HWCAP_HALF|HWCAP_THUMB|HWCAP_FAST_MULT|HWCAP_EDSP|HWCAP_JAVA
	unsigned int		elf_hwcap;
	//"ARMv6-compatible processor"
	const char		*cpu_name;
	struct processor	*proc;
	//arch/arm/mm/tlb-v6.S定义的v6wbi_tlb_fns
	struct cpu_tlb_fns	*tlb;
	//copypage-v6.c中定义的v6_user_fns
	struct cpu_user_fns	*user;
	//cache-v6.S中定义的v6_cache_fns
	struct cpu_cache_fns	*cache;
};

#else	/* __KERNEL__ */
#include <asm/elf.h>
#warning "Please include asm/elf.h instead"
#endif	/* __KERNEL__ */
#endif
