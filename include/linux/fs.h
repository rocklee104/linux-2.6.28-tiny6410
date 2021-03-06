﻿#ifndef _LINUX_FS_H
#define _LINUX_FS_H

/*
 * This file has definitions for some important file table
 * structures etc.
 */

#include <linux/limits.h>
#include <linux/ioctl.h>

/*
 * It's silly to have NR_OPEN bigger than NR_FILE, but you can change
 * the file limit at runtime and only root can increase the per-process
 * nr_file rlimit, so it's safe to set up a ridiculously high absolute
 * upper limit on files-per-process.
 *
 * Some programs (notably those using select()) may have to be
 * recompiled to take full advantage of the new limits..
 */

/* Fixed constants first: */
#undef NR_OPEN
extern int sysctl_nr_open;
#define INR_OPEN 1024		/* Initial setting for nfile rlimits */

#define BLOCK_SIZE_BITS 10
#define BLOCK_SIZE (1<<BLOCK_SIZE_BITS)

#define SEEK_SET	0	/* seek relative to beginning of file */
#define SEEK_CUR	1	/* seek relative to current file position */
#define SEEK_END	2	/* seek relative to end of file */
#define SEEK_MAX	SEEK_END

/* And dynamically-tunable limits and defaults: */
struct files_stat_struct {
	int nr_files;		/* read only */
	int nr_free_files;	/* read only */
	int max_files;		/* tunable */
};
extern struct files_stat_struct files_stat;
extern int get_max_files(void);

struct inodes_stat_t {
	int nr_inodes;
	int nr_unused;
	int dummy[5];		/* padding for sysctl ABI compatibility */
};
extern struct inodes_stat_t inodes_stat;

extern int leases_enable, lease_break_time;

#ifdef CONFIG_DNOTIFY
extern int dir_notify_enable;
#endif

#define NR_FILE  8192	/* this can well be larger on a larger system */

#define MAY_EXEC 1
#define MAY_WRITE 2
#define MAY_READ 4
#define MAY_APPEND 8
#define MAY_ACCESS 16
#define MAY_OPEN 32

/* file is open for reading */
#define FMODE_READ		((__force fmode_t)1)
/* file is open for writing */
#define FMODE_WRITE		((__force fmode_t)2)
/* file is seekable */
#define FMODE_LSEEK		((__force fmode_t)4)
/* file can be accessed using pread/pwrite */
#define FMODE_PREAD		((__force fmode_t)8)
#define FMODE_PWRITE		FMODE_PREAD	/* These go hand in hand */
/* File is opened for execution with sys_execve / sys_uselib */
#define FMODE_EXEC		((__force fmode_t)16)
/* File is opened with O_NDELAY (only set for block devices) */
#define FMODE_NDELAY		((__force fmode_t)32)
/* File is opened with O_EXCL (only set for block devices) */
#define FMODE_EXCL		((__force fmode_t)64)
/* File is opened using open(.., 3, ..) and is writeable only for ioctls
   (specialy hack for floppy.c) */
#define FMODE_WRITE_IOCTL	((__force fmode_t)128)

#define RW_MASK		1
#define RWA_MASK	2
#define READ 0
#define WRITE 1
#define READA 2		/* read-ahead  - don't block if no resources */
#define SWRITE 3	/* for ll_rw_block() - wait for buffer lock */
#define READ_SYNC	(READ | (1 << BIO_RW_SYNC))
#define READ_META	(READ | (1 << BIO_RW_META))
#define WRITE_SYNC	(WRITE | (1 << BIO_RW_SYNC))
#define SWRITE_SYNC	(SWRITE | (1 << BIO_RW_SYNC))
#define WRITE_BARRIER	(WRITE | (1 << BIO_RW_BARRIER))
#define DISCARD_NOBARRIER (1 << BIO_RW_DISCARD)
#define DISCARD_BARRIER ((1 << BIO_RW_DISCARD) | (1 << BIO_RW_BARRIER))

#define SEL_IN		1
#define SEL_OUT		2
#define SEL_EX		4

/* public flags for file_system_type */
/* 对于需要磁盘的文件系统,需要在file_system_type.flag中赋值FS_REQUIRES_DEV */
#define FS_REQUIRES_DEV 1
#define FS_BINARY_MOUNTDATA 2
#define FS_HAS_SUBTYPE 4
#define FS_REVAL_DOT	16384	/* Check the paths ".", ".." for staleness */
#define FS_RENAME_DOES_D_MOVE	32768	/* FS will handle d_move()
					 * during rename() internally.
					 */

/*
 * These are the fs-independent mount-flags: up to 32 flags are supported
 */
/* 下面这些标志是用户空间传递下来的标志,不会写入mnt->mnt_flags */
/* 指定文件系统为只读 */
#define MS_RDONLY	 1	/* Mount read-only */
/* 执行程序时，不遵照set-user-ID和set-group-ID位 */
#define MS_NOSUID	 2	/* Ignore suid and sgid bits */
/* 不允许访问设备文件 */
#define MS_NODEV	 4	/* Disallow access to device special files */
/* 不允许在挂上的文件系统上执行程序 */
#define MS_NOEXEC	 8	/* Disallow program execution */
/* 同步文件的更新 */
#define MS_SYNCHRONOUS	16	/* Writes are synced at once */
/* 重新加载文件系统。这允许你改变现存文件系统的mountflag和数据，而无需使用先卸载，再挂上文件系统的方式 */
#define MS_REMOUNT	32	/* Alter flags of a mounted FS */
/* 允许在文件上执行强制锁 */
#define MS_MANDLOCK	64	/* Allow mandatory locks on an FS */
/* 同步目录的更新 */
#define MS_DIRSYNC	128	/* Directory modifications are synchronous */
/* 不要更新文件上的访问时间 */
#define MS_NOATIME	1024	/* Do not update access times. */
/* 不允许更新目录上的访问时间。*/
#define MS_NODIRATIME	2048	/* Do not update directory access times */
//执行bind挂载，使文件或者子目录树在文件系统内的另一个点上可视。
/* 1<<12 */
#define MS_BIND		4096
/* 移动子目录树 */
/* 1<<13 */
#define MS_MOVE		8192
/* 1<<14 */
#define MS_REC		16384
/* 1<<15 */
#define MS_VERBOSE	32768	/* War is peace. Verbosity is silence.
				   MS_VERBOSE is deprecated. */
#define MS_SILENT	32768
#define MS_POSIXACL	(1<<16)	/* VFS does not apply the umask */
#define MS_UNBINDABLE	(1<<17)	/* change to unbindable */
#define MS_PRIVATE	(1<<18)	/* change to private */
#define MS_SLAVE	(1<<19)	/* change to slave */
#define MS_SHARED	(1<<20)	/* change to shared */
#define MS_RELATIME	(1<<21)	/* Update atime relative to mtime/ctime. */
#define MS_KERNMOUNT	(1<<22) /* this is a kern_mount call */
#define MS_I_VERSION	(1<<23) /* Update inode I_version field */
#define MS_ACTIVE	(1<<30)
#define MS_NOUSER	(1<<31)

/*
 * Superblock flags that can be altered by MS_REMOUNT
 */
#define MS_RMT_MASK	(MS_RDONLY|MS_SYNCHRONOUS|MS_MANDLOCK|MS_I_VERSION)

/*
 * Old magic mount flag and mask
 */
#define MS_MGC_VAL 0xC0ED0000
#define MS_MGC_MSK 0xffff0000

/* Inode flags - they have nothing to superblock flags now */

#define S_SYNC		1	/* Writes are synced at once */
#define S_NOATIME	2	/* Do not update access times */
#define S_APPEND	4	/* Append-only file */
#define S_IMMUTABLE	8	/* Immutable file */
#define S_DEAD		16	/* removed, but still open directory */
#define S_NOQUOTA	32	/* Inode is not counted to quota */
#define S_DIRSYNC	64	/* Directory modifications are synchronous */
#define S_NOCMTIME	128	/* Do not update file c/mtime */
#define S_SWAPFILE	256	/* Do not truncate: swapon got its bmaps */
#define S_PRIVATE	512	/* Inode is fs-internal */

/*
 * Note that nosuid etc flags are inode-specific: setting some file-system
 * flags just means all the inodes inherit those flags by default. It might be
 * possible to override it selectively if you really wanted to with some
 * ioctl() that is not currently implemented.
 *
 * Exception: MS_RDONLY is always applied to the entire file system.
 *
 * Unfortunately, it is possible to change a filesystems flags with it mounted
 * with files in use.  This means that all of the inodes will not have their
 * i_flags updated.  Hence, i_flags no longer inherit the superblock mount
 * flags, so these have to be checked separately. -- rmk@arm.uk.linux.org
 */
#define __IS_FLG(inode,flg) ((inode)->i_sb->s_flags & (flg))

#define IS_RDONLY(inode) ((inode)->i_sb->s_flags & MS_RDONLY)
#define IS_SYNC(inode)		(__IS_FLG(inode, MS_SYNCHRONOUS) || \
					((inode)->i_flags & S_SYNC))
#define IS_DIRSYNC(inode)	(__IS_FLG(inode, MS_SYNCHRONOUS|MS_DIRSYNC) || \
					((inode)->i_flags & (S_SYNC|S_DIRSYNC)))
#define IS_MANDLOCK(inode)	__IS_FLG(inode, MS_MANDLOCK)
#define IS_NOATIME(inode)   __IS_FLG(inode, MS_RDONLY|MS_NOATIME)
#define IS_I_VERSION(inode)   __IS_FLG(inode, MS_I_VERSION)

#define IS_NOQUOTA(inode)	((inode)->i_flags & S_NOQUOTA)
#define IS_APPEND(inode)	((inode)->i_flags & S_APPEND)
#define IS_IMMUTABLE(inode)	((inode)->i_flags & S_IMMUTABLE)
#define IS_POSIXACL(inode)	__IS_FLG(inode, MS_POSIXACL)

#define IS_DEADDIR(inode)	((inode)->i_flags & S_DEAD)
#define IS_NOCMTIME(inode)	((inode)->i_flags & S_NOCMTIME)
#define IS_SWAPFILE(inode)	((inode)->i_flags & S_SWAPFILE)
#define IS_PRIVATE(inode)	((inode)->i_flags & S_PRIVATE)

/* the read-only stuff doesn't really belong here, but any other place is
   probably as bad and I don't want to create yet another include file. */

#define BLKROSET   _IO(0x12,93)	/* set device read-only (0 = read-write) */
#define BLKROGET   _IO(0x12,94)	/* get read-only status (0 = read_write) */
#define BLKRRPART  _IO(0x12,95)	/* re-read partition table */
#define BLKGETSIZE _IO(0x12,96)	/* return device size /512 (long *arg) */
#define BLKFLSBUF  _IO(0x12,97)	/* flush buffer cache */
#define BLKRASET   _IO(0x12,98)	/* set read ahead for block device */
#define BLKRAGET   _IO(0x12,99)	/* get current read ahead setting */
#define BLKFRASET  _IO(0x12,100)/* set filesystem (mm/filemap.c) read-ahead */
#define BLKFRAGET  _IO(0x12,101)/* get filesystem (mm/filemap.c) read-ahead */
#define BLKSECTSET _IO(0x12,102)/* set max sectors per request (ll_rw_blk.c) */
#define BLKSECTGET _IO(0x12,103)/* get max sectors per request (ll_rw_blk.c) */
#define BLKSSZGET  _IO(0x12,104)/* get block device sector size */
#if 0
#define BLKPG      _IO(0x12,105)/* See blkpg.h */

/* Some people are morons.  Do not use sizeof! */

#define BLKELVGET  _IOR(0x12,106,size_t)/* elevator get */
#define BLKELVSET  _IOW(0x12,107,size_t)/* elevator set */
/* This was here just to show that the number is taken -
   probably all these _IO(0x12,*) ioctls should be moved to blkpg.h. */
#endif
/* A jump here: 108-111 have been used for various private purposes. */
#define BLKBSZGET  _IOR(0x12,112,size_t)
#define BLKBSZSET  _IOW(0x12,113,size_t)
#define BLKGETSIZE64 _IOR(0x12,114,size_t)	/* return device size in bytes (u64 *arg) */
#define BLKTRACESETUP _IOWR(0x12,115,struct blk_user_trace_setup)
#define BLKTRACESTART _IO(0x12,116)
#define BLKTRACESTOP _IO(0x12,117)
#define BLKTRACETEARDOWN _IO(0x12,118)
#define BLKDISCARD _IO(0x12,119)

#define BMAP_IOCTL 1		/* obsolete - kept for compatibility */
#define FIBMAP	   _IO(0x00,1)	/* bmap access */
#define FIGETBSZ   _IO(0x00,2)	/* get the block size used for bmap */

#define	FS_IOC_GETFLAGS			_IOR('f', 1, long)
#define	FS_IOC_SETFLAGS			_IOW('f', 2, long)
#define	FS_IOC_GETVERSION		_IOR('v', 1, long)
#define	FS_IOC_SETVERSION		_IOW('v', 2, long)
#define FS_IOC_FIEMAP			_IOWR('f', 11, struct fiemap)
#define FS_IOC32_GETFLAGS		_IOR('f', 1, int)
#define FS_IOC32_SETFLAGS		_IOW('f', 2, int)
#define FS_IOC32_GETVERSION		_IOR('v', 1, int)
#define FS_IOC32_SETVERSION		_IOW('v', 2, int)

