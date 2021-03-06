﻿/*
 * fs/dcache.c
 *
 * Complete reimplementation
 * (C) 1997 Thomas Schoebel-Theuer,
 * with heavy changes by Linus Torvalds
 */

/*
 * Notes on the allocation strategy:
 *
 * The dcache is a master of the icache - whenever a dcache entry
 * exists, the inode will always exist. "iput()" is done either when
 * the dcache entry is deleted or garbage collected.
 */

#include <linux/syscalls.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/fdtable.h>
#include <linux/fs.h>
#include <linux/fsnotify.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/hash.h>
#include <linux/cache.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/file.h>
#include <asm/uaccess.h>
#include <linux/security.h>
#include <linux/seqlock.h>
#include <linux/swap.h>
#include <linux/bootmem.h>
#include "internal.h"


int sysctl_vfs_cache_pressure __read_mostly = 100;
EXPORT_SYMBOL_GPL(sysctl_vfs_cache_pressure);

 __cacheline_aligned_in_smp DEFINE_SPINLOCK(dcache_lock);
__cacheline_aligned_in_smp DEFINE_SEQLOCK(rename_lock);

EXPORT_SYMBOL(dcache_lock);

static struct kmem_cache *dentry_cache __read_mostly;

//由于struct dentry可能没有字节对齐,DNAME_INLINE_LEN 会大于等于DNAME_INLINE_LEN_MIN
#define DNAME_INLINE_LEN (sizeof(struct dentry)-offsetof(struct dentry,d_iname))

/*
 * This is the single most critical data structure when it comes
 * to the dcache: the hashtable for lookups. Somebody should try
 * to make this good - I've just made it work.
 *
 * This hash-function tries to avoid losing too many bits of hash
 * information, yet avoid using a prime hash-size or similar.
 */
#define D_HASHBITS     d_hash_shift
#define D_HASHMASK     d_hash_mask

static unsigned int d_hash_mask __read_mostly;
static unsigned int d_hash_shift __read_mostly;
//记录内存中活动的dentry
static struct hlist_head *dentry_hashtable __read_mostly;

/* Statistics gathering. */
//dentry_stat.nr_unused用于记录dentry_unused中元素的个数
struct dentry_stat_t dentry_stat = {
	.age_limit = 45,
};

static void __d_free(struct dentry *dentry)
{
	WARN_ON(!list_empty(&dentry->d_alias));
	if (dname_external(dentry))
		kfree(dentry->d_name.name);
	kmem_cache_free(dentry_cache, dentry); 
}

static void d_callback(struct rcu_head *head)
{
	struct dentry * dentry = container_of(head, struct dentry, d_u.d_rcu);
	__d_free(dentry);
}

/*
 * no dcache_lock, please.  The caller must decrement dentry_stat.nr_dentry
 * inside dcache_lock.
 */
//释放dentry在kmem_cache中的空间 
static void d_free(struct dentry *dentry)
{
	if (dentry->d_op && dentry->d_op->d_release)
		dentry->d_op->d_release(dentry);
	/* if dentry was never inserted into hash, immediate free is OK */
	if (hlist_unhashed(&dentry->d_hash))
		//hlist是通过RCU保护的,如果不在hlist中,就可以直接删除
		__d_free(dentry);
	else
		/*
		 * 如果dentry在hlist中,就需要调用call_rcu注册回调函数,伺机删除.
		 * 因为读取RCU之前会将kernel抢占关闭,所以是要时间片开始轮转,就
		 * 说明没有reader持有旧的指针,就可以将旧指针删除.
		 */
		call_rcu(&dentry->d_u.d_rcu, d_callback);
}

/*
 * Release the dentry's inode, using the filesystem
 * d_iput() operation if defined.
 */
static void dentry_iput(struct dentry * dentry)
	__releases(dentry->d_lock)
	__releases(dcache_lock)
{
	struct inode *inode = dentry->d_inode;
	if (inode) {
		dentry->d_inode = NULL;
		//解除对inode的指向 
		list_del_init(&dentry->d_alias);
		spin_unlock(&dentry->d_lock);
		spin_unlock(&dcache_lock);
		if (!inode->i_nlink)
			fsnotify_inoderemove(inode);
		if (dentry->d_op && dentry->d_op->d_iput)
			dentry->d_op->d_iput(dentry, inode);
		else
			iput(inode);
	} else {
		spin_unlock(&dentry->d_lock);
		spin_unlock(&dcache_lock);
	}
}

/*
 * dentry_lru_(add|add_tail|del|del_init) must be called with dcache_lock held.
 */
static void dentry_lru_add(struct dentry *dentry)
{
	list_add(&dentry->d_lru, &dentry->d_sb->s_dentry_lru);
	dentry->d_sb->s_nr_dentry_unused++;
	dentry_stat.nr_unused++;
}

static void dentry_lru_add_tail(struct dentry *dentry)
{
	list_add_tail(&dentry->d_lru, &dentry->d_sb->s_dentry_lru);
	dentry->d_sb->s_nr_dentry_unused++;
	dentry_stat.nr_unused++;
}

static void dentry_lru_del(struct dentry *dentry)
{
	if (!list_empty(&dentry->d_lru)) {
		list_del(&dentry->d_lru);
		dentry->d_sb->s_nr_dentry_unused--;
		dentry_stat.nr_unused--;
	}
}

//remove dentry from lru list
static void dentry_lru_del_init(struct dentry *dentry)
{
	if (likely(!list_empty(&dentry->d_lru))) {
		list_del_init(&dentry->d_lru);
		dentry->d_sb->s_nr_dentry_unused--;
		dentry_stat.nr_unused--;
	}
}

/**
 * d_kill - kill dentry and return parent
 * @dentry: dentry to kill
 *
 * The dentry must already be unhashed and removed from the LRU.
 *
 * If this is the root of the dentry tree, return NULL.
 */
static struct dentry *d_kill(struct dentry *dentry)
	__releases(dentry->d_lock)
	__releases(dcache_lock)
{
	struct dentry *parent;

	//从其父目录的子目录链表中删除
	list_del(&dentry->d_u.d_child);
	dentry_stat.nr_dentry--;	/* For d_free, below */
	/*drops the locks, at that point nobody can reach this dentry */
	dentry_iput(dentry);
	if (IS_ROOT(dentry))
		parent = NULL;
	else
		parent = dentry->d_parent;
	d_free(dentry);
	return parent;
}

/* 
 * This is dput
 *
 * This is complicated by the fact that we do not want to put
 * dentries that are no longer on any hash chain on the unused
 * list: we'd much rather just get rid of them immediately.
 *
 * However, that implies that we have to traverse the dentry
 * tree upwards to the parents which might _also_ now be
 * scheduled for deletion (it may have been only waiting for
 * its last child to go away).
 *
 * This tail recursion is done by hand as we don't want to depend
 * on the compiler to always get this right (gcc generally doesn't).
 * Real recursion would eat up our stack space.
 */
 //尾递归是极其重要的，不用尾递归，函数的堆栈耗用难以估量，需要保存很多中间函数的堆栈

/*
 * dput - release a dentry
 * @dentry: dentry to release 
 *
 * Release a dentry. This will drop the usage count and if appropriate
 * call the dentry unlink method as well as removing it from the queues and
 * releasing its resources. If the parent dentries were scheduled for release
 * they too may now get deleted.
 *
 * no dcache lock, please.
 */

void dput(struct dentry *dentry)
{
	if (!dentry)
		return;

repeat:
	if (atomic_read(&dentry->d_count) == 1)
		might_sleep();
	/* 这里只对dentry->d_count做减1操作，如果减1后dentry->d_count不为0，那立刻返回。*/
    // atomic_dec_and_lock (x, &lock), 如果x - 1 == 0, 返回1， 否则，返回0
	if (!atomic_dec_and_lock(&dentry->d_count, &dcache_lock))
		return;

	spin_lock(&dentry->d_lock);
	if (atomic_read(&dentry->d_count)) {
		//因为在atomic_dec_and_lock中减一等于0了,这里不应该进来
		spin_unlock(&dentry->d_lock);
		spin_unlock(&dcache_lock);
		return;
	}

	/*
	 * AV: ->d_delete() is _NOT_ allowed to block now.
	 */
	/* 如果该dentry有自己的d_delete()函数，那直接调用它，指向完之后，调到unhash_it。*/
	if (dentry->d_op && dentry->d_op->d_delete) {
		if (dentry->d_op->d_delete(dentry))
			//d_delete操作成功返回1
			goto unhash_it;
	}
	/* Unreachable? Get rid of it */
 	if (d_unhashed(dentry))
		//已经有了DCACHE_UNHASHED标志,这时候就需要彻底删除
		goto kill_it;
  	if (list_empty(&dentry->d_lru)) {
		//如果此dentry没在lru list中,并且这个dentry还在hash表中,表示这个dentry最近被使用过
  		dentry->d_flags |= DCACHE_REFERENCED;
		//将其加入dentry_unused
		dentry_lru_add(dentry);
  	}
 	spin_unlock(&dentry->d_lock);
	spin_unlock(&dcache_lock);
	return;

unhash_it:
	__d_drop(dentry);
kill_it:
	/* if dentry was on the d_lru list delete it from there */
			//如果dentry已经在lru list上了, 就将其从lru list上删除
	dentry_lru_del(dentry);
        //从目录树中删除
	dentry = d_kill(dentry);
	if (dentry)
		goto repeat;
}

/**
 * d_invalidate - invalidate a dentry
 * @dentry: dentry to invalidate
 *
 * Try to invalidate the dentry if it turns out to be
 * possible. If there are other dentries that can be
 * reached through this one we can't delete it and we
 * return -EBUSY. On success we return 0.
 *
 * no dcache lock.
 */
 
