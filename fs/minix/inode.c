/*
 *  linux/fs/minix/inode.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  Copyright (C) 1996  Gertjan van Wingerde    (gertjan@cs.vu.nl)
 *	Minix V2 fs support.
 *
 *  Modified for 680x0 by Andreas Schwab
 *  Updated to filesystem version 3 by Daniel Aragones
 */

#include <linux/module.h>
#include "minix.h"
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/highuid.h>
#include <linux/vfs.h>

static int minix_write_inode(struct inode * inode, int wait);
static int minix_statfs(struct dentry *dentry, struct kstatfs *buf);
static int minix_remount (struct super_block * sb, int * flags, char * data);

static void minix_delete_inode(struct inode *inode)
{
	/* 清除inode的address_space中的所有数据 */
	truncate_inode_pages(&inode->i_data, 0);
	/* inode表示的文件大小为0,之后minix_truncate由于inode->i_size = 0也不会做什么实际内容 */
	inode->i_size = 0;
	minix_truncate(inode);
	minix_free_inode(inode);
}

/* 销毁minix_fill_super分配的sb数据,剩下根目录使用的inode和dentry没有释放掉了 */
static void minix_put_super(struct super_block *sb)
{
	int i;
	struct minix_sb_info *sbi = minix_sb(sb);

	if (!(sb->s_flags & MS_RDONLY)) {
		/* 如果文件系统可读写 */
		if (sbi->s_version != MINIX_V3)	 /* s_state is now out from V3 sb */
			/* v1/v2 super block才有s_state */
			sbi->s_ms->s_state = sbi->s_mount_state;
		/* buffer中super block内容被改变了,将buffer标记为dirty */
		mark_buffer_dirty(sbi->s_sbh);
	}
	/* 减少map所在的buffer的引用计数,之后这些buffer就可能被释放 */
	for (i = 0; i < sbi->s_imap_blocks; i++)
		brelse(sbi->s_imap[i]);
	for (i = 0; i < sbi->s_zmap_blocks; i++)
		brelse(sbi->s_zmap[i]);
	/* 减少sb buffer的引用计数 */
	brelse (sbi->s_sbh);
	/* 仅仅释放imap就行了,因为zmap不是单独分配的而是指向imap空间的一部分 */
	kfree(sbi->s_imap);
	sb->s_fs_info = NULL;
	kfree(sbi);

	return;
}

static struct kmem_cache * minix_inode_cachep;

/* 分配一个inode info,并将其中的vfs_inode返回 */
static struct inode *minix_alloc_inode(struct super_block *sb)
{
	struct minix_inode_info *ei;
	/*
	 * 从minix_inode缓存池中获取一个inode_info,在分配这个inode_info过程中,
	 * inode_info已经被init_once初始化过了.
	 */
	ei = (struct minix_inode_info *)kmem_cache_alloc(minix_inode_cachep, GFP_KERNEL);
	if (!ei)
		return NULL;
	return &ei->vfs_inode;
}

static void minix_destroy_inode(struct inode *inode)
{
	kmem_cache_free(minix_inode_cachep, minix_i(inode));
}

static void init_once(void *foo)
{
	struct minix_inode_info *ei = (struct minix_inode_info *) foo;

	/* 初始化vfs inode */
	inode_init_once(&ei->vfs_inode);
}

