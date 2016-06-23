#ifndef __LINUX_UIO_H
#define __LINUX_UIO_H

#include <linux/compiler.h>
#include <linux/types.h>

/*
 *	Berkeley style UIO structures	-	Alan Cox 1994.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */

/*
 * read系统调用只读取数据到一个用户空间缓冲区,而另外还有一个系统调用readv
 * 可以一次性读取数据到多个用户空间缓冲区.为了同时支持着两个系统调用,引入了
 * 数据结构iovec,字面意思为I/O向量,实际上代表的是一个用户空间缓冲区
 */
struct iovec
{
	/* 用户缓冲区地址 */
	void __user *iov_base;	/* BSD uses caddr_t (1003.1g requires void *) */
	/* 缓冲区长度 */
	__kernel_size_t iov_len; /* Must be size_t (1003.1g) */
};

#ifdef __KERNEL__

struct kvec {
	void *iov_base; /* and that should *never* hold a userland pointer */
	size_t iov_len;
};

#endif

/*
 *	UIO_MAXIOV shall be at least 16 1003.1g (5.4.1.1)
 */
 
#define UIO_FASTIOV	8
#define UIO_MAXIOV	1024

/*
 * Total number of bytes covered by an iovec.
 *
 * NOTE that it is not safe to use this function until all the iovec's
 * segment lengths have been validated.  Because the individual lengths can
 * overflow a size_t when added together.
 */
static inline size_t iov_length(const struct iovec *iov, unsigned long nr_segs)
{
	unsigned long seg;
	size_t ret = 0;

	for (seg = 0; seg < nr_segs; seg++)
		ret += iov[seg].iov_len;
	return ret;
}

unsigned long iov_shorten(struct iovec *iov, unsigned long nr_segs, size_t to);

#endif
