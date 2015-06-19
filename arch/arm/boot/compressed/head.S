/*
 *  linux/arch/arm/boot/compressed/head.S
 *
 *  Copyright (C) 1996-2002 Russell King
 *  Copyright (C) 2004 Hyok S. Choi (MPU support)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/linkage.h>

/*
 * Debugging stuff
 *
 * Note that these macros must not contain any code which is not
 * 100% relocatable.  Any attempt to do so will result in a crash.
 * Please select one of the following when turning on debugging.
 */
#ifdef DEBUG

#if defined(CONFIG_DEBUG_ICEDCC)

#ifdef CONFIG_CPU_V6
		.macro	loadsp, rb
		.endm
		.macro	writeb, ch, rb
		mcr	p14, 0, \ch, c0, c5, 0
		.endm
#else
		.macro	loadsp, rb
		.endm
		.macro	writeb, ch, rb
		mcr	p14, 0, \ch, c1, c0, 0
		.endm
#endif

#else

#include <mach/debug-macro.S>

		.macro	writeb,	ch, rb
		senduart \ch, \rb
		.endm

#if defined(CONFIG_ARCH_SA1100)
		.macro	loadsp, rb
		mov	\rb, #0x80000000	@ physical base address
#ifdef CONFIG_DEBUG_LL_SER3
		add	\rb, \rb, #0x00050000	@ Ser3
#else
		add	\rb, \rb, #0x00010000	@ Ser1
#endif
		.endm
#elif defined(CONFIG_ARCH_S3C2410)
		.macro loadsp, rb
		mov	\rb, #0x50000000
		add	\rb, \rb, #0x4000 * CONFIG_S3C_LOWLEVEL_UART_PORT
		.endm
#else
		.macro	loadsp,	rb
		addruart \rb
		.endm
#endif
#endif
#endif

		.macro	kputc,val
		mov	r0, \val
		bl	putc
		.endm

		.macro	kphex,val,len
		mov	r0, \val
		mov	r1, #\len
		bl	phex
		.endm

		.macro	debug_reloc_start
#ifdef DEBUG
		kputc	#'\n'
		kphex	r6, 8		/* processor id */
		kputc	#':'
		kphex	r7, 8		/* architecture id */
#ifdef CONFIG_CPU_CP15
		kputc	#':'
		mrc	p15, 0, r0, c1, c0
		kphex	r0, 8		/* control reg */
#endif
		kputc	#'\n'
		kphex	r5, 8		/* decompressed kernel start */
		kputc	#'-'
		kphex	r9, 8		/* decompressed kernel end  */
		kputc	#'>'
		kphex	r4, 8		/* kernel execution address */
		kputc	#'\n'
#endif
		.endm

		.macro	debug_reloc_end
#ifdef DEBUG
		kphex	r5, 8		/* end of kernel */
		kputc	#'\n'
		mov	r0, r4
		bl	memdump		/* dump 256 bytes at start of kernel */
#endif
		.endm

		.section ".start", #alloc, #execinstr
/*
 * sort out different calling conventions
 */
		.align
start:
		.type	start,#function
		#重复8次,空出中断向量表的位置. 
		.rept	8
		#就是nop指令
		mov	r0, r0
		#.endr结束循环
		.endr

		b	1f
		.word	0x016f2818		@ Magic numbers to help the loader
		.word	start			@ absolute load/run zImage address
		.word	_edata			@ zImage end address
        /*
         * u-boot中调用theKernel(0, machid, bd->bi_boot_params);
         * 将machid保存在r1.参数链表物理地址保存在r2.
         */
		#将machid移动到r7
1:		mov	r7, r1			@ save architecture ID
		#将参数链表地址移动到r8
		mov	r8, r2			@ save atags pointer

#ifndef __ARM_ARCH_2__
		/*
		 * Booting from Angel - need to enter SVC mode and disable
		 * FIQs/IRQs (numeric definitions from angel arm.h source).
		 * We only do this if we were in user mode on entry.
		 */
		#取出cpsr的值
		mrs	r2, cpsr		@ get current mode
		#user模式下cpsr最后2位均为0
		tst	r2, #3			@ not user?
		bne	not_angel
		mov	r0, #0x17		@ angel_SWIreason_EnterSVC
		swi	0x123456		@ angel_SWI_ARM
not_angel:
		#取出cpsr的值
		mrs	r2, cpsr		@ turn off interrupts to
		#orr按位或,禁止irq/fiq 
		orr	r2, r2, #0xc0		@ prevent angel from running
		#将r2写入cpsr
		msr	cpsr_c, r2
#else
		teqp	pc, #0x0c000003		@ turn off interrupts