/*
 * Inode flags (FS_IOC_GETFLAGS / FS_IOC_SETFLAGS)
 */
#define	FS_SECRM_FL			0x00000001 /* Secure deletion */
#define	FS_UNRM_FL			0x00000002 /* Undelete */
#define	FS_COMPR_FL			0x00000004 /* Compress file */
#define FS_SYNC_FL			0x00000008 /* Synchronous updates */
#define FS_IMMUTABLE_FL			0x00000010 /* Immutable file */
#define FS_APPEND_FL			0x00000020 /* writes to file may only append */
#define FS_NODUMP_FL			0x00000040 /* do not dump file */
#define FS_NOATIME_FL			0x00000080 /* do not update atime */
/* Reserved for compression usage... */
#define FS_DIRTY_FL			0x00000100
#define FS_COMPRBLK_FL			0x00000200 /* One or more compressed clusters */
#define FS_NOCOMP_FL			0x00000400 /* Don't compress */
#define FS_ECOMPR_FL			0x00000800 /* Compression error */
/* End compression flags --- maybe not all used */
#define FS_BTREE_FL			0x00001000 /* btree format dir */
#define FS_INDEX_FL			0x00001000 /* hash-indexed directory */
#define FS_IMAGIC_FL			0x00002000 /* AFS directory */
#define FS_JOURNAL_DATA_FL		0x00004000 /* Reserved for ext3 */
#define FS_NOTAIL_FL			0x00008000 /* file tail should not be merged */
#define FS_DIRSYNC_FL			0x00010000 /* dirsync behaviour (directories only) */
#define FS_TOPDIR_FL			0x00020000 /* Top of directory hierarchies*/
#define FS_EXTENT_FL			0x00080000 /* Extents */
#define FS_DIRECTIO_FL			0x00100000 /* Use direct i/o */
#define FS_RESERVED_FL			0x80000000 /* reserved for ext2 lib */

#define FS_FL_USER_VISIBLE		0x0003DFFF /* User visible flags */
#define FS_FL_USER_MODIFIABLE		0x000380FF /* User modifiable flags */


#define SYNC_FILE_RANGE_WAIT_BEFORE	1
#define SYNC_FILE_RANGE_WRITE		2
#define SYNC_FILE_RANGE_WAIT_AFTER	4

#ifdef __KERNEL__

#include <linux/linkage.h>
#include <linux/wait.h>
#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/dcache.h>
#include <linux/path.h>
#include <linux/stat.h>
#include <linux/cache.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/radix-tree.h>
#include <linux/prio_tree.h>
#include <linux/init.h>
#include <linux/pid.h>
#include <linux/mutex.h>
#include <linux/capability.h>
#include <linux/semaphore.h>
#include <linux/fiemap.h>

#include <asm/atomic.h>
#include <asm/byteorder.h>

struct export_operations;
struct hd_geometry;
struct iovec;
struct nameidata;
struct kiocb;
struct pipe_inode_info;
struct poll_table_struct;
struct kstatfs;
struct vm_area_struct;
struct vfsmount;

extern void __init inode_init(void);
extern void __init inode_init_early(void);
extern void __init files_init(unsigned long);

struct buffer_head;
typedef int (get_block_t)(struct inode *inode, sector_t iblock,
			struct buffer_head *bh_result, int create);
typedef void (dio_iodone_t)(struct kiocb *iocb, loff_t offset,
			ssize_t bytes, void *private);

/*
 * Attribute flags.  These should be or-ed together to figure out what
 * has been changed!
 */
#define ATTR_MODE	(1 << 0)
#define ATTR_UID	(1 << 1)
#define ATTR_GID	(1 << 2)
#define ATTR_SIZE	(1 << 3)
#define ATTR_ATIME	(1 << 4)
#define ATTR_MTIME	(1 << 5)
#define ATTR_CTIME	(1 << 6)
#define ATTR_ATIME_SET	(1 << 7)
#define ATTR_MTIME_SET	(1 << 8)
#define ATTR_FORCE	(1 << 9) /* Not a change, but a change it */
#define ATTR_ATTR_FLAG	(1 << 10)
#define ATTR_KILL_SUID	(1 << 11)
#define ATTR_KILL_SGID	(1 << 12)
#define ATTR_FILE	(1 << 13)
#define ATTR_KILL_PRIV	(1 << 14)
#define ATTR_OPEN	(1 << 15) /* Truncating from open(O_TRUNC) */
#define ATTR_TIMES_SET	(1 << 16)

/*
 * This is the Inode Attributes structure, used for notify_change().  It
 * uses the above definitions as flags, to know which values have changed.
 * Also, in this manner, a Filesystem can look at only the values it cares
 * about.  Basically, these are the attributes that the VFS layer can
 * request to change from the FS layer.
 *
 * Derek Atkins <warlord@MIT.EDU> 94-10-20
 */
struct iattr {
	unsigned int	ia_valid;
	umode_t		ia_mode;
	uid_t		ia_uid;
	gid_t		ia_gid;
	loff_t		ia_size;
	struct timespec	ia_atime;
	struct timespec	ia_mtime;
	struct timespec	ia_ctime;

	/*
	 * Not an attribute, but an auxilary info for filesystems wanting to
	 * implement an ftruncate() like method.  NOTE: filesystem should
	 * check for (ia_valid & ATTR_FILE), and not for (ia_file != NULL).
	 */
	struct file	*ia_file;
};

/*
 * Includes for diskquotas.
 */
#include <linux/quota.h>

/**
 * enum positive_aop_returns - aop return codes with specific semantics
 *
 * @AOP_WRITEPAGE_ACTIVATE: Informs the caller that page writeback has
 * 			    completed, that the page is still locked, and
 * 			    should be considered active.  The VM uses this hint
 * 			    to return the page to the active list -- it won't
 * 			    be a candidate for writeback again in the near
 * 			    future.  Other callers must be careful to unlock
 * 			    the page if they get this return.  Returned by
 * 			    writepage();
 *
 * @AOP_TRUNCATED_PAGE: The AOP method that was handed a locked page has
 *  			unlocked it and the page might have been truncated.
 *  			The caller should back up to acquiring a new page and
 *  			trying again.  The aop will be taking reasonable
 *  			precautions not to livelock.  If the caller held a page
 *  			reference, it should drop it before retrying.  Returned
 *  			by readpage().
 *
 * address_space_operation functions return these large constants to indicate
 * special semantics to the caller.  These are much larger than the bytes in a
 * page to allow for functions that return the number of bytes operated on in a
 * given page.
 */

enum positive_aop_returns {
	AOP_WRITEPAGE_ACTIVATE	= 0x80000,
	AOP_TRUNCATED_PAGE	= 0x80001,
};

#define AOP_FLAG_UNINTERRUPTIBLE	0x0001 /* will not do a short write */
#define AOP_FLAG_CONT_EXPAND		0x0002 /* called from cont_expand */
#define AOP_FLAG_NOFS			0x0004 /* used by filesystem to direct
						* helper code (eg buffer layer)
						* to clear GFP_FS from alloc */

/*
 * oh the beauties of C type declarations.
 */
struct page;
struct address_space;
struct writeback_control;

/* IO向量迭代器 */
struct iov_iter {
	/* 指向剩余IO向量数组的指针 */
	const struct iovec *iov;
	/* 剩余IO向量数组的有效项数 */
	unsigned long nr_segs;
	/* 在当前IO向量中的偏移 */
	size_t iov_offset;
	/* 剩余IO向量数组的有效字节数 */
	size_t count;
};

size_t iov_iter_copy_from_user_atomic(struct page *page,
		struct iov_iter *i, unsigned long offset, size_t bytes);
size_t iov_iter_copy_from_user(struct page *page,
		struct iov_iter *i, unsigned long offset, size_t bytes);
void iov_iter_advance(struct iov_iter *i, size_t bytes);
int iov_iter_fault_in_readable(struct iov_iter *i, size_t bytes);
size_t iov_iter_single_seg_count(struct iov_iter *i);

static inline void iov_iter_init(struct iov_iter *i,
			const struct iovec *iov, unsigned long nr_segs,
			size_t count, size_t written)
{
	i->iov = iov;
	i->nr_segs = nr_segs;
	i->iov_offset = 0;
	/* 将count域初始化为所有字节的总数,然后调用iov_iter_advance向前推进,跳过已处理的字节数 */
	i->count = count + written;

	iov_iter_advance(i, written);
}

static inline size_t iov_iter_count(struct iov_iter *i)
{
	return i->count;
}

/*
 * "descriptor" for what we're up to with a read.
 * This allows us to use the same read code yet
 * have multiple different users of the data that
 * we read from a file.
 *
 * The simplest case just copies the data to user
 * mode.
 */
/* 引入这个数据结构是为了代码重用,对于普通读取,套接字读取都适用 */
typedef struct {
	/* 已经写入用户buffer的长度 */
	size_t written;
	/* 读取后还剩余的长度 */
	size_t count;
	union {
		/* 用户空间缓冲区指针 */
		char __user *buf;
		/* 被其他用户,如socket,自己定义和解释 */
		void *data;
	} arg;
	/* 错误码 */
	int error;
} read_descriptor_t;

typedef int (*read_actor_t)(read_descriptor_t *, struct page *,
		unsigned long, unsigned long);

struct address_space_operations {
	/* 在调用这个函数之前,内存页面中已经包含了文件的最新数据 */
	int (*writepage)(struct page *page, struct writeback_control *wbc);
	/* 具体文件系统在该函数的实现中通常调用mpage_readpage或者block_read_full_page */
	int (*readpage)(struct file *, struct page *);
	/*
	 * 这个函数是可选的,只在等待回写完成时.对于设置了PG_Writeback标记的页面调用.
	 * 它一般"泄流"设备.让I/O数据传输尽快完成.
	 */
	void (*sync_page)(struct page *);

	/* Write back some dirty pages from this mapping. */
	/*
	 * 如果同步模式为WBC_SYNC_ALL,则回写控制会指定要写出页面的范围.
	 * 否则.如果是WBC_SYNC_NONE.那么回写控制会给定要尽量写出的页面数目.
	 */
	int (*writepages)(struct address_space *, struct writeback_control *);

	/* Set a page dirty.  Return true if this dirtied it */
	/*
	 * 设置页面为脏,特别用在具体文件系统的地址空间页面中有关联的私有数据,
	 * 并且这些数据在页面"弄脏"的同时被更新的场合
	 */
	int (*set_page_dirty)(struct page *page);

	/* 一般用在预读的场合,所以读取错误可以被忽略 */
	int (*readpages)(struct file *filp, struct address_space *mapping,
			struct list_head *pages, unsigned nr_pages);

	/*
	 * 具体文件系统要负责确保本次写操作能够完成,包括必要时分配空间,在非覆写时先从磁盘读取数据到页面等.
	 *
	 * pagep是输入/输出参数,如果输入为NULL,则需要有具体文件系统分配页面,并通过它返回加过锁的页面.
	 * 以便调用者安全写入数据.
	 *
	 * fsdata参数为返回指向由具体文件系统解释的数据结构.它被传递给write_end函数.
	 * 例如,reiserfs文件系统使用它传递特定的标志,表示是否在cont_expand(expanding truncate)
	 * 环境下被调用,如果是这样,则用它通知write_end做协调处理.这个函数在成功时返回0;否则返回负的错误码.
	 * 如果出错,write_end函数不会被调用.
	 */
	int (*write_begin)(struct file *, struct address_space *mapping,
				loff_t pos, unsigned len, unsigned flags,
				struct page **pagep, void **fsdata);
	/*
	 * VFS通过调用write_end告诉具体文件系统,数据已经被复制到页面,现在可以被提交到磁盘上了.
	 * len为要写的字节数,即最初传递给write_begin的长度.
	 * copied为已写的字节数,即已从用户缓冲区复制到页面的字节数.
	 *
	 * 具体文件系统需要负责解锁页面,释放它的引用计数.并更新i_size.失败返回负的错误码;
	 * 否则返回能够被复制到页面缓存页面的字节数(<= copied)
	 */
	int (*write_end)(struct file *, struct address_space *mapping,
				loff_t pos, unsigned len, unsigned copied,
				struct page *page, void *fsdata);

