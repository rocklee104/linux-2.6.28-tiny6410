/*
 * MTD partitioning layer definitions
 *
 * (C) 2000 Nicolas Pitre <nico@cam.org>
 *
 * This code is GPL
 */

#ifndef MTD_PARTITIONS_H
#define MTD_PARTITIONS_H

#include <linux/types.h>


/*
 * Partition definition structure:
 *
 * An array of struct partition is passed along with a MTD object to
 * add_mtd_partitions() to create them.
 *
 * For each partition, these fields are available:
 * name: string that will be used to label the partition's MTD device.
 * size: the partition size; if defined as MTDPART_SIZ_FULL, the partition
 * 	will extend to the end of the master MTD device.
 * offset: absolute starting position within the master MTD device; if
 * 	defined as MTDPART_OFS_APPEND, the partition will start where the
 * 	previous one ended; if MTDPART_OFS_NXTBLK, at the next erase block.
 * mask_flags: contains flags that have to be masked (removed) from the
 * 	master MTD flag set for the corresponding MTD partition.
 * 	For example, to force a read-only partition, simply adding
 * 	MTD_WRITEABLE to the mask_flags will do the trick.
 *
 * Note: writeable partitions require their size and offset be
 * erasesize aligned (e.g. use MTDPART_OFS_NEXTBLK).
 */

/* 注意,可写的分区需要size和offset和擦除的大小对齐 */
struct mtd_partition {
	char *name;			/* identifier string */
	/* 如果被设置成MTDPART_SIZ_FULL,这个分区会拓展到主MTD设备的最后 */
	u_int32_t size;			/* partition size */
	/*
	 * 当前分区在主mtd中的起始位置.
	 * 如果被定义成MTDPART_OFS_APPEND,这个分区就会从上一个分区结束位置开始.
	 * 如果被定义成MTDPART_OFS_NXTBLK,这个分区就会从下个block开始
	*/
	u_int32_t offset;		/* offset within the master MTD space */
	/*
	 * flag掩码,用于从主mtd的flag中去除某些特性.
	 * 比如,在mask_flags中添加MTD_WRITEABLE,就能将当前分区设置成只读的
	*/
	u_int32_t mask_flags;		/* master MTD flags to mask out for this partition */
	struct nand_ecclayout *ecclayout;	/* out of band layout for this partition (NAND only)*/
	struct mtd_info **mtdp;		/* pointer to store the MTD object */
};

#define MTDPART_OFS_NXTBLK	(-2)
#define MTDPART_OFS_APPEND	(-1)
#define MTDPART_SIZ_FULL	(0)


int add_mtd_partitions(struct mtd_info *, const struct mtd_partition *, int);
int del_mtd_partitions(struct mtd_info *);

/*
 * Functions dealing with the various ways of partitioning the space
 */

struct mtd_part_parser {
	struct list_head list;
	struct module *owner;
	const char *name;
	int (*parse_fn)(struct mtd_info *, struct mtd_partition **, unsigned long);
};

extern int register_mtd_parser(struct mtd_part_parser *parser);
extern int deregister_mtd_parser(struct mtd_part_parser *parser);
extern int parse_mtd_partitions(struct mtd_info *master, const char **types,
				struct mtd_partition **pparts, unsigned long origin);

#define put_partition_parser(p) do { module_put((p)->owner); } while(0)

struct device;
struct device_node;

int __devinit of_mtd_parse_partitions(struct device *dev,
                                      struct device_node *node,
                                      struct mtd_partition **pparts);

#endif
