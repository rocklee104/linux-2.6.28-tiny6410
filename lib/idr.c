/*
 * 2002-10-18  written by Jim Houston jim.houston@ccur.com
 *	Copyright (C) 2002 by Concurrent Computer Corporation
 *	Distributed under the GNU GPL license version 2.
 *
 * Modified by George Anzinger to reuse immediately and to use
 * find bit instructions.  Also removed _irq on spinlocks.
 *
 * Modified by Nadia Derbey to make it RCU safe.
 *
 * Small id to pointer translation service.
 *
 * It uses a radix tree like structure as a sparse array indexed
 * by the id to obtain the pointer.  The bitmap makes allocating
 * a new id quick.
 *
 * You call it to allocate an id (an int) an associate with that id a
 * pointer or what ever, we treat it as a (void *).  You can pass this
 * id to a user for him to pass back at a later time.  You then pass
 * that id to this code and it returns your pointer.

 * You can release ids at any time. When all ids are released, most of
 * the memory is returned (we keep IDR_FREE_MAX) in a local pool so we
 * don't need to go to the memory "store" during an id allocate, just
 * so you don't need to be too concerned about locking and conflicts
 * with the slab allocator.
 */

#ifndef TEST                        // to test in user space...
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/module.h>
#endif
#include <linux/err.h>
#include <linux/string.h>
#include <linux/idr.h>

static struct kmem_cache *idr_layer_cache;

//从预备链表中取出一个idr_layer
static struct idr_layer *get_from_free_list(struct idr *idp)
{
	struct idr_layer *p;
	unsigned long flags;

	spin_lock_irqsave(&idp->lock, flags);
	if ((p = idp->id_free)) {
		//如果idr的预备链表不是空的,就将链表的头取出来
		idp->id_free = p->ary[0];
		idp->id_free_cnt--;
		p->ary[0] = NULL;
	}
	spin_unlock_irqrestore(&idp->lock, flags);
	return(p);
}

static void idr_layer_rcu_free(struct rcu_head *head)
{
	struct idr_layer *layer;

	layer = container_of(head, struct idr_layer, rcu_head);
	kmem_cache_free(idr_layer_cache, layer);
}

static inline void free_layer(struct idr_layer *p)
{
	call_rcu(&p->rcu_head, idr_layer_rcu_free);
}

/* only called when idp->lock is held */
static void __move_to_free_list(struct idr *idp, struct idr_layer *p)
{
	//将p头插入idp->id_free链表中
	p->ary[0] = idp->id_free;
	idp->id_free = p;
	idp->id_free_cnt++;
}

static void move_to_free_list(struct idr *idp, struct idr_layer *p)
{
	unsigned long flags;

	/*
	 * Depends on the return element being zeroed.
	 */
	spin_lock_irqsave(&idp->lock, flags);
	__move_to_free_list(idp, p);
	spin_unlock_irqrestore(&idp->lock, flags);
}

static void idr_mark_full(struct idr_layer **pa, int id)
{
	struct idr_layer *p = pa[0];
	int l = 0;

	__set_bit(id & IDR_MASK, &p->bitmap);
	/*
	 * If this layer is full mark the bit in the layer above to
	 * show that this part of the radix tree is full.  This may
	 * complete the layer above and require walking up the radix
	 * tree.
	 */
	while (p->bitmap == IDR_FULL) {
		//如果当前节点的bitmap已满,就需要将上层对应这个节点的bitmap置位
		if (!(p = pa[++l]))
			//如果遇到pa[l] == NULL,说明已经将id的所有节点遍历完成
			break;
		id = id >> IDR_BITS;
		__set_bit((id & IDR_MASK), &p->bitmap);
	}
}

/**
 * idr_pre_get - reserver resources for idr allocation
 * @idp:	idr handle
 * @gfp_mask:	memory allocation flags
 *
 * This function should be called prior to locking and calling the
 * idr_get_new* functions. It preallocates enough memory to satisfy
 * the worst possible allocation.
 *
 * If the system is REALLY out of memory this function returns 0,
 * otherwise 1.
 */