	/* Unfortunately this kludge is needed for FIBMAP. Don't use it */
	/*
 	 * 将文件中的逻辑块扇区编号映射为对应设备上的物理块扇区编号.这个回调函数被用于
 	 * FIBMAP ioctl和交换文件(swap file)一起工作。要交换到一个文件,文件必须在块设备上
 	 * 有稳定的映射.交换系统并不通过文件系统,而是直接使用bmap找到并使用文件数据块在
 	 * 设备上的地址.
	 */
	sector_t (*bmap)(struct address_space *, sector_t);
	/*
	 * 使某个页面全部或部分失效,被用在截断文件时.
	 * 第一个参数为指向页面描述符的指针;
	 * 第二个参数为要使之失效的起始字节偏移,为0表示使整个页面失效.
	 */
	void (*invalidatepage) (struct page *, unsigned long);
	/* 被日志文件系统使用以准备释放页面 */
	int (*releasepage) (struct page *, gfp_t);
	/*
	 * 被通用read/write函数调用执行direct_IO,即I/O请求会绕过页面缓存(PageCache),
	 * 在磁盘与应用程序缓冲区之间进行直接数据传输.
	 * 第一个参数为读/写标志
	 * 第二个参数是指向内核I/O控制块的指针;
	 * 第三个参数是指向用户空间I/O数组的指针;
	 * 第四个参数为I/O起始偏移位置;
	 * 第五个参数为用户空间I/O数组的项数.
	 * 返回读到的字节数,或者负的错误码.
	 */
	ssize_t (*direct_IO)(int, struct kiocb *, const struct iovec *iov,
			loff_t offset, unsigned long nr_segs);
	/*
	 * 需要使用文件就地执行(eXecute In Place)的文件系统需要实现这个函数.
	 * 第一个参数为指向地址空间描述符的指针;
	 * 第二个参数为包含数据的页面在页面缓存中的索引;
	 * 第三个参数表示是否需要创建文件逻辑块到磁盘逻辑块的映射;
	 * 第四个和第五个参数均为输出参数,分别返回被映射到的页面指针,以及其页帧编号.
	 */
	int (*get_xip_mem)(struct address_space *, pgoff_t, int,
						void **, unsigned long *);
	/* migrate the contents of a page to the specified target */
	/*
	 * 将页面的内容移动到指定的目标
	 * 第一个参数为指向地址空间的指针;
	 * 第二个参数为指向新页面的指针;
	 * 第三个参数为指向旧页面的指针.这个函数被用于compact物理内存使用.如果需要重新定位一个页面
	 * (比如接收到信号表明内存卡即将出错),会换入一个新页面和一个旧页面到这个函数.而它要负责转移
	 * 所有的私有数据,以及更新引用计数.
	 */
	int (*migratepage) (struct address_space *,
			struct page *, struct page *);
	/* 在释放一个页面之前被调用,回写一个dirty页面 */
	int (*launder_page) (struct page *);
	/*
	 * 在处理缓冲读I/O请求时,被调用以判断要读取的这部分数据在页面中是否为最新.
	 * 第一个参数为指向页面描述符的指针;
	 * 第二个参数为指向读描述符结构的指针；
	 * 第三个参数为文件数据在页面中的起始偏移;
	 * 函数返回1表明页面相关数据已经被更新,否则返回0
	 */
	int (*is_partially_uptodate) (struct page *, read_descriptor_t *,
					unsigned long);
};

/*
 * pagecache_write_begin/pagecache_write_end must be used by general code
 * to write into the pagecache.
 */
int pagecache_write_begin(struct file *, struct address_space *mapping,
				loff_t pos, unsigned len, unsigned flags,
				struct page **pagep, void **fsdata);

int pagecache_write_end(struct file *, struct address_space *mapping,
				loff_t pos, unsigned len, unsigned copied,
				struct page *page, void *fsdata);

struct backing_dev_info;
struct address_space {
	/* 指向拥有该对象的inode的指针 */
	struct inode		*host;		/* owner: inode, block_device */
	/* 地址空间页面组织成radix树的形式,page_tree为树根 */
	struct radix_tree_root	page_tree;	/* radix tree of all pages */
	/*
	 * tree_lock is used to synchronise concurrent access and modification of the
	 * radix-tree, and to control access to the pagecache in general
	 */
	/* 保护radix tree的读写锁 */
	spinlock_t		tree_lock;	/* and lock protecting it */
	/* 地址空间中共享内存映射的个数 */
	unsigned int		i_mmap_writable;/* count VM_SHARED mappings */
	/*
	 * 为方便查找,一个共享映射文件的所有虚拟内存区间或私有映射文件的写时复制(COW)
	 * 虚拟内存空间被组织成一个radix优先查找树(priority search tree),文件地址空间的
	 * i_mmap域成为树根.
	 *
	 * 为方便页面回收.内核采用逆向映射技术,将内存区间(vm_area_struct)组织成radix
	 * 优先查找树形式,该域为树根,vm_area_struct结构的prio_tree_node为连入此树的连接件.
	 */
	struct prio_tree_root	i_mmap;		/* tree of private and shared mappings */
	/*
	 * 如果映射为非线性的,即文件页面在内存中并非顺序,则将内存区间(vm_area_struct)
	 * 组织成链表的形式,该域为表头,vm_area_struct结构的list为链入此表的连接件
	 */
	struct list_head	i_mmap_nonlinear;/*list VM_NONLINEAR mappings */
	/* 保护radix优先搜索树,非线性链表等的自旋锁 */
	spinlock_t		i_mmap_lock;	/* protect tree, count, list */
	/* 截断文件时使用的顺序计数器 */
	unsigned int		truncate_count;	/* Cover race condition with truncate */
	/* 地址空间中有多少个page */
	unsigned long		nrpages;	/* number of total pages */
	/*
	 * 为了不占用过多资源,linux内核将地址空间中页面回写的行为分为若干轮次,writeback_index
	 * 记录了上次回写操作的最后页面索引,下一次回写操作将从该位置开始.
	 */
	pgoff_t			writeback_index;/* writeback starts here */
	/* 地址空间(页面)的操作函数 */
	const struct address_space_operations *a_ops;	/* methods */
	/* 错误位和内存分配器的标志 */
	unsigned long		flags;		/* error bits/gfp mask */
	/*
	 * 指向这个地址空间的后备设备信息描述符,对于块设备主inode,指向块设备请求队列的内嵌
	 * 后备设备信息,某些磁盘文件系统的文件inode可能使用前者,而其他的可能自行定义.
	 */
	struct backing_dev_info *backing_dev_info; /* device readahead, etc */
	/* 用于保护地址空间私有链表的自旋锁 */
	spinlock_t		private_lock;	/* for use by the address_space */
	/*
	 * 地址空间私有链表的链表头,通常连接与inode相关的间接块的脏缓冲区的链表,
	 * 链表成员是bh->b_assoc_buffers.
	 */
	struct list_head	private_list;	/* ditto */
	/*
	 * 通常是指向间接块所在块设备的address_space对象的指针.每个inode代表一个文件,
	 * 并且拥有一个地址空间,在地址空间的page中包含了这个文件所有的数据块数据.然而
	 * 一些文件系统在访问文件的时候用到了间接块,这些间接块并不包含在数据块中.
	 * assoc_mapping用于保存这些被操作过的间接块.
	*/
	struct address_space	*assoc_mapping;	/* ditto */
} __attribute__((aligned(sizeof(long))));
	/*
	 * On most architectures that alignment is already the case; but
	 * must be enforced here for CRIS, to let the least signficant bit
	 * of struct page's "mapping" pointer be used for PAGE_MAPPING_ANON.
	 */

/* struct block_device可以表示一个完整的逻辑块设备,也可以表示逻辑块设备中的一个分区 */
struct block_device {
	dev_t			bd_dev;  /* not a kdev_t - it's a search key */
    /* bdevfs中的inode */
	struct inode *		bd_inode;	/* will die */
	/* 当前block_device被打开的次数 */
	int			bd_openers;
	struct mutex		bd_mutex;	/* open/close mutex */
	struct semaphore	bd_mount_sem;
	/* 链表头,链表成员是inode->i_devices, 该链表包含了表示该设备的设备特殊文件的所有inode */
	struct list_head	bd_inodes;
	/*
	 * 存放代表块设备持有者的线性地址。持有者并不是进行I/O数据传送的块设备驱动程序；
	 * 准确地说，它是一个内核组件，使用设备并拥有独一无二的特权
	 * (例如，它可以自由使用块设备描述符的bd_private字段)
	 * 典型地，块设备的持有者是安装在该设备上的文件系统.
	 * 当块设备文件被打开进行互斥访问时，另一个普遍的问题出现了:
	 * 持有者就是对应的文件对象.
	 */
	void *			bd_holder;
	/*
	 * 同一个内核组件可以多次调用bdclaim()函数，每调用一次都增加bd_holders的值。
	 * 为了释放块设备，内核组件必须调用bd_release()函数bd_holders次
	 */
	int			bd_holders;
#ifdef CONFIG_SYSFS
	struct list_head	bd_holder_list;
#endif
	/* 当block_device表示块设备中的某一分区时,bd_contains指向该分区所在的块设备 */
	struct block_device *	bd_contains;
	unsigned		bd_block_size;
	/* 当block_device表示一个完整的块设备时,bd_part指向该块设备的分区结构信息 */
	struct hd_struct *	bd_part;
	/* number of times partitions within this device have been opened. */
	/* 当前block_device代表一个磁盘时,这个磁盘中的分区被打开的次数 */
	unsigned		bd_part_count;
	/*
	 * 设置为1时,表示该分区在内核中的信息无效,因为磁盘上的分区已经改变,
	 * 下一次打开该设备时,将要重新扫描分区表.
	 */
	int			bd_invalidated;
	struct gendisk *	bd_disk;
	/* 链表元素,链表头是全局变量all_bdevs,用于记录所有的块设备 */
	struct list_head	bd_list;
	struct backing_dev_info *bd_inode_backing_dev_info;
	/*
	 * Private data.  You must have bd_claim'ed the block_device
	 * to use this.  NOTE:  bd_claim allows an owner to claim
	 * the same device multiple times, the owner must take special
	 * care to not mess up bd_private for that case.
	 */
	unsigned long		bd_private;
};

/*
 * Radix-tree tags, for tagging dirty and writeback pages within the pagecache
 * radix trees
 */
/* radix-tree 中radix_tree_node->tag两个64位的数组用于标记子节点的page状态 */
/* 表示页是脏的 */
#define PAGECACHE_TAG_DIRTY	0
/* 表示页正在被回写磁盘 */
#define PAGECACHE_TAG_WRITEBACK	1

int mapping_tagged(struct address_space *mapping, int tag);

/*
 * Might pages of this file be mapped into userspace?
 */
static inline int mapping_mapped(struct address_space *mapping)
{
	return	!prio_tree_empty(&mapping->i_mmap) ||
		!list_empty(&mapping->i_mmap_nonlinear);
}

/*
 * Might pages of this file have been modified in userspace?
 * Note that i_mmap_writable counts all VM_SHARED vmas: do_mmap_pgoff
 * marks vma as VM_SHARED if it is shared, and the file was opened for
 * writing i.e. vma may be mprotected writable even if now readonly.
 */
static inline int mapping_writably_mapped(struct address_space *mapping)
{
	return mapping->i_mmap_writable != 0;
}

/*
 * Use sequence counter to get consistent i_size on 32-bit processors.
 */
#if BITS_PER_LONG==32 && defined(CONFIG_SMP)
#include <linux/seqlock.h>
#define __NEED_I_SIZE_ORDERED
#define i_size_ordered_init(inode) seqcount_init(&inode->i_size_seqcount)
#else
#define i_size_ordered_init(inode) do { } while (0)
#endif

struct inode {
	/* 用于将inode接入hash表中 */
	struct hlist_node	i_hash;
   /*
    * i_list用作连接一下3个链表中的一个：
    * 1.inode_unused:inode未使用链表,inode是clean的,并且i_count为0,链表头是inode_unused
    * 2.inode_in_use:inode正在使用链表,inode是clean的,并且i_count>0,链表头是inode_in_use
    * 3.dirty链表:dirty的inode链表，链表头是sb->s_dirty
    *
    * 4.用于记录整个fs中的inode的链表
    * i_state的值等于I_DIRTY_SYNC，I_DIRTY_DATASYNC，I_DIRTY_PAGES其中一个，
    * 就表示inode dirty
    */
	struct list_head	i_list;
    /* 链表元素,用于记录整个fs的inode,链表头是super_block->s_inodes */
	struct list_head	i_sb_list;
	/* 链表头,成员是struct dentry中的d_alias */
	struct list_head	i_dentry;
	/* inode number */
	unsigned long		i_ino;
	/* 有多少进程访问此INODE */
	atomic_t		i_count;
	/* inode的硬连接引用计数 */
	unsigned int		i_nlink;
	uid_t			i_uid;
	gid_t			i_gid;
	/* 关联的设备号 */
	dev_t			i_rdev;
	u64			i_version;
	/* 文件大小,如果这个inode是bdev_inode的成员,i_size在bd_set_size中被设置成块设备的大小 */
	loff_t			i_size;
#ifdef __NEED_I_SIZE_ORDERED
	seqcount_t		i_size_seqcount;
#endif
	struct timespec		i_atime;
	struct timespec		i_mtime;
	struct timespec		i_ctime;
	unsigned int		i_blkbits;
	blkcnt_t		i_blocks;
	unsigned short          i_bytes;
	/* 文件的格式,权限等一些模式 */
	umode_t			i_mode;
	spinlock_t		i_lock;	/* i_blocks, i_bytes, maybe i_size */
	struct mutex		i_mutex;
	struct rw_semaphore	i_alloc_sem;
	const struct inode_operations	*i_op;
	const struct file_operations	*i_fop;	/* former ->i_op->default_file_ops */
	struct super_block	*i_sb;
	struct file_lock	*i_flock;
	/* 指向address_space对象的指针 */
	struct address_space	*i_mapping;
	/* 文件的address_space对象,省得每次分配inode的时候分配地址空间 */
	struct address_space	i_data;
#ifdef CONFIG_QUOTA
	struct dquot		*i_dquot[MAXQUOTAS];
#endif
	struct list_head	i_devices;
	union {
		struct pipe_inode_info	*i_pipe;
		struct block_device	*i_bdev;
		struct cdev		*i_cdev;
	};
	/* 拥有一组次设备号的设备文件的索引 */
	int			i_cindex;

