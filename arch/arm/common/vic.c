﻿/*
 *  linux/arch/arm/common/vic.c
 *
 *  Copyright (C) 1999 - 2003 ARM Limited
 *  Copyright (C) 2000 Deep Blue Solutions Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <linux/init.h>
#include <linux/list.h>
#include <linux/io.h>

#include <asm/mach/irq.h>
#include <asm/hardware/vic.h>

static void vic_mask_irq(unsigned int irq)
{
	void __iomem *base = get_irq_chip_data(irq);
	irq &= 31;
	writel(1 << irq, base + VIC_INT_ENABLE_CLEAR);
}

static void vic_unmask_irq(unsigned int irq)
{
	void __iomem *base = get_irq_chip_data(irq);
	irq &= 31;
	writel(1 << irq, base + VIC_INT_ENABLE);
}

static struct irq_chip vic_chip = {
	.name	= "VIC",
	.ack	= vic_mask_irq,
	/* 对中断禁止寄存器的操作 */
	.mask	= vic_mask_irq,
	/* 中断使能寄存器的操作 */
	.unmask	= vic_unmask_irq,
};

/**
 * vic_init - initialise a vectored interrupt controller
 * @base: iomem base address
 * @irq_start: starting interrupt number, must be muliple of 32
 * @vic_sources: bitmask of interrupt sources to allow
 */
void __init vic_init(void __iomem *base, unsigned int irq_start,
		     u32 vic_sources)
{
	unsigned int i;

	/* Disable all interrupts initially. */

	/* victor中所有中断的类型选择irq */
	writel(0, base + VIC_INT_SELECT);
	/* 关闭VIC0INTENABLE, note:数据手册上提到这个寄存器只能置位,写0是没有任何效果的 */
	//writel(0, base + VIC_INT_ENABLE);
	/* interrupt disabled in VICINTENABLE Register */
	/* 这个寄存器只能写1,写0没有任何效果, disable中断 */
	writel(~0, base + VIC_INT_ENABLE_CLEAR);
	/* 对于mini6410来说, VICxIRQSTATUS寄存器只能读 */
	//writel(0, base + VIC_IRQ_STATUS);
    //writel(0, base + VIC_ITCR);
	//writel(0, base + VIC_IRQ_STATUS);
	/* mini6410不存在VIC_ITCR */
	//writel(0, base + VIC_ITCR);
	/* software interrupt disabled in the VICSOFTINT Registe */
	/* 只能写1, 软中断disabled */
	writel(~0, base + VIC_INT_SOFT_CLEAR);

	/*
	 * Make sure we clear all existing interrupts
	 */
	 /* mini6410使用的矢量中断控制器是PL192 */
#if 0
	writel(0, base + VIC_PL190_VECT_ADDR);
	for (i = 0; i < 19; i++) {
		unsigned int value;

		value = readl(base + VIC_PL190_VECT_ADDR);
		writel(value, base + VIC_PL190_VECT_ADDR);
	}
#endif

	/* mini6410没有VIC_VECT_CNTL0寄存器 */
#if 0
	for (i = 0; i < 16; i++) {
		void __iomem *reg = base + VIC_VECT_CNTL0 + (i * 4);
		writel(VIC_VECT_CNTL_ENABLE | i, reg);
	}
#endif

	/* mini6410使用的矢量中断控制器是PL192 */
	//writel(32, base + VIC_PL190_DEF_VECT_ADDR);

	for (i = 0; i < 32; i++) {
		/* 对于VIC0,irq最低从32开始, VIC1从64开始 */
		unsigned int irq = irq_start + i;

		/* 中断描述符和中断芯片关联 */
		set_irq_chip(irq, &vic_chip);
		/* 保存中断控制寄存器基址到中断描述符 */
		set_irq_chip_data(irq, base);

		/* 当前中断没有被屏蔽 */
		if (vic_sources & (1 << i)) {
			set_irq_handler(irq, handle_level_irq);
			/* 设置中断有效 */
			set_irq_flags(irq, IRQF_VALID | IRQF_PROBE);
		}
	}
}