/* 
 * 要获得一个ID号要分两个步骤,首先分配内存,其次是获得ID号.
 * 本函数会导致睡眠,不能用锁.
 * 
 * 分配成功返回1,失败返回0.
 */
//一次性分配足够的预备idr_layer
int idr_pre_get(struct idr *idp, gfp_t gfp_mask)
{
	while (idp->id_free_cnt < IDR_FREE_MAX) {
		struct idr_layer *new;
		new = kmem_cache_zalloc(idr_layer_cache, gfp_mask);
		if (new == NULL)
			return (0);
		move_to_free_list(idp, new);
	}
	return 1;
}
EXPORT_SYMBOL(idr_pre_get);

/* 
 * 获取空闲的id,并创建id对应的节点.并将这些对应于id的节点保存在pa数组中,
 * pa这个指针数组中下标小的成员保存key值低位表示的节点.
 *
 * sub_alloc -- 根据id值在idr tree中分配对应于id的子节点
 */
static int sub_alloc(struct idr *idp, int *starting_id, struct idr_layer **pa)
{
	int n, m, sh;
	struct idr_layer *p, *new;
	//id的值根据IDR_BITS分成layers等分,每一份代表那一层的bitmap值
	int l, id, oid;
	unsigned long bm;

	id = *starting_id;
 restart:
 	//找到idr的根top指向的idr_layer
	p = idp->top;
	//idr中的layers层数量
	l = idp->layers;
	//为了在while中计算,l使用后需要自减1
	pa[l--] = NULL;
	while (1) {
		/*
		 * We run around this while until we reach the leaf node...
		 */
		n = (id >> (IDR_BITS*l)) & IDR_MASK;
		bm = ~p->bitmap;
		//从位图中的第n位开始,查找第一个不为0的位,表示该位可用,为1的位表示已经被使用
		m = find_next_bit(&bm, IDR_SIZE, n);
		if (m == IDR_SIZE) {
			//位图已满
			/* no space available go back to previous layer. */
			//本层已满,需要回退到上一层
			l++;
			oid = id;
			/* 
			 * 重新计算id,当前layer中bitmap已满,说明这个layer的所有子节点的bitmap也全部满了.
			 * 将id对应这一层及这层在叶子之间的所有层的bitmap全部置位,并且在此基础上加1.
			 * 下次循环的时候将从这个新的id开始计算.会对上一层产生影响.
			 */
			id = (id | ((1 << (IDR_BITS * l)) - 1)) + 1;

			/* if already at the top layer, we need to grow */
			if (!(p = pa[l])) {
				//pa[idp->layers]总是为NULL
				*starting_id = id;
				//最顶层的bitmap都满了,就需要增加树的高度
				return IDR_NEED_TO_GROW;
			}

			/* If we need to go up one layer, continue the
			 * loop; otherwise, restart from the top.
			 */
			sh = IDR_BITS * (l + 1);
			if (oid >> sh == id >> sh)
				/* 
				 * id = (id | ((1 << (IDR_BITS * l)) - 1)) + 1;这个操作只对本层和上层有影响,
				 * 对更上层没有影响(进位),那么只需要重新对上层计算id.否则需要从top开始重新计算id
				 */
				continue;
			else
				goto restart;
		}
		// 期望的n值被占用,但可找到可用的m值,重新计算id值.
		if (m != n) {
			sh = IDR_BITS*l;
			id = ((id >> sh) ^ n ^ m) << sh;
		}
		if ((id >= MAX_ID_BIT) || (id < 0))
			//id超过所能分配的最大值(1 << 31)或者小于0,则出错返回
			return IDR_NOMORE_SPACE;
		if (l == 0)
			//一层层循环计算直到到达叶子节点处l才为0,然后才跳出循环
			break;
		/*
		 * Create the layer below if it is missing.
		 */
		//通过bitmap查得m位空闲,分配需要的节点
		if (!p->ary[m]) {
			new = get_from_free_list(idp);
			if (!new)
				return -1;
			//new将要是p的子节点,故layer比p的layer小1
			new->layer = l-1;
			//p->ary[m]指向新分配的节点
			rcu_assign_pointer(p->ary[m], new);
			//bitmap中有效位加1
			p->count++;
		}
		pa[l--] = p;
		//向子节点遍历
		p = p->ary[m];
	}

	/*
	 * 对于一个layers == 3的树,
	 * p[3]保存NULL,
	 * p[2]保存layer1对应(id >> (IDR_BITS*2)) & IDR_MASK的节点.
	 * p[1]保存layer2对应(id >> (IDR_BITS)) & IDR_MASK的节点.
	 * p[0]保存layer3对应id  & IDRe_MASK的节点.
	 */
	pa[l] = p;
	return id;
}

