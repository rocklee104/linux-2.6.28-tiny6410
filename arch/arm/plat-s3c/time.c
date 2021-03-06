﻿/* linux/arch/arm/plat-s3c/time.c
 *
 * Copyright (C) 2003-2005 Simtec Electronics
 *	Ben Dooks, <ben@simtec.co.uk>
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

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/io.h>

#include <asm/system.h>
#include <asm/leds.h>
#include <asm/mach-types.h>

#include <asm/irq.h>
#include <mach/map.h>
#include <plat/regs-timer.h>
#include <mach/regs-irq.h>
#include <asm/mach/time.h>
#include <mach/tick.h>

#include <plat/clock.h>
#include <plat/cpu.h>

static unsigned long timer_startval;
/* tick向微秒的转换,这个数是浮点数为了保证精度,放大后的数 */
static unsigned long timer_usec_ticks;

#ifndef TICK_MAX
#define TICK_MAX (0xffff)
#endif

/* 为了保证浮点数的精度,用于放大 */
#define TIMER_USEC_SHIFT 16

/* we use the shifted arithmetic to work out the ratio of timer ticks
 * to usecs, as often the peripheral clock is not a nice even multiple
 * of 1MHz.
 *
 * shift of 14 and 15 are too low for the 12MHz, 16 seems to be ok
 * for the current HZ value of 200 without producing overflows.
 *
 * Original patch by Dimitry Andric, updated by Ben Dooks
*/


/* timer_mask_usec_ticks
 *
 * given a clock and divisor, make the value to pass into timer_ticks_to_usec
 * to scale the ticks into usecs
*/

/* 频率转微秒.为了保存更大的精度,对结果放大了2<<16.实际等价于 t = (10^6)*(2^16)/(pclk/scaler) */
static inline unsigned long
timer_mask_usec_ticks(unsigned long scaler, unsigned long pclk)
{
	unsigned long den = pclk / 1000;

	/* + (den >> 1)是为了减少误差 */
	return ((1000 << TIMER_USEC_SHIFT) * scaler + (den >> 1)) / den;
}

/* timer_ticks_to_usec
 *
 * convert timer ticks to usec.
*/

static inline unsigned long timer_ticks_to_usec(unsigned long ticks)
{
	unsigned long res;

	res = ticks * timer_usec_ticks;
	res += 1 << (TIMER_USEC_SHIFT - 4);	/* round up slightly */

	return res >> TIMER_USEC_SHIFT;
}

/***
 * Returns microsecond  since last clock interrupt.  Note that interrupts
 * will have been disabled by do_gettimeoffset()
 * IRQs are disabled before entering here from do_gettimeofday()
 */

static unsigned long s3c2410_gettimeoffset (void)
{
	unsigned long tdone;
	unsigned long tval;

	/* work out how many ticks have gone since last timer interrupt */

	tval =  __raw_readl(S3C2410_TCNTO(4));
	tdone = timer_startval - tval;

	/* check to see if there is an interrupt pending */

	if (s3c24xx_ostimer_pending()) {
		/* re-read the timer, and try and fix up for the missed
		 * interrupt. Note, the interrupt may go off before the
		 * timer has re-loaded from wrapping.
		 */

		tval =  __raw_readl(S3C2410_TCNTO(4));
		tdone = timer_startval - tval;

		if (tval != 0)
			tdone += timer_startval;
	}

	return timer_ticks_to_usec(tdone);
}

/*
 * IRQ handler for the timer
 */
static irqreturn_t
s3c2410_timer_interrupt(int irq, void *dev_id)
{
	timer_tick();
	return IRQ_HANDLED;
}

static struct irqaction s3c2410_timer_irq = {
	.name		= "S3C2410 Timer Tick",
	.flags		= IRQF_DISABLED | IRQF_TIMER | IRQF_IRQPOLL,
	.handler	= s3c2410_timer_interrupt,
};

