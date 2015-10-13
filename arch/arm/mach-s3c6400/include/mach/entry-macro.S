/* linux/arch/arm/mach-s3c6400/include/mach/entry-macro.S
 *
 * Copyright 2008 Openmoko, Inc.
 * Copyright 2008 Simtec Electronics
 *	http://armlinux.simtec.co.uk/
 *	Ben Dooks <ben@simtec.co.uk>
 *
 * Low-level IRQ helper macros for the Samsung S3C64XX series
 *
 * This file is licensed under  the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
*/

#include <asm/hardware/vic.h>
#include <mach/map.h>
#include <plat/irqs.h>

	.macro	disable_fiq
	.endm

	.macro	get_irqnr_preamble, base, tmp
	ldr	\base, =S3C_VA_VIC0
	.endm

	.macro	arch_ret_to_user, tmp1, tmp2
	.endm

	#通过读取VIC0IRQSTATUS来获取发生中断的中断号
	.macro	get_irqnr_and_base, irqnr, irqstat, base, tmp

	@ check the vic0
	#获取中断号,这里的中断号和硬件芯片手册的有差别,当前的irqnr是在芯片手册上增加了31
	mov	\irqnr, # S3C_IRQ_OFFSET + 31
	#读取vic0的VIC_IRQ_STATUS
	ldr	\irqstat, [ \base, # VIC_IRQ_STATUS ]
	#测试vic0的irqstat是否为0
	teq	\irqstat, #0

	#如果vic0 的irqstat为0,需要检查vic1
	@ otherwise try vic1
	#tmp中保存vic的基址
	addeq	\tmp, \base, #(S3C_VA_VIC1 - S3C_VA_VIC0)
	#vic1中的中断号
	addeq	\irqnr, \irqnr, #32
	#读取vic1的VIC_IRQ_STATUS
	ldreq	\irqstat, [ \tmp, # VIC_IRQ_STATUS ]
	#测试vic1的irqstat是否为0
	teqeq	\irqstat, #0

	#如果vic0和vic1中的irqstat不为0,计算前导零的数目
	clzne	\irqstat, \irqstat
	subne	\irqnr, \irqnr, \irqstat
	.endm