/* 
 * 在idr tree中找到空闲的id,并且分配对应id的所有节点,并将这些节点保存在pa数组中.
 * 最后返回这个id值
 */
static int idr_get_empty_slot(struct idr *idp, int starting_id,
			      struct idr_layer **pa)
{
	struct idr_layer *p, *new;
	//layer记录tree的idr_layer层数
	int layers, v, id;
	unsigned long flags;

	id = starting_id;
build_up:
	//第一次申请id号时,根top指向的idr_layer为NULL
	p = idp->top;
	//第一次申请id号时,layers层数量idp->layers为0
	layers = idp->layers;
	if (unlikely(!p)) {
		//如果idr的top是NULL,那么需要从预备链表中选取一个
		if (!(p = get_from_free_list(idp)))
			return -1;
		//p是叶子节点,layer为0
		p->layer = 0;
		layers = 1;
	}
	/*
	 * Add a new layer to the top of the tree if the requested
	 * id is larger than the currently allocated space.
	 */
	//如果起始的id号超过该idr中设定的idr_layer层数所能设置的id号最大值,则需要扩展idr树(增加高度)
	while ((layers < (MAX_LEVEL - 1)) && (id >= (1 << (layers*IDR_BITS)))) {
		layers++;
		if (!p->count) {
			/* special case: if the tree is currently empty,
			 * then we grow the tree by moving the top node
			 * upwards.
			 */
			p->layer++;
			continue;
		}
		if (!(new = get_from_free_list(idp))) {
			//如果从预分配链表中取出idr_layer失败
			/*
			 * The allocation failed.  If we built part of
			 * the structure tear it down.
			 */
			spin_lock_irqsave(&idp->lock, flags);
			for (new = p; p && p != idp->top; new = p) {
				p = p->ary[0];
				new->ary[0] = NULL;
				new->bitmap = new->count = 0;
				//将之前分配成功的idr_layer放回free list
				__move_to_free_list(idp, new);
			}
			spin_unlock_irqrestore(&idp->lock, flags);
			return -1;
		}
		/* 
		 * 增长tree的高度时,首先只增长每一层的ary[0],这时因为在idr_get_empty_slot中,
		 * 树的增长是分配一个新的idr_layer,然后将这个idr_layer作为root,也就是向上增长。
		 * 这样是没有办法确定id最终的值,所以不能根据id的值来增加相应的节点。
		 * 
		 * 而当p->count == 0的时候,整个树都是空的,id的值可以确定,继而调用sub_alloc就可以
		 * 根据id的值增长树的高度(向下增长)
		 */ 
		new->ary[0] = p;
		new->count = 1;
		new->layer = layers-1;
		if (p->bitmap == IDR_FULL)
			//p的bitmap满后,将new中对应于p的bitmap中的位置位
			__set_bit(0, &new->bitmap);
		//设置p指向新加入的idr_layer节点
		p = new;
	}
	//idp的top使用新的idr_layer
	rcu_assign_pointer(idp->top, p);
	idp->layers = layers;
	/* 
	 * 高度增长完成,需要调整id,并且分配id对应的节点.
	 * 从idr的top指针指向的idr_layer树中获得id号,分配路径记录在pa数组中
	 */
	v = sub_alloc(idp, &id, pa);
	if (v == IDR_NEED_TO_GROW)
		//在sub_alloc时可能遇到顶层bitmap没有空闲位的情况,这时候需要再次增加树的高度
		goto build_up;
	return(v);
}