#endif

		/*
		 * Note that some cache flushing and other stuff may
		 * be needed here - is there an Angel SWI call for this?
		 */

		/*
		 * some architecture specific code can be inserted
		 * by the linker here, but it should preserve r7, r8, and r9.
		 */

		.text
		#r0中保存当前正在运行时LC0的地址
		adr	r0, LC0
		/*
		 * ia,每次传送后地址加4,将r0指向的地址中的数据读取到
		 * r1, r2, r3, r4, r5, r6, ip, sp中.最后结果:
		 *
		 * LC0			   -> r1
		 * __bss_start	   -> r2
		 * _end			   -> r3
		 * zreladdr		   -> r4
		 * _start		   -> r5
		 * _got_start	   -> r6
		 * _got_end		   -> ip
		 * user_stack+4096 -> sp
		 */
		ldmia	r0, {r1, r2, r3, r4, r5, r6, ip, sp}
		#当前运行地址与链接地址的偏移量存入r0中
		subs	r0, r0, r1		@ calculate the delta offset

						@ if delta is zero, we are
		#r0中的值和r1中的一致,就跳转到not_relocated
		beq	not_relocated		@ running at the address we
						@ were linked at.

		/*
		 * We're running at a different address.  We need to fix
		 * up various pointers:
		 *   r5 - zImage base address
		 *   r6 - GOT start
		 *   ip - GOT end
		 */
		#如果链接地址和运行地址不一致,那么就需要调整其他寄存器中的值
		add	r5, r5, r0
		#修正GOT表的起始位置
		add	r6, r6, r0
		add	ip, ip, r0

#ifndef CONFIG_ZBOOT_ROM
		/*
		 * If we're running fully PIC === CONFIG_ZBOOT_ROM = n,
		 * we need to fix up pointers into the BSS region.
		 *   r2 - BSS start
		 *   r3 - BSS end
		 *   sp - stack pointer
		 */
		add	r2, r2, r0
		add	r3, r3, r0
		add	sp, sp, r0

		/*
		 * Relocate all entries in the GOT table.
		 */
		#取出GOT表中的第一个entry
