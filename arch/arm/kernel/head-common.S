/*
 *  linux/arch/arm/kernel/head-common.S
 *
 *  Copyright (C) 1994-2002 Russell King
 *  Copyright (c) 2003 ARM Limited
 *  All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#define ATAG_CORE 0x54410001
#u-boot中定义了type_size(type) ((sizeof(struct tag_header) + sizeof(struct type)) >> 2)
#define ATAG_CORE_SIZE ((2*4 + 3*4) >> 2)

	.type	__switch_data, %object
#__switch_data中记录MMU开启后需要执行的操作,由于开启了MMU,所以不是PIC
__switch_data:
	.long	__mmap_switched
	.long	__data_loc			@ r4
	.long	__data_start			@ r5
	.long	__bss_start			@ r6
	.long	_end				@ r7
	.long	processor_id			@ r4
	.long	__machine_arch_type		@ r5
	.long	__atags_pointer			@ r6
	.long	cr_alignment			@ r7
	#init_thread_union定义在数据段的开头,大小为8k,THREAD_START_SP为8k-8
	.long	init_thread_union + THREAD_START_SP @ sp

/*
 * The following fragment of code is executed with the MMU on in MMU mode,
 * and uses absolute addresses; this is not position independent.
 *
 *  r0  = cp#15 control register
 *  r1  = machine ID
 *  r2  = atags pointer
 *  r9  = processor ID
 */
__mmap_switched:
	#r3记录__data_loc
	adr	r3, __switch_data + 4
	/*
	 * r4:__data_loc
	 * r5:__data_start
	 * r6:__bss_start
	 * r7:_end
	 */
	ldmia	r3!, {r4, r5, r6, r7}
	#比较__data_loc和__data_start,理论上应该是相等的.
	cmp	r4, r5				@ Copy data segment if needed
	#如果不相等,就需要将__data_loc搬运到__data_start上去
1:	cmpne	r5, r6
	ldrne	fp, [r4], #4
	strne	fp, [r5], #4
	bne	1b

	mov	fp, #0				@ Clear BSS (and zero fp)
1:	cmp	r6, r7
	#cc为无符号小于
	strcc	fp, [r6],#4
	bcc	1b

	/*
	 * r3:processor_id
	 * r4:processor_id
	 * r5:__machine_arch_type
	 * r6:__atags_pointer
	 * r7:cr_alignment
	 * sp:init_thread_union + THREAD_START_SP
	 */
	ldmia	r3, {r4, r5, r6, r7, sp}
	#将processor id,machine type,atags pointer写入相应的地址
	str	r9, [r4]			@ Save processor ID
	str	r1, [r5]			@ Save machine type
	str	r2, [r6]			@ Save atags pointer
	bic	r4, r0, #CR_A			@ Clear 'A' bit
	/*
	 * 将A域以外的CP15 c1寄存器值保存到cr_no_alignment变量,
	 * 设置A域的值,保存到cr_alignment,这两个变量在entry-armv.S中定义
	 */
	stmia	r7, {r0, r4}			@ Save control register values
	b	start_kernel
ENDPROC(__mmap_switched)

/*
 * Exception handling.  Something went wrong and we can't proceed.  We
 * ought to tell the user, but since we don't have any guarantee that
 * we're even running on the right architecture, we do virtually nothing.
 *
 * If CONFIG_DEBUG_LL is set we try to print out something about the error
 * and hope for the best (useful if bootloader fails to pass a proper
 * machine ID for example).
 */
__error_p:
#ifdef CONFIG_DEBUG_LL
	adr	r0, str_p1
	bl	printascii
	mov	r0, r9
	bl	printhex8
	adr	r0, str_p2
	bl	printascii
	b	__error
str_p1:	.asciz	"\nError: unrecognized/unsupported processor variant (0x"
str_p2:	.asciz	").\n"
	.align
#endif
ENDPROC(__error_p)

