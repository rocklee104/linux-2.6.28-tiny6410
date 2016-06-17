/*
 *  linux/fs/minix/namei.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include "minix.h"

/*
 * 在dentry->d_parent下插入一个目录项. 插入的这个目录项不能是目录.
 * 对于目录,如果minix_add_link失败,需要调用2次inode_dec_link_count(inode).
 * 并且需要inode_dec_link_count(dir);
 */
static int add_nondir(struct dentry *dentry, struct inode *inode)
{
	/* 在dentry的父目录中插入一个目录项 */
	int err = minix_add_link(dentry, inode);
	if (!err) {
		/* 将inode和dentry关联起来 */
		d_instantiate(dentry, inode);
		return 0;
	}
	/*
	 * 1.如果minix_mknod,minix_symlink调用,link == 1
	 *		完全将inode删除
	 * 2.如果minix_unlink调用,link > 1
	 * 		减少link,并且减少i_count
	 */
	inode_dec_link_count(inode);
	/* iput中会进行atomit_dec(&inode->i_count)操作 */
	iput(inode);
	return err;
}

/*
 * dir:需要查找的文件父目录的inode
 * dentry:需要查找文件的dentry
 */
static struct dentry *minix_lookup(struct inode * dir, struct dentry *dentry, struct nameidata *nd)
{
	struct inode * inode = NULL;
	ino_t ino;

	dentry->d_op = dir->i_sb->s_root->d_op;

	if (dentry->d_name.len > minix_sb(dir->i_sb)->s_namelen)
		return ERR_PTR(-ENAMETOOLONG);

	/* 通过vfs dentry获取到inode number */
	ino = minix_inode_by_name(dentry);
	if (ino) {
		/* 通过inode number获取到vfs inode */
		inode = minix_iget(dir->i_sb, ino);
		if (IS_ERR(inode))
			return ERR_CAST(inode);
	}
	/*
	 * 在创建文件或者其他一些操作前,lookup最先执行,这时候dentry是刚创建出来的,
	 * 还没有加入hashtable,所以使用d_add而非d_instantiate.
	 */
	d_add(dentry, inode);
	return NULL;
}

/* 创建文件,不需要增加父目录的硬链接数量 */
static int minix_mknod(struct inode * dir, struct dentry *dentry, int mode, dev_t rdev)
{
	int error;
	struct inode *inode;

	if (!old_valid_dev(rdev))
		return -EINVAL;

	inode = minix_new_inode(dir, &error);

	if (inode) {
		inode->i_mode = mode;
		minix_set_inode(inode, rdev);
		/* init_special_inode中可能会改变inode的i_rdev */
		mark_inode_dirty(inode);
		/* 如果add_nondir中出现失败,会调用iput将之前分配的inode释放 */
		error = add_nondir(dentry, inode);
	}
	return error;
}

static int minix_create(struct inode * dir, struct dentry *dentry, int mode,
		struct nameidata *nd)
{
	return minix_mknod(dir, dentry, mode, 0);
}

/*
 * dir: 符号链接父目录的inode
 * dentry: 符号连接的dentry
 * sysname: 符号连接指向的文件名
 */
static int minix_symlink(struct inode * dir, struct dentry *dentry,
	  const char * symname)
{
	int err = -ENAMETOOLONG;
	int i = strlen(symname)+1;
	struct inode * inode;

	if (i > dir->i_sb->s_blocksize)
		goto out;

	inode = minix_new_inode(dir, &err);
	if (!inode)
		goto out;

	inode->i_mode = S_IFLNK | 0777;
	minix_set_inode(inode, 0);
	/* 将文件名写入符号链接的地址空间中 */
	err = page_symlink(inode, symname, i);
	if (err)
		goto out_fail;

	err = add_nondir(dentry, inode);
out:
	return err;

out_fail:
	inode_dec_link_count(inode);
	iput(inode);
	goto out;
}