int d_invalidate(struct dentry * dentry)
{
	/*
	 * If it's already been dropped, return OK.
	 */
	spin_lock(&dcache_lock);
	if (d_unhashed(dentry)) {
		spin_unlock(&dcache_lock);
		return 0;
	}
	/*
	 * Check whether to do a partial shrink_dcache
	 * to get rid of unused child entries.
	 */
	if (!list_empty(&dentry->d_subdirs)) {
		spin_unlock(&dcache_lock);
		shrink_dcache_parent(dentry);
		spin_lock(&dcache_lock);
	}

	/*
	 * Somebody else still using it?
	 *
	 * If it's a directory, we can't drop it
	 * for fear of somebody re-populating it
	 * with children (even though dropping it
	 * would make it unreachable from the root,
	 * we might still populate it if it was a
	 * working directory or similar).
	 */
	spin_lock(&dentry->d_lock);
	if (atomic_read(&dentry->d_count) > 1) {
		if (dentry->d_inode && S_ISDIR(dentry->d_inode->i_mode)) {
			spin_unlock(&dentry->d_lock);
			spin_unlock(&dcache_lock);
			return -EBUSY;
		}
	}

	__d_drop(dentry);
	spin_unlock(&dentry->d_lock);
	spin_unlock(&dcache_lock);
	return 0;
}

/* This should be called _only_ with dcache_lock held */

static inline struct dentry * __dget_locked(struct dentry *dentry)
{
	atomic_inc(&dentry->d_count);
	dentry_lru_del_init(dentry);
	return dentry;
}

struct dentry * dget_locked(struct dentry *dentry)
{
	return __dget_locked(dentry);
}

/**
 * d_find_alias - grab a hashed alias of inode
 * @inode: inode in question
 * @want_discon:  flag, used by d_splice_alias, to request
 *          that only a DISCONNECTED alias be returned.
 *
 * If inode has a hashed alias, or is a directory and has any alias,
 * acquire the reference to alias and return it. Otherwise return NULL.
 * Notice that if inode is a directory there can be only one alias and
 * it can be unhashed only if it has no children, or if it is the root
 * of a filesystem.
 *
 * If the inode has an IS_ROOT, DCACHE_DISCONNECTED alias, then prefer
 * any other hashed alias over that one unless @want_discon is set,
 * in which case only return an IS_ROOT, DCACHE_DISCONNECTED alias.
 */

static struct dentry * __d_find_alias(struct inode *inode, int want_discon)
{
	struct list_head *head, *next, *tmp;
	struct dentry *alias, *discon_alias=NULL;

	head = &inode->i_dentry;
	next = inode->i_dentry.next;
	while (next != head) {
		tmp = next;
		next = tmp->next;
		prefetch(next);
		alias = list_entry(tmp, struct dentry, d_alias);
 		if (S_ISDIR(inode->i_mode) || !d_unhashed(alias)) {
			if (IS_ROOT(alias) &&
			    (alias->d_flags & DCACHE_DISCONNECTED))
				discon_alias = alias;
			else if (!want_discon) {
				__dget_locked(alias);
				return alias;
			}
		}
	}
	if (discon_alias)
		__dget_locked(discon_alias);
	return discon_alias;
}

struct dentry * d_find_alias(struct inode *inode)
{
	struct dentry *de = NULL;

	if (!list_empty(&inode->i_dentry)) {
		spin_lock(&dcache_lock);
		de = __d_find_alias(inode, 0);
		spin_unlock(&dcache_lock);
	}
	return de;
}

/*
 *	Try to kill dentries associated with this inode.
 * WARNING: you must own a reference to inode.
 */
void d_prune_aliases(struct inode *inode)
{
	struct dentry *dentry;
restart:
	spin_lock(&dcache_lock);
	list_for_each_entry(dentry, &inode->i_dentry, d_alias) {
		spin_lock(&dentry->d_lock);
		if (!atomic_read(&dentry->d_count)) {
			__dget_locked(dentry);
			__d_drop(dentry);
			spin_unlock(&dentry->d_lock);
			spin_unlock(&dcache_lock);
			dput(dentry);
			goto restart;
		}
		spin_unlock(&dentry->d_lock);
	}
	spin_unlock(&dcache_lock);
}

/*
 * Throw away a dentry - free the inode, dput the parent.  This requires that
 * the LRU list has already been removed.
 *
 * Try to prune ancestors as well.  This is necessary to prevent
 * quadratic behavior of shrink_dcache_parent(), but is also expected
 * to be beneficial in reducing dentry cache fragmentation.
 */
static void prune_one_dentry(struct dentry * dentry)
	__releases(dentry->d_lock)
	__releases(dcache_lock)
	__acquires(dcache_lock)
{
	//从hash表中删除
	__d_drop(dentry);
	//将dentry完全删除
	dentry = d_kill(dentry);

	/*
	 * Prune ancestors.  Locking is simpler than in dput(),
	 * because dcache_lock needs to be taken anyway.
	 */
	spin_lock(&dcache_lock);
	while (dentry) {
		if (!atomic_dec_and_lock(&dentry->d_count, &dentry->d_lock))
			//如果没有对dentry上锁,也就是dentry的d_count域不是1的情况,直接返回
			return;

		if (dentry->d_op && dentry->d_op->d_delete)
			dentry->d_op->d_delete(dentry);
		//将dentry从其d_lru域链入的链表上取下来(这个链表不一定是sb的lru链表)
		dentry_lru_del_init(dentry);
	//释放dentry空间
		__d_drop(dentry);
		dentry = d_kill(dentry);
		spin_lock(&dcache_lock);
	}
}

/*
 * Shrink the dentry LRU on a given superblock.
 * @sb   : superblock to shrink dentry LRU.
 * @count: If count is NULL, we prune all dentries on superblock.
 * @flags: If flags is non-zero, we need to do special processing based on
 * which flags are set. This means we don't need to maintain multiple
 * similar copies of this loop.
 */
/**
 * @sb    : 特定的super block
 * @count : 如果count是NULL, 就要除去super block上所有的dentry
 * @flags : 如果flags不是0, 就需要一些特定的处理方式去处理这些dentry.如果
 *          flags == 0, 那么就只要除去dentry就行了.
 */
static void __shrink_dcache_sb(struct super_block *sb, int *count, int flags)
{
    //被引用过
	LIST_HEAD(referenced);
	LIST_HEAD(tmp);
	struct dentry *dentry;
	int cnt = 0;

	BUG_ON(!sb);
	BUG_ON((flags & DCACHE_REFERENCED) && count == NULL);
	spin_lock(&dcache_lock);
	if (count != NULL)
		/* called from prune_dcache() and shrink_dcache_parent() */
		cnt = *count;
restart:
	if (count == NULL)
        //将当前sb上lru链表中所有的dentry,放到tmp链表中去
		list_splice_init(&sb->s_dentry_lru, &tmp);
	else {
		while (!list_empty(&sb->s_dentry_lru)) {
            //从lru链表的尾端取出dentry,最尾端的dentry最不经常使用
			dentry = list_entry(sb->s_dentry_lru.prev,
					struct dentry, d_lru);
			BUG_ON(dentry->d_sb != sb);

			spin_lock(&dentry->d_lock);
			/*
			 * If we are honouring the DCACHE_REFERENCED flag and
			 * the dentry has this flag set, don't free it. Clear
			 * the flag and put it back on the LRU.
			 */
			if ((flags & DCACHE_REFERENCED)
				&& (dentry->d_flags & DCACHE_REFERENCED)) {
                /*
                 *  如果flags是DCACHE_REFERENCED,表示当前调用__shrink_dcache_sb需要
                 *  根据使用频率来区别对待dentry,对于d_flags域是DCACHE_REFERENCED的
                 *  dentry,也就是有使用过,但是目前没有其他地方使用,刚分配到lru上的
                 *  dentry,不能将其直接删除,而是去除其DCACHE_REFERENCED标志,重新将其
                 *  放回lru链表中去.而域没有DCACHE_REFERENCED标志的dentry接下来的工作
                 *  就是将其删除.
                 */
				dentry->d_flags &= ~DCACHE_REFERENCED;
                //将dentry从lru上取下,尾插入referenced链表
				list_move_tail(&dentry->d_lru, &referenced);
				spin_unlock(&dentry->d_lock);
			} else {
                //将dentry从lru上取下,尾插入tmp链表
				list_move_tail(&dentry->d_lru, &tmp);
				spin_unlock(&dentry->d_lock);
				cnt--;
				if (!cnt)
					break;
			}
			cond_resched_lock(&dcache_lock);
		}
	}
	while (!list_empty(&tmp)) {
		dentry = list_entry(tmp.prev, struct dentry, d_lru);
		dentry_lru_del_init(dentry);
		spin_lock(&dentry->d_lock);
		/*
		 * We found an inuse dentry which was not removed from
		 * the LRU because of laziness during lookup.  Do not free
		 * it - just keep it off the LRU list.
		 */
		if (atomic_read(&dentry->d_count)) {
			spin_unlock(&dentry->d_lock);
			continue;
		}
		prune_one_dentry(dentry);
		/* dentry->d_lock was dropped in prune_one_dentry() */
		cond_resched_lock(&dcache_lock);
	}
	if (count == NULL && !list_empty(&sb->s_dentry_lru))
		goto restart;
	if (count != NULL)
		*count = cnt;
	if (!list_empty(&referenced))
		//将referenced链表头插入sb的s_dentry_lru链表中去
		list_splice(&referenced, &sb->s_dentry_lru);
	spin_unlock(&dcache_lock);
}

/**
 * prune_dcache - shrink the dcache
 * @count: number of entries to try to free
 *
 * Shrink the dcache. This is done when we need more memory, or simply when we
 * need to unmount something (at which point we need to unuse all dentries).
 *
 * This function may fail to free any resources if all the dentries are in use.
 */
