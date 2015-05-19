﻿#ifndef _LINUX_GENHD_H
#define _LINUX_GENHD_H

/*
 * 	genhd.h Copyright (C) 1992 Drew Eckhardt
 *	Generic hard disk header file by  
 * 		Drew Eckhardt
 *
 *		<drew@colorado.edu>
 */

#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/rcupdate.h>

#ifdef CONFIG_BLOCK

#define kobj_to_dev(k)		container_of((k), struct device, kobj)
#define dev_to_disk(device)	container_of((device), struct gendisk, part0.__dev)
#define dev_to_part(device)	container_of((device), struct hd_struct, __dev)
#define disk_to_dev(disk)	(&(disk)->part0.__dev)
#define part_to_dev(part)	(&((part)->__dev))

extern struct device_type part_type;
extern struct kobject *block_depr;
extern struct class block_class;

enum {
/* These three have identical behaviour; use the second one if DOS FDISK gets
   confused about extended/logical partitions starting past cylinder 1023. */
	DOS_EXTENDED_PARTITION = 5,
	LINUX_EXTENDED_PARTITION = 0x85,
	WIN98_EXTENDED_PARTITION = 0x0f,

	SUN_WHOLE_DISK = DOS_EXTENDED_PARTITION,

	LINUX_SWAP_PARTITION = 0x82,
	LINUX_DATA_PARTITION = 0x83,
	LINUX_LVM_PARTITION = 0x8e,
	LINUX_RAID_PARTITION = 0xfd,	/* autodetect RAID partition */

	SOLARIS_X86_PARTITION =	LINUX_SWAP_PARTITION,
	NEW_SOLARIS_X86_PARTITION = 0xbf,

	DM6_AUX1PARTITION = 0x51,	/* no DDO:  use xlated geom */
	DM6_AUX3PARTITION = 0x53,	/* no DDO:  use xlated geom */
	DM6_PARTITION =	0x54,		/* has DDO: use xlated geom & offset */
	EZD_PARTITION =	0x55,		/* EZ-DRIVE */

	FREEBSD_PARTITION = 0xa5,	/* FreeBSD Partition ID */
	OPENBSD_PARTITION = 0xa6,	/* OpenBSD Partition ID */
	NETBSD_PARTITION = 0xa9,	/* NetBSD Partition ID */
	BSDI_PARTITION = 0xb7,		/* BSDI Partition ID */
	MINIX_PARTITION = 0x81,		/* Minix Partition ID */
	UNIXWARE_PARTITION = 0x63,	/* Same as GNU_HURD and SCO Unix */
};

#define DISK_MAX_PARTS			256
#define DISK_NAME_LEN			32

#include <linux/major.h>
#include <linux/device.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/workqueue.h>

struct partition {
	unsigned char boot_ind;		/* 0x80 - active */
	unsigned char head;		/* starting head */
	unsigned char sector;		/* starting sector */
	unsigned char cyl;		/* starting cylinder */
	unsigned char sys_ind;		/* What partition type */
	unsigned char end_head;		/* end head */
	unsigned char end_sector;	/* end sector */
	unsigned char end_cyl;		/* end cylinder */
	__le32 start_sect;	/* starting sector counting from 0 */
	__le32 nr_sects;		/* nr of sectors in partition */
} __attribute__((packed));

struct disk_stats {
	unsigned long sectors[2];	/* READs and WRITEs */
	unsigned long ios[2];
	unsigned long merges[2];
	unsigned long ticks[2];
	unsigned long io_ticks;
	unsigned long time_in_queue;
};
	