/* 添加一个硬链接,old_dentry和dentry指向同一个inode */
static int minix_link(struct dentry * old_dentry, struct inode * dir,
	struct dentry *dentry)
{
	struct inode *inode = old_dentry->d_inode;
	int err;

	/* inode的硬链接数量不能超过文件系统上限 */
	if (inode->i_nlink >= minix_sb(inode->i_sb)->s_link_max)
		return -EMLINK;

	/* inode的硬链接数量改变,但内容不改变,所以只需要更新ctime */
	inode->i_ctime = CURRENT_TIME_SEC;
	/*
	 * 增加inode的硬链接数量,已经将inode标记为dirty了,
	 * 所以在修改了inode->ctime之后不需要再显式调用mark_inode_dirty(inode);
	 */
	inode_inc_link_count(inode);
	atomic_inc(&inode->i_count);
	/* 在dentry的父目录中添加一个目录项,并将目录项和inode关联起来 */
	err = add_nondir(dentry, inode);
	return err;
}

static int minix_mkdir(struct inode * dir, struct dentry *dentry, int mode)
{
	struct inode * inode;
	int err = -EMLINK;

	if (dir->i_nlink >= minix_sb(dir->i_sb)->s_link_max)
		goto out;

	/* 增加父目录的硬链接数量,因为子目录中存在'..'的硬链接 */
	inode_inc_link_count(dir);

	inode = minix_new_inode(dir, &err);
	if (!inode)
		goto out_dir;

	inode->i_mode = S_IFDIR | mode;
	/* 继承父目录的SGID */
	if (dir->i_mode & S_ISGID)
		inode->i_mode |= S_ISGID;
	minix_set_inode(inode, 0);

	/* 目录的初始link为2 */
	inode_inc_link_count(inode);

	/* 为空目录写入'.'及'..'这两个目录项,其中的dir_commit_chunk会将inode标记为dirty */
	err = minix_make_empty(inode, dir);
	if (err)
		goto out_fail;

	err = minix_add_link(dentry, inode);
	if (err)
		goto out_fail;

	d_instantiate(dentry, inode);
out:
	return err;

out_fail:
	inode_dec_link_count(inode);
	inode_dec_link_count(inode);
	iput(inode);
out_dir:
	/* 减少父目录的硬链接数 */
	inode_dec_link_count(dir);
	goto out;
}

/* dir是dentry这个文件的父目录的inode */
static int minix_unlink(struct inode * dir, struct dentry *dentry)
{
	int err = -ENOENT;
	struct inode * inode = dentry->d_inode;
	struct page * page;
	struct minix_dir_entry * de;

	/* 找到目录项及目录项所在的page */
	de = minix_find_entry(dentry, &page);
	if (!de)
		goto end_unlink;

	err = minix_delete_entry(de, page);
	if (err)
		goto end_unlink;

	inode->i_ctime = dir->i_ctime;
	/* 减少inode硬链接数量,周期写入时会将根据vfs inode回写minix inode */
	inode_dec_link_count(inode);
end_unlink:
	return err;
}

/* dir是dentry这个文件的父目录的inode */
static int minix_rmdir(struct inode * dir, struct dentry *dentry)
{
	struct inode * inode = dentry->d_inode;
	int err = -ENOTEMPTY;

	/* 判断目录是否非空 */
	if (minix_empty_dir(inode)) {
		/* 在父目录中的数据块中将要删除的目录的inode number清零,并且减少目标目录的硬链接数 */
		err = minix_unlink(dir, dentry);
		if (!err) {
			/* 减少父目录的硬链接的个数 */
			inode_dec_link_count(dir);
			/* 减少目标目录的硬链接数 */
			inode_dec_link_count(inode);
		}
	}
	return err;
}

static int minix_rename(struct inode * old_dir, struct dentry *old_dentry,
			   struct inode * new_dir, struct dentry *new_dentry)
{
	struct minix_sb_info * info = minix_sb(old_dir->i_sb);
	struct inode * old_inode = old_dentry->d_inode;
	struct inode * new_inode = new_dentry->d_inode;
	struct page * dir_page = NULL;
	struct minix_dir_entry * dir_de = NULL;
	struct page * old_page;
	struct minix_dir_entry * old_de;
	int err = -ENOENT;