static int init_inodecache(void)
{
	minix_inode_cachep = kmem_cache_create("minix_inode_cache",
					     sizeof(struct minix_inode_info),
					     0, (SLAB_RECLAIM_ACCOUNT|
						SLAB_MEM_SPREAD),
					     init_once);
	if (minix_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void destroy_inodecache(void)
{
	kmem_cache_destroy(minix_inode_cachep);
}

static const struct super_operations minix_sops = {
	.alloc_inode	= minix_alloc_inode,
	.destroy_inode	= minix_destroy_inode,
	.write_inode	= minix_write_inode,
	.delete_inode	= minix_delete_inode,
	.put_super	= minix_put_super,
	.statfs		= minix_statfs,
	.remount_fs	= minix_remount,
};

static int minix_remount (struct super_block * sb, int * flags, char * data)
{
	struct minix_sb_info * sbi = minix_sb(sb);
	struct minix_super_block * ms;

	ms = sbi->s_ms;
	/* 当通过命令mount -o remount,ro执行remount的时候,ro以MS_RDONLY标记 */
	if ((*flags & MS_RDONLY) == (sb->s_flags & MS_RDONLY))
		return 0;
	/* 将之前rw的fs, remount为readonly */
	if (*flags & MS_RDONLY) {
		if (ms->s_state & MINIX_VALID_FS ||
		    !(sbi->s_mount_state & MINIX_VALID_FS))
		    /* 文件系统应该还没挂载起来 */
			return 0;
		/* Mounting a rw partition read-only. */
		if (sbi->s_version != MINIX_V3)
			/* v1/v2 */
			ms->s_state = sbi->s_mount_state;
		mark_buffer_dirty(sbi->s_sbh);
	} else {
	  	/* Mount a partition which is read-only, read-write. */
		if (sbi->s_version != MINIX_V3) {
			sbi->s_mount_state = ms->s_state;
			/* 用于判断文件系统是否经历过突然断电 */
			ms->s_state &= ~MINIX_VALID_FS;
		} else {
			sbi->s_mount_state = MINIX_VALID_FS;
		}
		mark_buffer_dirty(sbi->s_sbh);

		if (!(sbi->s_mount_state & MINIX_VALID_FS))
			printk("MINIX-fs warning: remounting unchecked fs, "
				"running fsck is recommended\n");
		else if ((sbi->s_mount_state & MINIX_ERROR_FS))
			printk("MINIX-fs warning: remounting fs with errors, "
				"running fsck is recommended\n");
	}
	return 0;
}

static int minix_fill_super(struct super_block *s, void *data, int silent)
{
	struct buffer_head *bh;
	struct buffer_head **map;
	struct minix_super_block *ms;
	struct minix3_super_block *m3s = NULL;
	unsigned long i, block;
	struct inode *root_inode;
	struct minix_sb_info *sbi;
	int ret = -EINVAL;

	/* 在内存中建立一个sb信息表,查阅sb信息的时候就不需要读取minix sb */
	sbi = kzalloc(sizeof(struct minix_sb_info), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	/* vfs sb和minix sb info关联起来 */
	s->s_fs_info = sbi;

	BUILD_BUG_ON(32 != sizeof (struct minix_inode));
	BUILD_BUG_ON(64 != sizeof(struct minix2_inode));

	/* 给vfs sb设置块大小 */
	if (!sb_set_blocksize(s, BLOCK_SIZE))
		goto out_bad_hblock;

	/* 从磁盘中读取含有sb的bh */
	if (!(bh = sb_bread(s, 1)))
		goto out_bad_sb;

	/* 获取buffer中的minix sb部分 */
	ms = (struct minix_super_block *) bh->b_data;
	sbi->s_ms = ms;
	sbi->s_sbh = bh;
	sbi->s_mount_state = ms->s_state;
	sbi->s_ninodes = ms->s_ninodes;
	sbi->s_nzones = ms->s_nzones;
	sbi->s_imap_blocks = ms->s_imap_blocks;
	sbi->s_zmap_blocks = ms->s_zmap_blocks;
	sbi->s_firstdatazone = ms->s_firstdatazone;
	sbi->s_log_zone_size = ms->s_log_zone_size;
	sbi->s_max_size = ms->s_max_size;
	s->s_magic = ms->s_magic;
	if (s->s_magic == MINIX_SUPER_MAGIC) {
		sbi->s_version = MINIX_V1;
		/* dirsize包含了2个字节的inode,和14个字节的name */
		sbi->s_dirsize = 16;
		sbi->s_namelen = 14;
		sbi->s_link_max = MINIX_LINK_MAX;
	} else if (s->s_magic == MINIX_SUPER_MAGIC2) {
		sbi->s_version = MINIX_V1;
		sbi->s_dirsize = 32;
		sbi->s_namelen = 30;
		sbi->s_link_max = MINIX_LINK_MAX;
	} else if (s->s_magic == MINIX2_SUPER_MAGIC) {
		sbi->s_version = MINIX_V2;
		sbi->s_nzones = ms->s_zones;
		sbi->s_dirsize = 16;
		sbi->s_namelen = 14;
		sbi->s_link_max = MINIX2_LINK_MAX;
	} else if (s->s_magic == MINIX2_SUPER_MAGIC2) {
		sbi->s_version = MINIX_V2;
		/* v2 sb使用s_zones */
		sbi->s_nzones = ms->s_zones;
		sbi->s_dirsize = 32;
		sbi->s_namelen = 30;
		sbi->s_link_max = MINIX2_LINK_MAX;
		/* v3中的magic位置和之前的sb不一样 */
	} else if ( *(__u16 *)(bh->b_data + 24) == MINIX3_SUPER_MAGIC) {
		m3s = (struct minix3_super_block *) bh->b_data;
		s->s_magic = m3s->s_magic;
		sbi->s_imap_blocks = m3s->s_imap_blocks;
		sbi->s_zmap_blocks = m3s->s_zmap_blocks;
		sbi->s_firstdatazone = m3s->s_firstdatazone;
		sbi->s_log_zone_size = m3s->s_log_zone_size;
		sbi->s_max_size = m3s->s_max_size;
		sbi->s_ninodes = m3s->s_ninodes;
		sbi->s_nzones = m3s->s_zones;
		/* inode占用4个字节 */
		sbi->s_dirsize = 64;
		/* 文件名支持60字节 */
		sbi->s_namelen = 60;
		sbi->s_version = MINIX_V3;
		sbi->s_link_max = MINIX2_LINK_MAX;
		/* v3只要一挂载,就必定是有效的 */
		sbi->s_mount_state = MINIX_VALID_FS;
		/* v3支持不同的block size */
		sb_set_blocksize(s, m3s->s_blocksize);
	} else
		goto out_no_fs;

	/*
	 * Allocate the buffer map to keep the superblock small.
	 */
	if (sbi->s_imap_blocks == 0 || sbi->s_zmap_blocks == 0)
		goto out_illegal_sb;
	/* 将bitmap读取到内存中 */
	i = (sbi->s_imap_blocks + sbi->s_zmap_blocks) * sizeof(bh);
	map = kzalloc(i, GFP_KERNEL);
	if (!map)
		goto out_no_map;
	sbi->s_imap = &map[0];
	sbi->s_zmap = &map[sbi->s_imap_blocks];

	block=2;
	for (i=0 ; i < sbi->s_imap_blocks ; i++) {
		if (!(sbi->s_imap[i]=sb_bread(s, block)))
			goto out_no_bitmap;
		block++;
	}
	for (i=0 ; i < sbi->s_zmap_blocks ; i++) {
		if (!(sbi->s_zmap[i]=sb_bread(s, block)))
			goto out_no_bitmap;
		block++;
	}

	minix_set_bit(0,sbi->s_imap[0]->b_data);
	minix_set_bit(0,sbi->s_zmap[0]->b_data);

	/* set up enough so that it can read an inode */
	s->s_op = &minix_sops;
	/* 构建root */
	root_inode = minix_iget(s, MINIX_ROOT_INO);
	if (IS_ERR(root_inode)) {
		ret = PTR_ERR(root_inode);
		goto out_no_root;
	}

	ret = -ENOMEM;
	s->s_root = d_alloc_root(root_inode);
	if (!s->s_root)
		goto out_iput;

	/* 文件系统可读写,就要考虑突然断电的情况 */
	if (!(s->s_flags & MS_RDONLY)) {
		if (sbi->s_version != MINIX_V3) /* s_state is now out from V3 sb */
			/* v1,v2需要标记文件系统无效,将无效标志写入磁盘 */
			ms->s_state &= ~MINIX_VALID_FS;
		mark_buffer_dirty(bh);
	}
	/* 判断从磁盘中读取的s_mount_state,正常状态sbi中MINIX_VALID_FS,ms中~MINIX_VALID_FS */
	if (!(sbi->s_mount_state & MINIX_VALID_FS))
		printk("MINIX-fs: mounting unchecked file system, "
			"running fsck is recommended\n");
 	else if (sbi->s_mount_state & MINIX_ERROR_FS)
		printk("MINIX-fs: mounting file system with errors, "
			"running fsck is recommended\n");
	return 0;

out_iput:
	iput(root_inode);
	goto out_freemap;

out_no_root:
	if (!silent)
		printk("MINIX-fs: get root inode failed\n");
	goto out_freemap;

out_no_bitmap:
	printk("MINIX-fs: bad superblock or unable to read bitmaps\n");
out_freemap:
	for (i = 0; i < sbi->s_imap_blocks; i++)
		brelse(sbi->s_imap[i]);
	for (i = 0; i < sbi->s_zmap_blocks; i++)
		brelse(sbi->s_zmap[i]);
	/* 仅仅释放imap就行了,因为zmap不是单独分配的而是指向imap空间的一部分 */
	kfree(sbi->s_imap);
	goto out_release;

out_no_map:
	ret = -ENOMEM;
	if (!silent)
		printk("MINIX-fs: can't allocate map\n");
	goto out_release;

out_illegal_sb:
	if (!silent)
		printk("MINIX-fs: bad superblock\n");
	goto out_release;

out_no_fs:
	if (!silent)
		printk("VFS: Can't find a Minix filesystem V1 | V2 | V3 "
		       "on device %s.\n", s->s_id);
out_release:
	brelse(bh);
	goto out;

out_bad_hblock:
	printk("MINIX-fs: blocksize too small for device\n");
	goto out;

out_bad_sb:
	printk("MINIX-fs: unable to read superblock\n");
out:
	s->s_fs_info = NULL;
	kfree(sbi);
	return ret;
}

/* 列出文件或者目录的状态 */
static int minix_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct minix_sb_info *sbi = minix_sb(dentry->d_sb);
	buf->f_type = dentry->d_sb->s_magic;
	buf->f_bsize = dentry->d_sb->s_blocksize;
	/*
	 * 文件系统中除元数据外的block总数.本来减去s_first_data_block后,可用block数还需要加1,
	 * 但是rootdir占用1个block,这个时候,可用block数又需要减1,相互抵消.
	 */
	buf->f_blocks = (sbi->s_nzones - sbi->s_firstdatazone) << sbi->s_log_zone_size;
	/* 除metadata和已经被使用的block,还有多少可用block */
	buf->f_bfree = minix_count_free_blocks(sbi);
	buf->f_bavail = buf->f_bfree;
	buf->f_files = sbi->s_ninodes;
	buf->f_ffree = minix_count_free_inodes(sbi);
	buf->f_namelen = sbi->s_namelen;
	return 0;
}

static int minix_get_block(struct inode *inode, sector_t block,
		    struct buffer_head *bh_result, int create)
{
	if (INODE_VERSION(inode) == MINIX_V1)
		return V1_minix_get_block(inode, block, bh_result, create);
	else
		return V2_minix_get_block(inode, block, bh_result, create);
}

static int minix_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page, minix_get_block, wbc);
}

