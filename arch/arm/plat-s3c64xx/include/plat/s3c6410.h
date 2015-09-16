/* arch/arm/plat-s3c64xx/include/plat/s3c6410.h
 *
 * Copyright 2008 Openmoko,  Inc.
 * Copyright 2008 Simtec Electronics
 *	Ben Dooks <ben@simtec.co.uk>
 *	http://armlinux.simtec.co.uk/
 *
 * Header file for s3c6410 cpu support
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/
extern  int s3c6410_init(void);
extern void s3c6410_init_irq(void);
extern void s3c6410_init_uarts(struct s3c2410_uartcfg *cfg, int no);
extern void s3c6410_map_io(void);
extern void s3c6410_init_clocks(int xtal);