	/* 旧文件目录项 */
	old_de = minix_find_entry(old_dentry, &old_page);
	if (!old_de)
		goto out;

	if (S_ISDIR(old_inode->i_mode)) {
		err = -EIO;
		/* 找到old目录中".."的目录项 */
		dir_de = minix_dotdot(old_inode, &dir_page);
		if (!dir_de)
			goto out_old;
	}

	if (new_inode) {
		/* 新文件本身就存在 */
		struct page * new_page;
		struct minix_dir_entry * new_de;

		err = -ENOTEMPTY;
		/* 新目录必须是空目录才能rename */
		if (dir_de && !minix_empty_dir(new_inode))
			goto out_dir;

		err = -ENOENT;
		/* 找出新文件的目录项 */
		new_de = minix_find_entry(new_dentry, &new_page);
		if (!new_de)
			goto out_dir;
		/*
		 * 新文件的dentry指向旧文件的inode,对于目录来说还需要增加link一次,
		 * 但是在之后删除inode条目时,只减少了link一次,所以对于目录来说,
		 * 这里不需要再增加link.
		 */
		inode_inc_link_count(old_inode);
		/*
		 * 新文件的目录项中记录旧文件的inode,不需要再dir下插入目录项.
		 * vfs层, olde_dentry仍然指向old_inode, new_dentry指向new_inode.
		 * vfs_rename会将调用d_move处理old_dentry和new_dentry.
		 */
		minix_set_link(new_de, new_page, old_inode);
		/* new inode的nlink的变化也会改变ctime */
		new_inode->i_ctime = CURRENT_TIME_SEC;
		/* new_inode表示一个目录的时候 */
		if (dir_de)
			drop_nlink(new_inode);
		inode_dec_link_count(new_inode);
	} else {
		/* 新文件不存在,需要创建 */
		if (dir_de) {
			err = -EMLINK;
			if (new_dir->i_nlink >= info->s_link_max)
				goto out_dir;
		}
		/* 因为有新的dentry使用了这个inode,要增加硬链接计数 */
		inode_inc_link_count(old_inode);
		/* 在new_dentry的父目录下插入一条目录项,指向old_inode */
		err = minix_add_link(new_dentry, old_inode);
		if (err) {
			inode_dec_link_count(old_inode);
			goto out_dir;
		}
		/* dir_de不为空,表示rename的是一个目录,新目录的父目录硬链接数量需要增加 */
		if (dir_de)
			inode_inc_link_count(new_dir);
	}

	/* 在磁盘上删除旧文件目录项 */
	minix_delete_entry(old_de, old_page);
	/* 旧的dentry要被删除,需要减少old_inode的硬链接计数 */
	inode_dec_link_count(old_inode);

	if (dir_de) {
		/* dir_de是old_inode地址空间中的".."的目录项,old_inode的父目录需要改变成new_dir */
		minix_set_link(dir_de, dir_page, new_dir);
		/* 旧的目录被删除,需要减少旧目录的父目录硬链接数量 */
		inode_dec_link_count(old_dir);
	}
	return 0;

out_dir:
	if (dir_de) {
		kunmap(dir_page);
		page_cache_release(dir_page);
	}
out_old:
	kunmap(old_page);
	page_cache_release(old_page);
out:
	return err;
}

/*
 * directories can handle most operations...
 */
const struct inode_operations minix_dir_inode_operations = {
	.create		= minix_create,
	.lookup		= minix_lookup,
	.link		= minix_link,
	.unlink		= minix_unlink,
	.symlink	= minix_symlink,
	.mkdir		= minix_mkdir,
	.rmdir		= minix_rmdir,
	.mknod		= minix_mknod,
	.rename		= minix_rename,
	.getattr	= minix_getattr,
};
