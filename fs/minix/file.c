/*
 *  linux/fs/minix/file.c
 *
 *  Copyright (C) 1991, 1992 Linus Torvalds
 *
 *  minix regular file handling primitives
 */

#include <linux/buffer_head.h>		/* for fsync_inode_buffers() */
#include "minix.h"

/*
 * We have mostly NULLs here: the current defaults are OK for
 * the minix filesystem.
 */
int minix_sync_file(struct file *, struct dentry *, int);

const struct file_operations minix_file_operations = {
	.llseek		= generic_file_llseek,
	.read		= do_sync_read,
	.aio_read	= generic_file_aio_read,
	.write		= do_sync_write,
	.aio_write	= generic_file_aio_write,
	.mmap		= generic_file_mmap,
	.fsync		= minix_sync_file,
	.splice_read	= generic_file_splice_read,
};

const struct inode_operations minix_file_inode_operations = {
	.truncate	= minix_truncate,
	.getattr	= minix_getattr,
};

/* 同步文件使用的间接块及文件inode所在的buffer, datasync用于表示是否只同步数据块及间接块 */
int minix_sync_file(struct file * file, struct dentry *dentry, int datasync)
{
	struct inode *inode = dentry->d_inode;
	int err;

	/* 同步文件所使用的间接块,文件dirty的数据块内容已经在vfs层完成 */
	err = sync_mapping_buffers(inode->i_mapping);
	if (!(inode->i_state & I_DIRTY))
		return err;
	/* 仅仅需要同步数据,不需要同步metadata.并且inode没有需要马上回写的metadata */
	if (datasync && !(inode->i_state & I_DIRTY_DATASYNC))
		return err;

	/*
	 * 1.fdatasync调用本函数(只回写数据,不回写metadata),
	 *   但是inode metadata需要马上回写(由I_DIRTY_DATASYNC标记).
	 * 2.fsync调用本函数.
	 *
	 * 这样就实现了fdatasync基本要求---除万不得已,一般不回写metadata
	 */
	err |= minix_sync_inode(inode);
	return err ? -EIO : 0;
}
