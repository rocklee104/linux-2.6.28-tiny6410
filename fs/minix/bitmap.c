/*
 *  linux/fs/minix/bitmap.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * Modified for 680x0 by Hamish Macdonald
 * Fixed for 680x0 by Andreas Schwab
 */

/* bitmap.c contains the code that handles the inode and block bitmaps */

#include "minix.h"
#include <linux/smp_lock.h>
#include <linux/buffer_head.h>
#include <linux/bitops.h>
#include <linux/sched.h>

/* 0-15内每一个数字转换成2进制后,数字中0的个数 */
static const int nibblemap[] = { 4,3,3,2,3,2,2,1,3,2,2,1,2,1,1,0 };

static unsigned long count_free(struct buffer_head *map[], unsigned numblocks, __u32 numbits)
{
	unsigned i, j, sum = 0;
	struct buffer_head *bh;

  	/* 首先计算numbits中以一个block bits对齐部分的空闲bit */
	for (i=0; i<numblocks-1; i++) {
		if (!(bh=map[i]))
			return(0);
		for (j=0; j<bh->b_size; j++)
			sum += nibblemap[bh->b_data[j] & 0xf]
				+ nibblemap[(bh->b_data[j]>>4) & 0xf];
	}

	if (numblocks==0 || !(bh=map[numblocks-1]))
		/* 如果统计的block个数为0或者map中没有对应最后一个block的buffer */
		return(0);
	/*
	 * bh目前仍然指向map中最后一个buffer,处理numbits处于bitmap中
	 * 未以block bits对齐但是以16bit对齐的部分.
	 */
	i = ((numbits - (numblocks-1) * bh->b_size * 8) / 16) * 2;
	for (j=0; j<i; j++) {
		sum += nibblemap[bh->b_data[j] & 0xf]
			+ nibblemap[(bh->b_data[j]>>4) & 0xf];
	}

	/* 处理numbits中未以16bits对齐的部分 */
	i = numbits%16;
	if (i!=0) {
		i = *(__u16 *)(&bh->b_data[j]) | ~((1<<i) - 1);
		sum += nibblemap[i & 0xf] + nibblemap[(i>>4) & 0xf];
		sum += nibblemap[(i>>8) & 0xf] + nibblemap[(i>>12) & 0xf];
	}
	return(sum);
}

/* 仅仅清除block对应bitmap中的bit */
void minix_free_block(struct inode *inode, unsigned long block)
{
	struct super_block *sb = inode->i_sb;
	struct minix_sb_info *sbi = minix_sb(sb);
	struct buffer_head *bh;
	/* s_blocksize_bits表示的是字节,需要将其转换为bit */
	int k = sb->s_blocksize_bits + 3;
	unsigned long bit, zone;

	/* block范围检查 */
	if (block < sbi->s_firstdatazone || block >= sbi->s_nzones) {
		printk("Trying to free block not in datazone\n");
		return;
	}
	/* metadata使用的block并不在zmap中 */
	zone = block - sbi->s_firstdatazone + 1;
	/* 未以block size对齐部分 */
	bit = zone & ((1<<k) - 1);
	/* 位图中第几个block */
	zone >>= k;
	if (zone >= sbi->s_zmap_blocks) {
		printk("minix_free_block: nonexistent bitmap buffer\n");
		return;
	}
	/* 获取对应bit的buffer */
	bh = sbi->s_zmap[zone];
	lock_kernel();
	/* 清除buffer中对应block的bit */
	if (!minix_test_and_clear_bit(bit, bh->b_data))
		printk("minix_free_block (%s:%lu): bit already cleared\n",
		       sb->s_id, block);
	unlock_kernel();
	mark_buffer_dirty(bh);
	return;
}