struct hd_struct {
	//该分区的起始扇区
	sector_t start_sect;
	//通用磁盘的大小,单位是扇区
	sector_t nr_sects;
	//磁盘上的一个分区也被视为一个设备
	struct device __dev;
	struct kobject *holder_dir;
	//policy为1表示分区只读,partno表示分区编号
	int policy, partno;
#ifdef CONFIG_FAIL_MAKE_REQUEST
	int make_it_fail;
#endif
	//统计磁盘队列使用情况的时间戳
	unsigned long stamp;
	//正在进行的io操作数
	int in_flight;
#ifdef	CONFIG_SMP
	//SMP环境中,统计每个cpu使用磁盘的情况
	struct disk_stats *dkstats;
#else
	struct disk_stats dkstats;
#endif
	struct rcu_head rcu_head;
};

#define GENHD_FL_REMOVABLE			1
#define GENHD_FL_DRIVERFS			2
#define GENHD_FL_MEDIA_CHANGE_NOTIFY		4
#define GENHD_FL_CD				8
//磁盘将被初始化并可以使用
#define GENHD_FL_UP				16
#define GENHD_FL_SUPPRESS_PARTITION_INFO	32
/* 
 * allows the disk to use extended partition numbers;
 * once the number of minor numbers given to alloc_disk() is exhausted,
 * any additional partitions will be numbered in the extended space
 */
//使用扩展分区号
#define GENHD_FL_EXT_DEVT			64 /* allow extended devt */

#define BLK_SCSI_MAX_CMDS	(256)
#define BLK_SCSI_CMD_PER_LONG	(BLK_SCSI_MAX_CMDS / (sizeof(long) * 8))

struct blk_scsi_cmd_filter {
	unsigned long read_ok[BLK_SCSI_CMD_PER_LONG];
	unsigned long write_ok[BLK_SCSI_CMD_PER_LONG];
	struct kobject kobj;
};

struct disk_part_tbl {
	struct rcu_head rcu_head;
	//part[]中成员的个数
	int len;
	struct hd_struct *part[];
};

struct gendisk {
	/* major, first_minor and minors are input parameters only,
	 * don't use directly.  Use disk_devt() and disk_max_parts().
	 */
	//磁盘主设备号
	int major;			/* major number of driver */
	//与磁盘关联的第一个次设备号, 如sda的次设备号
	int first_minor;
	/* 
	 * 主次分区的总个数,如果为1,表示只有主分区,无法分配次分区,例/dev/sda 8,0, 
     * 其主设备号是8,次设备号是0,first_minor=0,minors=16
     */
	int minors;                     /* maximum number of minors, =1 for
                                         * disks that can't be partitioned. */

	char disk_name[DISK_NAME_LEN];	/* name of major driver */

	/* Array of pointers to partitions indexed by partno.
	 * Protected with matching bdev lock but stat and other
	 * non-critical accesses use RCU.  Always access through
	 * helpers.
	 */
	//磁盘分区表信息,由partno索引的分区指针的数组
	struct disk_part_tbl *part_tbl;
	//当前块设备的第一个分区,如果没有分区则指代整个设备
	struct hd_struct part0;

	struct block_device_operations *fops;
	//指向磁盘请求队列的指针
	struct request_queue *queue;
	void *private_data;

	/* 
	 * 磁盘类型的标志,GENHD_FL_UP表示磁盘初始化并且可以使用,GENHD_FL_REMOVABLE: 
	 * 如果是可移动磁盘就要设置该标志.
	 */
	int flags;
	//标识该磁盘所属的硬件设备,指针指向驱动模型的一个对象
	struct device *driverfs_dev;  // FIXME: remove
	struct kobject *slave_dir;

	//该指针指向这个数据结构记录磁盘中断的定时,由内核内置的随机数发生器使用
	struct timer_rand_state *random;

	atomic_t sync_io;		/* RAID */
	struct work_struct async_notify;
#ifdef  CONFIG_BLK_DEV_INTEGRITY
	struct blk_integrity *integrity;
#endif
	int node_id;
};