//将ptr插到idr tree的叶子节点下
static int idr_get_new_above_int(struct idr *idp, void *ptr, int starting_id)
{
	struct idr_layer *pa[MAX_LEVEL];
	int id;

	//在该idr的idr_layer树中分配一个合适的id，并且分配的idr_layer路径记录在pa数组中
	id = idr_get_empty_slot(idp, starting_id, pa);
	if (id >= 0) {
		/*
		 * Successfully found an empty slot.  Install the user
		 * pointer and mark the slot full.
		 */
		//p[0]是id最底层的一个节点,在其bitmap对应id的slot中保存数据
		rcu_assign_pointer(pa[0]->ary[id & IDR_MASK],
				(struct idr_layer *)ptr);
		//增加有效数据slot的计数
		pa[0]->count++;
		idr_mark_full(pa, id);
	}

	return id;
}

/**
 * idr_get_new_above - allocate new idr entry above or equal to a start id
 * @idp: idr handle
 * @ptr: pointer you want associated with the ide
 * @start_id: id to start search at
 * @id: pointer to the allocated handle
 *
 * This is the allocate id function.  It should be called with any
 * required locks.
 *
 * If memory is required, it will return -EAGAIN, you should unlock
 * and go back to the idr_pre_get() call.  If the idr is full, it will
 * return -ENOSPC.
 *
 * @id returns a value in the range 0 ... 0x7fffffff
 */
int idr_get_new_above(struct idr *idp, void *ptr, int starting_id, int *id)
{
	int rv;

	rv = idr_get_new_above_int(idp, ptr, starting_id);
	/*
	 * This is a cheap hack until the IDR code can be fixed to
	 * return proper error values.
	 */
	if (rv < 0)
		return _idr_rc_to_errno(rv);
	*id = rv;
	return 0;
}
EXPORT_SYMBOL(idr_get_new_above);

/**
 * idr_get_new - allocate new idr entry
 * @idp: idr handle
 * @ptr: pointer you want associated with the ide
 * @id: pointer to the allocated handle
 *
 * This is the allocate id function.  It should be called with any
 * required locks.
 *
 * If memory is required, it will return -EAGAIN, you should unlock
 * and go back to the idr_pre_get() call.  If the idr is full, it will
 * return -ENOSPC.
 *
 * @id returns a value in the range 0 ... 0x7fffffff
 */
int idr_get_new(struct idr *idp, void *ptr, int *id)
{
	int rv;

	rv = idr_get_new_above_int(idp, ptr, 0);
	/*
	 * This is a cheap hack until the IDR code can be fixed to
	 * return proper error values.
	 */
	if (rv < 0)
		return _idr_rc_to_errno(rv);
	*id = rv;
	return 0;
}
EXPORT_SYMBOL(idr_get_new);

static void idr_remove_warning(int id)
{
	printk(KERN_WARNING
		"idr_remove called for id=%d which is not allocated.\n", id);
	dump_stack();
}

//减少id对应节点的计数,当计数等于0时,才能移除对应的节点
static void sub_remove(struct idr *idp, int shift, int id)
{
	struct idr_layer *p = idp->top;
	//指针数组,数组中成员是2级指针
	struct idr_layer **pa[MAX_LEVEL];
	//pa数组中第一个成员的地址
	struct idr_layer ***paa = &pa[0];
	struct idr_layer *to_free;
	int n;

	//将pa[0]设置成NULL
	*paa = NULL;
	//将pa[1]赋值为&idp->top
	*++paa = &idp->top;

	while ((shift > 0) && p) {
		/* 
		 * 清除对应于id的bitmap,并且将各层节点记录到了pa[]中,
		 * pa[0] == NULL,pa[1]记录top节点,pa[2]记录layer1节点,pa[3]记录layer2节点...
		 */
		n = (id >> shift) & IDR_MASK;
		__clear_bit(n, &p->bitmap);
		*++paa = &p->ary[n];
		p = p->ary[n];
		shift -= IDR_BITS;
	}
	n = id & IDR_MASK;
	if (likely(p != NULL && test_bit(n, &p->bitmap))){
		//一般情况下,这时候p指向了叶子节点
		__clear_bit(n, &p->bitmap);
		rcu_assign_pointer(p->ary[n], NULL);
		to_free = NULL;
		while(*paa && ! --((**paa)->count)){
			//当某一节点的count - 1后等于0,该节点才能释放
			if (to_free)
				free_layer(to_free);
			to_free = **paa;
			**paa-- = NULL;
		}
		if (!*paa)
			//当*paa == NULL时,在idr中id使用的节点全部释放完毕
			idp->layers = 0;
		if (to_free)
			free_layer(to_free);
	} else
		idr_remove_warning(id);
}