static int minix_readpage(struct file *file, struct page *page)
{
	return block_read_full_page(page,minix_get_block);
}

int __minix_write_begin(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned flags,
			struct page **pagep, void **fsdata)
{
	return block_write_begin(file, mapping, pos, len, flags, pagep, fsdata,
				minix_get_block);
}

static int minix_write_begin(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned flags,
			struct page **pagep, void **fsdata)
{
	/* 当创建一个文件的时候,inode没有关联的page */
	*pagep = NULL;
	return __minix_write_begin(file, mapping, pos, len, flags, pagep, fsdata);
}

static sector_t minix_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping,block,minix_get_block);
}

static const struct address_space_operations minix_aops = {
	.readpage = minix_readpage,
	.writepage = minix_writepage,
	.sync_page = block_sync_page,
	.write_begin = minix_write_begin,
	.write_end = generic_write_end,
	.bmap = minix_bmap
};

static const struct inode_operations minix_symlink_inode_operations = {
	.readlink	= generic_readlink,
	.follow_link	= page_follow_link_light,
	.put_link	= page_put_link,
	.getattr	= minix_getattr,
};

/* 给vfs inode一些操作函数集 */
void minix_set_inode(struct inode *inode, dev_t rdev)
{
	if (S_ISREG(inode->i_mode)) {
		inode->i_op = &minix_file_inode_operations;
		inode->i_fop = &minix_file_operations;
		inode->i_mapping->a_ops = &minix_aops;
	} else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &minix_dir_inode_operations;
		inode->i_fop = &minix_dir_operations;
		inode->i_mapping->a_ops = &minix_aops;
	} else if (S_ISLNK(inode->i_mode)) {
		inode->i_op = &minix_symlink_inode_operations;
		inode->i_mapping->a_ops = &minix_aops;
	} else
		/* 特殊设备 */
		init_special_inode(inode, inode->i_mode, rdev);
}