/* 完成了PWM时钟源的初始化工作 */
static void s3c64xx_timer_setup (void)
{
	unsigned long tcon;
	unsigned long tcnt;
	unsigned long tcfg1;
	unsigned long tcfg0;
	unsigned long pclk;
	struct clk *clk;

	tcnt = TICK_MAX;  /* default value for tcnt */
	/* read the current timer configuration bits */

	/* 读取TCON寄存器 */
	tcon = __raw_readl(S3C2410_TCON);
	/* 读取TCFG1寄存器 */
	tcfg1 = __raw_readl(S3C2410_TCFG1);
	/* 读取TCFG0寄存器 */
	tcfg0 = __raw_readl(S3C2410_TCFG0);

	/* configure the system for whichever machine is in use */

	/* for the h1940 (and others), we use the pclk from the core
	 * to generate the timer values. since values around 50 to
	 * 70MHz are not values we can directly generate the timer
	 * value from, we need to pre-scale and divide before using it.
	 *
	 * for instance, using 50.7MHz and dividing by 6 gives 8.45MHz
	 * (8.45 ticks per usec)
	 */

	/* this is used as default if no other timer can be found */
	/* 获取PWM时钟 */
	clk = clk_get(NULL, "timers");
	if (IS_ERR(clk))
		panic("failed to get clock for system timer");

	clk_enable(clk);

	/* pclk == 6500000 */
	pclk = clk_get_rate(clk);

	/* configure clock tick */
	/* 1微秒包含的TICK数目存入timer_usec_ticks, 这个tick数是PWM timer4的输出频率 */
	timer_usec_ticks = timer_mask_usec_ticks(6, pclk);

	/* 选择定时器4的MUX输入为0b0000,也即1/1分频 */
	tcfg1 &= ~S3C2410_TCFG1_MUX4_MASK;
	tcfg1 |= S3C2410_TCFG1_MUX4_DIV1;

	/* 预分频器1的值为6,显然它控制定时器2,3和4 */
	tcfg0 &= ~S3C2410_TCFG_PRESCALER1_MASK;
	tcfg0 |= (6) << S3C2410_TCFG_PRESCALER1_SHIFT;

	/* 
	 * 根据公式Timer input clock Frequency = PCLK / ( {prescaler value + 1} ) / {divider value}
	 * 计算得到input clock frequency, 我们这里设置1s产生HZ个中断,就需要将这个frequency再除以HZ
	 * 得到每个中断需要的计数.(tcnt = 47500)
	 */
	tcnt = (pclk / 7) / HZ;

	/* timers reload after counting zero, so reduce the count by 1 */
	/* 
	 * 假设我们需要3个计数产生一个次中断,如果设置tcnt = 3,一旦tcnt == 0,就产生中断,
	 * 这里经过了3个计数3,2,1(0不包含在内),实际上由于自动装载时,计数器可以计数到0,
	 * 所以本次中断到下次中断将会经历4个计数,所以需要将tcnt再减1
	 */
	tcnt--;

	printk(KERN_DEBUG "timer tcon=%08lx, tcnt %04lx, tcfg %08lx,%08lx, usec %08lx\n",
	       tcon, tcnt, tcfg0, tcfg1, timer_usec_ticks);

	/* check to see if timer is within 16bit range... */
	if (tcnt > TICK_MAX) {
		panic("setup_timer: HZ is too small, cannot configure timer!");
		return;
	}

	__raw_writel(tcfg1, S3C2410_TCFG1);
	__raw_writel(tcfg0, S3C2410_TCFG0);

	timer_startval = tcnt;
	/* 写入tcntb4 */
	__raw_writel(tcnt, S3C2410_TCNTB(4));

	/* ensure timer is stopped... */

	tcon &= ~(7<<20);
	/* 设置timer4 Auto-Reload */
	tcon |= S3C2410_TCON_T4RELOAD;
	/* 设置timer4 manual update = 1, TCNTB4中的数值将被加载到TCNT4 */
	tcon |= S3C2410_TCON_T4MANUALUPD;

	__raw_writel(tcon, S3C2410_TCON);
	/* 寄存器TCNTB4,物理地址0x7F00603c */
	__raw_writel(tcnt, S3C2410_TCNTB(4));
	/* 寄存器TCNTO4(timer4没有TCNTP4寄存器),物理地址0x7F006040,TCNTO4是只读的寄存器,这条语句错误*/
	__raw_writel(tcnt, S3C2410_TCMPB(4));

	/* start the timer running */
	tcon |= S3C2410_TCON_T4START;
	/* 设置timer4 manual update = 0 */
	tcon &= ~S3C2410_TCON_T4MANUALUPD;
	__raw_writel(tcon, S3C2410_TCON);

	/* Timer interrupt Enable */
	/* 
	 * 通过CSTAT寄存器使能Timer 4 Interrupt.注意,此时PWM时钟4已开始向中断控制器提交中断信号,
	 * 中断控制器也已被初始化,但是此时是禁中断的
	 */
	__raw_writel(__raw_readl(S3C64XX_TINT_CSTAT) | S3C_TINT_CSTAT_T4INTEN , S3C64XX_TINT_CSTAT);
}

static void __init s3c64xx_timer_init(void)
{
	s3c64xx_timer_setup();
	/* 安装了用于更新jiffy的IRQ_TIMER4中断函数s3c2410_timer_irq */
	setup_irq(IRQ_TIMER4, &s3c2410_timer_irq);
}

struct sys_timer s3c64xx_timer = {
	.init		= s3c64xx_timer_init,
	.offset		= s3c2410_gettimeoffset,
	.resume		= s3c64xx_timer_setup
};

