#ifndef _LINUX_STATFS_H
#define _LINUX_STATFS_H

#include <linux/types.h>

#include <asm/statfs.h>

struct kstatfs {
	/* 文件系统类型,实际上也就是文件系统的magic number */
	long f_type;
	long f_bsize;
	/* 文件系统中除元数据外的block总数 */
	u64 f_blocks;
	/* 除metadata和已经被使用的block,还有多少可用block */
	u64 f_bfree;
	/* 除metadata和已经被使用的block,还有多少可用block */
	u64 f_bavail;
	/* 文件系统中最多可有多少文件(inode) */
	u64 f_files;
	/* 文件系统中还可以创建多少个文件(还有多少可用inode) */
	u64 f_ffree;
	__kernel_fsid_t f_fsid;
	/* 支持文件名长度 */
	long f_namelen;
	long f_frsize;
	long f_spare[5];
};

#endif
