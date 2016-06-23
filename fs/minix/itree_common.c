/* Generic part */

typedef struct {
	/* buffer中存放key的地址 */
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

/* 找齐depth个chain成员,如果其中有索引为0,返回这个chain成员,方便对这个成员进行处理.否则返回NULL */
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
	/* 根节点->p = minix_inode_info->u.i1_data + offsets[0] */
	add_chain (chain, NULL, i_data(inode) + *offsets);
	/* 如果p->key == 0,表示这个branch中没有分配过任何的block */
	if (!p->key)
		goto no_block;
	/* i_data中已经处理掉一个depth */
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
	/*
	 * 当--depth == 0时,但是p->key存在有效的block no,这种情况会出现在truncate时.
	 * 文件被截断时,p->key指向文件之前使用到的block.
	 */
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

/* 分配了branch,同时也分配了该branch用到的数据块 */
static int alloc_branch(struct inode *inode,
			     int num,
			     int *offsets,
			     Indirect *branch)
{
	int n = 0;
	int i;
	int parent = minix_new_block(inode);

	/* 对于brach的根节点,没有像其他节点一样对其p赋值,是因为其p没有指向buffer */
	branch[0].key = cpu_to_block(parent);
	/* 创建符号连接的时候,num == 1,不会执行循环中的内容 */
	if (parent) for (n = 1; n < num; n++) {
		struct buffer_head *bh;
		/* Allocate the next block */
		int nr = minix_new_block(inode);
		if (!nr)
			break;
		branch[n].key = cpu_to_block(nr);
		bh = sb_getblk(inode->i_sb, parent);
		lock_buffer(bh);
		/* 先清空buffer中的数据,保证写入磁盘后,不会有一些杂乱数据干扰 */
		memset(bh->b_data, 0, bh->b_size);
		branch[n].bh = bh;
		branch[n].p = (block_t*) bh->b_data + offsets[n];
		*branch[n].p = branch[n].key;
		set_buffer_uptodate(bh);
		unlock_buffer(bh);
		/*
		 * 1.将buffer标记为dirty.
		 * 2.将这个buffer和inode关联起来.
		 */
		mark_buffer_dirty_inode(bh, inode);
		parent = nr;
	}
	if (n == num)
		return 0;

    /* 分配失败,需要清除之前分配的bh */
	/* Allocation failed, free what we already allocated */
	for (i = 1; i < n; i++)
		bforget(branch[i].bh);
	/* 释放之前通过minix_new_block获取到的block */
	for (i = 0; i < n; i++)
		minix_free_block(inode, block_to_cpu(branch[i].key));
	return -ENOSPC;
}

/* 在调用alloc_branch之后,branch的where中并没有成员指向分配的第一个block */
static inline int splice_branch(struct inode *inode,
				     Indirect chain[DEPTH],
				     Indirect *where,
				     int num)
{
	int i;

	write_lock(&pointers_lock);

	/* 当alloc_branch中只分配了父节点的时候*where->p == 0,但是where->key != 0 */
	/* Verify that place we are splicing to is still there and vacant */
	if (!verify_chain(chain, where-1) || *where->p)
		goto changed;

	/* 将block number写入上一级block */
	*where->p = where->key;

	write_unlock(&pointers_lock);

	/* We are done with atomic stuff, now do the rest of housekeeping */

	inode->i_ctime = CURRENT_TIME_SEC;

	/* had we spliced it onto indirect block? */
	if (where->bh)
		mark_buffer_dirty_inode(where->bh, inode);

	/* inode时间更新,需要将inode标记为dirty */
	mark_inode_dirty(inode);
	return 0;

changed:
	write_unlock(&pointers_lock);
	/* i从1开始,以为branch根节点没有依赖buffer */
	for (i = 1; i < num; i++)
		bforget(where[i].bh);
	/* 释放branch中包括根节点在内的所有block */
	for (i = 0; i < num; i++)
		minix_free_block(inode, block_to_cpu(where[i].key));
	return -EAGAIN;
}

/*
 * 参数中block是地址空间中的block number, map_bh后bh指向了磁盘上特定一个block(从0开始)
 * 并不是文件大小block的个数(从1开始).
 */
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
	/* partial指向branch中最后一个节点,通常是接下来要操作的节点 */
	partial = get_branch(inode, depth, offsets, chain, &err);

	/* Simplest case - block found, no allocation needed */
	/*
	 * partial == NULL表示文件结尾的p->key仍然指向有效的block,
	 * 不需要分配block,这种情况出现在文件截断或者读取文件内容时.
	 */
	if (!partial) {
got_it:
		/*
		 * 仅仅是设置buffer对应的b_blocknr,并没有将数据块内容导入buffer中.
		 * bh指向的是文件系统中的block,而不是地址空间的block
		 */
		map_bh(bh, inode->i_sb, block_to_cpu(chain[depth-1].key));
		/* Clean up and exit */
		/* 指向最后一个chain,为了释放chain上所有的bh */
		partial = chain+depth-1; /* the whole chain */
		goto cleanup;
	}

	/* Next simple case - plain lookup or failed read of indirect block */
	/*
	 * 当partial不为NULL, 也就是还未达到depth的节点的key==0, 并且在读取过程中.
	 * 或者是写入过程中get_branch获取bh出错.
	 */
	if (!create || err == -EIO) {
cleanup:
		/*
		 * 1.当读取block过程中,这些间接块的bh不会被改变,可以直接释放.
		 * 2.当在get_branch获取bh失败,也需要将所有间接块bh释放.
		 * 3.当不断给文件写入数据,间接块的bh会被改变,暂时不能释放.
		 *   当写入过成完成后,方能将间接块的bh释放.
		 */
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

	/* 有多少个branch节点 */
	left = (chain + depth) - partial;
	/* offsets和partial要一一对应 */
	err = alloc_branch(inode, left, offsets+(partial-chain), partial);
	if (err)
		goto cleanup;

	if (splice_branch(inode, chain, partial, left) < 0)
		goto changed;

	/* 标记buffer对应磁盘上分配的新的block */
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
	/* 当partial不等于NULL的时候,partial->p指向文件结尾第一个无效的block */
	partial = get_branch(inode, k, offsets, chain, &err);

	write_lock(&pointers_lock);
	if (!partial)
		partial = chain + k-1;
	/* 如果在get_branch过程中遇到key == 0,但是其partial指向的地址数据不为0 */
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
	/* p->p指向文件有效数据的最后一个block */
	if (p == chain + k - 1 && p > chain) {
		p->p--;
	} else {
		*top = *p->p;
		*p->p = 0;
	}
	write_unlock(&pointers_lock);

	/* 如果all_zeroes成立的话才会走到下面循环中 */
	while(partial > p)
	{
		/* partial大于p的bh不需要回写,只需要回写p->bh */
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
			/*
			 * free_branches也只是将间接块中的block索引清0,并没有真正写入磁盘,
			 * bforget清除buffer的dirty状态,再减少buffer的引用计数.抛弃这个buffer,
			 * 也就是不会清除间接块中的索引.
			 */
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
	 * 其他的buffer在调用truncate之前已经清除完成.
	 */
	block_truncate_page(inode->i_mapping, inode->i_size, get_block);

	/* 需要几级索引 */
	n = block_to_path(inode, iblock, offsets);
	if (!n)
		return;

	if (n == 1) {
		/* 将minix_inode_info中文件没有用到的直接块索引清除,并且zmap中清除块索引指向的block bit */
		free_data(inode, idata+offsets[0], idata + DIRECT);
		first_whole = 0;
		/* 间接块由do_indirects处理 */
		goto do_indirects;
	}

	/* 需要几级间接块 */
	first_whole = offsets[0] + 1 - DIRECT;
	/* partial->p指向了文件最后一个有效的block */
	partial = find_shared(inode, n, offsets, chain, &nr);
	if (nr) {
		if (partial == chain)
			mark_inode_dirty(inode);
		else
			/* 如果文件使用了间接块 */
			mark_buffer_dirty_inode(partial->bh, inode);
		free_branches(inode, &nr, &nr+1, (chain+n-1) - partial);
	}
	/* Clear the ends of indirect blocks on the shared branch */
	while (partial > chain) {
		/*
		 * partial->p + 1真是神来之笔,和find_shared中p->p--前后呼应.整条chain中,(chain+n-1)->key
		 * 指向的是需要释放的数据,但是之前的节点->key均指向文件需要保留的数据.在find_shared中将
		 * p--,这样(chain+n-1)->key指向的是文件的保留数据.这样在整条chain中,partial->p + 1就指向
		 * 每个节点需要释放的起始地址.
		 */
		free_branches(inode, partial->p + 1, block_end(partial->bh),
				(chain+n-1) - partial);
		/* 只有直接调用free_branches的partial->bh才有回写的必要 */
		mark_buffer_dirty_inode(partial->bh, inode);
		brelse (partial->bh);
		partial--;
	}
do_indirects:
	/* Kill the remaining (whole) subtrees */
	/*
	 * 如果文件只用到了直接块,那么释放一级间接块.
	 * 如果文件只用到了直接块和一级间接块,那么释放二级间接块.
	 */
	while (first_whole < DEPTH-1) {
		nr = idata[DIRECT+first_whole];
		if (nr) {
			/* 如果idata指向了有效数据块号,释放整条branch */
			idata[DIRECT+first_whole] = 0;
			mark_inode_dirty(inode);
			/* 注意:这里的&nr是临时变量nr的地址,和idata没关系 */
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