static void prune_dcache(int count)
{
	struct super_block *sb;
	int w_count;
	int unused = dentry_stat.nr_unused;
	int prune_ratio;
	int pruned;

	if (unused == 0 || count == 0)
		return;
	spin_lock(&dcache_lock);
restart:
	if (count >= unused)
		prune_ratio = 1;
	else
		prune_ratio = unused / count;
	spin_lock(&sb_lock);
	list_for_each_entry(sb, &super_blocks, s_list) {
		//遍历系统中所有的super block
		if (sb->s_nr_dentry_unused == 0)
			continue;
		sb->s_count++;
		/* Now, we reclaim unused dentrins with fairness.
		 * We reclaim them same percentage from each superblock.
		 * We calculate number of dentries to scan on this sb
		 * as follows, but the implementation is arranged to avoid
		 * overflows:
		 * number of dentries to scan on this sb =
		 * count * (number of dentries on this sb /
		 * number of dentries in the machine)
		 */
		spin_unlock(&sb_lock);
		if (prune_ratio != 1)
			//对于系统中每一个sb,回收的dentry占s_nr_dentry_unused的比率是一样的
			w_count = (sb->s_nr_dentry_unused / prune_ratio) + 1;
		else
			//全部回收
			w_count = sb->s_nr_dentry_unused;
		pruned = w_count;
		/*
		 * We need to be sure this filesystem isn't being unmounted,
		 * otherwise we could race with generic_shutdown_super(), and
		 * end up holding a reference to an inode while the filesystem
		 * is unmounted.  So we try to get s_umount, and make sure
		 * s_root isn't NULL.
		 */
		 /*
		  * 确保sb所在的文件系统没有处于卸载过程,否则可能和generic_shutdown_super()
		  * 产生竞争,继而出现文件系统已经卸载，但是我们还持有其中一个inode.
		 */
		if (down_read_trylock(&sb->s_umount)) {
			if ((sb->s_root != NULL) &&
			    (!list_empty(&sb->s_dentry_lru))) {
				spin_unlock(&dcache_lock);
				__shrink_dcache_sb(sb, &w_count,
						DCACHE_REFERENCED);
				pruned -= w_count;
				spin_lock(&dcache_lock);
			}
			up_read(&sb->s_umount);
		}
		spin_lock(&sb_lock);
		count -= pruned;
		/*
		 * restart only when sb is no longer on the list and
		 * we have more work to do.
		 */
		if (__put_super_and_need_restart(sb) && count > 0) {
			spin_unlock(&sb_lock);
			goto restart;
		}
	}
	spin_unlock(&sb_lock);
	spin_unlock(&dcache_lock);
}

/**
 * shrink_dcache_sb - shrink dcache for a superblock
 * @sb: superblock
 *
 * Shrink the dcache for the specified super block. This
 * is used to free the dcache before unmounting a file
 * system
 */
void shrink_dcache_sb(struct super_block * sb)
{
	__shrink_dcache_sb(sb, NULL, 0);
}

/*
 * destroy a single subtree of dentries for unmount
 * - see the comments on shrink_dcache_for_umount() for a description of the
 *   locking
 */
//dcache has two parts:1.hlist 2.lru list
//always find and delete the left-most(first child) dentry
static void shrink_dcache_for_umount_subtree(struct dentry *dentry)
{
	struct dentry *parent;
    //record the number of freed dentries
	unsigned detached = 0;

	BUG_ON(!IS_ROOT(dentry));

	/* detach this root from the system */
	spin_lock(&dcache_lock);
	//delete dentry in lru list
	dentry_lru_del_init(dentry);
	//remove dentry from hash list
	__d_drop(dentry);
	spin_unlock(&dcache_lock);

	for (;;) {
		/* descend to the first leaf in the current subtree */
		while (!list_empty(&dentry->d_subdirs)) {
			struct dentry *loop;

			/* this is a branch with children - detach all of them
			 * from the system in one go */
			spin_lock(&dcache_lock);
			list_for_each_entry(loop, &dentry->d_subdirs,
					    d_u.d_child) {
                //delete dentry in lru list
				dentry_lru_del_init(loop);
                //remove dentry from hash list
				__d_drop(loop);
				cond_resched_lock(&dcache_lock);
			}
			spin_unlock(&dcache_lock);

			/* move to the first child */
			dentry = list_entry(dentry->d_subdirs.next,
					    struct dentry, d_u.d_child);
		}

		/* consume the dentries from this leaf up through its parents
		 * until we find one with children or run out altogether */
		do {
			struct inode *inode;

            //we want to free dentry in this loop, if dentry->d_count != 0, there must be something wrong
			if (atomic_read(&dentry->d_count) != 0) {
				printk(KERN_ERR
				       "BUG: Dentry %p{i=%lx,n=%s}"
				       " still in use (%d)"
				       " [unmount of %s %s]\n",
				       dentry,
				       dentry->d_inode ?
				       dentry->d_inode->i_ino : 0UL,
				       dentry->d_name.name,
				       atomic_read(&dentry->d_count),
				       dentry->d_sb->s_type->name,
				       dentry->d_sb->s_id);
				BUG();
			}

			if (IS_ROOT(dentry))
				parent = NULL;
			else {
				parent = dentry->d_parent;
                //if child dentry freed, it's parent's d_count should be decreased
				atomic_dec(&parent->d_count);
			}

			list_del(&dentry->d_u.d_child);
			detached++;

			inode = dentry->d_inode;
			if (inode) {
				dentry->d_inode = NULL;
                //detach dentry from inode's i_dentry list
				list_del_init(&dentry->d_alias);
				if (dentry->d_op && dentry->d_op->d_iput)
					dentry->d_op->d_iput(dentry, inode);
				else
					iput(inode);
			}
            //free dentry's pointer
			d_free(dentry);

			/* finished when we fall off the top of the tree,
			 * otherwise we ascend to the parent and move to the
			 * next sibling if there is one */
			if (!parent)
				goto out;

			dentry = parent;

        //if dentry has no child, continue
		} while (list_empty(&dentry->d_subdirs));

        //if dentry has childs
		dentry = list_entry(dentry->d_subdirs.next,
				    struct dentry, d_u.d_child);
	}
out:
	/* several dentries were freed, need to correct nr_dentry */
	spin_lock(&dcache_lock);
	dentry_stat.nr_dentry -= detached;
	spin_unlock(&dcache_lock);
}

/*
 * destroy the dentries attached to a superblock on unmounting
 * - we don't need to use dentry->d_lock, and only need dcache_lock when
 *   removing the dentry from the system lists and hashes because:
 *   - the superblock is detached from all mountings and open files, so the
 *     dentry trees will not be rearranged by the VFS
 *   - s_umount is write-locked, so the memory pressure shrinker will ignore
 *     any dentries belonging to this superblock that it comes across
 *   - the filesystem itself is no longer permitted to rearrange the dentries
 *     in this superblock
 */
void shrink_dcache_for_umount(struct super_block *sb)
{
	struct dentry *dentry;

	if (down_read_trylock(&sb->s_umount))
		BUG();

	dentry = sb->s_root;
	sb->s_root = NULL;
	atomic_dec(&dentry->d_count);
	shrink_dcache_for_umount_subtree(dentry);

    //free anonymous dentry
	while (!hlist_empty(&sb->s_anon)) {
		dentry = hlist_entry(sb->s_anon.first, struct dentry, d_hash);
		shrink_dcache_for_umount_subtree(dentry);
	}
}

/*
 * Search for at least 1 mount point in the dentry's subdirs.
 * We descend to the next level whenever the d_subdirs
 * list is non-empty and continue searching.
 */
 
/**
 * have_submounts - check for mounts over a dentry
 * @parent: dentry to check.
 *
 * Return true if the parent or its subdirectories contain
 * a mount point
 */
 
int have_submounts(struct dentry *parent)
{
	struct dentry *this_parent = parent;
	struct list_head *next;

	spin_lock(&dcache_lock);
	if (d_mountpoint(parent))
		goto positive;
repeat:
	next = this_parent->d_subdirs.next;
resume:
	while (next != &this_parent->d_subdirs) {
		struct list_head *tmp = next;
		struct dentry *dentry = list_entry(tmp, struct dentry, d_u.d_child);
		next = tmp->next;
		/* Have we found a mount point ? */
		if (d_mountpoint(dentry))
			goto positive;
		if (!list_empty(&dentry->d_subdirs)) {
			this_parent = dentry;
			goto repeat;
		}
	}
	/*
	 * All done at this level ... ascend and resume the search.
	 */
	if (this_parent != parent) {
		next = this_parent->d_u.d_child.next;
		this_parent = this_parent->d_parent;
		goto resume;
	}
	spin_unlock(&dcache_lock);
	return 0; /* No mount points found in tree */
positive:
	spin_unlock(&dcache_lock);
	return 1;
}

/*
 * Search the dentry child list for the specified parent,
 * and move any unused dentries to the end of the unused
 * list for prune_dcache(). We descend to the next level
 * whenever the d_subdirs list is non-empty and continue
 * searching.
 *
 * It returns zero iff there are no unused children,
 * otherwise  it returns the number of children moved to
 * the end of the unused list. This may not be the total
 * number of unused children, because select_parent can
 * drop the lock and return early due to latency
 * constraints.
 */
 //只查找没有子目录的dentry,因为如果一个dentry有子目录,其dentry->d_count肯定不为0
static int select_parent(struct dentry * parent)
{
	struct dentry *this_parent = parent;
	struct list_head *next;
	int found = 0;

	spin_lock(&dcache_lock);
repeat:
	next = this_parent->d_subdirs.next;
resume:
	while (next != &this_parent->d_subdirs) {
		struct list_head *tmp = next;
		struct dentry *dentry = list_entry(tmp, struct dentry, d_u.d_child);
		next = tmp->next;

		dentry_lru_del_init(dentry);
		/* 
		 * move only zero ref count dentries to the end 
		 * of the unused list for prune_dcache
		 */
		 //如果dentry->d_count为0, 则将dentry插入dentry_unused尾部
		if (!atomic_read(&dentry->d_count)) {
			dentry_lru_add_tail(dentry);
			found++;
		}

		/*
		 * We can return to the caller if we have found some (this
		 * ensures forward progress). We'll be coming back to find
		 * the rest.
		 */
		 //如果找到目标dentry, 并且need_resched为真,就退出,下次进来的时候继续
		if (found && need_resched())
			goto out;

		/*
		 * Descend a level if the d_subdirs list is non-empty.
		 */
		if (!list_empty(&dentry->d_subdirs)) {
			this_parent = dentry;
			goto repeat;
		}
	}
	/*
	 * All done at this level ... ascend and resume the search.
	 */
	if (this_parent != parent) {
		next = this_parent->d_u.d_child.next;
		this_parent = this_parent->d_parent;
		goto resume;
	}
out:
	spin_unlock(&dcache_lock);
	return found;
}