	/* inode版本号 */
	__u32			i_generation;

#ifdef CONFIG_DNOTIFY
	/* 目录通知事件的位掩码 */
	unsigned long		i_dnotify_mask; /* Directory notify events */
	/* 用于目录通知 */
	struct dnotify_struct	*i_dnotify; /* for directory notifications */
#endif

#ifdef CONFIG_INOTIFY
	struct list_head	inotify_watches; /* watches on this inode */
	struct mutex		inotify_mutex;	/* protects the watches list */
#endif

	/* inode的状态标志,比如I_NEW */
	unsigned long		i_state;
	unsigned long		dirtied_when;	/* jiffies of first dirtying */

	/* fs的安装标志 */
	unsigned int		i_flags;

	/* 写进程的引用计数 */
	atomic_t		i_writecount;
#ifdef CONFIG_SECURITY
	void			*i_security;
#endif
	void			*i_private; /* fs or device private pointer */
};

/*
 * inode->i_mutex nesting subclasses for the lock validator:
 *
 * 0: the object of the current VFS operation
 * 1: parent
 * 2: child/target
 * 3: quota file
 *
 * The locking order between these classes is
 * parent -> child -> normal -> xattr -> quota
 */
enum inode_i_mutex_lock_class
{
	I_MUTEX_NORMAL,
	I_MUTEX_PARENT,
	I_MUTEX_CHILD,
	I_MUTEX_XATTR,
	I_MUTEX_QUOTA
};

extern void inode_double_lock(struct inode *inode1, struct inode *inode2);
extern void inode_double_unlock(struct inode *inode1, struct inode *inode2);

/*
 * NOTE: in a 32bit arch with a preemptable kernel and
 * an UP compile the i_size_read/write must be atomic
 * with respect to the local cpu (unlike with preempt disabled),
 * but they don't need to be atomic with respect to other cpus like in
 * true SMP (so they need either to either locally disable irq around
 * the read or for example on x86 they can be still implemented as a
 * cmpxchg8b without the need of the lock prefix). For SMP compiles
 * and 64bit archs it makes no difference if preempt is enabled or not.
 */
static inline loff_t i_size_read(const struct inode *inode)
{
#if BITS_PER_LONG==32 && defined(CONFIG_SMP)
	loff_t i_size;
	unsigned int seq;

	do {
		seq = read_seqcount_begin(&inode->i_size_seqcount);
		i_size = inode->i_size;
	} while (read_seqcount_retry(&inode->i_size_seqcount, seq));
	return i_size;
#elif BITS_PER_LONG==32 && defined(CONFIG_PREEMPT)
	loff_t i_size;

	preempt_disable();
	i_size = inode->i_size;
	preempt_enable();
	return i_size;
#else
	return inode->i_size;
#endif
}

/*
 * NOTE: unlike i_size_read(), i_size_write() does need locking around it
 * (normally i_mutex), otherwise on 32bit/SMP an update of i_size_seqcount
 * can be lost, resulting in subsequent i_size_read() calls spinning forever.
 */
static inline void i_size_write(struct inode *inode, loff_t i_size)
{
#if BITS_PER_LONG==32 && defined(CONFIG_SMP)
	write_seqcount_begin(&inode->i_size_seqcount);
	inode->i_size = i_size;
	write_seqcount_end(&inode->i_size_seqcount);
#elif BITS_PER_LONG==32 && defined(CONFIG_PREEMPT)
	preempt_disable();
	inode->i_size = i_size;
	preempt_enable();
#else
	inode->i_size = i_size;
#endif
}

static inline unsigned iminor(const struct inode *inode)
{
	return MINOR(inode->i_rdev);
}

static inline unsigned imajor(const struct inode *inode)
{
	return MAJOR(inode->i_rdev);
}

extern struct block_device *I_BDEV(struct inode *inode);

struct fown_struct {
	rwlock_t lock;          /* protects pid, uid, euid fields */
	struct pid *pid;	/* pid or -pgrp where SIGIO should be sent */
	enum pid_type pid_type;	/* Kind of process group SIGIO should be sent to */
	uid_t uid, euid;	/* uid/euid of process setting the owner */
	int signum;		/* posix.1b rt signal to be delivered on IO */
};

/*
 * Track a single file's readahead state
 */
/* 文件预读状态 */
struct file_ra_state {
	/* 预读的起始位置 */
	pgoff_t start;			/* where readahead started */
	/* 预读的页数 */
	unsigned int size;		/* # of readahead pages */
	/* 阈值,在读取方向上剩余页数为该值时,启动异步预读 */
	unsigned int async_size;	/* do asynchronous readahead when
					   there are only # of pages ahead */

	/* 预读的最大长度 */
	unsigned int ra_pages;		/* Maximum readahead window */
	int mmap_miss;			/* Cache miss stat for mmap accesses */
	/* 缓存的上一次read()位置 */
	loff_t prev_pos;		/* Cache last read() position */
};

/*
 * Check if @index falls in the readahead windows.
 */
static inline int ra_has_index(struct file_ra_state *ra, pgoff_t index)
{
	return (index >= ra->start &&
		index <  ra->start + ra->size);
}

#define FILE_MNT_WRITE_TAKEN	1
#define FILE_MNT_WRITE_RELEASED	2

struct file {
	/*
	 * fu_list becomes invalid after file_free is called and queued via
	 * fu_rcuhead for RCU freeing
	 */
	union {
		struct list_head	fu_list;
		struct rcu_head 	fu_rcuhead;
	} f_u;
	/* 提供了文件名和inode之间的联系 */
	struct path		f_path;
#define f_dentry	f_path.dentry
#define f_vfsmnt	f_path.mnt
	/* 文件操作调用的各个函数 */
	const struct file_operations	*f_op;
	/* 访问本文件的的进程数目的计数器 */
	atomic_long_t		f_count;
	/* open系统调用时传递的额外标志,如O_RDONLY,O_NONBLOCK和O_SYNC,为了检查用户请求是否是非阻塞式操作 */
	unsigned int 		f_flags;
	/* 打开文件时传递的模式参数,通过FMODE_READ和FMODE_WRITE位来标识文件是否可读可写 */
	fmode_t			f_mode;
	/* 文件位置指针 */
	loff_t			f_pos;
	struct fown_struct	f_owner;
	unsigned int		f_uid, f_gid;
	/* 是否预读数据,如何预读 */
	struct file_ra_state	f_ra;

	/* 检查file实例是否与相关inode兼容 */
	u64			f_version;
#ifdef CONFIG_SECURITY
	void			*f_security;
#endif
	/* needed for tty driver, and maybe others */
	/* 对于sysfs来说,这个指针就指向了sysfs_buffer */
	void			*private_data;

#ifdef CONFIG_EPOLL
	/* Used by fs/eventpoll.c to link all the hooks to this file */
	struct list_head	f_ep_links;
	spinlock_t		f_ep_lock;
#endif /* #ifdef CONFIG_EPOLL */
	/* 指向文件相关inode实例的地址空间映射 */
	struct address_space	*f_mapping;
#ifdef CONFIG_DEBUG_WRITECOUNT
	unsigned long f_mnt_write_state;
#endif
};
extern spinlock_t files_lock;
#define file_list_lock() spin_lock(&files_lock);
#define file_list_unlock() spin_unlock(&files_lock);

#define get_file(x)	atomic_long_inc(&(x)->f_count)
#define file_count(x)	atomic_long_read(&(x)->f_count)

#ifdef CONFIG_DEBUG_WRITECOUNT
static inline void file_take_write(struct file *f)
{
	WARN_ON(f->f_mnt_write_state != 0);
	f->f_mnt_write_state = FILE_MNT_WRITE_TAKEN;
}
static inline void file_release_write(struct file *f)
{
	f->f_mnt_write_state |= FILE_MNT_WRITE_RELEASED;
}
static inline void file_reset_write(struct file *f)
{
	f->f_mnt_write_state = 0;
}
static inline void file_check_state(struct file *f)
{
	/*
	 * At this point, either both or neither of these bits
	 * should be set.
	 */
	WARN_ON(f->f_mnt_write_state == FILE_MNT_WRITE_TAKEN);
	WARN_ON(f->f_mnt_write_state == FILE_MNT_WRITE_RELEASED);
}
static inline int file_check_writeable(struct file *f)
{
	if (f->f_mnt_write_state == FILE_MNT_WRITE_TAKEN)
		return 0;
	printk(KERN_WARNING "writeable file with no "
			    "mnt_want_write()\n");
	WARN_ON(1);
	return -EINVAL;
}
#else /* !CONFIG_DEBUG_WRITECOUNT */
static inline void file_take_write(struct file *filp) {}
static inline void file_release_write(struct file *filp) {}
static inline void file_reset_write(struct file *filp) {}
static inline void file_check_state(struct file *filp) {}
static inline int file_check_writeable(struct file *filp)
{
	return 0;
}
#endif /* CONFIG_DEBUG_WRITECOUNT */

#define	MAX_NON_LFS	((1UL<<31) - 1)

/* Page cache limit. The filesystems should put that into their s_maxbytes
   limits, otherwise bad things can happen in VM. */
#if BITS_PER_LONG==32
#define MAX_LFS_FILESIZE	(((u64)PAGE_CACHE_SIZE << (BITS_PER_LONG-1))-1)
#elif BITS_PER_LONG==64
#define MAX_LFS_FILESIZE 	0x7fffffffffffffffUL
#endif

#define FL_POSIX	1
#define FL_FLOCK	2
#define FL_ACCESS	8	/* not trying to lock, just looking */
#define FL_EXISTS	16	/* when unlocking, test for existence */
#define FL_LEASE	32	/* lease held on this file */
#define FL_CLOSE	64	/* unlock on close */
#define FL_SLEEP	128	/* A blocking lock */

/*
 * Special return value from posix_lock_file() and vfs_lock_file() for
 * asynchronous locking.
 */
#define FILE_LOCK_DEFERRED 1

/*
 * The POSIX file lock owner is determined by
 * the "struct files_struct" in the thread group
 * (or NULL for no owner - BSD locks).
 *
 * Lockd stuffs a "host" pointer into this.
 */
typedef struct files_struct *fl_owner_t;

struct file_lock_operations {
	void (*fl_copy_lock)(struct file_lock *, struct file_lock *);
	void (*fl_release_private)(struct file_lock *);
};

struct lock_manager_operations {
	int (*fl_compare_owner)(struct file_lock *, struct file_lock *);
	void (*fl_notify)(struct file_lock *);	/* unblock callback */
	int (*fl_grant)(struct file_lock *, struct file_lock *, int);
	void (*fl_copy_lock)(struct file_lock *, struct file_lock *);
	void (*fl_release_private)(struct file_lock *);
	void (*fl_break)(struct file_lock *);
	int (*fl_mylease)(struct file_lock *, struct file_lock *);
	int (*fl_change)(struct file_lock **, int);
};

struct lock_manager {
	struct list_head list;
};

void locks_start_grace(struct lock_manager *);
void locks_end_grace(struct lock_manager *);
int locks_in_grace(void);

/* that will die - we need it for nfs_lock_info */
#include <linux/nfs_fs_i.h>

struct file_lock {
	struct file_lock *fl_next;	/* singly linked list for this inode  */
	struct list_head fl_link;	/* doubly linked list of all locks */
	struct list_head fl_block;	/* circular list of blocked processes */
	fl_owner_t fl_owner;
	unsigned char fl_flags;
	unsigned char fl_type;
	unsigned int fl_pid;
	struct pid *fl_nspid;
	wait_queue_head_t fl_wait;
	struct file *fl_file;
	loff_t fl_start;
	loff_t fl_end;

	struct fasync_struct *	fl_fasync; /* for lease break notifications */
	unsigned long fl_break_time;	/* for nonblocking lease breaks */

	struct file_lock_operations *fl_ops;	/* Callbacks for filesystems */
	struct lock_manager_operations *fl_lmops;	/* Callbacks for lockmanagers */
	union {
		struct nfs_lock_info	nfs_fl;
		struct nfs4_lock_info	nfs4_fl;
		struct {
			struct list_head link;	/* link in AFS vnode's pending_locks list */
			int state;		/* state of grant or error if -ve */
		} afs;
	} fl_u;
};

