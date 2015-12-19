/* Generic part */

typedef struct {
	block_t	*p;
	block_t	key;
	struct buffer_head *bh;
} Indirect;

static DEFINE_RWLOCK(pointers_lock);

/* 通过横向路径和i_data生成横向路径节点 */
static inline void add_chain(Indirect *p, struct buffer_head *bh, block_t *v)
{
	p->key = *(p->p = v);
	p->bh = bh;
}

static inline int verify_chain(Indirect *from, Indirect *to)
{
	while (from <= to && from->key == *from->p)
		from++;
	return (from > to);
}

static inline block_t *block_end(struct buffer_head *bh)
{
	return (block_t *)((char*)bh->b_data + bh->b_size);
}

static inline Indirect *get_branch(struct inode *inode,
					int depth,
					int *offsets,
					Indirect chain[DEPTH],
					int *err)
{
	struct super_block *sb = inode->i_sb;
	Indirect *p = chain;
	struct buffer_head *bh;

	*err = 0;
	/* i_data is not going away, no lock needed */
	add_chain (chain, NULL, i_data(inode) + *offsets);
	/* p->key记录一级间接块的块号,如果p->key == 0,表示这个文件只有直接块 */
	if (!p->key)
		goto no_block;
	while (--depth) {
		/* 读取一级间接块 */
		bh = sb_bread(sb, block_to_cpu(p->key));
		if (!bh)
			goto failure;
		read_lock(&pointers_lock);
		/* 验证chain的可靠性 */
		if (!verify_chain(chain, p))
			goto changed;
		/* 获取二级间接块的节点 */
		add_chain(++p, bh, (block_t *)bh->b_data + *++offsets);
		read_unlock(&pointers_lock);
		/*
		 * 对于offsets中最后一个成员, (block_t *)bh->b_data + *offsets - 1
		 * 记录的是文件最后一个block,*((block_t *)bh->b_data + *offsets)应该为0
		 */
		if (!p->key)
			goto no_block;
	}
	return NULL;

changed:
	read_unlock(&pointers_lock);
	brelse(bh);
	*err = -EAGAIN;
	goto no_block;
failure:
	*err = -EIO;
no_block:
	/* p指向chain[]中最后一个成员,p->key应该等于0,p->bh指向最后一个间接块*/
	return p;
}

static int alloc_branch(struct inode *inode,
			     int num,
			     int *offsets,
			     Indirect *branch)
{
	int n = 0;
	int i;
	int parent = minix_new_block(inode);

	branch[0].key = cpu_to_block(parent);
	if (parent) for (n = 1; n < num; n++) {
		struct buffer_head *bh;
		/* Allocate the next block */
		int nr = minix_new_block(inode);
		if (!nr)
			break;
		branch[n].key = cpu_to_block(nr);
		bh = sb_getblk(inode->i_sb, parent);
		lock_buffer(bh);
		memset(bh->b_data, 0, bh->b_size);
		branch[n].bh = bh;
		branch[n].p = (block_t*) bh->b_data + offsets[n];
		*branch[n].p = branch[n].key;
		set_buffer_uptodate(bh);
		unlock_buffer(bh);
		mark_buffer_dirty_inode(bh, inode);
		parent = nr;
	}
	if (n == num)
		return 0;

	/* Allocation failed, free what we already allocated */
	for (i = 1; i < n; i++)
		bforget(branch[i].bh);
	for (i = 0; i < n; i++)
		minix_free_block(inode, block_to_cpu(branch[i].key));
	return -ENOSPC;
}

static inline int splice_branch(struct inode *inode,
				     Indirect chain[DEPTH],
				     Indirect *where,
				     int num)
{
	int i;

	write_lock(&pointers_lock);

	/* Verify that place we are splicing to is still there and vacant */
	if (!verify_chain(chain, where-1) || *where->p)
		goto changed;

	*where->p = where->key;

	write_unlock(&pointers_lock);

	/* We are done with atomic stuff, now do the rest of housekeeping */

	inode->i_ctime = CURRENT_TIME_SEC;

	/* had we spliced it onto indirect block? */
	if (where->bh)
		mark_buffer_dirty_inode(where->bh, inode);

	mark_inode_dirty(inode);
	return 0;

changed:
	write_unlock(&pointers_lock);
	for (i = 1; i < num; i++)
		bforget(where[i].bh);
	for (i = 0; i < num; i++)
		minix_free_block(inode, block_to_cpu(where[i].key));
	return -EAGAIN;
}

