#include <linux/buffer_head.h>
#include "minix.h"

enum {DEPTH = 3, DIRECT = 7};	/* Only double indirect */

typedef u16 block_t;	/* 16 bit, host order */

static inline unsigned long block_to_cpu(block_t n)
{
	return n;
}

static inline block_t cpu_to_block(unsigned long n)
{
	return n;
}

/* 返回inode指向的数据块 */
static inline block_t *i_data(struct inode *inode)
{
	/* minix_i(inode)->u.i1_data本来是__u16 *类型的,每个成员是__u16类型 */
	return (block_t *)minix_i(inode)->u.i1_data;
}

/*
 * 记录block的横向保存路径
 */
static int block_to_path(struct inode * inode, long block, int offsets[DEPTH])
{
	int n = 0;
	char b[BDEVNAME_SIZE];

	if (block < 0) {
		printk("MINIX-fs: block_to_path: block %ld < 0 on dev %s\n",
			block, bdevname(inode->i_sb->s_bdev, b));
	} else if (block >= (minix_sb(inode->i_sb)->s_max_size/BLOCK_SIZE)) {
		if (printk_ratelimit())
			printk("MINIX-fs: block_to_path: "
			       "block %ld too big on dev %s\n",
				block, bdevname(inode->i_sb->s_bdev, b));
	} else if (block < 7) {
		/* block - 1才是文件最后一个有效索引 */
		offsets[n++] = block;
	} else if ((block -= 7) < 512) {
		/* 直接索引中,最后一个有效的index是7 */
		offsets[n++] = 7;
		/* 一级间接块中的第一个无效的index */
		offsets[n++] = block;
	} else {
		/* 上一个else if分支中已经将block减了7 */
		block -= 512;
		/* 直接索引中,最后一个有效的index是8 */
		offsets[n++] = 8;
		/* 一级间接块中的最后一个有效的index */
		offsets[n++] = block>>9;
		/* 二级间接块中的第一个无效的index */
		offsets[n++] = block & 511;
	}
	return n;
}

#include "itree_common.c"

int V1_minix_get_block(struct inode * inode, long block,
			struct buffer_head *bh_result, int create)
{
	return get_block(inode, block, bh_result, create);
}

void V1_minix_truncate(struct inode * inode)
{
	truncate(inode);
}

unsigned V1_minix_blocks(loff_t size, struct super_block *sb)
{
	return nblocks(size, sb);
}