/**
 * idr_remove - remove the given id and free it's slot
 * @idp: idr handle
 * @id: unique key
 */
void idr_remove(struct idr *idp, int id)
{
	struct idr_layer *p;
	struct idr_layer *to_free;

	/* Mask off upper bits we don't use for the search. */
	id &= MAX_ID_MASK;

	//减少id对应节点的计数,当计数等于0时才释放对应的slot
	sub_remove(idp, (idp->layers - 1) * IDR_BITS, id);
	if (idp->top && idp->top->count == 1 && (idp->layers > 1) &&
	    idp->top->ary[0]) {
	    //当top中只有最左边的节点,我们需要释放这个top,将top重新指向top的子节点
		/*
		 * Single child at leftmost slot: we can shrink the tree.
		 * This level is not needed anymore since when layers are
		 * inserted, they are inserted at the top of the existing
		 * tree.
		 */
		to_free = idp->top;
		p = idp->top->ary[0];
		rcu_assign_pointer(idp->top, p);
		--idp->layers;
		to_free->bitmap = to_free->count = 0;
		free_layer(to_free);
	}
	while (idp->id_free_cnt >= IDR_FREE_MAX) {
		//如果预备链表中的成员超过数量,就需要释放一部分成员
		p = get_from_free_list(idp);
		/*
		 * Note: we don't call the rcu callback here, since the only
		 * layers that fall into the freelist are those that have been
		 * preallocated.
		 */
		kmem_cache_free(idr_layer_cache, p);
	}
	return;
}
EXPORT_SYMBOL(idr_remove);

/**
 * idr_remove_all - remove all ids from the given idr tree
 * @idp: idr handle
 *
 * idr_destroy() only frees up unused, cached idp_layers, but this
 * function will remove all id mappings and leave all idp_layers
 * unused.
 *
 * A typical clean-up sequence for objects stored in an idr tree, will
 * use idr_for_each() to free all objects, if necessay, then
 * idr_remove_all() to remove all ids, and idr_destroy() to free
 * up the cached idr_layers.
 */
void idr_remove_all(struct idr *idp)
{
	int n, id, max;
	struct idr_layer *p;
	struct idr_layer *pa[MAX_LEVEL];
	struct idr_layer **paa = &pa[0];

	n = idp->layers * IDR_BITS;
	p = idp->top;
	max = 1 << n;

	id = 0;
	while (id < max) {
		while (n > IDR_BITS && p) {
			n -= IDR_BITS;
			*paa++ = p;
			p = p->ary[(id >> n) & IDR_MASK];
		}

		id += 1 << n;
		while (n < fls(id)) {
			if (p)
				free_layer(p);
			n += IDR_BITS;
			p = *--paa;
		}
	}
	rcu_assign_pointer(idp->top, NULL);
	idp->layers = 0;
}
EXPORT_SYMBOL(idr_remove_all);

/**
 * idr_destroy - release all cached layers within an idr tree
 * idp: idr handle
 */
void idr_destroy(struct idr *idp)
{
	while (idp->id_free_cnt) {
		struct idr_layer *p = get_from_free_list(idp);
		kmem_cache_free(idr_layer_cache, p);
	}
}
EXPORT_SYMBOL(idr_destroy);

