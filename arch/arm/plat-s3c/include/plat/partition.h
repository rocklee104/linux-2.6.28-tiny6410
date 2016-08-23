/* linux/arch/arm/plat-s3c/include/plat/partition.h
 *
 * Copyright (c) 2008 Samsung Electronics
 *
 * Partition information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <asm/mach/flash.h>

struct mtd_partition s3c_partition_info[] = {
        {
                .name		= "Bootloader",
                .offset		= 0,
                .size		= (4 * 128 *SZ_1K),
                .mask_flags	= MTD_CAP_NANDFLASH,
        },
        {
                .name		= "Kernel",
                .offset		= (4 * 128 *SZ_1K),
                .size		= (5*SZ_1M) ,
                .mask_flags	= MTD_CAP_NANDFLASH,
        },
        {
                .name		= "File System",
                .offset		= MTDPART_OFS_APPEND,
                /* 使用剩下的所有空间 */
                .size		= MTDPART_SIZ_FULL,
        }
};

struct s3c_nand_mtd_info s3c_nand_mtd_part_info = {
	/* 1块flash芯片 */
	.chip_nr = 1,
	.mtd_part_nr = ARRAY_SIZE(s3c_partition_info),
	.partition = s3c_partition_info,
};