//通过分区返回通用磁盘指针
static inline struct gendisk *part_to_disk(struct hd_struct *part)
{
	if (likely(part)) {
		if (part->partno)
			//如果当前分区编号大于0,返回其父device的gendisk
			return dev_to_disk(part_to_dev(part)->parent);
		else
			//如果当前分区编号等于0,返回本身的gendisk
			return dev_to_disk(part_to_dev(part));
	}
	return NULL;
}

static inline int disk_max_parts(struct gendisk *disk)
{
	if (disk->flags & GENHD_FL_EXT_DEVT)
		//使用扩展的的dev table
		return DISK_MAX_PARTS;
	//未使用扩展的dev table
	return disk->minors;
}

static inline bool disk_partitionable(struct gendisk *disk)
{
	return disk_max_parts(disk) > 1;
}

static inline dev_t disk_devt(struct gendisk *disk)
{
	return disk_to_dev(disk)->devt;
}

static inline dev_t part_devt(struct hd_struct *part)
{
	return part_to_dev(part)->devt;
}

extern struct hd_struct *disk_get_part(struct gendisk *disk, int partno);

static inline void disk_put_part(struct hd_struct *part)
{
	if (likely(part))
		put_device(part_to_dev(part));
}

/*
 * Smarter partition iterator without context limits.
 */
#define DISK_PITER_REVERSE	(1 << 0) /* iterate in the reverse direction */
#define DISK_PITER_INCL_EMPTY	(1 << 1) /* include 0-sized parts */
#define DISK_PITER_INCL_PART0	(1 << 2) /* include partition 0 */

struct disk_part_iter {
	struct gendisk		*disk;
	struct hd_struct	*part;
	int			idx;
	unsigned int		flags;
};

extern void disk_part_iter_init(struct disk_part_iter *piter,
				 struct gendisk *disk, unsigned int flags);
extern struct hd_struct *disk_part_iter_next(struct disk_part_iter *piter);
extern void disk_part_iter_exit(struct disk_part_iter *piter);

extern struct hd_struct *disk_map_sector_rcu(struct gendisk *disk,
					     sector_t sector);

/*
 * Macros to operate on percpu disk statistics:
 *
 * {disk|part|all}_stat_{add|sub|inc|dec}() modify the stat counters
 * and should be called between disk_stat_lock() and
 * disk_stat_unlock().
 *
 * part_stat_read() can be called at any time.
 *
 * part_stat_{add|set_all}() and {init|free}_part_stats are for
 * internal use only.
 */
#ifdef	CONFIG_SMP
#define part_stat_lock()	({ rcu_read_lock(); get_cpu(); })
#define part_stat_unlock()	do { put_cpu(); rcu_read_unlock(); } while (0)

#define __part_stat_add(cpu, part, field, addnd)			\
	(per_cpu_ptr((part)->dkstats, (cpu))->field += (addnd))

#define part_stat_read(part, field)					\
({									\
	typeof((part)->dkstats->field) res = 0;				\
	int i;								\
	for_each_possible_cpu(i)					\
		res += per_cpu_ptr((part)->dkstats, i)->field;		\
	res;								\
})

static inline void part_stat_set_all(struct hd_struct *part, int value)
{
	int i;

	for_each_possible_cpu(i)
		memset(per_cpu_ptr(part->dkstats, i), value,
				sizeof(struct disk_stats));
}

//对于SMP环境,dkstats是一个per-CPU变量
static inline int init_part_stats(struct hd_struct *part)
{
	part->dkstats = alloc_percpu(struct disk_stats);
	if (!part->dkstats)
		return 0;
	return 1;
}

static inline void free_part_stats(struct hd_struct *part)
{
	free_percpu(part->dkstats);
}

#else /* !CONFIG_SMP */
#define part_stat_lock()	({ rcu_read_lock(); 0; })
#define part_stat_unlock()	rcu_read_unlock()

#define __part_stat_add(cpu, part, field, addnd)				\
	((part)->dkstats.field += addnd)

#define part_stat_read(part, field)	((part)->dkstats.field)