/**
 * idr_find - return pointer for given id
 * @idp: idr handle
 * @id: lookup key
 *
 * Return the pointer given the id it has been registered with.  A %NULL
 * return indicates that @id is not valid or you passed %NULL in
 * idr_get_new().
 *
 * This function can be called under rcu_read_lock(), given that the leaf
 * pointers lifetimes are correctly managed.
 */
void *idr_find(struct idr *idp, int id)
{
	int n;
	struct idr_layer *p;

	p = rcu_dereference(idp->top);
	if (!p)
		return NULL;
	n = (p->layer+1) * IDR_BITS;

	/* Mask off upper bits we don't use for the search. */
	id &= MAX_ID_MASK;

	if (id >= (1 << n))
		return NULL;
	BUG_ON(n == 0);

	while (n > 0 && p) {
		n -= IDR_BITS;
		BUG_ON(n != p->layer*IDR_BITS);
		p = rcu_dereference(p->ary[(id >> n) & IDR_MASK]);
	}
	return((void *)p);
}
EXPORT_SYMBOL(idr_find);

/**
 * idr_for_each - iterate through all stored pointers
 * @idp: idr handle
 * @fn: function to be called for each pointer
 * @data: data passed back to callback function
 *
 * Iterate over the pointers registered with the given idr.  The
 * callback function will be called for each pointer currently
 * registered, passing the id, the pointer and the data pointer passed
 * to this function.  It is not safe to modify the idr tree while in
 * the callback, so functions such as idr_get_new and idr_remove are
 * not allowed.
 *
 * We check the return of @fn each time. If it returns anything other
 * than 0, we break out and return that value.
 *
 * The caller must serialize idr_for_each() vs idr_get_new() and idr_remove().
 */
int idr_for_each(struct idr *idp,
		 int (*fn)(int id, void *p, void *data), void *data)
{
	int n, id, max, error = 0;
	struct idr_layer *p;
	struct idr_layer *pa[MAX_LEVEL];
	struct idr_layer **paa = &pa[0];

	n = idp->layers * IDR_BITS;
	p = rcu_dereference(idp->top);
	max = 1 << n;

	id = 0;
	while (id < max) {
		while (n > 0 && p) {
			n -= IDR_BITS;
			*paa++ = p;
			p = rcu_dereference(p->ary[(id >> n) & IDR_MASK]);
		}

		if (p) {
			error = fn(id, (void *)p, data);
			if (error)
				break;
		}

		id += 1 << n;
		while (n < fls(id)) {
			n += IDR_BITS;
			p = *--paa;
		}
	}

	return error;
}
EXPORT_SYMBOL(idr_for_each);

/**
 * idr_replace - replace pointer for given id
 * @idp: idr handle
 * @ptr: pointer you want associated with the id
 * @id: lookup key
 *
 * Replace the pointer registered with an id and return the old value.
 * A -ENOENT return indicates that @id was not found.
 * A -EINVAL return indicates that @id was not within valid constraints.
 *
 * The caller must serialize with writers.
 */
void *idr_replace(struct idr *idp, void *ptr, int id)
{
	int n;
	struct idr_layer *p, *old_p;

	p = idp->top;
	if (!p)
		return ERR_PTR(-EINVAL);

	n = (p->layer+1) * IDR_BITS;

	id &= MAX_ID_MASK;

	if (id >= (1 << n))
		return ERR_PTR(-EINVAL);

	n -= IDR_BITS;
	while ((n > 0) && p) {
		p = p->ary[(id >> n) & IDR_MASK];
		n -= IDR_BITS;
	}

	n = id & IDR_MASK;
	if (unlikely(p == NULL || !test_bit(n, &p->bitmap)))
		return ERR_PTR(-ENOENT);

	old_p = p->ary[n];
	rcu_assign_pointer(p->ary[n], ptr);

	return old_p;
}
EXPORT_SYMBOL(idr_replace);