/* The following constant reflects the upper bound of the file/locking space */
#ifndef OFFSET_MAX
#define INT_LIMIT(x)	(~((x)1 << (sizeof(x)*8 - 1)))
#define OFFSET_MAX	INT_LIMIT(loff_t)
#define OFFT_OFFSET_MAX	INT_LIMIT(off_t)
#endif

#include <linux/fcntl.h>

extern void send_sigio(struct fown_struct *fown, int fd, int band);

/* fs/sync.c */
extern int do_sync_mapping_range(struct address_space *mapping, loff_t offset,
			loff_t endbyte, unsigned int flags);

#ifdef CONFIG_FILE_LOCKING
extern int fcntl_getlk(struct file *, struct flock __user *);
extern int fcntl_setlk(unsigned int, struct file *, unsigned int,
			struct flock __user *);

#if BITS_PER_LONG == 32
extern int fcntl_getlk64(struct file *, struct flock64 __user *);
extern int fcntl_setlk64(unsigned int, struct file *, unsigned int,
			struct flock64 __user *);
#endif

extern int fcntl_setlease(unsigned int fd, struct file *filp, long arg);
extern int fcntl_getlease(struct file *filp);

/* fs/locks.c */
extern void locks_init_lock(struct file_lock *);
extern void locks_copy_lock(struct file_lock *, struct file_lock *);
extern void __locks_copy_lock(struct file_lock *, const struct file_lock *);
extern void locks_remove_posix(struct file *, fl_owner_t);
extern void locks_remove_flock(struct file *);
extern void posix_test_lock(struct file *, struct file_lock *);
extern int posix_lock_file(struct file *, struct file_lock *, struct file_lock *);
extern int posix_lock_file_wait(struct file *, struct file_lock *);
extern int posix_unblock_lock(struct file *, struct file_lock *);
extern int vfs_test_lock(struct file *, struct file_lock *);
extern int vfs_lock_file(struct file *, unsigned int, struct file_lock *, struct file_lock *);
extern int vfs_cancel_lock(struct file *filp, struct file_lock *fl);
extern int flock_lock_file_wait(struct file *filp, struct file_lock *fl);
extern int __break_lease(struct inode *inode, unsigned int flags);
extern void lease_get_mtime(struct inode *, struct timespec *time);
extern int generic_setlease(struct file *, long, struct file_lock **);
extern int vfs_setlease(struct file *, long, struct file_lock **);
extern int lease_modify(struct file_lock **, int);
extern int lock_may_read(struct inode *, loff_t start, unsigned long count);
extern int lock_may_write(struct inode *, loff_t start, unsigned long count);
#else /* !CONFIG_FILE_LOCKING */
#define fcntl_getlk(a, b) ({ -EINVAL; })
#define fcntl_setlk(a, b, c, d) ({ -EACCES; })
#if BITS_PER_LONG == 32
#define fcntl_getlk64(a, b) ({ -EINVAL; })
#define fcntl_setlk64(a, b, c, d) ({ -EACCES; })
#endif
#define fcntl_setlease(a, b, c) ({ 0; })
#define fcntl_getlease(a) ({ 0; })
#define locks_init_lock(a) ({ })
#define __locks_copy_lock(a, b) ({ })
#define locks_copy_lock(a, b) ({ })
#define locks_remove_posix(a, b) ({ })
#define locks_remove_flock(a) ({ })
#define posix_test_lock(a, b) ({ 0; })
#define posix_lock_file(a, b, c) ({ -ENOLCK; })
#define posix_lock_file_wait(a, b) ({ -ENOLCK; })
#define posix_unblock_lock(a, b) (-ENOENT)
#define vfs_test_lock(a, b) ({ 0; })
#define vfs_lock_file(a, b, c, d) (-ENOLCK)
#define vfs_cancel_lock(a, b) ({ 0; })
#define flock_lock_file_wait(a, b) ({ -ENOLCK; })
#define __break_lease(a, b) ({ 0; })
#define lease_get_mtime(a, b) ({ })
#define generic_setlease(a, b, c) ({ -EINVAL; })
#define vfs_setlease(a, b, c) ({ -EINVAL; })
#define lease_modify(a, b) ({ -EINVAL; })
#define lock_may_read(a, b, c) ({ 1; })
#define lock_may_write(a, b, c) ({ 1; })
#endif /* !CONFIG_FILE_LOCKING */


struct fasync_struct {
	int	magic;
	int	fa_fd;
	struct	fasync_struct	*fa_next; /* singly linked list */
	struct	file 		*fa_file;
};

#define FASYNC_MAGIC 0x4601

/* SMP safe fasync helpers: */
extern int fasync_helper(int, struct file *, int, struct fasync_struct **);
/* can be called from interrupts */
extern void kill_fasync(struct fasync_struct **, int, int);
/* only for net: no internal synchronization */
extern void __kill_fasync(struct fasync_struct *, int, int);

extern int __f_setown(struct file *filp, struct pid *, enum pid_type, int force);
extern int f_setown(struct file *filp, unsigned long arg, int force);
extern void f_delown(struct file *filp);
extern pid_t f_getown(struct file *filp);
extern int send_sigurg(struct fown_struct *fown);

/*
 *	Umount options
 */

//强制卸载，即使文件系统处于忙状态
#define MNT_FORCE	0x00000001	/* Attempt to forcibily umount */
/*
 * Perform  a  lazy  unmount: make the mount point unavailable for new accesses,
 * and actually perform the unmount when the mount point ceases to be busy.
*/
#define MNT_DETACH	0x00000002	/* Just detach from the tree */
//将挂载点标志为过时
#define MNT_EXPIRE	0x00000004	/* Mark for expiry */

extern struct list_head super_blocks;
extern spinlock_t sb_lock;

#define sb_entry(list)  list_entry((list), struct super_block, s_list)
#define S_BIAS (1<<30)
struct super_block {
	struct list_head	s_list;		/* Keep this first */
	dev_t			s_dev;		/* search index; _not_ kdev_t */
	unsigned long		s_blocksize;
	/* 用2 << s_blocksize_bits表示block size  */
	unsigned char		s_blocksize_bits;
	/* 用于判断sb是否为dirty */
	unsigned char		s_dirt;
	unsigned long long	s_maxbytes;	/* Max file size */
	struct file_system_type	*s_type;
	const struct super_operations	*s_op;
	struct dquot_operations	*dq_op;
 	struct quotactl_ops	*s_qcop;
	const struct export_operations *s_export_op;
	/* 挂载文件系统的标志,如MS_RDONLY(只读) */
	unsigned long		s_flags;
	unsigned long		s_magic;
	struct dentry		*s_root;
	//the semaphore which is used in umounting
	struct rw_semaphore	s_umount;
	struct mutex		s_lock;
    //keep the value with 0x40000000, when s_active bigger than 0
	int			s_count;
	int			s_need_sync_fs;
	atomic_t		s_active;
#ifdef CONFIG_SECURITY
	void                    *s_security;
#endif
	struct xattr_handler	**s_xattr;

	struct list_head	s_inodes;	/* all inodes */
	/* dirty越早的inode越靠近链表尾端 */
	struct list_head	s_dirty;	/* dirty inodes */
	struct list_head	s_io;		/* parked for writeback */
	struct list_head	s_more_io;	/* parked for more writeback */
	struct hlist_head	s_anon;		/* anonymous dentries for (nfs) exporting */
	struct list_head	s_files;
	/* s_dentry_lru and s_nr_dentry_unused are protected by dcache_lock */
	//管理当前文件系统中所有的未使用的dentry
	struct list_head	s_dentry_lru;	/* unused dentry lru */
	//当前文件系统中所有的未使用的dentry的计数
	int			s_nr_dentry_unused;	/* # of dentry on lru */

	struct block_device	*s_bdev;
	struct mtd_info		*s_mtd;
	struct list_head	s_instances;
	struct quota_info	s_dquot;	/* Diskquota specific options */

	int			s_frozen;
	wait_queue_head_t	s_wait_unfrozen;

	char s_id[32];				/* Informational name */

	void 			*s_fs_info;	/* Filesystem private info */
	fmode_t			s_mode;

	/*
	 * The next field is for VFS *only*. No filesystems have any business
	 * even looking at it. You had been warned.
	 */
	struct mutex s_vfs_rename_mutex;	/* Kludge */

	/* Granularity of c/m/atime in ns.
	   Cannot be worse than a second */
	u32		   s_time_gran;

	/*
	 * Filesystem subtype.  If non-empty the filesystem type field
	 * in /proc/mounts will be "type.subtype"
	 */
	char *s_subtype;

	/*
	 * Saved mount options for lazy filesystems using
	 * generic_show_options()
	 */
	char *s_options;
};

extern struct timespec current_fs_time(struct super_block *sb);

/*
 * Snapshotting support.
 */
enum {
	SB_UNFROZEN = 0,
	SB_FREEZE_WRITE	= 1,
	SB_FREEZE_TRANS = 2,
};

#define vfs_check_frozen(sb, level) \
	wait_event((sb)->s_wait_unfrozen, ((sb)->s_frozen < (level)))

#define get_fs_excl() atomic_inc(&current->fs_excl)
#define put_fs_excl() atomic_dec(&current->fs_excl)
#define has_fs_excl() atomic_read(&current->fs_excl)

#define is_owner_or_cap(inode)	\
	((current->fsuid == (inode)->i_uid) || capable(CAP_FOWNER))

/* not quite ready to be deprecated, but... */
extern void lock_super(struct super_block *);
extern void unlock_super(struct super_block *);

/*
 * VFS helper functions..
 */
extern int vfs_permission(struct nameidata *, int);
extern int vfs_create(struct inode *, struct dentry *, int, struct nameidata *);
extern int vfs_mkdir(struct inode *, struct dentry *, int);
extern int vfs_mknod(struct inode *, struct dentry *, int, dev_t);
extern int vfs_symlink(struct inode *, struct dentry *, const char *);
extern int vfs_link(struct dentry *, struct inode *, struct dentry *);
extern int vfs_rmdir(struct inode *, struct dentry *);
extern int vfs_unlink(struct inode *, struct dentry *);
extern int vfs_rename(struct inode *, struct dentry *, struct inode *, struct dentry *);

/*
 * VFS dentry helper functions.
 */
extern void dentry_unhash(struct dentry *dentry);

/*
 * VFS file helper functions.
 */
extern int file_permission(struct file *, int);

/*
 * VFS FS_IOC_FIEMAP helper definitions.
 */
struct fiemap_extent_info {
	unsigned int fi_flags;		/* Flags as passed from user */
	unsigned int fi_extents_mapped;	/* Number of mapped extents */
	unsigned int fi_extents_max;	/* Size of fiemap_extent array */
	struct fiemap_extent *fi_extents_start; /* Start of fiemap_extent
						 * array */
};
int fiemap_fill_next_extent(struct fiemap_extent_info *info, u64 logical,
			    u64 phys, u64 len, u32 flags);
int fiemap_check_flags(struct fiemap_extent_info *fieinfo, u32 fs_flags);

/*
 * File types
 *
 * NOTE! These match bits 12..15 of stat.st_mode
 * (ie "(i_mode >> 12) & 15").
 */
#define DT_UNKNOWN	0
#define DT_FIFO		1
#define DT_CHR		2
#define DT_DIR		4
#define DT_BLK		6
#define DT_REG		8
#define DT_LNK		10
#define DT_SOCK		12
#define DT_WHT		14

#define OSYNC_METADATA	(1<<0)
#define OSYNC_DATA	(1<<1)
#define OSYNC_INODE	(1<<2)
int generic_osync_inode(struct inode *, struct address_space *, int);

/*
 * This is the "filldir" function type, used by readdir() to let
 * the kernel specify what kind of dirent layout it wants to have.
 * This allows the kernel to read directories into kernel space or
 * to have different dirent layouts depending on the binary type.
 */
typedef int (*filldir_t)(void *, const char *, int, loff_t, u64, unsigned);
struct block_device_operations;

/* These macros are for out of kernel modules to test that
 * the kernel supports the unlocked_ioctl and compat_ioctl
 * fields in struct file_operations. */
#define HAVE_COMPAT_IOCTL 1
#define HAVE_UNLOCKED_IOCTL 1

/*
 * NOTE:
 * read, write, poll, fsync, readv, writev, unlocked_ioctl and compat_ioctl
 * can be called without the big kernel lock held in all filesystems.
 */