/*
 * The minix V1 function to read an inode.
 */
/* 用磁盘inode填充vfs inode */
static struct inode *V1_minix_iget(struct inode *inode)
{
	struct buffer_head * bh;
	struct minix_inode * raw_inode;
	/* 从vfs inode中获取minix inode info */
	struct minix_inode_info *minix_inode = minix_i(inode);
	int i;

	raw_inode = minix_V1_raw_inode(inode->i_sb, inode->i_ino, &bh);
	if (!raw_inode) {
		iget_failed(inode);
		return ERR_PTR(-EIO);
	}
	inode->i_mode = raw_inode->i_mode;
	inode->i_uid = (uid_t)raw_inode->i_uid;
	inode->i_gid = (gid_t)raw_inode->i_gid;
	inode->i_nlink = raw_inode->i_nlinks;
	inode->i_size = raw_inode->i_size;
	inode->i_mtime.tv_sec = inode->i_atime.tv_sec = inode->i_ctime.tv_sec = raw_inode->i_time;
	inode->i_mtime.tv_nsec = 0;
	inode->i_atime.tv_nsec = 0;
	inode->i_ctime.tv_nsec = 0;
	inode->i_blocks = 0;
	for (i = 0; i < 9; i++)
		minix_inode->u.i1_data[i] = raw_inode->i_zone[i];
	/* 给vfs inode设置minix相关的操作函数集 */
	minix_set_inode(inode, old_decode_dev(raw_inode->i_zone[0]));
	brelse(bh);
	/* 激活inode */
	unlock_new_inode(inode);
	return inode;
}