static inline int get_block(struct inode * inode, sector_t block,
			struct buffer_head *bh, int create)
{
	int err = -EIO;
	int offsets[DEPTH];
	Indirect chain[DEPTH];
	Indirect *partial;
	int left;
	int depth = block_to_path(inode, block, offsets);

	if (depth == 0)
		goto out;

reread:
	partial = get_branch(inode, depth, offsets, chain, &err);

	/* Simplest case - block found, no allocation needed */
	if (!partial) {
got_it:
		map_bh(bh, inode->i_sb, block_to_cpu(chain[depth-1].key));
		/* Clean up and exit */
		partial = chain+depth-1; /* the whole chain */
		goto cleanup;
	}

	/* Next simple case - plain lookup or failed read of indirect block */
	if (!create || err == -EIO) {
cleanup:
		while (partial > chain) {
			brelse(partial->bh);
			partial--;
		}
out:
		return err;
	}

	/*
	 * Indirect block might be removed by truncate while we were
	 * reading it. Handling of that case (forget what we've got and
	 * reread) is taken out of the main path.
	 */
	if (err == -EAGAIN)
		goto changed;

	left = (chain + depth) - partial;
	err = alloc_branch(inode, left, offsets+(partial-chain), partial);
	if (err)
		goto cleanup;

	if (splice_branch(inode, chain, partial, left) < 0)
		goto changed;

	set_buffer_new(bh);
	goto got_it;

changed:
	while (partial > chain) {
		brelse(partial->bh);
		partial--;
	}
	goto reread;
}

static inline int all_zeroes(block_t *p, block_t *q)
{
	while (p < q)
		if (*p++)
			return 0;
	return 1;
}

static Indirect *find_shared(struct inode *inode,
				int depth,
				int offsets[DEPTH],
				Indirect chain[DEPTH],
				block_t *top)
{
	Indirect *partial, *p;
	int k, err;

	*top = 0;
	/*
	 * 从offsets的尾部开始搜索,如果offsets最后一个元素为0,
	 * 表示文件并没有达到depth这种深度,k需要递减,直到k记录实际的深度
	 */
	for (k = depth; k > 1 && !offsets[k-1]; k--)
		;
	partial = get_branch(inode, k, offsets, chain, &err);

	write_lock(&pointers_lock);
	if (!partial)
		partial = chain + k-1;
	if (!partial->key && *partial->p) {
		write_unlock(&pointers_lock);
		goto no_top;
	}
	/*
	 * 遍历chain,如果buffer中[0,*p->p]为0,那么这个chain节点无效.
	 * 从chain的最后向前搜索,找到第一个有效chain节点为止.
	 */
	for (p=partial;p>chain && all_zeroes((block_t*)p->bh->b_data,p->p);p--)
		;
	if (p == chain + k - 1 && p > chain) {
		/* p->p指向文件有效数据的最后一个block */
		p->p--;
	} else {
		*top = *p->p;
		*p->p = 0;
	}
	write_unlock(&pointers_lock);

	/* 目前貌似不会走到下面这个循环 */
	while(partial > p)
	{
		brelse(partial->bh);
		partial--;
	}
no_top:
	return partial;
}

/*
 * free_data:将数据块指针清零,并且在zmap中清除对应的block
 * p:起始block number
 * q:结束block number
 */
static inline void free_data(struct inode *inode, block_t *p, block_t *q)
{
	unsigned long nr;

	for ( ; p < q ; p++) {
		nr = block_to_cpu(*p);
		if (nr) {
			/* 将指向数据块的指针清零 */
			*p = 0;
			/* 清除block对应bitmap中的bit */
			minix_free_block(inode, nr);
		}
	}
}