int minix_new_block(struct inode * inode)
{
	struct minix_sb_info *sbi = minix_sb(inode->i_sb);
	int bits_per_zone = 8 * inode->i_sb->s_blocksize;
	int i;

	for (i = 0; i < sbi->s_zmap_blocks; i++) {
		struct buffer_head *bh = sbi->s_zmap[i];
		int j;

		lock_kernel();
		j = minix_find_first_zero_bit(bh->b_data, bits_per_zone);
		if (j < bits_per_zone) {
			minix_set_bit(j, bh->b_data);
			unlock_kernel();
			mark_buffer_dirty(bh);
			j += i * bits_per_zone + sbi->s_firstdatazone-1;
			if (j < sbi->s_firstdatazone || j >= sbi->s_nzones)
				break;
			return j;
		}
		unlock_kernel();
	}
	return 0;
}

/* 返回除metadata外还有多少可用block */
unsigned long minix_count_free_blocks(struct minix_sb_info *sbi)
{
	return (count_free(sbi->s_zmap, sbi->s_zmap_blocks,
		sbi->s_nzones - sbi->s_firstdatazone + 1)
		<< sbi->s_log_zone_size);
}

struct minix_inode *
minix_V1_raw_inode(struct super_block *sb, ino_t ino, struct buffer_head **bh)
{
	int block;
	struct minix_sb_info *sbi = minix_sb(sb);
	struct minix_inode *p;

	/* inode取值区间是(0,sbi->s_ninodes] */
	if (!ino || ino > sbi->s_ninodes) {
		printk("Bad inode number on dev %s: %ld is out of range\n",
		       sb->s_id, (long)ino);
		return NULL;
	}
	/* inode从1开始计数,为了后面在inode table中查找inode方便,使ino从0开始 */
	ino--;
	/* 找到inode所在的block */
	block = 2 + sbi->s_imap_blocks + sbi->s_zmap_blocks +
		 ino / MINIX_INODES_PER_BLOCK;
	*bh = sb_bread(sb, block);
	if (!*bh) {
		printk("Unable to read inode block\n");
		return NULL;
	}
	p = (void *)(*bh)->b_data;
	return p + ino % MINIX_INODES_PER_BLOCK;
}

struct minix2_inode *
minix_V2_raw_inode(struct super_block *sb, ino_t ino, struct buffer_head **bh)
{
	int block;
	struct minix_sb_info *sbi = minix_sb(sb);
	struct minix2_inode *p;
	/* v2没有一个类似于MINIX_INODES_PER_BLOCK这样的宏 */
	int minix2_inodes_per_block = sb->s_blocksize / sizeof(struct minix2_inode);

	*bh = NULL;
	if (!ino || ino > sbi->s_ninodes) {
		printk("Bad inode number on dev %s: %ld is out of range\n",
		       sb->s_id, (long)ino);
		return NULL;
	}
	ino--;
	block = 2 + sbi->s_imap_blocks + sbi->s_zmap_blocks +
		 ino / minix2_inodes_per_block;
	*bh = sb_bread(sb, block);
	if (!*bh) {
		printk("Unable to read inode block\n");
		return NULL;
	}
	p = (void *)(*bh)->b_data;
	return p + ino % minix2_inodes_per_block;
}

/* Clear the link count and mode of a deleted inode on disk. */

static void minix_clear_inode(struct inode *inode)
{
	struct buffer_head *bh = NULL;

	if (INODE_VERSION(inode) == MINIX_V1) {
		struct minix_inode *raw_inode;
		/* 从磁盘上读取inode到buffer中 */
		raw_inode = minix_V1_raw_inode(inode->i_sb, inode->i_ino, &bh);
		if (raw_inode) {
			/* 改变buffer中inode的值 */
			raw_inode->i_nlinks = 0;
			raw_inode->i_mode = 0;
		}
	} else {
		struct minix2_inode *raw_inode;
		raw_inode = minix_V2_raw_inode(inode->i_sb, inode->i_ino, &bh);
		if (raw_inode) {
			raw_inode->i_nlinks = 0;
			raw_inode->i_mode = 0;
		}
	}
	if (bh) {
		mark_buffer_dirty(bh);
		brelse (bh);
	}
}