1:		ldr	r1, [r6, #0]		@ relocate entries in the GOT
		#entry指向的地址也需要相对重定向到运行地址上来
		add	r1, r1, r0		@ table.  This fixes up the
		#将新的地址写入GOT表的entry中,r6 = r6 + 4,也就是指向下一entry
		str	r1, [r6], #4		@ C references.
		#判断是否超过_got_end,cmp指令实际上是进行了一次减法运算,更行条件标志位
		cmp	r6, ip
		#当r6小于ip,就跳转到之前的1这个标签中
		blo	1b
#else
		#若定义了CONFIG_ZBOOT_ROM,只对GOT表中bss段以外的符号进行重定位
		/*
		 * Relocate entries in the GOT table.  We only relocate
		 * the entries that are outside the (relocated) BSS region.
		 */
1:		ldr	r1, [r6, #0]		@ relocate entries in the GOT
		cmp	r1, r2			@ entry < bss_start ||
		cmphs	r3, r1			@ _end < entry
		addlo	r1, r1, r0		@ table.  This fixes up the
		str	r1, [r6], #4		@ C references.
		cmp	r6, ip
		blo	1b
#endif

not_relocated:	mov	r0, #0
#初始化bss段
1:		str	r0, [r2], #4		@ clear bss
		str	r0, [r2], #4
		str	r0, [r2], #4
		str	r0, [r2], #4
		cmp	r2, r3
		#还在bss段内,就跳转到之前的1标签中
		blo	1b

		/*
		 * The C runtime environment should now be setup
		 * sufficiently.  Turn the cache on, set up some
		 * pointers, and start decompressing.
		 */
		#C runtime环境已经设置完成,下面要打开cache和mmu,lr中保存当前PC指针
		bl	cache_on

		#r1中保存栈的起始地址
		mov	r1, sp			@ malloc space above stack
		#r2保存栈的结束地址
		add	r2, sp, #0x10000	@ 64k max

/*
 * Check to see if we will overwrite ourselves.
 *   r4 = final kernel address
 *   r5 = start of this image
 *   r2 = end of malloc space (and therefore this image)
 * We basically want:
 *   r4 >= r2 -> OK
 *   r4 + image length <= r5 -> OK
 */
		#r4中保存kernel将要解压到的地址
		cmp	r4, r2
		#如果kernel要解压到的地址大于等于栈的结束地址
		bhs	wont_overwrite
		sub	r3, sp, r5		@ > compressed kernel size
		add	r0, r4, r3, lsl #2	@ allow for 4x expansion
		cmp	r0, r5
		bls	wont_overwrite

		mov	r5, r2			@ decompress after malloc space
		mov	r0, r5
		mov	r3, r7
		bl	decompress_kernel

		add	r0, r0, #127 + 128	@ alignment + stack
		bic	r0, r0, #127		@ align the kernel length
/*
 * r0     = decompressed kernel length
 * r1-r3  = unused
 * r4     = kernel execution address
 * r5     = decompressed kernel start
 * r6     = processor ID
 * r7     = architecture ID
 * r8     = atags pointer
 * r9-r14 = corrupted
 */
		add	r1, r5, r0		@ end of decompressed kernel
		adr	r2, reloc_start
		ldr	r3, LC1
		add	r3, r2, r3
1:		ldmia	r2!, {r9 - r14}		@ copy relocation code
		stmia	r1!, {r9 - r14}
		ldmia	r2!, {r9 - r14}
		stmia	r1!, {r9 - r14}
		cmp	r2, r3
		blo	1b
		add	sp, r1, #128		@ relocate the stack

		bl	cache_clean_flush
		add	pc, r5, r0		@ call relocation code

/*
 * We're not in danger of overwriting ourselves.  Do this the simple way.
 *
 * r4     = kernel execution address
 * r7     = architecture ID
 */
#r0中保存解压后的内核起始地址
wont_overwrite:	mov	r0, r4
		#r3保存architecture ID 
		mov	r3, r7
		#解压内核
		bl	decompress_kernel
		b	call_kernel

		.type	LC0, #object
#r1中保存的是链接时的LC0地址
LC0:		.word	LC0			@ r1
		.word	__bss_start		@ r2
		.word	_end			@ r3
		#zreladdr指向kernel解压后的物理地址
		.word	zreladdr		@ r4
		.word	_start			@ r5
		.word	_got_start		@ r6
		.word	_got_end		@ ip
		.word	user_stack+4096		@ sp
LC1:		.word	reloc_end - reloc_start
		.size	LC0, . - LC0

#ifdef CONFIG_ARCH_RPC
		.globl	params
params:		ldr	r0, =params_phys
		mov	pc, lr
		.ltorg
		.align
#endif

/*
 * Turn on the cache.  We need to setup some page tables so that we
 * can have both the I and D caches on.
 *
 * We place the page tables 16k down from the kernel execution address,
 * and we hope that nothing else is using it.  If we're using it, we
 * will go pop!
 *
 * On entry,
 *  r4 = kernel execution address
 *  r6 = processor ID
 *  r7 = architecture number
 *  r8 = atags pointer
 *  r9 = run-time address of "start"  (???)
 * On exit,
 *  r1, r2, r3, r9, r10, r12 corrupted
 * This routine must preserve:
 *  r4, r5, r6, r7, r8
 */
		.align	5
cache_on:	mov	r3, #8			@ cache_on function
		b	call_cache_fn

/*
 * Initialize the highest priority protection region, PR7
 * to cover all 32bit address and cacheable and bufferable.
 */
__armv4_mpu_cache_on:
		mov	r0, #0x3f		@ 4G, the whole
		mcr	p15, 0, r0, c6, c7, 0	@ PR7 Area Setting
		mcr 	p15, 0, r0, c6, c7, 1

		mov	r0, #0x80		@ PR7
		mcr	p15, 0, r0, c2, c0, 0	@ D-cache on
		mcr	p15, 0, r0, c2, c0, 1	@ I-cache on
		mcr	p15, 0, r0, c3, c0, 0	@ write-buffer on

		mov	r0, #0xc000
		mcr	p15, 0, r0, c5, c0, 1	@ I-access permission
		mcr	p15, 0, r0, c5, c0, 0	@ D-access permission

		mov	r0, #0
		mcr	p15, 0, r0, c7, c10, 4	@ drain write buffer
		mcr	p15, 0, r0, c7, c5, 0	@ flush(inval) I-Cache
		mcr	p15, 0, r0, c7, c6, 0	@ flush(inval) D-Cache
		mrc	p15, 0, r0, c1, c0, 0	@ read control reg
						@ ...I .... ..D. WC.M
		orr	r0, r0, #0x002d		@ .... .... ..1. 11.1
		orr	r0, r0, #0x1000		@ ...1 .... .... ....

		mcr	p15, 0, r0, c1, c0, 0	@ write control reg

		mov	r0, #0
		mcr	p15, 0, r0, c7, c5, 0	@ flush(inval) I-Cache
		mcr	p15, 0, r0, c7, c6, 0	@ flush(inval) D-Cache
		mov	pc, lr

__armv3_mpu_cache_on:
		mov	r0, #0x3f		@ 4G, the whole
		mcr	p15, 0, r0, c6, c7, 0	@ PR7 Area Setting

		mov	r0, #0x80		@ PR7
		mcr	p15, 0, r0, c2, c0, 0	@ cache on
		mcr	p15, 0, r0, c3, c0, 0	@ write-buffer on

		mov	r0, #0xc000
		mcr	p15, 0, r0, c5, c0, 0	@ access permission

		mov	r0, #0
		mcr	p15, 0, r0, c7, c0, 0	@ invalidate whole cache v3
		mrc	p15, 0, r0, c1, c0, 0	@ read control reg
						@ .... .... .... WC.M
		orr	r0, r0, #0x000d		@ .... .... .... 11.1
		mov	r0, #0
		mcr	p15, 0, r0, c1, c0, 0	@ write control reg

		mcr	p15, 0, r0, c7, c0, 0	@ invalidate whole cache v3
		mov	pc, lr

		/* 
		 * r4中保存kernel解压后的物理地址,r3保存page dir的起始地址.
		 * 由于使用段页式映射,寻址空间4GB,每段1MB,需要4096段.页表项
		 * 需要4096项,每项4byte, 故page dir总共占用16KB.
		 *
		 * 6410中,zreladdr的地址为0x50008000,
		 * 页目录的地址经过下面的计算应该为0x50004000.
		 */
__setup_mmu:	sub	r3, r4, #16384		@ Page directory size
		#bic是逻辑与非指令,一般用于clear bit,对r3按16KB对齐
		bic	r3, r3, #0xff		@ Align the pointer
		bic	r3, r3, #0x3f00
/*
 * Initialise the page tables, turning on the cacheable and bufferable
 * bits for the RAM area only.
 */
		#r0中保存page dir起始地址
		mov	r0, r3
		#page dir起始地址向下256KB对齐后作为ram的起始地址
		mov	r9, r0, lsr #18
		#经过位移操作后ram的起始地址是0x50000000,和datasheet中的一致
		mov	r9, r9, lsl #18		@ start of RAM
		#r10=页表起始地址+256M,作为RAM结束地址,这里假定RAM大小为256MB
		add	r10, r9, #0x10000000	@ a reasonable RAM size
		/*
		 * 页表项的初始值:最低两位为10,说明是section map,1MB映射.
		 * CB(cacheable/bufferable)两位为00,默认禁止缓存和缓冲.
		 */
		mov	r1, #0x12
		/*
		 * r1的值为0xc12, 访问控制位设置为11,完全允许访问.
		 * 31-20位为0x000,section物理基址为0x000
		 */
		orr	r1, r1, #3 << 10
		#r2=页表起始地址+16k,作为页表结束地址
		add	r2, r3, #16384
		#判断section的物理基址是否落在RAM范围内,因为对RAM应该开启缓存和缓冲
1:		cmp	r1, r9			@ if virt > start of RAM
		/*
		 * hs条件码,表示无符号数大于等于,CPSR中条件标志位为C=1
		 * 如果落在RAM之内,就开启cache和buffer
		 */
		orrhs	r1, r1, #0x0c		@ set cacheable, bufferable
		cmp	r1, r10			@ if virt > end of RAM
		#如果落在RAM之外,就关闭cache和buffer
		bichs	r1, r1, #0x0c		@ clear cacheable, bufferable
		#把页表项写入页表,并跳转页表指针指向下一个页表项
		str	r1, [r0], #4		@ 1:1 mapping
		#把页表项指向下一个section的物理基址,1MB
		add	r1, r1, #1048576
		#检查是否到了页表结束地址，如果是则说明页表初始化完成了
		teq	r0, r2
		#否则写入下一个页表项
		bne	1b
/*
 * If ever we are running from Flash, then we surely want the cache
 * to be enabled also for our execution instance...  We map 2MB of it
 * so there is no map overlap problem for up to 1 MB compressed kernel.
 * If the execution is in RAM then we would only be duplicating the above.
 */
		/*
		 * 如果我们当前在flash运行,
		 * 把当前PC所在地址１M对齐的地方的2M空间开启cache和write buffer
		 * 为了加快代码在 nor flash中运行的速度
		 */
		mov	r1, #0x1e
		#开启读写权限
		orr	r1, r1, #3 << 10
		#r2中记录pc右移20位的值,也就是pc这个地址在页表项中的index
		mov	r2, pc, lsr #20
		#r1保存了页表项的值
		orr	r1, r1, r2, lsl #20
		/* 
		 * 页表的起始地址(r3)加上pc对应的页表项在页表内的偏移(index * 4),
		 * 也就是r2 << 2, 获取了pc对应的页表项的物理地址(r0).
		 */
		add	r0, r3, r2, lsl #2
		#将值写入页表项
		str	r1, [r0], #4
		#写入下一个页表项
		add	r1, r1, #1048576
		str	r1, [r0]
		#返回到__armv4_mmu_cache_on
		mov	pc, lr
#ENDPROC用于定义一个函数,在include/asm/linkage.h有定义
ENDPROC(__setup_mmu)

__armv4_mmu_cache_on:
		#保存返回地址
		mov	r12, lr
		bl	__setup_mmu
		mov	r0, #0
		#清空write buffer
		mcr	p15, 0, r0, c7, c10, 4	@ drain write buffer
		#使unified TLB无效,即使instruction TLB和data TLB无效
		mcr	p15, 0, r0, c8, c7, 0	@ flush I,D TLBs
		#Read Control Register configuration data到r0
		mrc	p15, 0, r0, c1, c0, 0	@ read control reg
		#使能icache, 设置icache的替换策略为round-robin
		orr	r0, r0, #0x5000		@ I-cache enable, RR cache replacement
		#ARM1176JZF-S手册第3-46页,读取read control reg的时候,这两个位是1
		orr	r0, r0, #0x0030
		#开启MMU,icache,dcache
		bl	__common_mmu_cache_on
		mov	r0, #0
		mcr	p15, 0, r0, c8, c7, 0	@ flush I,D TLBs
		mov	pc, r12

__armv7_mmu_cache_on:
		mov	r12, lr
		mrc	p15, 0, r11, c0, c1, 4	@ read ID_MMFR0
		tst	r11, #0xf		@ VMSA
		blne	__setup_mmu
		mov	r0, #0
		mcr	p15, 0, r0, c7, c10, 4	@ drain write buffer
		tst	r11, #0xf		@ VMSA
		mcrne	p15, 0, r0, c8, c7, 0	@ flush I,D TLBs
		mrc	p15, 0, r0, c1, c0, 0	@ read control reg
		orr	r0, r0, #0x5000		@ I-cache enable, RR cache replacement
		orr	r0, r0, #0x003c		@ write buffer
		orrne	r0, r0, #1		@ MMU enabled
		movne	r1, #-1
		mcrne	p15, 0, r3, c2, c0, 0	@ load page table pointer
		mcrne	p15, 0, r1, c3, c0, 0	@ load domain access control
		mcr	p15, 0, r0, c1, c0, 0	@ load control register
		mrc	p15, 0, r0, c1, c0, 0	@ and read it back
		mov	r0, #0
		mcr	p15, 0, r0, c7, c5, 4	@ ISB
		mov	pc, r12

__arm6_mmu_cache_on:
		mov	r12, lr
		bl	__setup_mmu
		mov	r0, #0
		mcr	p15, 0, r0, c7, c0, 0	@ invalidate whole cache v3
		mcr	p15, 0, r0, c5, c0, 0	@ invalidate whole TLB v3
		mov	r0, #0x30
		bl	__common_mmu_cache_on
		mov	r0, #0
		mcr	p15, 0, r0, c5, c0, 0	@ invalidate whole TLB v3
		mov	pc, r12

#r0中保存Control Register configuration data,并且使能了icache
__common_mmu_cache_on:
#ifndef DEBUG
		#开启MMU,dcache,dcache,关闭Strict alignment fault checking
		orr	r0, r0, #0x000d		@ Write buffer, mmu
#endif
		#-1的补码是0xffffffff
		mov	r1, #-1
		#将页表基址写入cp15的c2寄存器
		mcr	p15, 0, r3, c2, c0, 0	@ load page table pointer
		/* 
		 * 将domain access control寄存器的16个域全部置1,
		 * 这样所有的域都设置成管理模式,允许任何访问.
		 */
		mcr	p15, 0, r1, c3, c0, 0	@ load domain access control
		b	1f
		.align	5			@ cache line aligned
		#将r0中保存的c1值写入c1(开启MMU,icache,dcache)
1:		mcr	p15, 0, r0, c1, c0, 0	@ load control register
		#r0中保存Control Register configuration data
		mrc	p15, 0, r0, c1, c0, 0	@ and read it back to
		/*
		 * 下面其实是2条语句.r0, lsr #32这是第一条;sub pc,lr这是第二条;
		 * 但是第一条语句对lr没有任何作用,它的作用仅仅是一条空语句.因为
		 * arm是3级流水线执行指令的,插入这条空语句保证pc值的获取在是在
		 * mmu开启以后获取到的.
		 */
		sub	pc, lr, r0, lsr #32	@ properly flush pipeline

/*
 * All code following this line is relocatable.  It is relocated by
 * the above code to the end of the decompressed kernel image and
 * executed there.  During this time, we have no stacks.
 *
 * r0     = decompressed kernel length
 * r1-r3  = unused
 * r4     = kernel execution address
 * r5     = decompressed kernel start
 * r6     = processor ID
 * r7     = architecture ID
 * r8     = atags pointer
 * r9-r14 = corrupted
 */
		.align	5
reloc_start:	add	r9, r5, r0
		sub	r9, r9, #128		@ do not copy the stack
		debug_reloc_start
		mov	r1, r4
1:
		.rept	4
		ldmia	r5!, {r0, r2, r3, r10 - r14}	@ relocate kernel
		stmia	r1!, {r0, r2, r3, r10 - r14}
		.endr

		cmp	r5, r9
		blo	1b
		add	sp, r1, #128		@ relocate the stack
		debug_reloc_end

#关闭MMU,dcache/icache,tlb
call_kernel:	bl	cache_clean_flush
		bl	cache_off
		mov	r0, #0			@ must be zero
		mov	r1, r7			@ restore architecture number
		mov	r2, r8			@ restore atags pointer
		mov	pc, r4			@ call kernel

/*
 * Here follow the relocatable cache support functions for the
 * various processors.  This is a generic hook for locating an
 * entry and jumping to an instruction at the specified offset
 * from the start of the block.  Please note this is all position
 * independent code.
 *
 *  r1  = corrupted
 *  r2  = corrupted
 *  r3  = block offset
 *  r6  = corrupted
 *  r12 = corrupted
 */

call_cache_fn:	adr	r12, proc_types
#ifdef CONFIG_CPU_CP15
		#将协处理器cp15的c0寄存器中的CPU ID保存到r6中
		mrc	p15, 0, r6, c0, c0	@ get processor ID
#else
		ldr	r6, =CONFIG_PROCESSOR_ID
#endif
#通过((real_id ^ match) & mask) == 0,在proc_types中找到对应的条目
1:		ldr	r1, [r12, #0]		@ get value
		ldr	r2, [r12, #4]		@ get mask
		#eor:逻辑异或.
		eor	r1, r1, r6		@ (real ^ match)
		tst	r1, r2			@       & mask
		/*
		 * tst结果为0,执行对应条目的cache on function,cache on function
		 * 执行完成后,将会返回not_relocated
		 */
		addeq	pc, r12, r3		@ call cache function
		#每个条目有5条语句,r12指向下一个条目
		add	r12, r12, #4*5
		b	1b

/*
 * Table for cache operations.  This is basically:
 *   - CPU ID match
 *   - CPU ID mask
 *   - 'cache on' method instruction
 *   - 'cache off' method instruction
 *   - 'cache flush' method instruction
 *
 * We match an entry using: ((real_id ^ match) & mask) == 0
 *
 * Writethrough caches generally only need 'on' and 'off'
 * methods.  Writeback caches _must_ have the flush method
 * defined.
 */
		/*
		 * writethrough的cache只需要提供cache on/off的方法,
		 * writeback的cache还需要flush的方法.
		 */
		.type	proc_types,#object
proc_types:
		.word	0x41560600		@ ARM6/610
		.word	0xffffffe0
		b	__arm6_mmu_cache_off	@ works, but slow
		b	__arm6_mmu_cache_off
		mov	pc, lr
@		b	__arm6_mmu_cache_on		@ untested
@		b	__arm6_mmu_cache_off
@		b	__armv3_mmu_cache_flush

		.word	0x00000000		@ old ARM ID
		.word	0x0000f000
		mov	pc, lr
		mov	pc, lr
		mov	pc, lr

		.word	0x41007000		@ ARM7/710
		.word	0xfff8fe00
		b	__arm7_mmu_cache_off
		b	__arm7_mmu_cache_off
		mov	pc, lr

		.word	0x41807200		@ ARM720T (writethrough)
		.word	0xffffff00
		b	__armv4_mmu_cache_on
		b	__armv4_mmu_cache_off
		mov	pc, lr

		.word	0x41007400		@ ARM74x
		.word	0xff00ff00
		b	__armv3_mpu_cache_on
		b	__armv3_mpu_cache_off
		b	__armv3_mpu_cache_flush
		
		.word	0x41009400		@ ARM94x
		.word	0xff00ff00
		b	__armv4_mpu_cache_on
		b	__armv4_mpu_cache_off
		b	__armv4_mpu_cache_flush

		.word	0x00007000		@ ARM7 IDs
		.word	0x0000f000
		mov	pc, lr
		mov	pc, lr
		mov	pc, lr

		@ Everything from here on will be the new ID system.

		.word	0x4401a100		@ sa110 / sa1100
		.word	0xffffffe0
		b	__armv4_mmu_cache_on
		b	__armv4_mmu_cache_off
		b	__armv4_mmu_cache_flush

		.word	0x6901b110		@ sa1110
		.word	0xfffffff0
		b	__armv4_mmu_cache_on
		b	__armv4_mmu_cache_off
		b	__armv4_mmu_cache_flush

		.word	0x56050000		@ Feroceon
		.word	0xff0f0000
		b	__armv4_mmu_cache_on
		b	__armv4_mmu_cache_off
		b	__armv5tej_mmu_cache_flush

		@ These match on the architecture ID

		.word	0x00020000		@ ARMv4T
		.word	0x000f0000
		b	__armv4_mmu_cache_on
		b	__armv4_mmu_cache_off
		b	__armv4_mmu_cache_flush

		.word	0x00050000		@ ARMv5TE
		.word	0x000f0000
		b	__armv4_mmu_cache_on
		b	__armv4_mmu_cache_off
		b	__armv4_mmu_cache_flush

		.word	0x00060000		@ ARMv5TEJ
		.word	0x000f0000
		b	__armv4_mmu_cache_on
		b	__armv4_mmu_cache_off
		b	__armv5tej_mmu_cache_flush

		#6410属于armv6
		.word	0x0007b000		@ ARMv6
		.word	0x000ff000
		#armv6采用与armv4相同的方法cache on/off/flush
		b	__armv4_mmu_cache_on
		b	__armv4_mmu_cache_off
		b	__armv6_mmu_cache_flush

		.word	0x000f0000		@ new CPU Id
		.word	0x000f0000
		b	__armv7_mmu_cache_on
		b	__armv7_mmu_cache_off
		b	__armv7_mmu_cache_flush

		.word	0			@ unrecognised type
		.word	0
		mov	pc, lr
		mov	pc, lr
		mov	pc, lr

		.size	proc_types, . - proc_types

/*
 * Turn off the Cache and MMU.  ARMv3 does not support
 * reading the control register, but ARMv4 does.
 *
 * On entry,  r6 = processor ID
 * On exit,   r0, r1, r2, r3, r12 corrupted
 * This routine must preserve: r4, r6, r7
 */
		.align	5
cache_off:	mov	r3, #12			@ cache_off function
		#跳转到__armv4_mmu_cache_off
		b	call_cache_fn

__armv4_mpu_cache_off:
		mrc	p15, 0, r0, c1, c0
		bic	r0, r0, #0x000d
		mcr	p15, 0, r0, c1, c0	@ turn MPU and cache off
		mov	r0, #0
		mcr	p15, 0, r0, c7, c10, 4	@ drain write buffer
		mcr	p15, 0, r0, c7, c6, 0	@ flush D-Cache
		mcr	p15, 0, r0, c7, c5, 0	@ flush I-Cache
		mov	pc, lr

__armv3_mpu_cache_off:
		mrc	p15, 0, r0, c1, c0
		bic	r0, r0, #0x000d
		mcr	p15, 0, r0, c1, c0, 0	@ turn MPU and cache off
		mov	r0, #0
		mcr	p15, 0, r0, c7, c0, 0	@ invalidate whole cache v3
		mov	pc, lr

__armv4_mmu_cache_off:
		#Read Control Register configuration data
		mrc	p15, 0, r0, c1, c0
		#关闭MMU,dcache
		bic	r0, r0, #0x000d
		#将r0写入read control register
		mcr	p15, 0, r0, c1, c0	@ turn MMU and cache off
		mov	r0, #0
		#使所有的cache无效
		mcr	p15, 0, r0, c7, c7	@ invalidate whole cache v4
		#使instruction/data tlb无效
		mcr	p15, 0, r0, c8, c7	@ invalidate whole TLB v4
		mov	pc, lr

__armv7_mmu_cache_off:
		mrc	p15, 0, r0, c1, c0
		bic	r0, r0, #0x000d
		mcr	p15, 0, r0, c1, c0	@ turn MMU and cache off
		mov	r12, lr
		bl	__armv7_mmu_cache_flush
		mov	r0, #0
		mcr	p15, 0, r0, c8, c7, 0	@ invalidate whole TLB
		mov	pc, r12

__arm6_mmu_cache_off:
		mov	r0, #0x00000030		@ ARM6 control reg.
		b	__armv3_mmu_cache_off

__arm7_mmu_cache_off:
		mov	r0, #0x00000070		@ ARM7 control reg.
		b	__armv3_mmu_cache_off

__armv3_mmu_cache_off:
		mcr	p15, 0, r0, c1, c0, 0	@ turn MMU and cache off
		mov	r0, #0
		mcr	p15, 0, r0, c7, c0, 0	@ invalidate whole cache v3
		mcr	p15, 0, r0, c5, c0, 0	@ invalidate whole TLB v3
		mov	pc, lr

/*
 * Clean and flush the cache to maintain consistency.
 *
 * On entry,
 *  r6 = processor ID
 * On exit,
 *  r1, r2, r3, r11, r12 corrupted
 * This routine must preserve:
 *  r0, r4, r5, r6, r7
 */
		.align	5
cache_clean_flush:
		mov	r3, #16
		# 跳转到__armv4_mmu_cache_flush
		b	call_cache_fn

__armv4_mpu_cache_flush:
		mov	r2, #1
		mov	r3, #0
		mcr	p15, 0, ip, c7, c6, 0	@ invalidate D cache
		mov	r1, #7 << 5		@ 8 segments
1:		orr	r3, r1, #63 << 26	@ 64 entries
2:		mcr	p15, 0, r3, c7, c14, 2	@ clean & invalidate D index
		subs	r3, r3, #1 << 26
		bcs	2b			@ entries 63 to 0
		subs 	r1, r1, #1 << 5
		bcs	1b			@ segments 7 to 0

		teq	r2, #0
		mcrne	p15, 0, ip, c7, c5, 0	@ invalidate I cache
		mcr	p15, 0, ip, c7, c10, 4	@ drain WB
		mov	pc, lr
		

__armv6_mmu_cache_flush:
		mov	r1, #0
		mcr	p15, 0, r1, c7, c14, 0	@ clean+invalidate D
		mcr	p15, 0, r1, c7, c5, 0	@ invalidate I+BTB
		mcr	p15, 0, r1, c7, c15, 0	@ clean+invalidate unified
		mcr	p15, 0, r1, c7, c10, 4	@ drain WB
		mov	pc, lr

__armv7_mmu_cache_flush:
		mrc	p15, 0, r10, c0, c1, 5	@ read ID_MMFR1
		tst	r10, #0xf << 16		@ hierarchical cache (ARMv7)
		beq	hierarchical
		mov	r10, #0
		mcr	p15, 0, r10, c7, c14, 0	@ clean+invalidate D
		b	iflush
hierarchical:
		stmfd	sp!, {r0-r5, r7, r9-r11}
		mrc	p15, 1, r0, c0, c0, 1	@ read clidr
		ands	r3, r0, #0x7000000	@ extract loc from clidr
		mov	r3, r3, lsr #23		@ left align loc bit field
		beq	finished		@ if loc is 0, then no need to clean
		mov	r10, #0			@ start clean at cache level 0
loop1:
		add	r2, r10, r10, lsr #1	@ work out 3x current cache level
		mov	r1, r0, lsr r2		@ extract cache type bits from clidr
		and	r1, r1, #7		@ mask of the bits for current cache only
		cmp	r1, #2			@ see what cache we have at this level
		blt	skip			@ skip if no cache, or just i-cache
		mcr	p15, 2, r10, c0, c0, 0	@ select current cache level in cssr
		mcr	p15, 0, r10, c7, c5, 4	@ isb to sych the new cssr&csidr
		mrc	p15, 1, r1, c0, c0, 0	@ read the new csidr
		and	r2, r1, #7		@ extract the length of the cache lines
		add	r2, r2, #4		@ add 4 (line length offset)
		ldr	r4, =0x3ff
		ands	r4, r4, r1, lsr #3	@ find maximum number on the way size
		clz	r5, r4			@ find bit position of way size increment
		ldr	r7, =0x7fff
		ands	r7, r7, r1, lsr #13	@ extract max number of the index size
loop2:
		mov	r9, r4			@ create working copy of max way size
loop3:
		orr	r11, r10, r9, lsl r5	@ factor way and cache number into r11
		orr	r11, r11, r7, lsl r2	@ factor index number into r11
		mcr	p15, 0, r11, c7, c14, 2	@ clean & invalidate by set/way
		subs	r9, r9, #1		@ decrement the way
		bge	loop3
		subs	r7, r7, #1		@ decrement the index
		bge	loop2
skip:
		add	r10, r10, #2		@ increment cache number
		cmp	r3, r10
		bgt	loop1
finished:
		mov	r10, #0			@ swith back to cache level 0
		mcr	p15, 2, r10, c0, c0, 0	@ select current cache level in cssr
		ldmfd	sp!, {r0-r5, r7, r9-r11}
iflush:
		mcr	p15, 0, r10, c7, c5, 0	@ invalidate I+BTB
		mcr	p15, 0, r10, c7, c10, 4	@ drain WB
		mov	pc, lr

__armv5tej_mmu_cache_flush:
1:		mrc	p15, 0, r15, c7, c14, 3	@ test,clean,invalidate D cache
		bne	1b
		mcr	p15, 0, r0, c7, c5, 0	@ flush I cache
		mcr	p15, 0, r0, c7, c10, 4	@ drain WB
		mov	pc, lr

__armv4_mmu_cache_flush:
		mov	r2, #64*1024		@ default: 32K dcache size (*2)
		mov	r11, #32		@ default: 32 byte line size
		mrc	p15, 0, r3, c0, c0, 1	@ read cache type
		teq	r3, r6			@ cache ID register present?
		beq	no_cache_id
		mov	r1, r3, lsr #18
		and	r1, r1, #7
		mov	r2, #1024
		mov	r2, r2, lsl r1		@ base dcache size *2
		tst	r3, #1 << 14		@ test M bit
		addne	r2, r2, r2, lsr #1	@ +1/2 size if M == 1
		mov	r3, r3, lsr #12
		and	r3, r3, #3
		mov	r11, #8
		mov	r11, r11, lsl r3	@ cache line size in bytes
no_cache_id:
		bic	r1, pc, #63		@ align to longest cache line
		add	r2, r1, r2
1:		ldr	r3, [r1], r11		@ s/w flush D cache
		teq	r1, r2
		bne	1b

		mcr	p15, 0, r1, c7, c5, 0	@ flush I cache
		mcr	p15, 0, r1, c7, c6, 0	@ flush D cache
		mcr	p15, 0, r1, c7, c10, 4	@ drain WB
		mov	pc, lr

__armv3_mmu_cache_flush:
__armv3_mpu_cache_flush:
		mov	r1, #0
		mcr	p15, 0, r0, c7, c0, 0	@ invalidate whole cache v3
		mov	pc, lr

/*
 * Various debugging routines for printing hex characters and
 * memory, which again must be relocatable.
 */
#ifdef DEBUG
		.type	phexbuf,#object
phexbuf:	.space	12
		.size	phexbuf, . - phexbuf

phex:		adr	r3, phexbuf
		mov	r2, #0
		strb	r2, [r3, r1]
1:		subs	r1, r1, #1
		movmi	r0, r3
		bmi	puts
		and	r2, r0, #15
		mov	r0, r0, lsr #4
		cmp	r2, #10
		addge	r2, r2, #7
		add	r2, r2, #'0'
		strb	r2, [r3, r1]
		b	1b

puts:		loadsp	r3
1:		ldrb	r2, [r0], #1
		teq	r2, #0
		moveq	pc, lr
2:		writeb	r2, r3
		mov	r1, #0x00020000
3:		subs	r1, r1, #1
		bne	3b
		teq	r2, #'\n'
		moveq	r2, #'\r'
		beq	2b
		teq	r0, #0
		bne	1b
		mov	pc, lr
putc:
		mov	r2, r0
		mov	r0, #0
		loadsp	r3
		b	2b

memdump:	mov	r12, r0
		mov	r10, lr
		mov	r11, #0
2:		mov	r0, r11, lsl #2
		add	r0, r0, r12
		mov	r1, #8
		bl	phex
		mov	r0, #':'
		bl	putc
1:		mov	r0, #' '
		bl	putc
		ldr	r0, [r12, r11, lsl #2]
		mov	r1, #8
		bl	phex
		and	r0, r11, #7
		teq	r0, #3
		moveq	r0, #' '
		bleq	putc
		and	r0, r11, #7
		add	r11, r11, #1
		teq	r0, #7
		bne	1b
		mov	r0, #'\n'
		bl	putc
		cmp	r11, #64
		blt	2b
		mov	pc, r10
#endif

		.ltorg
reloc_end:

		.align
		.section ".stack", "w"
user_stack:	.space	4096