static inline void part_stat_set_all(struct hd_struct *part, int value)
{
	memset(&part->dkstats, value, sizeof(struct disk_stats));
}

//在UP环境中,返回1
static inline int init_part_stats(struct hd_struct *part)
{
	return 1;
}

static inline void free_part_stats(struct hd_struct *part)
{
}

#endif /* CONFIG_SMP */

#define part_stat_add(cpu, part, field, addnd)	do {			\
	__part_stat_add((cpu), (part), field, addnd);			\
	if ((part)->partno)						\
		__part_stat_add((cpu), &part_to_disk((part))->part0,	\
				field, addnd);				\
} while (0)

#define part_stat_dec(cpu, gendiskp, field)				\
	part_stat_add(cpu, gendiskp, field, -1)
#define part_stat_inc(cpu, gendiskp, field)				\
	part_stat_add(cpu, gendiskp, field, 1)
#define part_stat_sub(cpu, gendiskp, field, subnd)			\
	part_stat_add(cpu, gendiskp, field, -subnd)

static inline void part_inc_in_flight(struct hd_struct *part)
{
	part->in_flight++;
	if (part->partno)
		part_to_disk(part)->part0.in_flight++;
}

static inline void part_dec_in_flight(struct hd_struct *part)
{
	part->in_flight--;
	if (part->partno)
		part_to_disk(part)->part0.in_flight--;
}

/* drivers/block/ll_rw_blk.c */
extern void part_round_stats(int cpu, struct hd_struct *part);

/* drivers/block/genhd.c */
extern int get_blkdev_list(char *, int);
extern void add_disk(struct gendisk *disk);
extern void del_gendisk(struct gendisk *gp);
extern void unlink_gendisk(struct gendisk *gp);
extern struct gendisk *get_gendisk(dev_t dev, int *partno);
extern struct block_device *bdget_disk(struct gendisk *disk, int partno);

extern void set_device_ro(struct block_device *bdev, int flag);
extern void set_disk_ro(struct gendisk *disk, int flag);

static inline int get_disk_ro(struct gendisk *disk)
{
	return disk->part0.policy;
}

/* drivers/char/random.c */
extern void add_disk_randomness(struct gendisk *disk);
extern void rand_initialize_disk(struct gendisk *disk);

static inline sector_t get_start_sect(struct block_device *bdev)
{
	return bdev->bd_part->start_sect;
}
static inline sector_t get_capacity(struct gendisk *disk)
{
	return disk->part0.nr_sects;
}
static inline void set_capacity(struct gendisk *disk, sector_t size)
{
	disk->part0.nr_sects = size;
}

#ifdef CONFIG_SOLARIS_X86_PARTITION

#define SOLARIS_X86_NUMSLICE	16
#define SOLARIS_X86_VTOC_SANE	(0x600DDEEEUL)

struct solaris_x86_slice {
	__le16 s_tag;		/* ID tag of partition */
	__le16 s_flag;		/* permission flags */
	__le32 s_start;		/* start sector no of partition */
	__le32 s_size;		/* # of blocks in partition */
};

struct solaris_x86_vtoc {
	unsigned int v_bootinfo[3];	/* info needed by mboot (unsupported) */
	__le32 v_sanity;		/* to verify vtoc sanity */
	__le32 v_version;		/* layout version */
	char	v_volume[8];		/* volume name */
	__le16	v_sectorsz;		/* sector size in bytes */
	__le16	v_nparts;		/* number of partitions */
	unsigned int v_reserved[10];	/* free space */
	struct solaris_x86_slice
		v_slice[SOLARIS_X86_NUMSLICE]; /* slice headers */
	unsigned int timestamp[SOLARIS_X86_NUMSLICE]; /* timestamp (unsupported) */
	char	v_asciilabel[128];	/* for compatibility */
};

#endif /* CONFIG_SOLARIS_X86_PARTITION */