struct file_operations {
	struct module *owner;
	loff_t (*llseek) (struct file *, loff_t, int);
	ssize_t (*read) (struct file *, char __user *, size_t, loff_t *);
	ssize_t (*write) (struct file *, const char __user *, size_t, loff_t *);
	ssize_t (*aio_read) (struct kiocb *, const struct iovec *, unsigned long, loff_t);
	ssize_t (*aio_write) (struct kiocb *, const struct iovec *, unsigned long, loff_t);
	int (*readdir) (struct file *, void *, filldir_t);
	unsigned int (*poll) (struct file *, struct poll_table_struct *);
	int (*ioctl) (struct inode *, struct file *, unsigned int, unsigned long);
	long (*unlocked_ioctl) (struct file *, unsigned int, unsigned long);
	long (*compat_ioctl) (struct file *, unsigned int, unsigned long);
	int (*mmap) (struct file *, struct vm_area_struct *);
	int (*open) (struct inode *, struct file *);
	int (*flush) (struct file *, fl_owner_t id);
	int (*release) (struct inode *, struct file *);
	int (*fsync) (struct file *, struct dentry *, int datasync);
	int (*aio_fsync) (struct kiocb *, int datasync);
	int (*fasync) (int, struct file *, int);
	int (*lock) (struct file *, int, struct file_lock *);
	ssize_t (*sendpage) (struct file *, struct page *, int, size_t, loff_t *, int);
	unsigned long (*get_unmapped_area)(struct file *, unsigned long, unsigned long, unsigned long, unsigned long);
	int (*check_flags)(int);
	int (*dir_notify)(struct file *filp, unsigned long arg);
	int (*flock) (struct file *, int, struct file_lock *);
	ssize_t (*splice_write)(struct pipe_inode_info *, struct file *, loff_t *, size_t, unsigned int);
	ssize_t (*splice_read)(struct file *, loff_t *, struct pipe_inode_info *, size_t, unsigned int);
	int (*setlease)(struct file *, long, struct file_lock **);
};

struct inode_operations {
	int (*create) (struct inode *,struct dentry *,int, struct nameidata *);
	struct dentry * (*lookup) (struct inode *,struct dentry *, struct nameidata *);
	int (*link) (struct dentry *,struct inode *,struct dentry *);
	int (*unlink) (struct inode *,struct dentry *);
	int (*symlink) (struct inode *,struct dentry *,const char *);
	int (*mkdir) (struct inode *,struct dentry *,int);
	int (*rmdir) (struct inode *,struct dentry *);
	int (*mknod) (struct inode *,struct dentry *,int,dev_t);
	int (*rename) (struct inode *, struct dentry *,
			struct inode *, struct dentry *);
	/* 将符号连接指向的路径返回用户空间 */
	int (*readlink) (struct dentry *, char __user *,int);
	/* 保存路径到nd->saved_names[nd->depth] */
	void * (*follow_link) (struct dentry *, struct nameidata *);
	void (*put_link) (struct dentry *, struct nameidata *, void *);
	void (*truncate) (struct inode *);
	int (*permission) (struct inode *, int);
	int (*setattr) (struct dentry *, struct iattr *);
	int (*getattr) (struct vfsmount *mnt, struct dentry *, struct kstat *);
	int (*setxattr) (struct dentry *, const char *,const void *,size_t,int);
	ssize_t (*getxattr) (struct dentry *, const char *, void *, size_t);
	ssize_t (*listxattr) (struct dentry *, char *, size_t);
	int (*removexattr) (struct dentry *, const char *);
	void (*truncate_range)(struct inode *, loff_t, loff_t);
	long (*fallocate)(struct inode *inode, int mode, loff_t offset,
			  loff_t len);
	int (*fiemap)(struct inode *, struct fiemap_extent_info *, u64 start,
		      u64 len);
};

struct seq_file;

ssize_t rw_copy_check_uvector(int type, const struct iovec __user * uvector,
				unsigned long nr_segs, unsigned long fast_segs,
				struct iovec *fast_pointer,
				struct iovec **ret_pointer);

extern ssize_t vfs_read(struct file *, char __user *, size_t, loff_t *);
extern ssize_t vfs_write(struct file *, const char __user *, size_t, loff_t *);
extern ssize_t vfs_readv(struct file *, const struct iovec __user *,
		unsigned long, loff_t *);
extern ssize_t vfs_writev(struct file *, const struct iovec __user *,
		unsigned long, loff_t *);

struct super_operations {
   	struct inode *(*alloc_inode)(struct super_block *sb);
	void (*destroy_inode)(struct inode *);

   	void (*dirty_inode) (struct inode *);
	int (*write_inode) (struct inode *, int);
	void (*drop_inode) (struct inode *);
	void (*delete_inode) (struct inode *);
	void (*put_super) (struct super_block *);
	/* 回写super block的方法,如果为NULL,表示当前fs不需要将内存中的sb和磁盘同步(如ram fs) */
	void (*write_super) (struct super_block *);
	int (*sync_fs)(struct super_block *sb, int wait);
	void (*write_super_lockfs) (struct super_block *);
	void (*unlockfs) (struct super_block *);
	int (*statfs) (struct dentry *, struct kstatfs *);
	int (*remount_fs) (struct super_block *, int *, char *);
	void (*clear_inode) (struct inode *);
	void (*umount_begin) (struct super_block *);

	int (*show_options)(struct seq_file *, struct vfsmount *);
	int (*show_stats)(struct seq_file *, struct vfsmount *);
#ifdef CONFIG_QUOTA
	ssize_t (*quota_read)(struct super_block *, int, char *, size_t, loff_t);
	ssize_t (*quota_write)(struct super_block *, int, const char *, size_t, loff_t);
#endif
};

/*
 * Inode state bits.  Protected by inode_lock.
 *
 * Three bits determine the dirty state of the inode, I_DIRTY_SYNC,
 * I_DIRTY_DATASYNC and I_DIRTY_PAGES.
 *
 * Four bits define the lifetime of an inode.  Initially, inodes are I_NEW,
 * until that flag is cleared.  I_WILL_FREE, I_FREEING and I_CLEAR are set at
 * various stages of removing an inode.
 *
 * Two bits are used for locking and completion notification, I_LOCK and I_SYNC.
 *
 * I_DIRTY_SYNC		Inode is dirty, but doesn't have to be written on
 *			fdatasync().  i_atime is the usual cause.
 * I_DIRTY_DATASYNC	Data-related inode changes pending. We keep track of
 *			these changes separately from I_DIRTY_SYNC so that we
 *			don't have to write inode on fdatasync() when only
 *			mtime has changed in it.
 * I_DIRTY_PAGES	Inode has dirty pages.  Inode itself may be clean.
 * I_NEW		get_new_inode() sets i_state to I_LOCK|I_NEW.  Both
 *			are cleared by unlock_new_inode(), called from iget().
 * I_WILL_FREE		Must be set when calling write_inode_now() if i_count
 *			is zero.  I_FREEING must be set when I_WILL_FREE is
 *			cleared.
 * I_FREEING		Set when inode is about to be freed but still has dirty
 *			pages or buffers attached or the inode itself is still
 *			dirty.
 * I_CLEAR		Set by clear_inode().  In this state the inode is clean
 *			and can be destroyed.
 *
 *			Inodes that are I_WILL_FREE, I_FREEING or I_CLEAR are
 *			prohibited for many purposes.  iget() must wait for
 *			the inode to be completely released, then create it
 *			anew.  Other functions will just ignore such inodes,
 *			if appropriate.  I_LOCK is used for waiting.
 *
 * I_LOCK		Serves as both a mutex and completion notification.
 *			New inodes set I_LOCK.  If two processes both create
 *			the same inode, one of them will release its inode and
 *			wait for I_LOCK to be released before returning.
 *			Inodes in I_WILL_FREE, I_FREEING or I_CLEAR state can
 *			also cause waiting on I_LOCK, without I_LOCK actually
 *			being set.  find_inode() uses this to prevent returning
 *			nearly-dead inodes.
 * I_SYNC		Similar to I_LOCK, but limited in scope to writeback
 *			of inode dirty data.  Having a separate lock for this
 *			purpose reduces latency and prevents some filesystem-
 *			specific deadlocks.
 *
 * Q: What is the difference between I_WILL_FREE and I_FREEING?
 * Q: igrab() only checks on (I_FREEING|I_WILL_FREE).  Should it also checks on
 *    I_CLEAR?  If not, why?
 */
/*
 * I_DIRTY_SYNC和I_DIRTY_DATASYNC共同标记inode metadata dirty.
 * I_DIRTY_SYNC标记inode中不用马上回写的metadata dirty
 * I_DIRTY_DATASYNC标记inode中需要马上回写的metadata dirty
 */
#define I_DIRTY_SYNC		1
/*
 * fdatasync一般只同步文件数据,必要的时候才会同步inode metadata.
 * 这个必要性就是通过I_DIRTY_DATASYNC标记的.
 */
#define I_DIRTY_DATASYNC	2
/* 地址空间中有page是dirty的 */
#define I_DIRTY_PAGES		4
#define I_NEW			8
#define I_WILL_FREE		16
#define I_FREEING		32
#define I_CLEAR			64
#define __I_LOCK		7
#define I_LOCK			(1 << __I_LOCK)
#define __I_SYNC		8
#define I_SYNC			(1 << __I_SYNC)

#define I_DIRTY (I_DIRTY_SYNC | I_DIRTY_DATASYNC | I_DIRTY_PAGES)

extern void __mark_inode_dirty(struct inode *, int);
static inline void mark_inode_dirty(struct inode *inode)
{
	__mark_inode_dirty(inode, I_DIRTY);
}

static inline void mark_inode_dirty_sync(struct inode *inode)
{
	__mark_inode_dirty(inode, I_DIRTY_SYNC);
}

/**
 * inc_nlink - directly increment an inode's link count
 * @inode: inode
 *
 * This is a low-level filesystem helper to replace any
 * direct filesystem manipulation of i_nlink.  Currently,
 * it is only here for parity with dec_nlink().
 */
static inline void inc_nlink(struct inode *inode)
{
	inode->i_nlink++;
}

static inline void inode_inc_link_count(struct inode *inode)
{
	inc_nlink(inode);
	mark_inode_dirty(inode);
}

/**
 * drop_nlink - directly drop an inode's link count
 * @inode: inode
 *
 * This is a low-level filesystem helper to replace any
 * direct filesystem manipulation of i_nlink.  In cases
 * where we are attempting to track writes to the
 * filesystem, a decrement to zero means an imminent
 * write when the file is truncated and actually unlinked
 * on the filesystem.
 */
static inline void drop_nlink(struct inode *inode)
{
	inode->i_nlink--;
}

/**
 * clear_nlink - directly zero an inode's link count
 * @inode: inode
 *
 * This is a low-level filesystem helper to replace any
 * direct filesystem manipulation of i_nlink.  See
 * drop_nlink() for why we care about i_nlink hitting zero.
 */
static inline void clear_nlink(struct inode *inode)
{
	inode->i_nlink = 0;
}

static inline void inode_dec_link_count(struct inode *inode)
{
	drop_nlink(inode);
	mark_inode_dirty(inode);
}

/**
 * inode_inc_iversion - increments i_version
 * @inode: inode that need to be updated
 *
 * Every time the inode is modified, the i_version field will be incremented.
 * The filesystem has to be mounted with i_version flag
 */

static inline void inode_inc_iversion(struct inode *inode)
{
       spin_lock(&inode->i_lock);
       inode->i_version++;
       spin_unlock(&inode->i_lock);
}

extern void touch_atime(struct vfsmount *mnt, struct dentry *dentry);
static inline void file_accessed(struct file *file)
{
	if (!(file->f_flags & O_NOATIME))
		touch_atime(file->f_path.mnt, file->f_path.dentry);
}

int sync_inode(struct inode *inode, struct writeback_control *wbc);

struct file_system_type {
	const char *name;
	int fs_flags;
	//must be defined, it is be used in vfs_kern_mount
	int (*get_sb) (struct file_system_type *, int,
		       const char *, void *, struct vfsmount *);
	//must be defined, it is be used in deactivate_super
	void (*kill_sb) (struct super_block *);
	struct module *owner;
	struct file_system_type * next;
	struct list_head fs_supers;

	struct lock_class_key s_lock_key;
	struct lock_class_key s_umount_key;

	struct lock_class_key i_lock_key;
	struct lock_class_key i_mutex_key;
	struct lock_class_key i_mutex_dir_key;
	struct lock_class_key i_alloc_sem_key;
};

extern int get_sb_bdev(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data,
	int (*fill_super)(struct super_block *, void *, int),
	struct vfsmount *mnt);
extern int get_sb_single(struct file_system_type *fs_type,
	int flags, void *data,
	int (*fill_super)(struct super_block *, void *, int),
	struct vfsmount *mnt);
extern int get_sb_nodev(struct file_system_type *fs_type,
	int flags, void *data,
	int (*fill_super)(struct super_block *, void *, int),
	struct vfsmount *mnt);
void generic_shutdown_super(struct super_block *sb);
void kill_block_super(struct super_block *sb);
void kill_anon_super(struct super_block *sb);
void kill_litter_super(struct super_block *sb);
void deactivate_super(struct super_block *sb);
int set_anon_super(struct super_block *s, void *data);
struct super_block *sget(struct file_system_type *type,
			int (*test)(struct super_block *,void *),
			int (*set)(struct super_block *,void *),
			void *data);
extern int get_sb_pseudo(struct file_system_type *, char *,
	const struct super_operations *ops, unsigned long,
	struct vfsmount *mnt);
extern int simple_set_mnt(struct vfsmount *mnt, struct super_block *sb);
int __put_super_and_need_restart(struct super_block *sb);

/* Alas, no aliases. Too much hassle with bringing module.h everywhere */
#define fops_get(fops) \
	(((fops) && try_module_get((fops)->owner) ? (fops) : NULL))