void __init idr_init_cache(void)
{
	idr_layer_cache = kmem_cache_create("idr_layer_cache",
				sizeof(struct idr_layer), 0, SLAB_PANIC, NULL);
}

/**
 * idr_init - initialize idr handle
 * @idp:	idr handle
 *
 * This function is use to set up the handle (@idp) that you will pass
 * to the rest of the functions.
 */
void idr_init(struct idr *idp)
{
	memset(idp, 0, sizeof(struct idr));
	spin_lock_init(&idp->lock);
}
EXPORT_SYMBOL(idr_init);


/*
 * IDA - IDR based ID allocator
 *
 * this is id allocator without id -> pointer translation.  Memory
 * usage is much lower than full blown idr because each id only
 * occupies a bit.  ida uses a custom leaf node which contains
 * IDA_BITMAP_BITS slots.
 *
 * 2007-04-25  written by Tejun Heo <htejun@gmail.com>
 */

static void free_bitmap(struct ida *ida, struct ida_bitmap *bitmap)
{
	unsigned long flags;

	if (!ida->free_bitmap) {
		spin_lock_irqsave(&ida->idr.lock, flags);
		if (!ida->free_bitmap) {
			ida->free_bitmap = bitmap;
			bitmap = NULL;
		}
		spin_unlock_irqrestore(&ida->idr.lock, flags);
	}

	kfree(bitmap);
}

/**
 * ida_pre_get - reserve resources for ida allocation
 * @ida:	ida handle
 * @gfp_mask:	memory allocation flag
 *
 * This function should be called prior to locking and calling the
 * following function.  It preallocates enough memory to satisfy the
 * worst possible allocation.
 *
 * If the system is REALLY out of memory this function returns 0,
 * otherwise 1.
 */
int ida_pre_get(struct ida *ida, gfp_t gfp_mask)
{
	/* allocate idr_layers */
	if (!idr_pre_get(&ida->idr, gfp_mask))
		return 0;

	/* allocate free_bitmap */
	if (!ida->free_bitmap) {
		struct ida_bitmap *bitmap;

		bitmap = kmalloc(sizeof(struct ida_bitmap), gfp_mask);
		if (!bitmap)
			return 0;

		free_bitmap(ida, bitmap);
	}

	return 1;
}
EXPORT_SYMBOL(ida_pre_get);

/**
 * ida_get_new_above - allocate new ID above or equal to a start id
 * @ida:	ida handle
 * @staring_id:	id to start search at
 * @p_id:	pointer to the allocated handle
 *
 * Allocate new ID above or equal to @ida.  It should be called with
 * any required locks.
 *
 * If memory is required, it will return -EAGAIN, you should unlock
 * and go back to the ida_pre_get() call.  If the ida is full, it will
 * return -ENOSPC.
 *
 * @p_id returns a value in the range 0 ... 0x7fffffff.
 */