#ifdef CONFIG_BSD_DISKLABEL
/*
 * BSD disklabel support by Yossi Gottlieb <yogo@math.tau.ac.il>
 * updated by Marc Espie <Marc.Espie@openbsd.org>
 */

/* check against BSD src/sys/sys/disklabel.h for consistency */

#define BSD_DISKMAGIC	(0x82564557UL)	/* The disk magic number */
#define BSD_MAXPARTITIONS	16
#define OPENBSD_MAXPARTITIONS	16
#define BSD_FS_UNUSED		0	/* disklabel unused partition entry ID */
struct bsd_disklabel {
	__le32	d_magic;		/* the magic number */
	__s16	d_type;			/* drive type */
	__s16	d_subtype;		/* controller/d_type specific */
	char	d_typename[16];		/* type name, e.g. "eagle" */
	char	d_packname[16];			/* pack identifier */ 
	__u32	d_secsize;		/* # of bytes per sector */
	__u32	d_nsectors;		/* # of data sectors per track */
	__u32	d_ntracks;		/* # of tracks per cylinder */
	__u32	d_ncylinders;		/* # of data cylinders per unit */
	__u32	d_secpercyl;		/* # of data sectors per cylinder */
	__u32	d_secperunit;		/* # of data sectors per unit */
	__u16	d_sparespertrack;	/* # of spare sectors per track */
	__u16	d_sparespercyl;		/* # of spare sectors per cylinder */
	__u32	d_acylinders;		/* # of alt. cylinders per unit */
	__u16	d_rpm;			/* rotational speed */
	__u16	d_interleave;		/* hardware sector interleave */
	__u16	d_trackskew;		/* sector 0 skew, per track */
	__u16	d_cylskew;		/* sector 0 skew, per cylinder */
	__u32	d_headswitch;		/* head switch time, usec */
	__u32	d_trkseek;		/* track-to-track seek, usec */
	__u32	d_flags;		/* generic flags */
#define NDDATA 5
	__u32	d_drivedata[NDDATA];	/* drive-type specific information */
#define NSPARE 5
	__u32	d_spare[NSPARE];	/* reserved for future use */
	__le32	d_magic2;		/* the magic number (again) */
	__le16	d_checksum;		/* xor of data incl. partitions */

			/* filesystem and partition information: */
	__le16	d_npartitions;		/* number of partitions in following */
	__le32	d_bbsize;		/* size of boot area at sn0, bytes */
	__le32	d_sbsize;		/* max size of fs superblock, bytes */
	struct	bsd_partition {		/* the partition table */
		__le32	p_size;		/* number of sectors in partition */
		__le32	p_offset;	/* starting sector */
		__le32	p_fsize;	/* filesystem basic fragment size */
		__u8	p_fstype;	/* filesystem type, see below */
		__u8	p_frag;		/* filesystem fragments per block */
		__le16	p_cpg;		/* filesystem cylinders per group */
	} d_partitions[BSD_MAXPARTITIONS];	/* actually may be more */
};

#endif	/* CONFIG_BSD_DISKLABEL */

#ifdef CONFIG_UNIXWARE_DISKLABEL
/*
 * Unixware slices support by Andrzej Krzysztofowicz <ankry@mif.pg.gda.pl>
 * and Krzysztof G. Baranowski <kgb@knm.org.pl>
 */

#define UNIXWARE_DISKMAGIC     (0xCA5E600DUL)	/* The disk magic number */
#define UNIXWARE_DISKMAGIC2    (0x600DDEEEUL)	/* The slice table magic nr */
#define UNIXWARE_NUMSLICE      16
#define UNIXWARE_FS_UNUSED     0		/* Unused slice entry ID */

struct unixware_slice {
	__le16   s_label;	/* label */
	__le16   s_flags;	/* permission flags */
	__le32   start_sect;	/* starting sector */
	__le32   nr_sects;	/* number of sectors in slice */
};