/**
 * shrink_dcache_parent - prune dcache
 * @parent: parent of entries to prune
 *
 * Prune the dcache to remove unused children of the parent dentry.
 */
 
void shrink_dcache_parent(struct dentry * parent)
{
	struct super_block *sb = parent->d_sb;
	int found;

	//将dentry_unused中的d_count为0的dentry放到list tail, 统计d_count为0的dentry的个数
	while ((found = select_parent(parent)) != 0)
		//将d_count为0的dentry,并且d_sb是parent->d_sb的dentry删除
		__shrink_dcache_sb(sb, &found, 0);
}

/*
 * Scan `nr' dentries and return the number which remain.
 *
 * We need to avoid reentering the filesystem if the caller is performing a
 * GFP_NOFS allocation attempt.  One example deadlock is:
 *
 * ext2_new_block->getblk->GFP->shrink_dcache_memory->prune_dcache->
 * prune_one_dentry->dput->dentry_iput->iput->inode->i_sb->s_op->put_inode->
 * ext2_discard_prealloc->ext2_free_blocks->lock_super->DEADLOCK.
 *
 * In this case we return -1 to tell the caller that we baled.
 */
static int shrink_dcache_memory(int nr, gfp_t gfp_mask)
{
	if (nr) {
		if (!(gfp_mask & __GFP_FS))
			return -1;
		prune_dcache(nr);
	}
	return (dentry_stat.nr_unused / 100) * sysctl_vfs_cache_pressure;
}

static struct shrinker dcache_shrinker = {
	.shrink = shrink_dcache_memory,
	.seeks = DEFAULT_SEEKS,
};

/**
 * d_alloc	-	allocate a dcache entry
 * @parent: parent of entry to allocate
 * @name: qstr of the name
 *
 * Allocates a dentry. It returns %NULL if there is insufficient memory
 * available. On a success the dentry is returned. The name passed in is
 * copied and the copy passed in may be reused after this call.
 */
 
struct dentry *d_alloc(struct dentry * parent, const struct qstr *name)
{
	struct dentry *dentry;
	char *dname;

	dentry = kmem_cache_alloc(dentry_cache, GFP_KERNEL);
	if (!dentry)
		return NULL;

	if (name->len > DNAME_INLINE_LEN-1) {
		//如果条件成立,那么dname需要分配空间用来保存文件名称
		dname = kmalloc(name->len + 1, GFP_KERNEL);
		if (!dname) {
			kmem_cache_free(dentry_cache, dentry); 
			return NULL;
		}
	} else  {
		//否则dname指向d_iname空间,和d_iname共用空间
		dname = dentry->d_iname;
	}	
	dentry->d_name.name = dname;

	dentry->d_name.len = name->len;
	dentry->d_name.hash = name->hash;
	memcpy(dname, name->name, name->len);
	dname[name->len] = 0;

	//把 dentry 的使用计数初始化为1
	atomic_set(&dentry->d_count, 1);
	//这时候还没加入hash表
	dentry->d_flags = DCACHE_UNHASHED;
	spin_lock_init(&dentry->d_lock);
	dentry->d_inode = NULL;
	dentry->d_parent = NULL;
	dentry->d_sb = NULL;
	dentry->d_op = NULL;
	dentry->d_fsdata = NULL;
	dentry->d_mounted = 0;
#ifdef CONFIG_PROFILING
	dentry->d_cookie = NULL;
#endif
	INIT_HLIST_NODE(&dentry->d_hash);
	INIT_LIST_HEAD(&dentry->d_lru);
	INIT_LIST_HEAD(&dentry->d_subdirs);
	INIT_LIST_HEAD(&dentry->d_alias);

	if (parent) {
		//dget是为了防止父目录在该 dentry 删除前被删除
		dentry->d_parent = dget(parent);
		dentry->d_sb = parent->d_sb;
	} else {
		INIT_LIST_HEAD(&dentry->d_u.d_child);
	}

	spin_lock(&dcache_lock);
	if (parent)
	//如果存在父目录,就把自己加入父目录的子目录链表中
		list_add(&dentry->d_u.d_child, &parent->d_subdirs);
	dentry_stat.nr_dentry++;
	spin_unlock(&dcache_lock);

	return dentry;
}

struct dentry *d_alloc_name(struct dentry *parent, const char *name)
{
	struct qstr q;

	q.name = name;
	q.len = strlen(name);
	q.hash = full_name_hash(q.name, q.len);
	return d_alloc(parent, &q);
}

/* the caller must hold dcache_lock */
static void __d_instantiate(struct dentry *dentry, struct inode *inode)
{
	if (inode)
		list_add(&dentry->d_alias, &inode->i_dentry);
	dentry->d_inode = inode;
	fsnotify_d_instantiate(dentry, inode);
}

/**
 * d_instantiate - fill in inode information for a dentry
 * @entry: dentry to complete
 * @inode: inode to attach to this dentry
 *
 * Fill in inode information in the entry.
 *
 * This turns negative dentries into productive full members
 * of society.
 *
 * NOTE! This assumes that the inode count has been incremented
 * (or otherwise set) by the caller to indicate that it is now
 * in use by the dcache.
 */
 
void d_instantiate(struct dentry *entry, struct inode * inode)
{
	BUG_ON(!list_empty(&entry->d_alias));
	spin_lock(&dcache_lock);
	__d_instantiate(entry, inode);
	spin_unlock(&dcache_lock);
	security_d_instantiate(entry, inode);
}

/**
 * d_instantiate_unique - instantiate a non-aliased dentry
 * @entry: dentry to instantiate
 * @inode: inode to attach to this dentry
 *
 * Fill in inode information in the entry. On success, it returns NULL.
 * If an unhashed alias of "entry" already exists, then we return the
 * aliased dentry instead and drop one reference to inode.
 *
 * Note that in order to avoid conflicts with rename() etc, the caller
 * had better be holding the parent directory semaphore.
 *
 * This also assumes that the inode count has been incremented
 * (or otherwise set) by the caller to indicate that it is now
 * in use by the dcache.
 */
static struct dentry *__d_instantiate_unique(struct dentry *entry,
					     struct inode *inode)
{
	struct dentry *alias;
	int len = entry->d_name.len;
	const char *name = entry->d_name.name;
	unsigned int hash = entry->d_name.hash;

	if (!inode) {
		__d_instantiate(entry, NULL);
		return NULL;
	}

	list_for_each_entry(alias, &inode->i_dentry, d_alias) {
		struct qstr *qstr = &alias->d_name;

		if (qstr->hash != hash)
			continue;
		if (alias->d_parent != entry->d_parent)
			continue;
		if (qstr->len != len)
			continue;
		if (memcmp(qstr->name, name, len))
			continue;
		dget_locked(alias);
		return alias;
	}

	__d_instantiate(entry, inode);
	return NULL;
}

struct dentry *d_instantiate_unique(struct dentry *entry, struct inode *inode)
{
	struct dentry *result;

	BUG_ON(!list_empty(&entry->d_alias));

	spin_lock(&dcache_lock);
	result = __d_instantiate_unique(entry, inode);
	spin_unlock(&dcache_lock);

	if (!result) {
		security_d_instantiate(entry, inode);
		return NULL;
	}

	BUG_ON(!d_unhashed(result));
	iput(inode);
	return result;
}

EXPORT_SYMBOL(d_instantiate_unique);

/**
 * d_alloc_root - allocate root dentry
 * @root_inode: inode to allocate the root for
 *
 * Allocate a root ("/") dentry for the inode given. The inode is
 * instantiated and returned. %NULL is returned if there is insufficient
 * memory or the inode passed is %NULL.
 */
 
struct dentry * d_alloc_root(struct inode * root_inode)
{
	struct dentry *res = NULL;

	if (root_inode) {
		static const struct qstr name = { .name = "/", .len = 1 };

		//根目录的父目录为NULL
		res = d_alloc(NULL, &name);
		if (res) {
			//根目录dentry的sb和根目录的inode指向的sb一致
			res->d_sb = root_inode->i_sb;
			//根目录的父目录是自己
			res->d_parent = res;
			d_instantiate(res, root_inode);
		}
	}
	return res;
}

/*
 * d_hash(dentry,hash) 为散列函数,它将dentry地址和hash值相组合,
 * 映射到 dentry_hashtable 表中,返回相应的散列链.
 */
static inline struct hlist_head *d_hash(struct dentry *parent,
					unsigned long hash)
{
	hash += ((unsigned long) parent ^ GOLDEN_RATIO_PRIME) / L1_CACHE_BYTES;
	hash = hash ^ ((hash ^ GOLDEN_RATIO_PRIME) >> D_HASHBITS);
	/* 保证hash值在hashtable中 */
	return dentry_hashtable + (hash & D_HASHMASK);
}

/**
 * d_obtain_alias - find or allocate a dentry for a given inode
 * @inode: inode to allocate the dentry for
 *
 * Obtain a dentry for an inode resulting from NFS filehandle conversion or
 * similar open by handle operations.  The returned dentry may be anonymous,
 * or may have a full name (if the inode was already in the cache).
 *
 * When called on a directory inode, we must ensure that the inode only ever
 * has one dentry.  If a dentry is found, that is returned instead of
 * allocating a new one.
 *
 * On successful return, the reference to the inode has been transferred
 * to the dentry.  In case of an error the reference on the inode is released.
 * To make it easier to use in export operations a %NULL or IS_ERR inode may
 * be passed in and will be the error will be propagate to the return value,
 * with a %NULL @inode replaced by ERR_PTR(-ESTALE).
 */
struct dentry *d_obtain_alias(struct inode *inode)
{
	static const struct qstr anonstring = { .name = "" };
	struct dentry *tmp;
	struct dentry *res;

	if (!inode)
		return ERR_PTR(-ESTALE);
	if (IS_ERR(inode))
		return ERR_CAST(inode);

	res = d_find_alias(inode);
	if (res)
		goto out_iput;

	tmp = d_alloc(NULL, &anonstring);
	if (!tmp) {
		res = ERR_PTR(-ENOMEM);
		goto out_iput;
	}
	tmp->d_parent = tmp; /* make sure dput doesn't croak */

	spin_lock(&dcache_lock);
	res = __d_find_alias(inode, 0);
	if (res) {
		spin_unlock(&dcache_lock);
		dput(tmp);
		goto out_iput;
	}