/*
 * The minix V2 function to read an inode.
 */
static struct inode *V2_minix_iget(struct inode *inode)
{
	struct buffer_head * bh;
	struct minix2_inode * raw_inode;
	struct minix_inode_info *minix_inode = minix_i(inode);
	int i;

	raw_inode = minix_V2_raw_inode(inode->i_sb, inode->i_ino, &bh);
	if (!raw_inode) {
		iget_failed(inode);
		return ERR_PTR(-EIO);
	}
	inode->i_mode = raw_inode->i_mode;
	inode->i_uid = (uid_t)raw_inode->i_uid;
	inode->i_gid = (gid_t)raw_inode->i_gid;
	inode->i_nlink = raw_inode->i_nlinks;
	inode->i_size = raw_inode->i_size;
	inode->i_mtime.tv_sec = raw_inode->i_mtime;
	inode->i_atime.tv_sec = raw_inode->i_atime;
	inode->i_ctime.tv_sec = raw_inode->i_ctime;
	inode->i_mtime.tv_nsec = 0;
	inode->i_atime.tv_nsec = 0;
	inode->i_ctime.tv_nsec = 0;
	inode->i_blocks = 0;
	/* 相比v1,v2多了一个3级间接块 */
	for (i = 0; i < 10; i++)
		minix_inode->u.i2_data[i] = raw_inode->i_zone[i];
	minix_set_inode(inode, old_decode_dev(raw_inode->i_zone[0]));
	brelse(bh);
	unlock_new_inode(inode);
	return inode;
}