struct unixware_disklabel {
	__le32   d_type;               	/* drive type */
	__le32   d_magic;                /* the magic number */
	__le32   d_version;              /* version number */
	char    d_serial[12];           /* serial number of the device */
	__le32   d_ncylinders;           /* # of data cylinders per device */
	__le32   d_ntracks;              /* # of tracks per cylinder */
	__le32   d_nsectors;             /* # of data sectors per track */
	__le32   d_secsize;              /* # of bytes per sector */
	__le32   d_part_start;           /* # of first sector of this partition */
	__le32   d_unknown1[12];         /* ? */
 	__le32	d_alt_tbl;              /* byte offset of alternate table */
 	__le32	d_alt_len;              /* byte length of alternate table */
 	__le32	d_phys_cyl;             /* # of physical cylinders per device */
 	__le32	d_phys_trk;             /* # of physical tracks per cylinder */
 	__le32	d_phys_sec;             /* # of physical sectors per track */
 	__le32	d_phys_bytes;           /* # of physical bytes per sector */
 	__le32	d_unknown2;             /* ? */
	__le32   d_unknown3;             /* ? */
	__le32	d_pad[8];               /* pad */

	struct unixware_vtoc {
		__le32	v_magic;		/* the magic number */
		__le32	v_version;		/* version number */
		char	v_name[8];		/* volume name */
		__le16	v_nslices;		/* # of slices */
		__le16	v_unknown1;		/* ? */
		__le32	v_reserved[10];		/* reserved */
		struct unixware_slice
			v_slice[UNIXWARE_NUMSLICE];	/* slice headers */
	} vtoc;

};  /* 408 */

#endif /* CONFIG_UNIXWARE_DISKLABEL */

#ifdef CONFIG_MINIX_SUBPARTITION
#   define MINIX_NR_SUBPARTITIONS  4
#endif /* CONFIG_MINIX_SUBPARTITION */

#define ADDPART_FLAG_NONE	0
#define ADDPART_FLAG_RAID	1
#define ADDPART_FLAG_WHOLEDISK	2

extern int blk_alloc_devt(struct hd_struct *part, dev_t *devt);
extern void blk_free_devt(dev_t devt);
extern dev_t blk_lookup_devt(const char *name, int partno);
extern char *disk_name (struct gendisk *hd, int partno, char *buf);

extern int disk_expand_part_tbl(struct gendisk *disk, int target);
extern int rescan_partitions(struct gendisk *disk, struct block_device *bdev);
extern struct hd_struct * __must_check add_partition(struct gendisk *disk,
						     int partno, sector_t start,
						     sector_t len, int flags);
extern void delete_partition(struct gendisk *, int);
extern void printk_all_partitions(void);

extern struct gendisk *alloc_disk_node(int minors, int node_id);
extern struct gendisk *alloc_disk(int minors);
extern struct kobject *get_disk(struct gendisk *disk);
extern void put_disk(struct gendisk *disk);
extern void blk_register_region(dev_t devt, unsigned long range,
			struct module *module,
			struct kobject *(*probe)(dev_t, int *, void *),
			int (*lock)(dev_t, void *),
			void *data);
extern void blk_unregister_region(dev_t devt, unsigned long range);

extern ssize_t part_size_show(struct device *dev,
			      struct device_attribute *attr, char *buf);
extern ssize_t part_stat_show(struct device *dev,
			      struct device_attribute *attr, char *buf);
#ifdef CONFIG_FAIL_MAKE_REQUEST
extern ssize_t part_fail_show(struct device *dev,
			      struct device_attribute *attr, char *buf);
extern ssize_t part_fail_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count);
#endif /* CONFIG_FAIL_MAKE_REQUEST */

#else /* CONFIG_BLOCK */

static inline void printk_all_partitions(void) { }

static inline dev_t blk_lookup_devt(const char *name, int partno)
{
	dev_t devt = MKDEV(0, 0);
	return devt;
}

#endif /* CONFIG_BLOCK */

#endif /* _LINUX_GENHD_H */