__error_a:
#ifdef CONFIG_DEBUG_LL
	mov	r4, r1				@ preserve machine ID
	adr	r0, str_a1
	bl	printascii
	mov	r0, r4
	bl	printhex8
	adr	r0, str_a2
	bl	printascii
	adr	r3, 3f
	ldmia	r3, {r4, r5, r6}		@ get machine desc list
	sub	r4, r3, r4			@ get offset between virt&phys
	add	r5, r5, r4			@ convert virt addresses to
	add	r6, r6, r4			@ physical address space
1:	ldr	r0, [r5, #MACHINFO_TYPE]	@ get machine type
	bl	printhex8
	mov	r0, #'\t'
	bl	printch
	ldr     r0, [r5, #MACHINFO_NAME]	@ get machine name
	add	r0, r0, r4
	bl	printascii
	mov	r0, #'\n'
	bl	printch
	add	r5, r5, #SIZEOF_MACHINE_DESC	@ next machine_desc
	cmp	r5, r6
	blo	1b
	adr	r0, str_a3
	bl	printascii
	b	__error
ENDPROC(__error_a)

str_a1:	.asciz	"\nError: unrecognized/unsupported machine ID (r1 = 0x"
str_a2:	.asciz	").\n\nAvailable machine support:\n\nID (hex)\tNAME\n"
str_a3:	.asciz	"\nPlease check your kernel config and/or bootloader.\n"
	.align
#endif

__error:
#ifdef CONFIG_ARCH_RPC
/*
 * Turn the screen red on a error - RiscPC only.
 */
	mov	r0, #0x02000000
	mov	r3, #0x11
	orr	r3, r3, r3, lsl #8
	orr	r3, r3, r3, lsl #16
	str	r3, [r0], #4
	str	r3, [r0], #4
	str	r3, [r0], #4
	str	r3, [r0], #4
#endif
1:	mov	r0, r0
	b	1b
ENDPROC(__error)


/*
 * Read processor ID register (CP#15, CR0), and look up in the linker-built
 * supported processor list.  Note that we can't use the absolute addresses
 * for the __proc_info lists since we aren't running with the MMU on
 * (and therefore, we are not in the correct address space).  We have to
 * calculate the offset.
 *
 *	r9 = cpuid
 * Returns:
 *	r3, r4, r6 corrupted
 *	r5 = proc_info pointer in physical address space
 *	r9 = cpuid (preserved)
 */
__lookup_processor_type:
   /*
	* adr是相对寻址,它的寻计算结果是将当前PC值加上3f符号与PC的偏移量,
    * 而PC是物理地址,因此r3的结果也是3f符号的物理地址
	*/
	adr	r3, 3f
	/*
	 * 编号低的寄存器在store/load数据时对应于存储器的低地址
	 *
	 * r5:__proc_info_begin
	 * r6:__proc_ino_end
	 * r7:., 也即3f符号的链接地址.请注意,在链接期间,__proc_info_begin和
	 * __proc_info_end以及.均是链接地址,也即虚执地址.
	 */
	ldmda	r3, {r5 - r7}
	#r3中保存了物理地址和虚拟地址之间的差值
	sub	r3, r3, r7			@ get offset between virt&phys
	#r5中保存__proc_info_begin的虚拟地址
	add	r5, r5, r3			@ convert virt addresses to
	#r6中__proc_info_end的虚拟地址
	add	r6, r6, r3			@ physical address space
	#r3中保存val = 0x0007b000,r4保存mask = 0x0007f000,arm1176jzf-S的id是410fb766
1:	ldmia	r5, {r3, r4}			@ value, mask
	and	r4, r4, r9			@ mask wanted bits
	teq	r3, r4
	beq	2f
	#匹配下个proc_info
	add	r5, r5, #PROC_INFO_SZ		@ sizeof(proc_info_list)
	cmp	r5, r6
	blo	1b
	mov	r5, #0				@ unknown processor
2:	mov	pc, lr
ENDPROC(__lookup_processor_type)

/*
 * This provides a C-API version of the above function.
 */
ENTRY(lookup_processor_type)
	stmfd	sp!, {r4 - r7, r9, lr}
	mov	r9, r0
	bl	__lookup_processor_type
	mov	r0, r5
	ldmfd	sp!, {r4 - r7, r9, pc}
ENDPROC(lookup_processor_type)

/*
 * Look in <asm/procinfo.h> and arch/arm/kernel/arch.[ch] for
 * more information about the __proc_info and __arch_info structures.
 */
	.long	__proc_info_begin
	.long	__proc_info_end
3:	.long	.
	.long	__arch_info_begin
	.long	__arch_info_end

/*
 * Lookup machine architecture in the linker-build list of architectures.
 * Note that we can't use the absolute addresses for the __arch_info
 * lists since we aren't running with the MMU on (and therefore, we are
 * not in the correct address space).  We have to calculate the offset.
 *
 *  r1 = machine architecture number
 * Returns:
 *  r3, r4, r6 corrupted
 *  r5 = mach_info pointer in physical address space
 */
__lookup_machine_type:
	adr	r3, 3b
	/*
	 * r4:., 也即3f符号的链接地址.请注意,在链接期间,__arch_info_begin和
	 * __arch_info_end以及.均是链接地址,也即虚执地址.
	 * r5:__arch_info_begin
	 * r6:__arch_info_end
	 *
	 * 每个board都会调用MACHINE_START/MACHINE_END将struct machine_desc添加到
	 * .arch.info.init段中
	 */
	ldmia	r3, {r4, r5, r6}
	#r3中保存了物理地址和虚拟地址之间的差值
	sub	r3, r3, r4			@ get offset between virt&phys
	#__arch_info_begin物理地址
	add	r5, r5, r3			@ convert virt addresses to
	#__arch_info_end物理地址
	add	r6, r6, r3			@ physical address space
	/*
	 * r5记录struct machine_desc的起始位置,MACHINFO_TYPE是nr在
	 * struct machine_desc中的offset,这样的话r3就记录了nr的值
	 */
1:	ldr	r3, [r5, #MACHINFO_TYPE]	@ get machine type
	#和u-boot中传来的architecture id(2520)相比
	teq	r3, r1				@ matches loader number?
	beq	2f				@ found
	#判断下一个struct machine_desc
	add	r5, r5, #SIZEOF_MACHINE_DESC	@ next machine_desc
	cmp	r5, r6
	blo	1b
	mov	r5, #0				@ unknown machine
2:	mov	pc, lr
ENDPROC(__lookup_machine_type)

/*
 * This provides a C-API version of the above function.
 */
ENTRY(lookup_machine_type)
	stmfd	sp!, {r4 - r6, lr}
	mov	r1, r0
	bl	__lookup_machine_type
	mov	r0, r5
	ldmfd	sp!, {r4 - r6, pc}
ENDPROC(lookup_machine_type)

/* Determine validity of the r2 atags pointer.  The heuristic requires
 * that the pointer be aligned, in the first 16k of physical RAM and
 * that the ATAG_CORE marker is first and present.  Future revisions
 * of this function may be more lenient with the physical address and
 * may also be able to move the ATAGS block if necessary.
 *
 * r8  = machinfo
 *
 * Returns:
 *  r2 either valid atags pointer, or zero
 *  r5, r6 corrupted
 */
__vet_atags:
	tst	r2, #0x3			@ aligned?
	bne	1f

	ldr	r5, [r2, #0]			@ is first tag ATAG_CORE?
	#struct tag的hdr.size - #ATAG_CORE_SIZE是否为0,subs影响cpsr的nzcv位
	subs	r5, r5, #ATAG_CORE_SIZE
	#tag_core的第一个成员size与#ATAG_CORE_SIZE不相等
	bne	1f
	#struct tag的hdr.tag与ATAG_CORE这个magic number相比
	ldr	r5, [r2, #4]
	ldr	r6, =ATAG_CORE
	cmp	r5, r6
	bne	1f

	#校验tag链表头成功
	mov	pc, lr				@ atag pointer is ok

1:	mov	r2, #0
	mov	pc, lr
ENDPROC(__vet_atags)