#define fops_put(fops) \
	do { if (fops) module_put((fops)->owner); } while(0)

extern int register_filesystem(struct file_system_type *);
extern int unregister_filesystem(struct file_system_type *);
extern struct vfsmount *kern_mount_data(struct file_system_type *, void *data);
#define kern_mount(type) kern_mount_data(type, NULL)
extern int may_umount_tree(struct vfsmount *);
extern int may_umount(struct vfsmount *);
extern long do_mount(char *, char *, char *, unsigned long, void *);
extern struct vfsmount *collect_mounts(struct vfsmount *, struct dentry *);
extern void drop_collected_mounts(struct vfsmount *);

extern int vfs_statfs(struct dentry *, struct kstatfs *);

/* /sys/fs */
extern struct kobject *fs_kobj;

extern int rw_verify_area(int, struct file *, loff_t *, size_t);

#define FLOCK_VERIFY_READ  1
#define FLOCK_VERIFY_WRITE 2

#ifdef CONFIG_FILE_LOCKING
extern int locks_mandatory_locked(struct inode *);
extern int locks_mandatory_area(int, struct inode *, struct file *, loff_t, size_t);

/*
 * Candidates for mandatory locking have the setgid bit set
 * but no group execute bit -  an otherwise meaningless combination.
 */

static inline int __mandatory_lock(struct inode *ino)
{
	return (ino->i_mode & (S_ISGID | S_IXGRP)) == S_ISGID;
}

/*
 * ... and these candidates should be on MS_MANDLOCK mounted fs,
 * otherwise these will be advisory locks
 */

static inline int mandatory_lock(struct inode *ino)
{
	return IS_MANDLOCK(ino) && __mandatory_lock(ino);
}

static inline int locks_verify_locked(struct inode *inode)
{
	if (mandatory_lock(inode))
		return locks_mandatory_locked(inode);
	return 0;
}

static inline int locks_verify_truncate(struct inode *inode,
				    struct file *filp,
				    loff_t size)
{
	if (inode->i_flock && mandatory_lock(inode))
		return locks_mandatory_area(
			FLOCK_VERIFY_WRITE, inode, filp,
			size < inode->i_size ? size : inode->i_size,
			(size < inode->i_size ? inode->i_size - size
			 : size - inode->i_size)
		);
	return 0;
}

static inline int break_lease(struct inode *inode, unsigned int mode)
{
	if (inode->i_flock)
		return __break_lease(inode, mode);
	return 0;
}
#else /* !CONFIG_FILE_LOCKING */
#define locks_mandatory_locked(a) ({ 0; })
#define locks_mandatory_area(a, b, c, d, e) ({ 0; })
#define __mandatory_lock(a) ({ 0; })
#define mandatory_lock(a) ({ 0; })
#define locks_verify_locked(a) ({ 0; })
#define locks_verify_truncate(a, b, c) ({ 0; })
#define break_lease(a, b) ({ 0; })
#endif /* CONFIG_FILE_LOCKING */

/* fs/open.c */

extern int do_truncate(struct dentry *, loff_t start, unsigned int time_attrs,
		       struct file *filp);
extern long do_sys_open(int dfd, const char __user *filename, int flags,
			int mode);
extern struct file *filp_open(const char *, int, int);
extern struct file * dentry_open(struct dentry *, struct vfsmount *, int);
extern int filp_close(struct file *, fl_owner_t id);
extern char * getname(const char __user *);

/* fs/dcache.c */
extern void __init vfs_caches_init_early(void);
extern void __init vfs_caches_init(unsigned long);

extern struct kmem_cache *names_cachep;

#define __getname()	kmem_cache_alloc(names_cachep, GFP_KERNEL)
#define __putname(name) kmem_cache_free(names_cachep, (void *)(name))
#ifndef CONFIG_AUDITSYSCALL
#define putname(name)   __putname(name)
#else
extern void putname(const char *name);
#endif

#ifdef CONFIG_BLOCK
extern int register_blkdev(unsigned int, const char *);
extern void unregister_blkdev(unsigned int, const char *);
extern struct block_device *bdget(dev_t);
extern void bd_set_size(struct block_device *, loff_t size);
extern void bd_forget(struct inode *inode);
extern void bdput(struct block_device *);
extern struct block_device *open_by_devnum(dev_t, fmode_t);
#else
static inline void bd_forget(struct inode *inode) {}
#endif
extern const struct file_operations def_blk_fops;
extern const struct file_operations def_chr_fops;
extern const struct file_operations bad_sock_fops;
extern const struct file_operations def_fifo_fops;
#ifdef CONFIG_BLOCK
extern int ioctl_by_bdev(struct block_device *, unsigned, unsigned long);
extern int blkdev_ioctl(struct block_device *, fmode_t, unsigned, unsigned long);
extern long compat_blkdev_ioctl(struct file *, unsigned, unsigned long);
extern int blkdev_get(struct block_device *, fmode_t);
extern int blkdev_put(struct block_device *, fmode_t);
extern int bd_claim(struct block_device *, void *);
extern void bd_release(struct block_device *);
#ifdef CONFIG_SYSFS
extern int bd_claim_by_disk(struct block_device *, void *, struct gendisk *);
extern void bd_release_from_disk(struct block_device *, struct gendisk *);
#else
#define bd_claim_by_disk(bdev, holder, disk)	bd_claim(bdev, holder)
#define bd_release_from_disk(bdev, disk)	bd_release(bdev)
#endif
#endif

/* fs/char_dev.c */
#define CHRDEV_MAJOR_HASH_SIZE	255
extern int alloc_chrdev_region(dev_t *, unsigned, unsigned, const char *);
extern int register_chrdev_region(dev_t, unsigned, const char *);
extern int register_chrdev(unsigned int, const char *,
			   const struct file_operations *);
extern void unregister_chrdev(unsigned int, const char *);
extern void unregister_chrdev_region(dev_t, unsigned);
extern void chrdev_show(struct seq_file *,off_t);

/* fs/block_dev.c */
#define BDEVNAME_SIZE	32	/* Largest string for a blockdev identifier */
#define BDEVT_SIZE	10	/* Largest string for MAJ:MIN for blkdev */

#ifdef CONFIG_BLOCK
#define BLKDEV_MAJOR_HASH_SIZE	255
extern const char *__bdevname(dev_t, char *buffer);
extern const char *bdevname(struct block_device *bdev, char *buffer);
extern struct block_device *lookup_bdev(const char *);
extern struct block_device *open_bdev_exclusive(const char *, fmode_t, void *);
extern void close_bdev_exclusive(struct block_device *, fmode_t);
extern void blkdev_show(struct seq_file *,off_t);

#else
#define BLKDEV_MAJOR_HASH_SIZE	0
#endif

extern void init_special_inode(struct inode *, umode_t, dev_t);

/* Invalid inode operations -- fs/bad_inode.c */
extern void make_bad_inode(struct inode *);
extern int is_bad_inode(struct inode *);

extern const struct file_operations read_pipefifo_fops;
extern const struct file_operations write_pipefifo_fops;
extern const struct file_operations rdwr_pipefifo_fops;

extern int fs_may_remount_ro(struct super_block *);

#ifdef CONFIG_BLOCK
/*
 * return READ, READA, or WRITE
 */
#define bio_rw(bio)		((bio)->bi_rw & (RW_MASK | RWA_MASK))

/*
 * return data direction, READ or WRITE
 */
#define bio_data_dir(bio)	((bio)->bi_rw & 1)

extern void check_disk_size_change(struct gendisk *disk,
				   struct block_device *bdev);
extern int revalidate_disk(struct gendisk *);
extern int check_disk_change(struct block_device *);
extern int __invalidate_device(struct block_device *);
extern int invalidate_partition(struct gendisk *, int);
#endif
extern int invalidate_inodes(struct super_block *);
unsigned long __invalidate_mapping_pages(struct address_space *mapping,
					pgoff_t start, pgoff_t end,
					bool be_atomic);
unsigned long invalidate_mapping_pages(struct address_space *mapping,
					pgoff_t start, pgoff_t end);

static inline unsigned long __deprecated
invalidate_inode_pages(struct address_space *mapping)
{
	return invalidate_mapping_pages(mapping, 0, ~0UL);
}

static inline void invalidate_remote_inode(struct inode *inode)
{
	if (S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
	    S_ISLNK(inode->i_mode))
		invalidate_mapping_pages(inode->i_mapping, 0, -1);
}
extern int invalidate_inode_pages2(struct address_space *mapping);
extern int invalidate_inode_pages2_range(struct address_space *mapping,
					 pgoff_t start, pgoff_t end);
extern void generic_sync_sb_inodes(struct super_block *sb,
				struct writeback_control *wbc);
extern int write_inode_now(struct inode *, int);
extern int filemap_fdatawrite(struct address_space *);
extern int filemap_flush(struct address_space *);
extern int filemap_fdatawait(struct address_space *);
extern int filemap_write_and_wait(struct address_space *mapping);
extern int filemap_write_and_wait_range(struct address_space *mapping,
				        loff_t lstart, loff_t lend);
extern int wait_on_page_writeback_range(struct address_space *mapping,
				pgoff_t start, pgoff_t end);
extern int __filemap_fdatawrite_range(struct address_space *mapping,
				loff_t start, loff_t end, int sync_mode);
extern int filemap_fdatawrite_range(struct address_space *mapping,
				loff_t start, loff_t end);

extern long do_fsync(struct file *file, int datasync);
extern void sync_supers(void);
extern void sync_filesystems(int wait);
extern void __fsync_super(struct super_block *sb);
extern void emergency_sync(void);
extern void emergency_remount(void);
extern int do_remount_sb(struct super_block *sb, int flags,
			 void *data, int force);
#ifdef CONFIG_BLOCK
extern sector_t bmap(struct inode *, sector_t);
#endif
extern int notify_change(struct dentry *, struct iattr *);
extern int inode_permission(struct inode *, int);
extern int generic_permission(struct inode *, int,
		int (*check_acl)(struct inode *, int));

static inline bool execute_ok(struct inode *inode)
{
	return (inode->i_mode & S_IXUGO) || S_ISDIR(inode->i_mode);
}

extern int get_write_access(struct inode *);
extern int deny_write_access(struct file *);
static inline void put_write_access(struct inode * inode)
{
	atomic_dec(&inode->i_writecount);
}
static inline void allow_write_access(struct file *file)
{
	if (file)
		atomic_inc(&file->f_path.dentry->d_inode->i_writecount);
}
extern int do_pipe(int *);
extern int do_pipe_flags(int *, int);
extern struct file *create_read_pipe(struct file *f, int flags);
extern struct file *create_write_pipe(int flags);
extern void free_write_pipe(struct file *);

extern struct file *do_filp_open(int dfd, const char *pathname,
		int open_flag, int mode);
extern int may_open(struct nameidata *, int, int);

extern int kernel_read(struct file *, unsigned long, char *, unsigned long);
extern struct file * open_exec(const char *);

/* fs/dcache.c -- generic fs support functions */
extern int is_subdir(struct dentry *, struct dentry *);
extern ino_t find_inode_number(struct dentry *, struct qstr *);

#include <linux/err.h>

/* needed for stackable file system support */
extern loff_t default_llseek(struct file *file, loff_t offset, int origin);

extern loff_t vfs_llseek(struct file *file, loff_t offset, int origin);

extern void inode_init_once(struct inode *);
extern void iput(struct inode *);
extern struct inode * igrab(struct inode *);
extern ino_t iunique(struct super_block *, ino_t);
extern int inode_needs_sync(struct inode *inode);
extern void generic_delete_inode(struct inode *inode);
extern void generic_drop_inode(struct inode *inode);

extern struct inode *ilookup5_nowait(struct super_block *sb,
		unsigned long hashval, int (*test)(struct inode *, void *),
		void *data);
extern struct inode *ilookup5(struct super_block *sb, unsigned long hashval,
		int (*test)(struct inode *, void *), void *data);
extern struct inode *ilookup(struct super_block *sb, unsigned long ino);

extern struct inode * iget5_locked(struct super_block *, unsigned long, int (*test)(struct inode *, void *), int (*set)(struct inode *, void *), void *);
extern struct inode * iget_locked(struct super_block *, unsigned long);
extern void unlock_new_inode(struct inode *);

extern void __iget(struct inode * inode);
extern void iget_failed(struct inode *);
extern void clear_inode(struct inode *);
extern void destroy_inode(struct inode *);
extern struct inode *new_inode(struct super_block *);
extern int should_remove_suid(struct dentry *);
extern int file_remove_suid(struct file *);

extern void __insert_inode_hash(struct inode *, unsigned long hashval);
extern void remove_inode_hash(struct inode *);
static inline void insert_inode_hash(struct inode *inode) {
	__insert_inode_hash(inode, inode->i_ino);
}

extern struct file * get_empty_filp(void);
extern void file_move(struct file *f, struct list_head *list);
extern void file_kill(struct file *f);
#ifdef CONFIG_BLOCK
struct bio;
extern void submit_bio(int, struct bio *);
extern int bdev_read_only(struct block_device *);
#endif
extern int set_blocksize(struct block_device *, int);
extern int sb_set_blocksize(struct super_block *, int);
extern int sb_min_blocksize(struct super_block *, int);
extern int sb_has_dirty_inodes(struct super_block *);