	/* attach a disconnected dentry */
	spin_lock(&tmp->d_lock);
	tmp->d_sb = inode->i_sb;
	tmp->d_inode = inode;
	tmp->d_flags |= DCACHE_DISCONNECTED;
	tmp->d_flags &= ~DCACHE_UNHASHED;
	list_add(&tmp->d_alias, &inode->i_dentry);
	hlist_add_head(&tmp->d_hash, &inode->i_sb->s_anon);
	spin_unlock(&tmp->d_lock);

	spin_unlock(&dcache_lock);
	return tmp;

 out_iput:
	iput(inode);
	return res;
}
EXPORT_SYMBOL_GPL(d_obtain_alias);

/**
 * d_splice_alias - splice a disconnected dentry into the tree if one exists
 * @inode:  the inode which may have a disconnected dentry
 * @dentry: a negative dentry which we want to point to the inode.
 *
 * If inode is a directory and has a 'disconnected' dentry (i.e. IS_ROOT and
 * DCACHE_DISCONNECTED), then d_move that in place of the given dentry
 * and return it, else simply d_add the inode to the dentry and return NULL.
 *
 * This is needed in the lookup routine of any filesystem that is exportable
 * (via knfsd) so that we can build dcache paths to directories effectively.
 *
 * If a dentry was found and moved, then it is returned.  Otherwise NULL
 * is returned.  This matches the expected return value of ->lookup.
 *
 */
struct dentry *d_splice_alias(struct inode *inode, struct dentry *dentry)
{
	struct dentry *new = NULL;

	if (inode && S_ISDIR(inode->i_mode)) {
		spin_lock(&dcache_lock);
		new = __d_find_alias(inode, 1);
		if (new) {
			BUG_ON(!(new->d_flags & DCACHE_DISCONNECTED));
			spin_unlock(&dcache_lock);
			security_d_instantiate(new, inode);
			d_rehash(dentry);
			d_move(new, dentry);
			iput(inode);
		} else {
			/* already taking dcache_lock, so d_add() by hand */
			__d_instantiate(dentry, inode);
			spin_unlock(&dcache_lock);
			security_d_instantiate(dentry, inode);
			d_rehash(dentry);
		}
	} else
		d_add(dentry, inode);
	return new;
}

/**
 * d_add_ci - lookup or allocate new dentry with case-exact name
 * @inode:  the inode case-insensitive lookup has found
 * @dentry: the negative dentry that was passed to the parent's lookup func
 * @name:   the case-exact name to be associated with the returned dentry
 *
 * This is to avoid filling the dcache with case-insensitive names to the
 * same inode, only the actual correct case is stored in the dcache for
 * case-insensitive filesystems.
 *
 * For a case-insensitive lookup match and if the the case-exact dentry
 * already exists in in the dcache, use it and return it.
 *
 * If no entry exists with the exact case name, allocate new dentry with
 * the exact case, and return the spliced entry.
 */
struct dentry *d_add_ci(struct dentry *dentry, struct inode *inode,
			struct qstr *name)
{
	int error;
	struct dentry *found;
	struct dentry *new;

	/* Does a dentry matching the name exist already? */
	found = d_hash_and_lookup(dentry->d_parent, name);
	/* If not, create it now and return */
	if (!found) {
		new = d_alloc(dentry->d_parent, name);
		if (!new) {
			error = -ENOMEM;
			goto err_out;
		}
		found = d_splice_alias(inode, new);
		if (found) {
			dput(new);
			return found;
		}
		return new;
	}
	/* Matching dentry exists, check if it is negative. */
	if (found->d_inode) {
		if (unlikely(found->d_inode != inode)) {
			/* This can't happen because bad inodes are unhashed. */
			BUG_ON(!is_bad_inode(inode));
			BUG_ON(!is_bad_inode(found->d_inode));
		}
		/*
		 * Already have the inode and the dentry attached, decrement
		 * the reference count to balance the iget() done
		 * earlier on.  We found the dentry using d_lookup() so it
		 * cannot be disconnected and thus we do not need to worry
		 * about any NFS/disconnectedness issues here.
		 */
		iput(inode);
		return found;
	}
	/*
	 * Negative dentry: instantiate it unless the inode is a directory and
	 * has a 'disconnected' dentry (i.e. IS_ROOT and DCACHE_DISCONNECTED),
	 * in which case d_move() that in place of the found dentry.
	 */
	if (!S_ISDIR(inode->i_mode)) {
		/* Not a directory; everything is easy. */
		d_instantiate(found, inode);
		return found;
	}
	spin_lock(&dcache_lock);
	if (list_empty(&inode->i_dentry)) {
		/*
		 * Directory without a 'disconnected' dentry; we need to do
		 * d_instantiate() by hand because it takes dcache_lock which
		 * we already hold.
		 */
		__d_instantiate(found, inode);
		spin_unlock(&dcache_lock);
		security_d_instantiate(found, inode);
		return found;
	}
	/*
	 * Directory with a 'disconnected' dentry; get a reference to the
	 * 'disconnected' dentry.
	 */
	new = list_entry(inode->i_dentry.next, struct dentry, d_alias);
	dget_locked(new);
	spin_unlock(&dcache_lock);
	/* Do security vodoo. */
	security_d_instantiate(found, inode);
	/* Move new in place of found. */
	d_move(new, found);
	/* Balance the iget() we did above. */
	iput(inode);
	/* Throw away found. */
	dput(found);
	/* Use new as the actual dentry. */
	return new;

err_out:
	iput(inode);
	return ERR_PTR(error);
}

/**
 * d_lookup - search for a dentry
 * @parent: parent dentry
 * @name: qstr of name we wish to find
 *
 * Searches the children of the parent dentry for the name in question. If
 * the dentry is found its reference count is incremented and the dentry
 * is returned. The caller must use d_put to free the entry when it has
 * finished using it. %NULL is returned on failure.
 *
 * __d_lookup is dcache_lock free. The hash list is protected using RCU.
 * Memory barriers are used while updating and doing lockless traversal. 
 * To avoid races with d_move while rename is happening, d_lock is used.
 *
 * Overflows in memcmp(), while d_move, are avoided by keeping the length
 * and name pointer in one structure pointed by d_qstr.
 *
 * rcu_read_lock() and rcu_read_unlock() are used to disable preemption while
 * lookup is going on.
 *
 * The dentry unused LRU is not updated even if lookup finds the required dentry
 * in there. It is updated in places such as prune_dcache, shrink_dcache_sb,
 * select_parent and __dget_locked. This laziness saves lookup from dcache_lock
 * acquisition.
 *
 * d_lookup() is protected against the concurrent renames in some unrelated
 * directory using the seqlockt_t rename_lock.
 */

//d_lookup(dentry,qstr) 在散列中找出以 dentry 作为父目录项 , 名称为 qstr 的目录项 
struct dentry * d_lookup(struct dentry * parent, struct qstr * name)
{
	struct dentry * dentry = NULL;
	unsigned long seq;

        do {
                seq = read_seqbegin(&rename_lock);
                dentry = __d_lookup(parent, name);
                if (dentry)
			break;
	} while (read_seqretry(&rename_lock, seq));
	return dentry;
}

struct dentry * __d_lookup(struct dentry * parent, struct qstr * name)
{
	unsigned int len = name->len;
	unsigned int hash = name->hash;
	const unsigned char *str = name->name;
	/* 
	 * 找到hash值所在的链表头,这个过程在__d_lockup函数中没有加锁,这就导致一个问题:
	 * 在获取到head之后,遍历hlist之前,这个dentry就被移动到别的hlist上去了.这就会
	 * 使查找失败,
	 */
	struct hlist_head *head = d_hash(parent,hash);
	struct dentry *found = NULL;
	struct hlist_node *node;
	struct dentry *dentry;

	/* 进行rcu之前, 必须rcu_read_lock,实际上就是关闭抢占 */
	rcu_read_lock();
	
	/* 
	 * dentry是需要使用的结构体指针,node是hash链表中的节点,head是链表头,
	 * d_hash是结构体中用于连入hash表的成员
	 */
	hlist_for_each_entry_rcu(dentry, node, head, d_hash) {
		struct qstr *qstr;

		/* 寻找hash值和name传进来的hash值一样的子目录 */
		if (dentry->d_name.hash != hash)
			continue;
		if (dentry->d_parent != parent)
			continue;

		spin_lock(&dentry->d_lock);

		/*
		 * Recheck the dentry after taking the lock - d_move may have
		 * changed things.  Don't bother checking the hash because we're
		 * about to compare the whole name anyway.
		 */
		if (dentry->d_parent != parent)
			goto next;

		/* non-existing due to RCU? */
		if (d_unhashed(dentry))
			goto next;

		/*
		 * It is safe to compare names since d_move() cannot
		 * change the qstr (protected by d_lock).
		 */
		qstr = &dentry->d_name;
		/* 如果父目录有比较函数 */
		if (parent->d_op && parent->d_op->d_compare) {
			if (parent->d_op->d_compare(parent, qstr, name))
				/* 一致返回0, 不一致返回非0 */
				goto next;
		} else {
			if (qstr->len != len)
				goto next;
			if (memcmp(qstr->name, str, len))
				goto next;
		}

		atomic_inc(&dentry->d_count);
		found = dentry;
		spin_unlock(&dentry->d_lock);
		break;
next:
		spin_unlock(&dentry->d_lock);
 	}
	/* 结束rcu保护 */
 	rcu_read_unlock();

 	return found;
}

/**
 * d_hash_and_lookup - hash the qstr then search for a dentry
 * @dir: Directory to search in
 * @name: qstr of name we wish to find
 *
 * On hash failure or on lookup failure NULL is returned.
 */
struct dentry *d_hash_and_lookup(struct dentry *dir, struct qstr *name)
{
	struct dentry *dentry = NULL;

	/*
	 * Check for a fs-specific hash function. Note that we must
	 * calculate the standard hash first, as the d_op->d_hash()
	 * routine may choose to leave the hash value unchanged.
	 */
	name->hash = full_name_hash(name->name, name->len);
	if (dir->d_op && dir->d_op->d_hash) {
		if (dir->d_op->d_hash(dir, name) < 0)
			goto out;
	}
	dentry = d_lookup(dir, name);
out:
	return dentry;
}

