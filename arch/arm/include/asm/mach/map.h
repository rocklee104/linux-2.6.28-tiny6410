﻿/*
 *  arch/arm/include/asm/map.h
 *
 *  Copyright (C) 1999-2000 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  Page table mapping constructs and function prototypes
 */
#include <asm/io.h>

struct map_desc {
	//起始虚拟地址
	unsigned long virtual;
	//起始物理地址的页帧号
	unsigned long pfn;
	//需要映射的长度
	unsigned long length;
	//memory type
	unsigned int type;
};

/* types 0-3 are defined in asm/io.h */
#define MT_UNCACHED		4
#define MT_CACHECLEAN		5
#define MT_MINICLEAN		6
#define MT_LOW_VECTORS		7
#define MT_HIGH_VECTORS		8
#define MT_MEMORY		9
#define MT_ROM			10

#ifdef CONFIG_MMU
extern void iotable_init(struct map_desc *, int);
#else
#define iotable_init(map,num)	do { } while (0)
#endif