/*
 * The global function to read an inode.
 */
struct inode *minix_iget(struct super_block *sb, unsigned long ino)
{
	struct inode *inode;

	/* 根据sb和ino获取inode */
	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	/* 如果inode不是新分配的,说明这个inode被初始化过了,就不需要再调用V1_minix_iget进行初始化了 */
	if (!(inode->i_state & I_NEW))
		return inode;

	if (INODE_VERSION(inode) == MINIX_V1)
		return V1_minix_iget(inode);
	else
		/* v2/v3 */
		return V2_minix_iget(inode);
}

/*
 * The minix V1 function to synchronize an inode.
 */
static struct buffer_head * V1_minix_update_inode(struct inode * inode)
{
	/* 包含minix inode的buffer */
	struct buffer_head * bh;
	struct minix_inode * raw_inode;
	struct minix_inode_info *minix_inode = minix_i(inode);
	int i;

	/* 从block layer获取minix inode,raw_inode可能内容为空(创建inode的时候) */
	raw_inode = minix_V1_raw_inode(inode->i_sb, inode->i_ino, &bh);
	if (!raw_inode)
		return NULL;

	/* 将vfs inode中的数据更新到磁盘上去 */
	raw_inode->i_mode = inode->i_mode;
	raw_inode->i_uid = fs_high2lowuid(inode->i_uid);
	raw_inode->i_gid = fs_high2lowgid(inode->i_gid);
	raw_inode->i_nlinks = inode->i_nlink;
	raw_inode->i_size = inode->i_size;
	raw_inode->i_time = inode->i_mtime.tv_sec;
	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
		/* 对于字符设备和块设备来说,不存在数据块,i_zone[0]用于保存设备号 */
		raw_inode->i_zone[0] = old_encode_dev(inode->i_rdev);
	else for (i = 0; i < 9; i++)
		raw_inode->i_zone[i] = minix_inode->u.i1_data[i];
	/* bh中的minix inode被修改了 */
	mark_buffer_dirty(bh);
	return bh;
}

/*
 * The minix V2 function to synchronize an inode.
 */
static struct buffer_head * V2_minix_update_inode(struct inode * inode)
{
	struct buffer_head * bh;
	struct minix2_inode * raw_inode;
	struct minix_inode_info *minix_inode = minix_i(inode);
	int i;

	raw_inode = minix_V2_raw_inode(inode->i_sb, inode->i_ino, &bh);
	if (!raw_inode)
		return NULL;
	raw_inode->i_mode = inode->i_mode;
	raw_inode->i_uid = fs_high2lowuid(inode->i_uid);
	raw_inode->i_gid = fs_high2lowgid(inode->i_gid);
	raw_inode->i_nlinks = inode->i_nlink;
	raw_inode->i_size = inode->i_size;
	raw_inode->i_mtime = inode->i_mtime.tv_sec;
	raw_inode->i_atime = inode->i_atime.tv_sec;
	raw_inode->i_ctime = inode->i_ctime.tv_sec;
	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
		raw_inode->i_zone[0] = old_encode_dev(inode->i_rdev);
	else for (i = 0; i < 10; i++)
		raw_inode->i_zone[i] = minix_inode->u.i2_data[i];
	mark_buffer_dirty(bh);
	return bh;
}