static void free_branches(struct inode *inode, block_t *p, block_t *q, int depth)
{
	struct buffer_head * bh;
	unsigned long nr;

	if (depth--) {
		/* 释放二级间接索引块中的索引 */
		for ( ; p < q ; p++) {
			nr = block_to_cpu(*p);
			if (!nr)
				continue;
			*p = 0;
			bh = sb_bread(inode->i_sb, nr);
			if (!bh)
				continue;
			/* 释放整块间接块中的所有索引 */
			free_branches(inode, (block_t*)bh->b_data,
				      block_end(bh), depth);
			bforget(bh);
			/* 在zmap中清除一级间接索引块的bit */
			minix_free_block(inode, nr);
			mark_inode_dirty(inode);
		}
	} else
		free_data(inode, p, q);
}

static inline void truncate (struct inode * inode)
{
	struct super_block *sb = inode->i_sb;
	/* 从inode中获取minix_inode_info的块索引数组 */
	block_t *idata = i_data(inode);
	/* offsets每个成员记录的是当前文件在当前节点中第一个无效的entry的位置(从0开始) */
	int offsets[DEPTH];
	Indirect chain[DEPTH];
	Indirect *partial;
	block_t nr = 0;
	int n;
	int first_whole;
	long iblock;

	/* 将size转换成block(以block size向上对齐) */
	iblock = (inode->i_size + sb->s_blocksize -1) >> sb->s_blocksize_bits;
	/*
	 * 清除文件最后一个buffer中, i_size%buffer_size 到buffer结尾的字节,
	 * 其他的buffer在调用truncate之前已经清除完成
	 */
	block_truncate_page(inode->i_mapping, inode->i_size, get_block);

	/* 需要几级索引 */
	n = block_to_path(inode, iblock, offsets);
	if (!n)
		return;

	if (n == 1) {
		/* 将minix_inode_info中直接块索引清除,并且zmap中清除块索引指向的block bit */
		free_data(inode, idata+offsets[0], idata + DIRECT);
		first_whole = 0;
		goto do_indirects;
	}

	/* 需要几级间接块 */
	first_whole = offsets[0] + 1 - DIRECT;
	partial = find_shared(inode, n, offsets, chain, &nr);
	if (nr) {
		if (partial == chain)
			mark_inode_dirty(inode);
		else
			mark_buffer_dirty_inode(partial->bh, inode);
		free_branches(inode, &nr, &nr+1, (chain+n-1) - partial);
	}
	/* Clear the ends of indirect blocks on the shared branch */
	/*
	 * 对于有2级间接块的chain,二级间接块只需要释放掉block中不属于当前inode的block索引.
	 * 其一级间接块,需要释放掉一级间接块中不属于当前inode的block索引,以及这些一级索引
	 * 指向block中的二级间接索引.
	 */
	while (partial > chain) {
		/* partial->p + 1指向文件末尾无效数据第一个block */
		free_branches(inode, partial->p + 1, block_end(partial->bh),
				(chain+n-1) - partial);
		mark_buffer_dirty_inode(partial->bh, inode);
		brelse (partial->bh);
		/* 指向上一级索引 */
		partial--;
	}
do_indirects:
	/* Kill the remaining (whole) subtrees */
	while (first_whole < DEPTH-1) {
		nr = idata[DIRECT+first_whole];
		if (nr) {
			idata[DIRECT+first_whole] = 0;
			mark_inode_dirty(inode);
			free_branches(inode, &nr, &nr+1, first_whole+1);
		}
		first_whole++;
	}
	inode->i_mtime = inode->i_ctime = CURRENT_TIME_SEC;
	mark_inode_dirty(inode);
}

/* size大小的文件(包含间接索引块)占用block的数量 */
static inline unsigned nblocks(loff_t size, struct super_block *sb)
{
	int k = sb->s_blocksize_bits - 10;
	unsigned blocks, res, direct = DIRECT, i = DEPTH;
	/* 文件占用的block个数 */
	blocks = (size + sb->s_blocksize - 1) >> (BLOCK_SIZE_BITS + k);
	res = blocks;
	while (--i && blocks > direct) {
		blocks -= direct;
		blocks += sb->s_blocksize/sizeof(block_t) - 1;
		blocks /= sb->s_blocksize/sizeof(block_t);
		res += blocks;
		direct = 1;
	}
	return res;
}