/**
 * d_validate - verify dentry provided from insecure source
 * @dentry: The dentry alleged to be valid child of @dparent
 * @dparent: The parent dentry (known to be valid)
 *
 * An insecure source has sent us a dentry, here we verify it and dget() it.
 * This is used by ncpfs in its readdir implementation.
 * Zero is returned in the dentry is invalid.
 */
 
int d_validate(struct dentry *dentry, struct dentry *dparent)
{
	struct hlist_head *base;
	struct hlist_node *lhp;

	/* Check whether the ptr might be valid at all.. */
	if (!kmem_ptr_validate(dentry_cache, dentry))
		goto out;

	if (dentry->d_parent != dparent)
		goto out;

	spin_lock(&dcache_lock);
	base = d_hash(dparent, dentry->d_name.hash);
	hlist_for_each(lhp,base) { 
		/* hlist_for_each_entry_rcu() not required for d_hash list
		 * as it is parsed under dcache_lock
		 */
		if (dentry == hlist_entry(lhp, struct dentry, d_hash)) {
			__dget_locked(dentry);
			spin_unlock(&dcache_lock);
			return 1;
		}
	}
	spin_unlock(&dcache_lock);
out:
	return 0;
}

/*
 * When a file is deleted, we have two options:
 * - turn this dentry into a negative dentry
 * - unhash this dentry and free it.
 *
 * Usually, we want to just turn this into
 * a negative dentry, but if anybody else is
 * currently using the dentry or the inode
 * we can't do that and we fall back on removing
 * it from the hash queues and waiting for
 * it to be deleted later when it has no users
 */
 
 /*当一个文件被删除时,我们有2种选择:*/
  /*- 将这个dentry变成negtive状态(没有具体inode)*/
  /*- 将这个dentry变成unhash状态(在dentry_hash中删除)并且释放.*/

  /*通常情况下,我们只是想要将dentry变成negtive状态,但是如果有其他*/
  /*地方真正使用这个dentry或者我们需要操作的inode不能被释放.我们只能*/
  /*退一步,暂时不删除这个dentry, 仅仅将其从hash队列中删除,当没有任何*/
  /*地方使用这个dentry时,再将其删除*/
 
/**
 * d_delete - delete a dentry
 * @dentry: The dentry to delete
 *
 * Turn the dentry into a negative dentry if possible, otherwise
 * remove it from the hash queues so it can be deleted later
 */
 
void d_delete(struct dentry * dentry)
{
	int isdir = 0;
	/*
	 * Are we the only user?
	 */
	spin_lock(&dcache_lock);
	spin_lock(&dentry->d_lock);
	isdir = S_ISDIR(dentry->d_inode->i_mode);
	if (atomic_read(&dentry->d_count) == 1) {
		//解除对inode的关联,并调用iput对inode的引用计数减一
		dentry_iput(dentry);
		fsnotify_nameremove(dentry, isdir);
		return;
	}

	//unhash this dentry,由于本函数调用了spin_lock(&dcache_lock),故此处不用d_drop
	if (!d_unhashed(dentry))
		__d_drop(dentry);

	spin_unlock(&dentry->d_lock);
	spin_unlock(&dcache_lock);

	fsnotify_nameremove(dentry, isdir);
}

static void __d_rehash(struct dentry * entry, struct hlist_head *list)
{

 	entry->d_flags &= ~DCACHE_UNHASHED;
 	hlist_add_head_rcu(&entry->d_hash, list);
}

static void _d_rehash(struct dentry * entry)
{
	__d_rehash(entry, d_hash(entry->d_parent, entry->d_name.hash));
}

/**
 * d_rehash	- add an entry back to the hash
 * @entry: dentry to add to the hash
 *
 * Adds a dentry to the hash according to its name.
 */

/* 将 dentry 加入散列表 */
void d_rehash(struct dentry * entry)
{
	spin_lock(&dcache_lock);
	spin_lock(&entry->d_lock);
	_d_rehash(entry);
	spin_unlock(&entry->d_lock);
	spin_unlock(&dcache_lock);
}

#define do_switch(x,y) do { \
	__typeof__ (x) __tmp = x; \
	x = y; y = __tmp; } while (0)

/*
 * When switching names, the actual string doesn't strictly have to
 * be preserved in the target - because we're dropping the target
 * anyway. As such, we can just do a simple memcpy() to copy over
 * the new name before we switch.
 *
 * Note that we have to be a lot more careful about getting the hash
 * switched - we have to switch the hash value properly even if it
 * then no longer matches the actual (corrupted) string of the target.
 * The hash value has to match the hash queue that the dentry is on..
 */
/*dentry有ext name的时候,文件名以ext name为准,没有ext name的时候,ext name指向其int name*/
static void switch_names(struct dentry *dentry, struct dentry *target)
{
	/* 通过d_alloc分配的dentry,其d_iname和dname.name是一样的 */
	if (dname_external(target)) {
		/* 
		 * 如果target的external name和internal name指向的空间不一致,
		 * 就说明存在external name.
		 */
		if (dname_external(dentry)) {
			/*
			 * Both external: swap the pointers
			 */
			//交换target和dentry的external name,dentry和target的internal name还是没有改变
			do_switch(target->d_name.name, dentry->d_name.name);
		} else {
			/*
			 * dentry:internal, target:external.  Steal target's
			 * storage and make target internal.
			 */
		    /*
 			 * 这个分支走完后,dentry的int name没有改变,其ext name指向target的ext name.
 			 * target的int name被赋值成dentry的int name.ext name指针指向自己的int name.
			 */
			memcpy(target->d_iname, dentry->d_name.name,
					dentry->d_name.len + 1);
			//dentry name指向target的external name的空间
			dentry->d_name.name = target->d_name.name;
			/* 
			 * target的external name和internal name使用同一空间,这时target就不再
			 * 具有external name空间.
			 */
			target->d_name.name = target->d_iname;
		}
	} else {
		if (dname_external(dentry)) {
			/*
			 * dentry:external, target:internal.  Give dentry's
			 * storage to target and make dentry internal
			 */
			/*
 			 * 这个分支走完后,target的int name没有改变,其ext name指向dentry的ext name.
 			 * dentry的int name被赋值成target的int name, ext name指针指向自己的int name.
			 */
			memcpy(dentry->d_iname, target->d_name.name,
					target->d_name.len + 1);
			target->d_name.name = dentry->d_name.name;
			dentry->d_name.name = dentry->d_iname;
		} else {
			/*
			 * Both are internal.  Just copy target to dentry
			 */
			/* 
			 * 执行完这个分支,dentry中的int(ext) name和target的ext(int) name一样了.
			 * 而target的ext(int) name并没有改变.
			 */
			memcpy(dentry->d_iname, target->d_name.name,
					target->d_name.len + 1);
			dentry->d_name.len = target->d_name.len;
			return;
		}
	}
	do_switch(dentry->d_name.len, target->d_name.len);
}

/*
 * We cannibalize "target" when moving dentry on top of it,
 * because it's going to be thrown away anyway. We could be more
 * polite about it, though.
 *
 * This forceful removal will result in ugly /proc output if
 * somebody holds a file open that got deleted due to a rename.
 * We could be nicer about the deleted file, and let it show
 * up under the name it had before it was deleted rather than
 * under the original name of the file that was moved on top of it.
 */
 
/*
 * d_move_locked - move a dentry
 * @dentry: entry to move
 * @target: new dentry
 *
 * Update the dcache to reflect the move of a file name. Negative
 * dcache entries should not be moved in this way.
 */
static void d_move_locked(struct dentry * dentry, struct dentry * target)
{
	struct hlist_head *list;

	if (!dentry->d_inode)
		printk(KERN_WARNING "VFS: moving negative dcache entry\n");

	write_seqlock(&rename_lock);
	/*
	 * XXXX: do we really need to take target->d_lock?
	 */
	if (target < dentry) {
		spin_lock(&target->d_lock);
		spin_lock_nested(&dentry->d_lock, DENTRY_D_LOCK_NESTED);
	} else {
		spin_lock(&dentry->d_lock);
		spin_lock_nested(&target->d_lock, DENTRY_D_LOCK_NESTED);
	}

	/* Move the dentry to the target hash queue, if on different bucket */
	if (d_unhashed(dentry))
		goto already_unhashed;

	/* 将dentry重其hash table中取下来 */
	hlist_del_rcu(&dentry->d_hash);

already_unhashed:
	/* 获取target的hlist链表头 */
	list = d_hash(target->d_parent, target->d_name.hash);
	/* 将dentry头插到target的hlist */
	__d_rehash(dentry, list);

	/* Unhash the target: dput() will then get rid of it */
	/* 将target从hlist中取下来 */
	__d_drop(target);

	/* 将dentry从其父目录的链表中取下来 */
	list_del(&dentry->d_u.d_child);
	/* 将target从其父目录的链表中取下来 */
	list_del(&target->d_u.d_child);

	/* Switch the names.. */
	/* 交换dentry和target的name string */
	switch_names(dentry, target);
	/* 
	 * 交换dentry和target的name hash,不管dentry是否有ext name,
	 * d_name.hash总是其name的hash.
	 * 
	 * 因为:
	 * 1.在存在ext name时,d_name.name指向name string的空间.
	 * 2.不存在ext name时,d_name.name指向d_iname.
	 */
	do_switch(dentry->d_name.hash, target->d_name.hash);

	/* ... and switch the parents */
	if (IS_ROOT(dentry)) {
		dentry->d_parent = target->d_parent;
		target->d_parent = target;
		INIT_LIST_HEAD(&target->d_u.d_child);
	} else {
		/* 交换dentry和target的parent */
		do_switch(dentry->d_parent, target->d_parent);

		/* And add them back to the (new) parent lists */
		list_add(&target->d_u.d_child, &target->d_parent->d_subdirs);
	}

	/* 将dentry加入target原来的parent的子文件链表中 */
	list_add(&dentry->d_u.d_child, &dentry->d_parent->d_subdirs);
	spin_unlock(&target->d_lock);
	fsnotify_d_move(dentry);
	spin_unlock(&dentry->d_lock);
	write_sequnlock(&rename_lock);
}

