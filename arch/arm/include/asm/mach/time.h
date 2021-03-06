﻿/*
 * arch/arm/include/asm/mach/time.h
 *
 * Copyright (C) 2004 MontaVista Software, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef __ASM_ARM_MACH_TIME_H
#define __ASM_ARM_MACH_TIME_H

#include <linux/sysdev.h>

/*
 * This is our kernel timer structure.
 *
 * - init
 *   Initialise the kernels jiffy timer source, claim interrupt
 *   using setup_irq.  This is called early on during initialisation
 *   while interrupts are still disabled on the local CPU.
 * - suspend
 *   Suspend the kernel jiffy timer source, if necessary.  This
 *   is called with interrupts disabled, after all normal devices
 *   have been suspended.  If no action is required, set this to
 *   NULL.
 * - resume
 *   Resume the kernel jiffy timer source, if necessary.  This
 *   is called with interrupts disabled before any normal devices
 *   are resumed.  If no action is required, set this to NULL.
 * - offset
 *   Return the timer offset in microseconds since the last timer
 *   interrupt.  Note: this must take account of any unprocessed
 *   timer interrupt which may be pending.
 */
struct sys_timer {
	struct sys_device	dev;
	/* 用于初始化内核jiffy时钟源 */
	void			(*init)(void);
	/* 用于挂起时钟中断 */
	void			(*suspend)(void);
	/* 用于恢复挂起的时钟中断 */
	void			(*resume)(void);
#ifndef CONFIG_GENERIC_TIME
	/* 返回上次中断后经过的时间，单位为微妙 */
	unsigned long		(*offset)(void);
#endif
};

extern struct sys_timer *system_timer;
extern void timer_tick(void);

/*
 * Kernel time keeping support.
 */
struct timespec;
extern int (*set_rtc)(void);
extern void save_time_delta(struct timespec *delta, struct timespec *rtc);
extern void restore_time_delta(struct timespec *delta, struct timespec *rtc);

#endif
