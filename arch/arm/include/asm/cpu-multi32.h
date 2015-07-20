/*
 *  arch/arm/include/asm/cpu-multi32.h
 *
 *  Copyright (C) 2000 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <asm/page.h>

struct mm_struct;

/*
 * Don't change this structure - ASM code
 * relies on it.
 */
//proc_v6.S中将processor赋值为v6_processor_functions
extern struct processor {
	/* MISC
	 * get data abort address/flags
	 */
	//v6_early_abort
	void (*_data_abort)(unsigned long pc);
	/*
	 * Retrieve prefetch fault address
	 */
	//pabort_noifar
	unsigned long (*_prefetch_abort)(unsigned long lr);
	/*
	 * Set up any processor specifics
	 */
	void (*_proc_init)(void);
	/*
	 * Disable any processor specifics
	 */
	void (*_proc_fin)(void);
	/*
	 * Special stuff for a reset
	 */
	void (*reset)(unsigned long addr) __attribute__((noreturn));
	/*
	 * Idle the processor
	 */
	int (*_do_idle)(void);
	/*
	 * Processor architecture specific
	 */
	/*
	 * clean a virtual address range from the
	 * D-cache without flushing the cache.
	 */
	void (*dcache_clean_area)(void *addr, int size);

	/*
	 * Set the page table
	 */
	void (*switch_mm)(unsigned long pgd_phys, struct mm_struct *mm);
	/*
	 * Set a possibly extended PTE.  Non-extended PTEs should
	 * ignore 'ext'.
	 */
	void (*set_pte_ext)(pte_t *ptep, pte_t pte, unsigned int ext);
} processor;

//cpu_v6_proc_init
#define cpu_proc_init()			processor._proc_init()
//cpu_v6_proc_fin
#define cpu_proc_fin()			processor._proc_fin()
//cpu_v6_reset
#define cpu_reset(addr)			processor.reset(addr)
//cpu_v6_do_idle
#define cpu_do_idle()			processor._do_idle()
//cpu_v6_dcache_clean_area
#define cpu_dcache_clean_area(addr,sz)	processor.dcache_clean_area(addr,sz)
//cpu_v6_set_pte_ext
#define cpu_set_pte_ext(ptep,pte,ext)	processor.set_pte_ext(ptep,pte,ext)
//cpu_v6_switch_mm
#define cpu_do_switch_mm(pgd,mm)	processor.switch_mm(pgd,mm)