/**
 * d_move - move a dentry
 * @dentry: entry to move
 * @target: new dentry
 *
 * Update the dcache to reflect the move of a file name. Negative
 * dcache entries should not be moved in this way.
 */

void d_move(struct dentry * dentry, struct dentry * target)
{
	spin_lock(&dcache_lock);
	d_move_locked(dentry, target);
	spin_unlock(&dcache_lock);
}

/**
 * d_ancestor - search for an ancestor
 * @p1: ancestor dentry
 * @p2: child dentry
 *
 * Returns the ancestor dentry of p2 which is a child of p1, if p1 is
 * an ancestor of p2, else NULL.
 */
struct dentry *d_ancestor(struct dentry *p1, struct dentry *p2)
{
	struct dentry *p;

	for (p = p2; !IS_ROOT(p); p = p->d_parent) {
        //从child也就是p2向其parent遍历,如果p2的ancestor的parent是p1,就说明p1也是p2的ancestor
		if (p->d_parent == p1)
			return p;
	}
	return NULL;
}

/*
 * This helper attempts to cope with remotely renamed directories
 *
 * It assumes that the caller is already holding
 * dentry->d_parent->d_inode->i_mutex and the dcache_lock
 *
 * Note: If ever the locking in lock_rename() changes, then please
 * remember to update this too...
 */
static struct dentry *__d_unalias(struct dentry *dentry, struct dentry *alias)
	__releases(dcache_lock)
{
	struct mutex *m1 = NULL, *m2 = NULL;
	struct dentry *ret;

	/* If alias and dentry share a parent, then no extra locks required */
	if (alias->d_parent == dentry->d_parent)
		goto out_unalias;

	/* Check for loops */
	ret = ERR_PTR(-ELOOP);
	if (d_ancestor(alias, dentry))
		goto out_err;

	/* See lock_rename() */
	ret = ERR_PTR(-EBUSY);
	if (!mutex_trylock(&dentry->d_sb->s_vfs_rename_mutex))
		goto out_err;
	m1 = &dentry->d_sb->s_vfs_rename_mutex;
	if (!mutex_trylock(&alias->d_parent->d_inode->i_mutex))
		goto out_err;
	m2 = &alias->d_parent->d_inode->i_mutex;
out_unalias:
	d_move_locked(alias, dentry);
	ret = alias;
out_err:
	spin_unlock(&dcache_lock);
	if (m2)
		mutex_unlock(m2);
	if (m1)
		mutex_unlock(m1);
	return ret;
}

/*
 * Prepare an anonymous dentry for life in the superblock's dentry tree as a
 * named dentry in place of the dentry to be replaced.
 */
static void __d_materialise_dentry(struct dentry *dentry, struct dentry *anon)
{
	struct dentry *dparent, *aparent;

	switch_names(dentry, anon);
	do_switch(dentry->d_name.hash, anon->d_name.hash);

	dparent = dentry->d_parent;
	aparent = anon->d_parent;

	dentry->d_parent = (aparent == anon) ? dentry : aparent;
	list_del(&dentry->d_u.d_child);
	if (!IS_ROOT(dentry))
		list_add(&dentry->d_u.d_child, &dentry->d_parent->d_subdirs);
	else
		INIT_LIST_HEAD(&dentry->d_u.d_child);

	anon->d_parent = (dparent == dentry) ? anon : dparent;
	list_del(&anon->d_u.d_child);
	if (!IS_ROOT(anon))
		list_add(&anon->d_u.d_child, &anon->d_parent->d_subdirs);
	else
		INIT_LIST_HEAD(&anon->d_u.d_child);

	anon->d_flags &= ~DCACHE_DISCONNECTED;
}

/**
 * d_materialise_unique - introduce an inode into the tree
 * @dentry: candidate dentry
 * @inode: inode to bind to the dentry, to which aliases may be attached
 *
 * Introduces an dentry into the tree, substituting an extant disconnected
 * root directory alias in its place if there is one
 */
struct dentry *d_materialise_unique(struct dentry *dentry, struct inode *inode)
{
	struct dentry *actual;

	BUG_ON(!d_unhashed(dentry));

	spin_lock(&dcache_lock);

	if (!inode) {
		actual = dentry;
		__d_instantiate(dentry, NULL);
		goto found_lock;
	}

	if (S_ISDIR(inode->i_mode)) {
		struct dentry *alias;

		/* Does an aliased dentry already exist? */
		alias = __d_find_alias(inode, 0);
		if (alias) {
			actual = alias;
			/* Is this an anonymous mountpoint that we could splice
			 * into our tree? */
			if (IS_ROOT(alias)) {
				spin_lock(&alias->d_lock);
				__d_materialise_dentry(dentry, alias);
				__d_drop(alias);
				goto found;
			}
			/* Nope, but we must(!) avoid directory aliasing */
			actual = __d_unalias(dentry, alias);
			if (IS_ERR(actual))
				dput(alias);
			goto out_nolock;
		}
	}

	/* Add a unique reference */
	actual = __d_instantiate_unique(dentry, inode);
	if (!actual)
		actual = dentry;
	else if (unlikely(!d_unhashed(actual)))
		goto shouldnt_be_hashed;

found_lock:
	spin_lock(&actual->d_lock);
found:
	_d_rehash(actual);
	spin_unlock(&actual->d_lock);
	spin_unlock(&dcache_lock);
out_nolock:
	if (actual == dentry) {
		security_d_instantiate(dentry, inode);
		return NULL;
	}

	iput(inode);
	return actual;

shouldnt_be_hashed:
	spin_unlock(&dcache_lock);
	BUG();
}

static int prepend(char **buffer, int *buflen, const char *str, int namelen)
{
	*buflen -= namelen;
	if (*buflen < 0)
		return -ENAMETOOLONG;
	*buffer -= namelen;
	memcpy(*buffer, str, namelen);
	return 0;
}

static int prepend_name(char **buffer, int *buflen, struct qstr *name)
{
	return prepend(buffer, buflen, name->name, name->len);
}

/**
 * __d_path - return the path of a dentry
 * @path: the dentry/vfsmount to report
 * @root: root vfsmnt/dentry (may be modified by this function)
 * @buffer: buffer to return value in
 * @buflen: buffer length
 *
 * Convert a dentry into an ASCII path name. If the entry has been deleted
 * the string " (deleted)" is appended. Note that this is ambiguous.
 *
 * Returns the buffer or an error code if the path was too long.
 *
 * "buflen" should be positive. Caller holds the dcache_lock.
 *
 * If path is not reachable from the supplied root, then the value of
 * root is changed (without modifying refcounts).
 */
char *__d_path(const struct path *path, struct path *root,
	       char *buffer, int buflen)
{
	struct dentry *dentry = path->dentry;
	struct vfsmount *vfsmnt = path->mnt;
	char *end = buffer + buflen;
	char *retval;

	spin_lock(&vfsmount_lock);
	prepend(&end, &buflen, "\0", 1);
	if (!IS_ROOT(dentry) && d_unhashed(dentry) &&
		(prepend(&end, &buflen, " (deleted)", 10) != 0))
			goto Elong;

	if (buflen < 1)
		goto Elong;
	/* Get '/' right */
	retval = end-1;
	*retval = '/';

	for (;;) {
		struct dentry * parent;

		if (dentry == root->dentry && vfsmnt == root->mnt)
			break;
		if (dentry == vfsmnt->mnt_root || IS_ROOT(dentry)) {
			/* Global root? */
			if (vfsmnt->mnt_parent == vfsmnt) {
				goto global_root;
			}
			dentry = vfsmnt->mnt_mountpoint;
			vfsmnt = vfsmnt->mnt_parent;
			continue;
		}
		parent = dentry->d_parent;
		prefetch(parent);
		if ((prepend_name(&end, &buflen, &dentry->d_name) != 0) ||
		    (prepend(&end, &buflen, "/", 1) != 0))
			goto Elong;
		retval = end;
		dentry = parent;
	}

out:
	spin_unlock(&vfsmount_lock);
	return retval;

global_root:
	retval += 1;	/* hit the slash */
	if (prepend_name(&retval, &buflen, &dentry->d_name) != 0)
		goto Elong;
	root->mnt = vfsmnt;
	root->dentry = dentry;
	goto out;

Elong:
	retval = ERR_PTR(-ENAMETOOLONG);
	goto out;
}

/**
 * d_path - return the path of a dentry
 * @path: path to report
 * @buf: buffer to return value in
 * @buflen: buffer length
 *
 * Convert a dentry into an ASCII path name. If the entry has been deleted
 * the string " (deleted)" is appended. Note that this is ambiguous.
 *
 * Returns the buffer or an error code if the path was too long.
 *
 * "buflen" should be positive.
 */
char *d_path(const struct path *path, char *buf, int buflen)
{
	char *res;
	struct path root;
	struct path tmp;

	/*
	 * We have various synthetic filesystems that never get mounted.  On
	 * these filesystems dentries are never used for lookup purposes, and
	 * thus don't need to be hashed.  They also don't need a name until a
	 * user wants to identify the object in /proc/pid/fd/.  The little hack
	 * below allows us to generate a name for these objects on demand:
	 */
	if (path->dentry->d_op && path->dentry->d_op->d_dname)
		return path->dentry->d_op->d_dname(path->dentry, buf, buflen);

	read_lock(&current->fs->lock);
	root = current->fs->root;
	path_get(&root);
	read_unlock(&current->fs->lock);
	spin_lock(&dcache_lock);
	tmp = root;
	res = __d_path(path, &tmp, buf, buflen);
	spin_unlock(&dcache_lock);
	path_put(&root);
	return res;
}

/*
 * Helper function for dentry_operations.d_dname() members
 */
char *dynamic_dname(struct dentry *dentry, char *buffer, int buflen,
			const char *fmt, ...)
{
	va_list args;
	char temp[64];
	int sz;

	va_start(args, fmt);
	sz = vsnprintf(temp, sizeof(temp), fmt, args) + 1;
	va_end(args);

	if (sz > sizeof(temp) || sz > buflen)
		return ERR_PTR(-ENAMETOOLONG);

	buffer += buflen - sz;
	return memcpy(buffer, temp, sz);
}