/* 将磁盘上i_nlinks及i_mode设置为0,并且在imap中清除这个inode */
void minix_free_inode(struct inode * inode)
{
	struct super_block *sb = inode->i_sb;
	struct minix_sb_info *sbi = minix_sb(inode->i_sb);
	struct buffer_head *bh;
	int k = sb->s_blocksize_bits + 3;
	unsigned long ino, bit;

	ino = inode->i_ino;
	if (ino < 1 || ino > sbi->s_ninodes) {
		printk("minix_free_inode: inode 0 or nonexistent inode\n");
		goto out;
	}
	/* 未对齐部分 */
	bit = ino & ((1<<k) - 1);
	/* block number */
	ino >>= k;
	if (ino >= sbi->s_imap_blocks) {
		printk("minix_free_inode: nonexistent imap in superblock\n");
		goto out;
	}

	/* 将磁盘上i_nlinks及i_mode设置为0 */
	minix_clear_inode(inode);	/* clear on-disk copy */

	/* 清除imap中对应inode的bit,并将这个imap回写 */
	bh = sbi->s_imap[ino];
	lock_kernel();
	if (!minix_test_and_clear_bit(bit, bh->b_data))
		printk("minix_free_inode: bit %lu already cleared\n", bit);
	unlock_kernel();
	mark_buffer_dirty(bh);
 out:
	clear_inode(inode);		/* clear in-memory copy */
}

/* 在dir(创建文件的父目录inode)下创建inode */
struct inode * minix_new_inode(const struct inode * dir, int * error)
{
	struct super_block *sb = dir->i_sb;
	struct minix_sb_info *sbi = minix_sb(sb);
	/* 从vfs中获取一个inode */
	struct inode *inode = new_inode(sb);
	struct buffer_head * bh;
	int bits_per_zone = 8 * sb->s_blocksize;
	unsigned long j;
	int i;

	if (!inode) {
		*error = -ENOMEM;
		return NULL;
	}
	j = bits_per_zone;
	bh = NULL;
	*error = -ENOSPC;
	lock_kernel();
	for (i = 0; i < sbi->s_imap_blocks; i++) {
		bh = sbi->s_imap[i];
		/* 每次搜索一个block */
		j = minix_find_first_zero_bit(bh->b_data, bits_per_zone);
		if (j < bits_per_zone)
			break;
	}
	if (!bh || j >= bits_per_zone) {
		unlock_kernel();
		iput(inode);
		return NULL;
	}
	/* 之前在bitmap中搜索空闲bit,现在要占用它 */
	if (minix_test_and_set_bit(j, bh->b_data)) {	/* shouldn't happen */
		unlock_kernel();
		printk("minix_new_inode: bit already set\n");
		iput(inode);
		return NULL;
	}
	unlock_kernel();
	/* 改变了位图,将这个buffer标记为dirty */
	mark_buffer_dirty(bh);
	/* 获取的bit在整个位图中的位置 */
	j += i * bits_per_zone;
	if (!j || j > sbi->s_ninodes) {
		iput(inode);
		return NULL;
	}
	inode->i_uid = current->fsuid;
	/* 如果父目录设置了sgid,那么创建的文件gid和父目录的gid相同 */
	inode->i_gid = (dir->i_mode & S_ISGID) ? dir->i_gid : current->fsgid;
	inode->i_ino = j;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME_SEC;
	inode->i_blocks = 0;
	/* minix_inode_info中没有可用数据块 */
	memset(&minix_i(inode)->u, 0, sizeof(minix_i(inode)->u));
	/* 将inode插入hash表 */
	insert_inode_hash(inode);
	mark_inode_dirty(inode);

	*error = 0;
	return inode;
}

unsigned long minix_count_free_inodes(struct minix_sb_info *sbi)
{
	/*
	 * sbi->s_ninodes + 1是因为inode 0虽然无效,但是却在imap中存在,
	 * 统计空闲inode时,需要查找sbi->s_ninodes个有效inode及一个无效inode
	 */
	return count_free(sbi->s_imap, sbi->s_imap_blocks, sbi->s_ninodes + 1);
}