int ida_get_new_above(struct ida *ida, int starting_id, int *p_id)
{
	struct idr_layer *pa[MAX_LEVEL];
	struct ida_bitmap *bitmap;
	unsigned long flags;
	int idr_id = starting_id / IDA_BITMAP_BITS;
	int offset = starting_id % IDA_BITMAP_BITS;
	int t, id;

 restart:
	/* get vacant slot */
	t = idr_get_empty_slot(&ida->idr, idr_id, pa);
	if (t < 0)
		return _idr_rc_to_errno(t);

	if (t * IDA_BITMAP_BITS >= MAX_ID_BIT)
		return -ENOSPC;

	if (t != idr_id)
		offset = 0;
	idr_id = t;

	/* if bitmap isn't there, create a new one */
	bitmap = (void *)pa[0]->ary[idr_id & IDR_MASK];
	if (!bitmap) {
		spin_lock_irqsave(&ida->idr.lock, flags);
		bitmap = ida->free_bitmap;
		ida->free_bitmap = NULL;
		spin_unlock_irqrestore(&ida->idr.lock, flags);

		if (!bitmap)
			return -EAGAIN;

		memset(bitmap, 0, sizeof(struct ida_bitmap));
		rcu_assign_pointer(pa[0]->ary[idr_id & IDR_MASK],
				(void *)bitmap);
		pa[0]->count++;
	}

	/* lookup for empty slot */
	t = find_next_zero_bit(bitmap->bitmap, IDA_BITMAP_BITS, offset);
	if (t == IDA_BITMAP_BITS) {
		/* no empty slot after offset, continue to the next chunk */
		idr_id++;
		offset = 0;
		goto restart;
	}

	id = idr_id * IDA_BITMAP_BITS + t;
	if (id >= MAX_ID_BIT)
		return -ENOSPC;

	__set_bit(t, bitmap->bitmap);
	if (++bitmap->nr_busy == IDA_BITMAP_BITS)
		idr_mark_full(pa, idr_id);

	*p_id = id;

	/* Each leaf node can handle nearly a thousand slots and the
	 * whole idea of ida is to have small memory foot print.
	 * Throw away extra resources one by one after each successful
	 * allocation.
	 */
	if (ida->idr.id_free_cnt || ida->free_bitmap) {
		struct idr_layer *p = get_from_free_list(&ida->idr);
		if (p)
			kmem_cache_free(idr_layer_cache, p);
	}

	return 0;
}
EXPORT_SYMBOL(ida_get_new_above);

/**
 * ida_get_new - allocate new ID
 * @ida:	idr handle
 * @p_id:	pointer to the allocated handle
 *
 * Allocate new ID.  It should be called with any required locks.
 *
 * If memory is required, it will return -EAGAIN, you should unlock
 * and go back to the idr_pre_get() call.  If the idr is full, it will
 * return -ENOSPC.
 *
 * @id returns a value in the range 0 ... 0x7fffffff.
 */
int ida_get_new(struct ida *ida, int *p_id)
{
	return ida_get_new_above(ida, 0, p_id);
}
EXPORT_SYMBOL(ida_get_new);

/**
 * ida_remove - remove the given ID
 * @ida:	ida handle
 * @id:		ID to free
 */
void ida_remove(struct ida *ida, int id)
{
	struct idr_layer *p = ida->idr.top;
	int shift = (ida->idr.layers - 1) * IDR_BITS;
	int idr_id = id / IDA_BITMAP_BITS;
	int offset = id % IDA_BITMAP_BITS;
	int n;
	struct ida_bitmap *bitmap;

	/* clear full bits while looking up the leaf idr_layer */
	while ((shift > 0) && p) {
		n = (idr_id >> shift) & IDR_MASK;
		__clear_bit(n, &p->bitmap);
		p = p->ary[n];
		shift -= IDR_BITS;
	}

	if (p == NULL)
		goto err;

	n = idr_id & IDR_MASK;
	__clear_bit(n, &p->bitmap);

	bitmap = (void *)p->ary[n];
	if (!test_bit(offset, bitmap->bitmap))
		goto err;

	/* update bitmap and remove it if empty */
	__clear_bit(offset, bitmap->bitmap);
	if (--bitmap->nr_busy == 0) {
		__set_bit(n, &p->bitmap);	/* to please idr_remove() */
		idr_remove(&ida->idr, idr_id);
		free_bitmap(ida, bitmap);
	}

	return;

 err:
	printk(KERN_WARNING
	       "ida_remove called for id=%d which is not allocated.\n", id);
}
EXPORT_SYMBOL(ida_remove);

/**
 * ida_destroy - release all cached layers within an ida tree
 * ida:		ida handle
 */
void ida_destroy(struct ida *ida)
{
	idr_destroy(&ida->idr);
	kfree(ida->free_bitmap);
}
EXPORT_SYMBOL(ida_destroy);

/**
 * ida_init - initialize ida handle
 * @ida:	ida handle
 *
 * This function is use to set up the handle (@ida) that you will pass
 * to the rest of the functions.
 */
void ida_init(struct ida *ida)
{
	memset(ida, 0, sizeof(struct ida));
	idr_init(&ida->idr);

}
EXPORT_SYMBOL(ida_init);