/*
 * Write full pathname from the root of the filesystem into the buffer.
 */
char *dentry_path(struct dentry *dentry, char *buf, int buflen)
{
	char *end = buf + buflen;
	char *retval;

	spin_lock(&dcache_lock);
	prepend(&end, &buflen, "\0", 1);
	if (!IS_ROOT(dentry) && d_unhashed(dentry) &&
		(prepend(&end, &buflen, "//deleted", 9) != 0))
			goto Elong;
	if (buflen < 1)
		goto Elong;
	/* Get '/' right */
	retval = end-1;
	*retval = '/';

	while (!IS_ROOT(dentry)) {
		struct dentry *parent = dentry->d_parent;

		prefetch(parent);
		if ((prepend_name(&end, &buflen, &dentry->d_name) != 0) ||
		    (prepend(&end, &buflen, "/", 1) != 0))
			goto Elong;

		retval = end;
		dentry = parent;
	}
	spin_unlock(&dcache_lock);
	return retval;
Elong:
	spin_unlock(&dcache_lock);
	return ERR_PTR(-ENAMETOOLONG);
}

/*
 * NOTE! The user-level library version returns a
 * character pointer. The kernel system call just
 * returns the length of the buffer filled (which
 * includes the ending '\0' character), or a negative
 * error value. So libc would do something like
 *
 *	char *getcwd(char * buf, size_t size)
 *	{
 *		int retval;
 *
 *		retval = sys_getcwd(buf, size);
 *		if (retval >= 0)
 *			return buf;
 *		errno = -retval;
 *		return NULL;
 *	}
 */
SYSCALL_DEFINE2(getcwd, char __user *, buf, unsigned long, size)
{
	int error;
	struct path pwd, root;
	char *page = (char *) __get_free_page(GFP_USER);

	if (!page)
		return -ENOMEM;

	read_lock(&current->fs->lock);
	pwd = current->fs->pwd;
	path_get(&pwd);
	root = current->fs->root;
	path_get(&root);
	read_unlock(&current->fs->lock);

	error = -ENOENT;
	/* Has the current directory has been unlinked? */
	spin_lock(&dcache_lock);
	if (IS_ROOT(pwd.dentry) || !d_unhashed(pwd.dentry)) {
		unsigned long len;
		struct path tmp = root;
		char * cwd;

		cwd = __d_path(&pwd, &tmp, page, PAGE_SIZE);
		spin_unlock(&dcache_lock);

		error = PTR_ERR(cwd);
		if (IS_ERR(cwd))
			goto out;

		error = -ERANGE;
		len = PAGE_SIZE + page - cwd;
		if (len <= size) {
			error = len;
			if (copy_to_user(buf, cwd, len))
				error = -EFAULT;
		}
	} else
		spin_unlock(&dcache_lock);

out:
	path_put(&pwd);
	path_put(&root);
	free_page((unsigned long) page);
	return error;
}

/*
 * Test whether new_dentry is a subdirectory of old_dentry.
 *
 * Trivially implemented using the dcache structure
 */

/**
 * is_subdir - is new dentry a subdirectory of old_dentry
 * @new_dentry: new dentry
 * @old_dentry: old dentry
 *
 * Returns 1 if new_dentry is a subdirectory of the parent (at any depth).
 * Returns 0 otherwise.
 * Caller must ensure that "new_dentry" is pinned before calling is_subdir()
 */
  
int is_subdir(struct dentry *new_dentry, struct dentry *old_dentry)
{
	int result;
	unsigned long seq;

	/* FIXME: This is old behavior, needed? Please check callers. */
	if (new_dentry == old_dentry)
        //new_dentry和old_dentry相同,也表示new_dentry是old_dentry的子目录
		return 1;

	/*
	 * Need rcu_readlock to protect against the d_parent trashing
	 * due to d_move
	 */
	rcu_read_lock();
	do {
		/* for restarting inner loop in case of seq retry */
		seq = read_seqbegin(&rename_lock);
		if (d_ancestor(old_dentry, new_dentry))
            //old_dentry是new_dentry的ancestor
		    //如果new_dentry向上追溯使new_dentry == old_dentry,说明new_dentry是old_dentry的子目录
			result = 1;
		else
			result = 0;
	} while (read_seqretry(&rename_lock, seq));
	rcu_read_unlock();

	return result;
}

//将文件系统中的所有dentry的引用计数减1,这是因为初始化dentry的时候d_count=1
//atomic_dec all the dentries
void d_genocide(struct dentry *root)
{
	struct dentry *this_parent = root;
	struct list_head *next;

	spin_lock(&dcache_lock);
repeat:
	//get child
	next = this_parent->d_subdirs.next;
resume:
	while (next != &this_parent->d_subdirs) {
		struct list_head *tmp = next;
		struct dentry *dentry = list_entry(tmp, struct dentry, d_u.d_child);
		next = tmp->next;
		if (d_unhashed(dentry)||!dentry->d_inode)
			continue;
		if (!list_empty(&dentry->d_subdirs)) {
			//向目录树的子节点移动
			this_parent = dentry;
			goto repeat;
		}
		//对无子节点的dentry引用计数减一
		atomic_dec(&dentry->d_count);
	}
	//子节点处理完成,父节点移向同级节点
	if (this_parent != root) {
		next = this_parent->d_u.d_child.next;
		//对有子节点的dentry引用计数减一
		atomic_dec(&this_parent->d_count);
		this_parent = this_parent->d_parent;
		goto resume;
	}
	spin_unlock(&dcache_lock);
}

/**
 * find_inode_number - check for dentry with name
 * @dir: directory to check
 * @name: Name to find.
 *
 * Check whether a dentry already exists for the given name,
 * and return the inode number if it has an inode. Otherwise
 * 0 is returned.
 *
 * This routine is used to post-process directory listings for
 * filesystems using synthetic inode numbers, and is necessary
 * to keep getcwd() working.
 */
 
ino_t find_inode_number(struct dentry *dir, struct qstr *name)
{
	struct dentry * dentry;
	ino_t ino = 0;

	dentry = d_hash_and_lookup(dir, name);
	if (dentry) {
		if (dentry->d_inode)
			ino = dentry->d_inode->i_ino;
		dput(dentry);
	}
	return ino;
}

static __initdata unsigned long dhash_entries;
static int __init set_dhash_entries(char *str)
{
	if (!str)
		return 0;
	dhash_entries = simple_strtoul(str, &str, 0);
	return 1;
}
__setup("dhash_entries=", set_dhash_entries);

static void __init dcache_init_early(void)
{
	int loop;

	/* If hashes are distributed across NUMA nodes, defer
	 * hash allocation until vmalloc space is available.
	 */
	if (hashdist)
		return;

	dentry_hashtable =
		alloc_large_system_hash("Dentry cache",
					sizeof(struct hlist_head),
					dhash_entries,
					13,
					HASH_EARLY,
					&d_hash_shift,
					&d_hash_mask,
					0);

	//把dentry_hashtable中所有的backet都初始化
	for (loop = 0; loop < (1 << d_hash_shift); loop++)
		INIT_HLIST_HEAD(&dentry_hashtable[loop]);
}

static void __init dcache_init(void)
{
	int loop;

	/* 
	 * A constructor could be added for stable state like the lists,
	 * but it is probably not worth it because of the cache nature
	 * of the dcache. 
	 */
	dentry_cache = KMEM_CACHE(dentry,
		SLAB_RECLAIM_ACCOUNT|SLAB_PANIC|SLAB_MEM_SPREAD);
	
	register_shrinker(&dcache_shrinker);

	/* Hash may have been set up in dcache_init_early */
	if (!hashdist)
		return;

	dentry_hashtable =
		alloc_large_system_hash("Dentry cache",
					sizeof(struct hlist_head),
					dhash_entries,
					13,
					0,
					&d_hash_shift,
					&d_hash_mask,
					0);

	for (loop = 0; loop < (1 << d_hash_shift); loop++)
		INIT_HLIST_HEAD(&dentry_hashtable[loop]);
}

/* SLAB cache for __getname() consumers */
struct kmem_cache *names_cachep __read_mostly;

/* SLAB cache for file structures */
struct kmem_cache *filp_cachep __read_mostly;

EXPORT_SYMBOL(d_genocide);

void __init vfs_caches_init_early(void)
{
	dcache_init_early();
	inode_init_early();
}

void __init vfs_caches_init(unsigned long mempages)
{
	unsigned long reserve;

	/* Base hash sizes on available memory, with a reserve equal to
           150% of current kernel size */

	reserve = min((mempages - nr_free_pages()) * 3/2, mempages - 1);
	mempages -= reserve;

	names_cachep = kmem_cache_create("names_cache", PATH_MAX, 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC, NULL);

	filp_cachep = kmem_cache_create("filp", sizeof(struct file), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC, NULL);

	dcache_init();
	inode_init();
	files_init(mempages);
	mnt_init();
	bdev_cache_init();
	chrdev_init();
}

EXPORT_SYMBOL(d_alloc);
EXPORT_SYMBOL(d_alloc_root);
EXPORT_SYMBOL(d_delete);
EXPORT_SYMBOL(d_find_alias);
EXPORT_SYMBOL(d_instantiate);
EXPORT_SYMBOL(d_invalidate);
EXPORT_SYMBOL(d_lookup);
EXPORT_SYMBOL(d_move);
EXPORT_SYMBOL_GPL(d_materialise_unique);
EXPORT_SYMBOL(d_path);
EXPORT_SYMBOL(d_prune_aliases);
EXPORT_SYMBOL(d_rehash);
EXPORT_SYMBOL(d_splice_alias);
EXPORT_SYMBOL(d_add_ci);
EXPORT_SYMBOL(d_validate);
EXPORT_SYMBOL(dget_locked);
EXPORT_SYMBOL(dput);
EXPORT_SYMBOL(find_inode_number);
EXPORT_SYMBOL(have_submounts);
EXPORT_SYMBOL(names_cachep);
EXPORT_SYMBOL(shrink_dcache_parent);
EXPORT_SYMBOL(shrink_dcache_sb);