static struct buffer_head *minix_update_inode(struct inode *inode)
{
	if (INODE_VERSION(inode) == MINIX_V1)
		return V1_minix_update_inode(inode);
	else
		/* v2/v3 */
		return V2_minix_update_inode(inode);
}

static int minix_write_inode(struct inode * inode, int wait)
{
	/* 将vfs中inode数据更新到buffer,减少这个buffer的引用计数是因为在获取磁盘inode时候,sb_bread增加了计数*/
	brelse(minix_update_inode(inode));
	return 0;
}

int minix_sync_inode(struct inode * inode)
{
	int err = 0;
	struct buffer_head *bh;

	/* 根据vfs inode更新buffer中的minix inode */
	bh = minix_update_inode(inode);
	if (bh && buffer_dirty(bh))
	{
		/* 如果minix inode被修改,就直接同步到磁盘上去,sync_dirty_buffer会set_buffer_uptodate */
		sync_dirty_buffer(bh);
		if (buffer_req(bh) && !buffer_uptodate(bh))
		{
			printk("IO error syncing minix inode [%s:%08lx]\n",
				inode->i_sb->s_id, inode->i_ino);
			err = -1;
		}
	} /* 当buffer clean的时候直接返回0 */
	else if (!bh)
		err = -1;
	brelse (bh);
	return err;
}

/* ls命令最终会调用到这个函数,显示的block数量是以512为单位的 */
int minix_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *stat)
{
	struct inode *dir = dentry->d_parent->d_inode;
	struct super_block *sb = dir->i_sb;
	generic_fillattr(dentry->d_inode, stat);
	if (INODE_VERSION(dentry->d_inode) == MINIX_V1)
		stat->blocks = (BLOCK_SIZE / 512) * V1_minix_blocks(stat->size, sb);
	else
		stat->blocks = (sb->s_blocksize / 512) * V2_minix_blocks(stat->size, sb);
	stat->blksize = sb->s_blocksize;
	return 0;
}

/*
 * The function that is called for file truncation.
 */
/* 进入这个函数之前,inode->i_zsize已经被改变 */
void minix_truncate(struct inode * inode)
{
	/* 对于普通文件,目录及symbol link才需要truncate */
	if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) || S_ISLNK(inode->i_mode)))
		return;
	if (INODE_VERSION(inode) == MINIX_V1)
		V1_minix_truncate(inode);
	else
		V2_minix_truncate(inode);
}

static int minix_get_sb(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data, struct vfsmount *mnt)
{
	return get_sb_bdev(fs_type, flags, dev_name, data, minix_fill_super,
			   mnt);
}

static struct file_system_type minix_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "minix",
	.get_sb		= minix_get_sb,
	.kill_sb	= kill_block_super,
	/* 如果没有这个标志,/proc/filesystem中minix就会有nodev标记 */
	.fs_flags	= FS_REQUIRES_DEV,
};

static int __init init_minix_fs(void)
{
	/* 初始化minix inode缓存池 */
	int err = init_inodecache();
	if (err)
		goto out1;
	err = register_filesystem(&minix_fs_type);
	if (err)
		goto out;
	return 0;
out:
	/* 销毁minix inode缓存池 */
	destroy_inodecache();
out1:
	return err;
}

static void __exit exit_minix_fs(void)
{
	unregister_filesystem(&minix_fs_type);
	destroy_inodecache();
}

module_init(init_minix_fs)
module_exit(exit_minix_fs)
MODULE_LICENSE("GPL");

