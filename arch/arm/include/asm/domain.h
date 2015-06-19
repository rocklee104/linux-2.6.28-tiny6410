/*
 *  arch/arm/include/asm/domain.h
 *
 *  Copyright (C) 1999 Russell King.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef __ASM_PROC_DOMAIN_H
#define __ASM_PROC_DOMAIN_H

/*
 * Domain numbers
 *
 *  DOMAIN_IO     - domain 2 includes all IO only
 *  DOMAIN_USER   - domain 1 includes all user memory only
 *  DOMAIN_KERNEL - domain 0 includes all kernel memory only
 *
 * The domain numbering depends on whether we support 36 physical
 * address for I/O or not.  Addresses above the 32 bit boundary can
 * only be mapped using supersections and supersections can only
 * be set for domain 0.  We could just default to DOMAIN_IO as zero,
 * but there may be systems with supersection support and no 36-bit
 * addressing.  In such cases, we want to map system memory with
 * supersections to reduce TLB misses and footprint.
 *
 * 36-bit addressing and supersections are only available on
 * CPUs based on ARMv6+ or the Intel XSC3 core.
 */
//arm11处理器定义了16种不同的域,但是linux只用到了D0-D2这3种
#ifndef CONFIG_IO_36
#define DOMAIN_KERNEL	0
#define DOMAIN_TABLE	0
#define DOMAIN_USER	1
#define DOMAIN_IO	2
#else
#define DOMAIN_KERNEL	2
#define DOMAIN_TABLE	2
#define DOMAIN_USER	1
#define DOMAIN_IO	0
#endif

/*
 * Domain types
 */
#define DOMAIN_NOACCESS	0
/*
 * 根据CP15的C1控制寄存器中的R和S位以及页表中地址变换条目中的访问权限控制位AP
 * 来确定是否允许各种系统工作模式的存储访问
 */
#define DOMAIN_CLIENT	1
/*
 * 不考虑CP15的C1控制寄存器中的R和S位以及页表中地址变换条目中的访问权限控制位AP,
 * 在这种情况下不管系统工作在特权模式还是用户模式都不会产生访问失效
 */
#define DOMAIN_MANAGER	3

/*
 * 每个域的访问权限分别由CP15的C3寄存器中的两位来设定，c3寄存器的大小为32bits,
 * 刚好可以设置16个域的访问权限.每个域占2位,故type需要左移2*(dom)位
 */
#define domain_val(dom,type)	((type) << (2*(dom)))

#ifndef __ASSEMBLY__

#ifdef CONFIG_MMU
//通过修改cp15 c3的值来设置访问控制域
#define set_domain(x)					\
	do {						\
	__asm__ __volatile__(				\
	"mcr	p15, 0, %0, c3, c0	@ set domain"	\
	  : : "r" (x));					\
	isb();						\
	} while (0)

//设置当前进程的访问控制域
#define modify_domain(dom,type)					\
	do {							\
	struct thread_info *thread = current_thread_info();	\
	unsigned int domain = thread->cpu_domain;		\
	domain &= ~domain_val(dom, DOMAIN_MANAGER);		\
	thread->cpu_domain = domain | domain_val(dom, type);	\
	set_domain(thread->cpu_domain);				\
	} while (0)

#else
#define set_domain(x)		do { } while (0)
#define modify_domain(dom,type)	do { } while (0)
#endif

#endif
#endif /* !__ASSEMBLY__ */