extern int generic_file_mmap(struct file *, struct vm_area_struct *);
extern int generic_file_readonly_mmap(struct file *, struct vm_area_struct *);
extern int file_read_actor(read_descriptor_t * desc, struct page *page, unsigned long offset, unsigned long size);
int generic_write_checks(struct file *file, loff_t *pos, size_t *count, int isblk);
extern ssize_t generic_file_aio_read(struct kiocb *, const struct iovec *, unsigned long, loff_t);
extern ssize_t generic_file_aio_write(struct kiocb *, const struct iovec *, unsigned long, loff_t);
extern ssize_t generic_file_aio_write_nolock(struct kiocb *, const struct iovec *,
		unsigned long, loff_t);
extern ssize_t generic_file_direct_write(struct kiocb *, const struct iovec *,
		unsigned long *, loff_t, loff_t *, size_t, size_t);
extern ssize_t generic_file_buffered_write(struct kiocb *, const struct iovec *,
		unsigned long, loff_t, loff_t *, size_t, ssize_t);
extern ssize_t do_sync_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos);
extern ssize_t do_sync_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos);
extern int generic_segment_checks(const struct iovec *iov,
		unsigned long *nr_segs, size_t *count, int access_flags);

/* fs/splice.c */
extern ssize_t generic_file_splice_read(struct file *, loff_t *,
		struct pipe_inode_info *, size_t, unsigned int);
extern ssize_t generic_file_splice_write(struct pipe_inode_info *,
		struct file *, loff_t *, size_t, unsigned int);
extern ssize_t generic_file_splice_write_nolock(struct pipe_inode_info *,
		struct file *, loff_t *, size_t, unsigned int);
extern ssize_t generic_splice_sendpage(struct pipe_inode_info *pipe,
		struct file *out, loff_t *, size_t len, unsigned int flags);
extern long do_splice_direct(struct file *in, loff_t *ppos, struct file *out,
		size_t len, unsigned int flags);

extern void
file_ra_state_init(struct file_ra_state *ra, struct address_space *mapping);
extern loff_t no_llseek(struct file *file, loff_t offset, int origin);
extern loff_t generic_file_llseek(struct file *file, loff_t offset, int origin);
extern loff_t generic_file_llseek_unlocked(struct file *file, loff_t offset,
			int origin);
extern int generic_file_open(struct inode * inode, struct file * filp);
extern int nonseekable_open(struct inode * inode, struct file * filp);

#ifdef CONFIG_FS_XIP
extern ssize_t xip_file_read(struct file *filp, char __user *buf, size_t len,
			     loff_t *ppos);
extern int xip_file_mmap(struct file * file, struct vm_area_struct * vma);
extern ssize_t xip_file_write(struct file *filp, const char __user *buf,
			      size_t len, loff_t *ppos);
extern int xip_truncate_page(struct address_space *mapping, loff_t from);
#else
static inline int xip_truncate_page(struct address_space *mapping, loff_t from)
{
	return 0;
}
#endif

#ifdef CONFIG_BLOCK
ssize_t __blockdev_direct_IO(int rw, struct kiocb *iocb, struct inode *inode,
	struct block_device *bdev, const struct iovec *iov, loff_t offset,
	unsigned long nr_segs, get_block_t get_block, dio_iodone_t end_io,
	int lock_type);

enum {
	DIO_LOCKING = 1, /* need locking between buffered and direct access */
	DIO_NO_LOCKING,  /* bdev; no locking at all between buffered/direct */
	DIO_OWN_LOCKING, /* filesystem locks buffered and direct internally */
};

static inline ssize_t blockdev_direct_IO(int rw, struct kiocb *iocb,
	struct inode *inode, struct block_device *bdev, const struct iovec *iov,
	loff_t offset, unsigned long nr_segs, get_block_t get_block,
	dio_iodone_t end_io)
{
	return __blockdev_direct_IO(rw, iocb, inode, bdev, iov, offset,
				nr_segs, get_block, end_io, DIO_LOCKING);
}

static inline ssize_t blockdev_direct_IO_no_locking(int rw, struct kiocb *iocb,
	struct inode *inode, struct block_device *bdev, const struct iovec *iov,
	loff_t offset, unsigned long nr_segs, get_block_t get_block,
	dio_iodone_t end_io)
{
	return __blockdev_direct_IO(rw, iocb, inode, bdev, iov, offset,
				nr_segs, get_block, end_io, DIO_NO_LOCKING);
}

static inline ssize_t blockdev_direct_IO_own_locking(int rw, struct kiocb *iocb,
	struct inode *inode, struct block_device *bdev, const struct iovec *iov,
	loff_t offset, unsigned long nr_segs, get_block_t get_block,
	dio_iodone_t end_io)
{
	return __blockdev_direct_IO(rw, iocb, inode, bdev, iov, offset,
				nr_segs, get_block, end_io, DIO_OWN_LOCKING);
}
#endif

extern const struct file_operations generic_ro_fops;

#define special_file(m) (S_ISCHR(m)||S_ISBLK(m)||S_ISFIFO(m)||S_ISSOCK(m))

extern int vfs_readlink(struct dentry *, char __user *, int, const char *);
extern int vfs_follow_link(struct nameidata *, const char *);
extern int page_readlink(struct dentry *, char __user *, int);
extern void *page_follow_link_light(struct dentry *, struct nameidata *);
extern void page_put_link(struct dentry *, struct nameidata *, void *);
extern int __page_symlink(struct inode *inode, const char *symname, int len,
		int nofs);
extern int page_symlink(struct inode *inode, const char *symname, int len);
extern const struct inode_operations page_symlink_inode_operations;
extern int generic_readlink(struct dentry *, char __user *, int);
extern void generic_fillattr(struct inode *, struct kstat *);
extern int vfs_getattr(struct vfsmount *, struct dentry *, struct kstat *);
void inode_add_bytes(struct inode *inode, loff_t bytes);
void inode_sub_bytes(struct inode *inode, loff_t bytes);
loff_t inode_get_bytes(struct inode *inode);
void inode_set_bytes(struct inode *inode, loff_t bytes);

extern int vfs_readdir(struct file *, filldir_t, void *);

extern int vfs_stat(char __user *, struct kstat *);
extern int vfs_lstat(char __user *, struct kstat *);
extern int vfs_stat_fd(int dfd, char __user *, struct kstat *);
extern int vfs_lstat_fd(int dfd, char __user *, struct kstat *);
extern int vfs_fstat(unsigned int, struct kstat *);

extern int do_vfs_ioctl(struct file *filp, unsigned int fd, unsigned int cmd,
		    unsigned long arg);
extern int generic_block_fiemap(struct inode *inode,
				struct fiemap_extent_info *fieinfo, u64 start,
				u64 len, get_block_t *get_block);

extern void get_filesystem(struct file_system_type *fs);
extern void put_filesystem(struct file_system_type *fs);
extern struct file_system_type *get_fs_type(const char *name);
extern struct super_block *get_super(struct block_device *);
extern struct super_block *user_get_super(dev_t);
extern void drop_super(struct super_block *sb);

extern int dcache_dir_open(struct inode *, struct file *);
extern int dcache_dir_close(struct inode *, struct file *);
extern loff_t dcache_dir_lseek(struct file *, loff_t, int);
extern int dcache_readdir(struct file *, void *, filldir_t);
extern int simple_getattr(struct vfsmount *, struct dentry *, struct kstat *);
extern int simple_statfs(struct dentry *, struct kstatfs *);
extern int simple_link(struct dentry *, struct inode *, struct dentry *);
extern int simple_unlink(struct inode *, struct dentry *);
extern int simple_rmdir(struct inode *, struct dentry *);
extern int simple_rename(struct inode *, struct dentry *, struct inode *, struct dentry *);
extern int simple_sync_file(struct file *, struct dentry *, int);
extern int simple_empty(struct dentry *);
extern int simple_readpage(struct file *file, struct page *page);
extern int simple_prepare_write(struct file *file, struct page *page,
			unsigned offset, unsigned to);
extern int simple_write_begin(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned flags,
			struct page **pagep, void **fsdata);
extern int simple_write_end(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned copied,
			struct page *page, void *fsdata);

extern struct dentry *simple_lookup(struct inode *, struct dentry *, struct nameidata *);
extern ssize_t generic_read_dir(struct file *, char __user *, size_t, loff_t *);
extern const struct file_operations simple_dir_operations;
extern const struct inode_operations simple_dir_inode_operations;
struct tree_descr { char *name; const struct file_operations *ops; int mode; };
struct dentry *d_alloc_name(struct dentry *, const char *);
extern int simple_fill_super(struct super_block *, int, struct tree_descr *);
extern int simple_pin_fs(struct file_system_type *, struct vfsmount **mount, int *count);
extern void simple_release_fs(struct vfsmount **mount, int *count);

extern ssize_t simple_read_from_buffer(void __user *to, size_t count,
			loff_t *ppos, const void *from, size_t available);

#ifdef CONFIG_MIGRATION
extern int buffer_migrate_page(struct address_space *,
				struct page *, struct page *);
#else
#define buffer_migrate_page NULL
#endif

extern int inode_change_ok(struct inode *, struct iattr *);
extern int __must_check inode_setattr(struct inode *, struct iattr *);

extern void file_update_time(struct file *file);

extern int generic_show_options(struct seq_file *m, struct vfsmount *mnt);
extern void save_mount_options(struct super_block *sb, char *options);

static inline ino_t parent_ino(struct dentry *dentry)
{
	ino_t res;

	spin_lock(&dentry->d_lock);
	res = dentry->d_parent->d_inode->i_ino;
	spin_unlock(&dentry->d_lock);
	return res;
}

/* Transaction based IO helpers */

/*
 * An argresp is stored in an allocated page and holds the
 * size of the argument or response, along with its content
 */
struct simple_transaction_argresp {
	ssize_t size;
	char data[0];
};

#define SIMPLE_TRANSACTION_LIMIT (PAGE_SIZE - sizeof(struct simple_transaction_argresp))

char *simple_transaction_get(struct file *file, const char __user *buf,
				size_t size);
ssize_t simple_transaction_read(struct file *file, char __user *buf,
				size_t size, loff_t *pos);
int simple_transaction_release(struct inode *inode, struct file *file);

static inline void simple_transaction_set(struct file *file, size_t n)
{
	struct simple_transaction_argresp *ar = file->private_data;

	BUG_ON(n > SIMPLE_TRANSACTION_LIMIT);

	/*
	 * The barrier ensures that ar->size will really remain zero until
	 * ar->data is ready for reading.
	 */
	smp_mb();
	ar->size = n;
}

/*
 * simple attribute files
 *
 * These attributes behave similar to those in sysfs:
 *
 * Writing to an attribute immediately sets a value, an open file can be
 * written to multiple times.
 *
 * Reading from an attribute creates a buffer from the value that might get
 * read with multiple read calls. When the attribute has been read
 * completely, no further read calls are possible until the file is opened
 * again.
 *
 * All attributes contain a text representation of a numeric value
 * that are accessed with the get() and set() functions.
 */
#define DEFINE_SIMPLE_ATTRIBUTE(__fops, __get, __set, __fmt)		\
static int __fops ## _open(struct inode *inode, struct file *file)	\
{									\
	__simple_attr_check_format(__fmt, 0ull);			\
	return simple_attr_open(inode, file, __get, __set, __fmt);	\
}									\
static struct file_operations __fops = {				\
	.owner	 = THIS_MODULE,						\
	.open	 = __fops ## _open,					\
	.release = simple_attr_release,					\
	.read	 = simple_attr_read,					\
	.write	 = simple_attr_write,					\
};

static inline void __attribute__((format(printf, 1, 2)))
__simple_attr_check_format(const char *fmt, ...)
{
	/* don't do anything, just let the compiler check the arguments; */
}

int simple_attr_open(struct inode *inode, struct file *file,
		     int (*get)(void *, u64 *), int (*set)(void *, u64),
		     const char *fmt);
int simple_attr_release(struct inode *inode, struct file *file);
ssize_t simple_attr_read(struct file *file, char __user *buf,
			 size_t len, loff_t *ppos);
ssize_t simple_attr_write(struct file *file, const char __user *buf,
			  size_t len, loff_t *ppos);


#ifdef CONFIG_SECURITY
static inline char *alloc_secdata(void)
{
	return (char *)get_zeroed_page(GFP_KERNEL);
}

static inline void free_secdata(void *secdata)
{
	free_page((unsigned long)secdata);
}
#else
static inline char *alloc_secdata(void)
{
	return (char *)1;
}

static inline void free_secdata(void *secdata)
{ }
#endif	/* CONFIG_SECURITY */

struct ctl_table;
int proc_nr_files(struct ctl_table *table, int write, struct file *filp,
		  void __user *buffer, size_t *lenp, loff_t *ppos);

int get_filesystem_list(char * buf);

#endif /* __KERNEL__ */
#endif /* _LINUX_FS_H */
