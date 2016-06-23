﻿/*
 * linux/fs/inode.c
 *
 * (C) 1997 Linus Torvalds
 */

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/dcache.h>
#include <linux/init.h>
#include <linux/quotaops.h>
#include <linux/slab.h>
#include <linux/writeback.h>
#include <linux/module.h>
#include <linux/backing-dev.h>
#include <linux/wait.h>
#include <linux/hash.h>
#include <linux/swap.h>
#include <linux/security.h>
#include <linux/pagemap.h>
#include <linux/cdev.h>
#include <linux/bootmem.h>
#include <linux/inotify.h>
#include <linux/mount.h>

/*
 * This is needed for the following functions:
 *  - inode_has_buffers
 *  - invalidate_inode_buffers
 *  - invalidate_bdev
 *
 * FIXME: remove all knowledge of the buffer layer from this file
 */
#include <linux/buffer_head.h>

/*
 * New inode.c implementation.
 *
 * This implementation has the basic premise of trying
 * to be extremely low-overhead and SMP-safe, yet be
 * simple enough to be "obviously correct".
 *
 * Famous last words.
 */

/* inode dynamic allocation 1999, Andrea Arcangeli <andrea@suse.de> */

/* #define INODE_PARANOIA 1 */
/* #define INODE_DEBUG 1 */

/*
 * Inode lookup is no longer as critical as it used to be:
 * most of the lookups are going to be through the dcache.
 */
#define I_HASHBITS	i_hash_shift
#define I_HASHMASK	i_hash_mask

//__read_mostly将一些数据加载到一个段内
static unsigned int i_hash_mask __read_mostly;
static unsigned int i_hash_shift __read_mostly;

/*
 * Each inode can be on two separate lists. One is
 * the hash list of the inode, used for lookups. The
 * other linked list is the "type" list:
 *  "in_use" - valid inode, i_count > 0, i_nlink > 0
 *  "dirty"  - as "in_use" but also dirty
 *  "unused" - valid inode, i_count = 0
 *
 * A "dirty" list is maintained for each super block,
 * allowing for low-overhead inode sync() operations.
 */

//inode_in_use表示正在使用,并且同步过的inode
LIST_HEAD(inode_in_use);
//inode_unused表示空闲的,未使用过的inode
LIST_HEAD(inode_unused);
//backet的位置根据sb和hash生成 
static struct hlist_head *inode_hashtable __read_mostly;

/*
 * A simple spinlock to protect the list manipulations.
 *
 * NOTE! You also have to own the lock if you change
 * the i_state of an inode while it is in use..
 */
DEFINE_SPINLOCK(inode_lock);

/*
 * iprune_mutex provides exclusion between the kswapd or try_to_free_pages
 * icache shrinking path, and the umount path.  Without this exclusion,
 * by the time prune_icache calls iput for the inode whose pages it has
 * been invalidating, or by the time it calls clear_inode & destroy_inode
 * from its final dispose_list, the struct super_block they refer to
 * (for inode->i_sb->s_op) may already have been freed and reused.
 */
static DEFINE_MUTEX(iprune_mutex);

/*
 * Statistics gathering..
 */
 //用于统计系统中的inode个数,为使用的个数
struct inodes_stat_t inodes_stat;

static struct kmem_cache * inode_cachep __read_mostly;

static void wake_up_inode(struct inode *inode)
{
	/*
	 * Prevent speculative execution through spin_unlock(&inode_lock);
	 */
	smp_mb();
	wake_up_bit(&inode->i_state, __I_LOCK);
}

static struct inode *alloc_inode(struct super_block *sb)
{
	static const struct address_space_operations empty_aops;
	static struct inode_operations empty_iops;
	static const struct file_operations empty_fops;
	struct inode *inode;

	if (sb->s_op->alloc_inode)
		/* 对于bdev伪文件系统来说,将会调用bdev_alloc_inode分配一个struct bdev_inode类型的对象 */
		inode = sb->s_op->alloc_inode(sb);
	else
		inode = (struct inode *) kmem_cache_alloc(inode_cachep, GFP_KERNEL);

	if (inode) {
		/* inode->i_mapping最终会指向&inode->i_data */
		struct address_space * const mapping = &inode->i_data;

		/* inode的i_sb指向sb */
		inode->i_sb = sb;
		inode->i_blkbits = sb->s_blocksize_bits;
		inode->i_flags = 0;
		atomic_set(&inode->i_count, 1);
		inode->i_op = &empty_iops;
		inode->i_fop = &empty_fops;
		inode->i_nlink = 1;
		atomic_set(&inode->i_writecount, 0);
		inode->i_size = 0;
		inode->i_blocks = 0;
		inode->i_bytes = 0;
		inode->i_generation = 0;
#ifdef CONFIG_QUOTA
		memset(&inode->i_dquot, 0, sizeof(inode->i_dquot));
#endif
		inode->i_pipe = NULL;
		inode->i_bdev = NULL;
		inode->i_cdev = NULL;
		inode->i_rdev = 0;
		inode->dirtied_when = 0;
		if (security_inode_alloc(inode)) {
			if (inode->i_sb->s_op->destroy_inode)
				inode->i_sb->s_op->destroy_inode(inode);
			else
				kmem_cache_free(inode_cachep, (inode));
			return NULL;
		}

		spin_lock_init(&inode->i_lock);
		lockdep_set_class(&inode->i_lock, &sb->s_type->i_lock_key);

		mutex_init(&inode->i_mutex);
		lockdep_set_class(&inode->i_mutex, &sb->s_type->i_mutex_key);

		init_rwsem(&inode->i_alloc_sem);
		lockdep_set_class(&inode->i_alloc_sem, &sb->s_type->i_alloc_sem_key);

		mapping->a_ops = &empty_aops;
 		mapping->host = inode;
		mapping->flags = 0;
		mapping_set_gfp_mask(mapping, GFP_HIGHUSER_PAGECACHE);
		/* inode间接块使用的mapping为NULL */
		mapping->assoc_mapping = NULL;
		mapping->backing_dev_info = &default_backing_dev_info;
		mapping->writeback_index = 0;

		/*
		 * If the block_device provides a backing_dev_info for client
		 * inodes then use that.  Otherwise the inode share the bdev's
		 * backing_dev_info.
		 */
        /*
         * 如果block_device给inode提供了一个backing_dev_info,那就用这个backing_dev_info,
         * 否则这个使用bdev的backing_dev_info。
        */
		if (sb->s_bdev) {
			struct backing_dev_info *bdi;

			bdi = sb->s_bdev->bd_inode_backing_dev_info;
			if (!bdi)
				bdi = sb->s_bdev->bd_inode->i_mapping->backing_dev_info;
			mapping->backing_dev_info = bdi;
		}
		inode->i_private = NULL;
		inode->i_mapping = mapping;
	}
	return inode;
}

void destroy_inode(struct inode *inode) 
{
	BUG_ON(inode_has_buffers(inode));
	security_inode_free(inode);
	if (inode->i_sb->s_op->destroy_inode)
		inode->i_sb->s_op->destroy_inode(inode);
	else
		kmem_cache_free(inode_cachep, (inode));
}


/*
 * These are initializations that only need to be done
 * once, because the fields are idempotent across use
 * of the inode, so let the slab aware of that.
 */
void inode_init_once(struct inode *inode)
{
	memset(inode, 0, sizeof(*inode));
	INIT_HLIST_NODE(&inode->i_hash);
	INIT_LIST_HEAD(&inode->i_dentry);
	INIT_LIST_HEAD(&inode->i_devices);
	INIT_RADIX_TREE(&inode->i_data.page_tree, GFP_ATOMIC);
	spin_lock_init(&inode->i_data.tree_lock);
	spin_lock_init(&inode->i_data.i_mmap_lock);
	/* private_list并不由private_lock保护,而是由其后备设备的地址空间中的private_lock保护 */
	INIT_LIST_HEAD(&inode->i_data.private_list);
	spin_lock_init(&inode->i_data.private_lock);
	INIT_RAW_PRIO_TREE_ROOT(&inode->i_data.i_mmap);
	INIT_LIST_HEAD(&inode->i_data.i_mmap_nonlinear);
	i_size_ordered_init(inode);
#ifdef CONFIG_INOTIFY
	INIT_LIST_HEAD(&inode->inotify_watches);
	mutex_init(&inode->inotify_mutex);
#endif
}

EXPORT_SYMBOL(inode_init_once);

static void init_once(void *foo)
{
	struct inode * inode = (struct inode *) foo;

	inode_init_once(inode);
}

/*
 * inode_lock must be held
 */
void __iget(struct inode * inode)
{
	/* 如果已经有进程引用了此inode, 仅仅对计数器加1 */
	if (atomic_read(&inode->i_count)) {
		atomic_inc(&inode->i_count);
		return;
	}
	atomic_inc(&inode->i_count);
	/* 
	 * 如果i_count为0,并且inode没有标记为dirty或没有标记为I_SYNC,将其移动到inode_in_use队列中.
	 */
	if (!(inode->i_state & (I_DIRTY|I_SYNC)))
		list_move(&inode->i_list, &inode_in_use);
	/* 更新inode的统计计数 */
	inodes_stat.nr_unused--;
}

/**
 * clear_inode - clear an inode
 * @inode: inode to clear
 *
 * This is called by the filesystem to tell us
 * that the inode is no longer useful. We just
 * terminate it with extreme prejudice.
 */
void clear_inode(struct inode *inode)
{
	might_sleep();
	/* 释放inode->i_data数据 */
	invalidate_inode_buffers(inode);
       
	BUG_ON(inode->i_data.nrpages);
	BUG_ON(!(inode->i_state & I_FREEING));
	BUG_ON(inode->i_state & I_CLEAR);
	/* 如果有其他进程操作此inode,就等待其完成 */
	inode_sync_wait(inode);
	DQUOT_DROP(inode);
	/* 调用具体文件系统的clear_inode */
	if (inode->i_sb->s_op->clear_inode)
		inode->i_sb->s_op->clear_inode(inode);
	/* 如果此inode是一个block device */
	if (S_ISBLK(inode->i_mode) && inode->i_bdev)
		bd_forget(inode);
	/* 如果此inode是一个char device */
	if (S_ISCHR(inode->i_mode) && inode->i_cdev)
		cd_forget(inode);
	inode->i_state = I_CLEAR;
}

EXPORT_SYMBOL(clear_inode);

/*
 * dispose_list - dispose of the contents of a local list
 * @head: the head of the list to free
 *
 * Dispose-list gets a local list with local inodes in it, so it doesn't
 * need to worry about list corruption and SMP locks.
 */
static void dispose_list(struct list_head *head)
{
	int nr_disposed = 0;

	while (!list_empty(head)) {
		struct inode *inode;

		//get the first inode
		inode = list_first_entry(head, struct inode, i_list);
		//remove the inode from inode_in_use
		list_del(&inode->i_list);

		if (inode->i_data.nrpages)
			//清除inode->i_data数据
			truncate_inode_pages(&inode->i_data, 0);
		clear_inode(inode);

		spin_lock(&inode_lock);
		//remove the inode from hash table
		hlist_del_init(&inode->i_hash);
		//从super_block->s_inodes为表头的链表中删除
		list_del_init(&inode->i_sb_list);
		spin_unlock(&inode_lock);

		//通知资源删除完成,就剩一个空的inode
		wake_up_inode(inode);
		destroy_inode(inode);
		nr_disposed++;
	}
	spin_lock(&inode_lock);
	//decrease the number of inode in system
	//inode的统计技术减去刚才释放的个数
	inodes_stat.nr_inodes -= nr_disposed;
	spin_unlock(&inode_lock);
}

/*
 * Invalidate all inodes for a device.
 */
 /*
  * 返回正在使用的inode的个数,为使用的inode保存在dispose为head的链表中.
  * 并将inode->i_state置为I_FREEING.
  */
static int invalidate_list(struct list_head *head, struct list_head *dispose)
{
	struct list_head *next;
	int busy = 0, count = 0;

	next = head->next;
	for (;;) {
		struct list_head * tmp = next;
		struct inode * inode;

		/*
		 * We can reschedule here without worrying about the list's
		 * consistency because the per-sb list of inodes must not
		 * change during umount anymore, and because iprune_mutex keeps
		 * shrink_icache_memory() away.
		 */
		cond_resched_lock(&inode_lock);

		next = next->next;
		/* 如果链表中有没inode了 */
		if (tmp == head)
			break;
		inode = list_entry(tmp, struct inode, i_sb_list);
		/* 回收inode的buffer */
		invalidate_inode_buffers(inode);
		//if there is no refcount in inode
		if (!atomic_read(&inode->i_count)) {
			list_move(&inode->i_list, dispose);
			inode->i_state |= I_FREEING;
			count++;
			continue;
		}
		//如果inode的引用计数大于0
		busy = 1;
	}
	/* only unused inodes may be cached with i_count zero */
	inodes_stat.nr_unused -= count;
	return busy;
}

/**
 *	invalidate_inodes	- discard the inodes on a device
 *	@sb: superblock
 *
 *	Discard all of the inodes for a given superblock. If the discard
 *	fails because there are busy inodes then a non zero value is returned.
 *	If the discard is successful all the inodes have been discarded.
 */
int invalidate_inodes(struct super_block * sb)
{
	int busy;
	LIST_HEAD(throw_away);

	mutex_lock(&iprune_mutex);
	spin_lock(&inode_lock);
	inotify_unmount_inodes(&sb->s_inodes);
	/* 将引用计数为0的inode加入throw_away链表 */
	busy = invalidate_list(&sb->s_inodes, &throw_away);
	spin_unlock(&inode_lock);

	//清除throw_away链表中的inode
	dispose_list(&throw_away);
	mutex_unlock(&iprune_mutex);

	return busy;
}

EXPORT_SYMBOL(invalidate_inodes);

/*
 *inode能够移动到inode_unse的条件有：
 *	1.i_state必须为0
 *	2.i_count必须为0
 *	3.inode没有与其关联的buffer
 *	4.inode的地址空间中没有与其关联的page
*/
static int can_unuse(struct inode *inode)
{
	if (inode->i_state)
		return 0;
	if (inode_has_buffers(inode))
		return 0;
	if (atomic_read(&inode->i_count))
		return 0;
	if (inode->i_data.nrpages)
		return 0;
	return 1;
}

/*
 * Scan `goal' inodes on the unused list for freeable ones. They are moved to
 * a temporary list and then are freed outside inode_lock by dispose_list().
 *
 * Any inodes which are pinned purely because of attached pagecache have their
 * pagecache removed.  We expect the final iput() on that inode to add it to
 * the front of the inode_unused list.  So look for it there and if the
 * inode is still freeable, proceed.  The right inode is found 99.9% of the
 * time in testing on a 4-way.
 *
 * If the inode has metadata buffers attached to mapping->private_list then
 * try to remove them.
 */
static void prune_icache(int nr_to_scan)
{
	LIST_HEAD(freeable);
	int nr_pruned = 0;
	int nr_scanned;
	unsigned long reap = 0;

	mutex_lock(&iprune_mutex);
	spin_lock(&inode_lock);
	for (nr_scanned = 0; nr_scanned < nr_to_scan; nr_scanned++) {
		struct inode *inode;

		if (list_empty(&inode_unused))
			break;

		//从inode_unused链表结尾开始遍历
		inode = list_entry(inode_unused.prev, struct inode, i_list);

		if (inode->i_state || atomic_read(&inode->i_count)) {
			/*
			 *如果inode->i_state不为0,或者inode->i_count>0, 
			 *就将这个inode移动到inode_unused的首部
			*/
			list_move(&inode->i_list, &inode_unused);
			continue;
		}
		if (inode_has_buffers(inode) || inode->i_data.nrpages) {
			//如果inode中关联的bh链表不为空，或者inode的address_space中关联的页面
			__iget(inode);
			spin_unlock(&inode_lock);
			if (remove_inode_buffers(inode))
				//与inode关联的所有bh全部移除后，就使与inode相关的pages无效
				reap += invalidate_mapping_pages(&inode->i_data,
								0, -1);
			iput(inode);
			spin_lock(&inode_lock);

			//这里不太明白为什么要这样判断
			if (inode != list_entry(inode_unused.next,
						struct inode, i_list))
				continue;	/* wrong inode or list_empty */
			if (!can_unuse(inode))
				continue;
		}
		list_move(&inode->i_list, &freeable);
		inode->i_state |= I_FREEING;
		nr_pruned++;
	}
	inodes_stat.nr_unused -= nr_pruned;
	if (current_is_kswapd())
		__count_vm_events(KSWAPD_INODESTEAL, reap);
	else
		__count_vm_events(PGINODESTEAL, reap);
	spin_unlock(&inode_lock);

	dispose_list(&freeable);
	mutex_unlock(&iprune_mutex);
}

/*
 * shrink_icache_memory() will attempt to reclaim some unused inodes.  Here,
 * "unused" means that no dentries are referring to the inodes: the files are
 * not open and the dcache references to those inodes have already been
 * reclaimed.
 *
 * This function is passed the number of inodes to scan, and it returns the
 * total number of remaining possibly-reclaimable inodes.
 */
static int shrink_icache_memory(int nr, gfp_t gfp_mask)
{
	if (nr) {
		/*
		 * Nasty deadlock avoidance.  We may hold various FS locks,
		 * and we don't want to recurse into the FS that called us
		 * in clear_inode() and friends..
	 	 */
		if (!(gfp_mask & __GFP_FS))
			return -1;
		prune_icache(nr);
	}
	return (inodes_stat.nr_unused / 100) * sysctl_vfs_cache_pressure;
}

static struct shrinker icache_shrinker = {
	.shrink = shrink_icache_memory,
	.seeks = DEFAULT_SEEKS,
};

static void __wait_on_freeing_inode(struct inode *inode);
/*
 * Called with the inode lock held.
 * NOTE: we are not increasing the inode-refcount, you must call __iget()
 * by hand after calling find_inode now! This simplifies iunique and won't
 * add any additional branch in the common code.
 */
static struct inode * find_inode(struct super_block * sb, struct hlist_head *head, int (*test)(struct inode *, void *), void *data)
{
	struct hlist_node *node;
	struct inode * inode = NULL;

repeat:
	hlist_for_each_entry(inode, node, head, i_hash) {
		if (inode->i_sb != sb)
			continue;
		if (!test(inode, data))
			continue;
		//如果inode将要被销毁,等待inode释放完毕
		if (inode->i_state & (I_FREEING|I_CLEAR|I_WILL_FREE)) {
			__wait_on_freeing_inode(inode);
			goto repeat;
		}
		break;
	}
	return node ? inode : NULL;
}

/*
 * find_inode_fast is the fast path version of find_inode, see the comment at
 * iget_locked for details.
 */
static struct inode * find_inode_fast(struct super_block * sb, struct hlist_head *head, unsigned long ino)
{
	struct hlist_node *node;
	struct inode * inode = NULL;

repeat:
	hlist_for_each_entry(inode, node, head, i_hash) {
		if (inode->i_ino != ino)
			continue;
		/* 不同文件系统的inode->i_ino可能会相同,还需要判断sb */
		if (inode->i_sb != sb)
			continue;
		if (inode->i_state & (I_FREEING|I_CLEAR|I_WILL_FREE)) {
			__wait_on_freeing_inode(inode);
			goto repeat;
		}
		break;
	}
	return node ? inode : NULL;
}

/**
 *	new_inode 	- obtain an inode
 *	@sb: superblock
 *
 *	Allocates a new inode for given superblock. The default gfp_mask
 *	for allocations related to inode->i_mapping is GFP_HIGHUSER_PAGECACHE.
 *	If HIGHMEM pages are unsuitable or it is known that pages allocated
 *	for the page cache are not reclaimable or migratable,
 *	mapping_set_gfp_mask() must be called with suitable flags on the
 *	newly created inode's mapping
 *
 */
/* 多个文件系统都可能调用此函数 */
struct inode *new_inode(struct super_block *sb)
{
	/* static 标记的局部变量会一直递增 */
	/*
	 * On a 32bit, non LFS stat() call, glibc will generate an EOVERFLOW
	 * error if st_ino won't fit in target struct field. Use 32bit counter
	 * here to attempt to avoid that.
	 */
	static unsigned int last_ino;
	struct inode * inode;

	spin_lock_prefetch(&inode_lock);
	
	inode = alloc_inode(sb);
	if (inode) {
		spin_lock(&inode_lock);
		inodes_stat.nr_inodes++;
		/* 将刚刚分配的inode加入使用中的链表 */
		list_add(&inode->i_list, &inode_in_use);
		/* 将刚刚分配的inode加入sb的inode链表中 */
		list_add(&inode->i_sb_list, &sb->s_inodes);
		inode->i_ino = ++last_ino;
		inode->i_state = 0;
		spin_unlock(&inode_lock);
	}
	return inode;
}

EXPORT_SYMBOL(new_inode);

void unlock_new_inode(struct inode *inode)
{
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	if (inode->i_mode & S_IFDIR) {
		struct file_system_type *type = inode->i_sb->s_type;

		/*
		 * ensure nobody is actually holding i_mutex
		 */
		mutex_destroy(&inode->i_mutex);
		mutex_init(&inode->i_mutex);
		lockdep_set_class(&inode->i_mutex, &type->i_mutex_dir_key);
	}
#endif
	/*
	 * This is special!  We do not need the spinlock
	 * when clearing I_LOCK, because we're guaranteed
	 * that nobody else tries to do anything about the
	 * state of the inode when it is locked, as we
	 * just created it (so there can be no old holders
	 * that haven't tested I_LOCK).
	 */
	inode->i_state &= ~(I_LOCK|I_NEW);
	wake_up_inode(inode);
}

EXPORT_SYMBOL(unlock_new_inode);

/*
 * This is called without the inode lock held.. Be careful.
 *
 * We no longer cache the sb_flags in i_flags - see fs.h
 *	-- rmk@arm.uk.linux.org
 */
static struct inode * get_new_inode(struct super_block *sb, struct hlist_head *head, int (*test)(struct inode *, void *), int (*set)(struct inode *, void *), void *data)
{
	struct inode * inode;

	inode = alloc_inode(sb);
	if (inode) {
		struct inode * old;

		spin_lock(&inode_lock);
		/* We released the lock, so.. */
		//根据sb和data在高速缓存中找到inode 
		old = find_inode(sb, head, test, data);
		if (!old) {
			if (set(inode, data))
				/*
				 * 在缓存中没找到inode, 才使用刚才分配的inode 
				 * 搜索时匹配的条件是 BDEV_I(inode)->bdev.bd_dev == *(dev_t *)data,
				 * 加入hash table之前,需要设置BDEV_I(inode)->bdev.bd_dev = *(dev_t *)data
				*/  
				goto set_failed;

			inodes_stat.nr_inodes++;
			list_add(&inode->i_list, &inode_in_use);
			list_add(&inode->i_sb_list, &sb->s_inodes);
			hlist_add_head(&inode->i_hash, head);
			//inode还不能使用
			inode->i_state = I_LOCK|I_NEW;
			spin_unlock(&inode_lock);

			/* Return the locked inode with I_NEW set, the
			 * caller is responsible for filling in the contents
			 */
			return inode;
		}

		/*
		 * Uhhuh, somebody else created the same inode under
		 * us. Use the old inode instead of the one we just
		 * allocated.
		 */
		__iget(old);
		spin_unlock(&inode_lock);
		destroy_inode(inode);
		inode = old;
		wait_on_inode(inode);
	}
	return inode;

set_failed:
	spin_unlock(&inode_lock);
	destroy_inode(inode);
	return NULL;
}

/*
 * get_new_inode_fast is the fast path version of get_new_inode, see the
 * comment at iget_locked for details.
 */
static struct inode * get_new_inode_fast(struct super_block *sb, struct hlist_head *head, unsigned long ino)
{
	struct inode * inode;

	inode = alloc_inode(sb);
	if (inode) {
		struct inode * old;

		spin_lock(&inode_lock);
		/* We released the lock, so.. */
		old = find_inode_fast(sb, head, ino);
		if (!old) {
			inode->i_ino = ino;
			inodes_stat.nr_inodes++;
			list_add(&inode->i_list, &inode_in_use);
			list_add(&inode->i_sb_list, &sb->s_inodes);
			hlist_add_head(&inode->i_hash, head);
			inode->i_state = I_LOCK|I_NEW;
			spin_unlock(&inode_lock);

			/* Return the locked inode with I_NEW set, the
			 * caller is responsible for filling in the contents
			 */
			return inode;
		}

		/*
		 * Uhhuh, somebody else created the same inode under
		 * us. Use the old inode instead of the one we just
		 * allocated.
		 */
		__iget(old);
		spin_unlock(&inode_lock);
		destroy_inode(inode);
		inode = old;
		wait_on_inode(inode);
	}
	return inode;
}

static unsigned long hash(struct super_block *sb, unsigned long hashval)
{
	unsigned long tmp;

	tmp = (hashval * (unsigned long)sb) ^ (GOLDEN_RATIO_PRIME + hashval) /
			L1_CACHE_BYTES;
	tmp = tmp ^ ((tmp ^ GOLDEN_RATIO_PRIME) >> I_HASHBITS);
	return tmp & I_HASHMASK;
}

/**
 *	iunique - get a unique inode number
 *	@sb: superblock
 *	@max_reserved: highest reserved inode number
 *
 *	Obtain an inode number that is unique on the system for a given
 *	superblock. This is used by file systems that have no natural
 *	permanent inode numbering system. An inode number is returned that
 *	is higher than the reserved limit but unique.
 *
 *	BUGS:
 *	With a large number of inodes live on the file system this function
 *	currently becomes quite slow.
 */
ino_t iunique(struct super_block *sb, ino_t max_reserved)
{
	/*
	 * On a 32bit, non LFS stat() call, glibc will generate an EOVERFLOW
	 * error if st_ino won't fit in target struct field. Use 32bit counter
	 * here to attempt to avoid that.
	 */
	static unsigned int counter;
	struct inode *inode;
	struct hlist_head *head;
	ino_t res;

	spin_lock(&inode_lock);
	do {
		if (counter <= max_reserved)
			counter = max_reserved + 1;
		res = counter++;
		head = inode_hashtable + hash(sb, res);
		inode = find_inode_fast(sb, head, res);
	} while (inode != NULL);
	spin_unlock(&inode_lock);

	return res;
}
EXPORT_SYMBOL(iunique);

struct inode *igrab(struct inode *inode)
{
	spin_lock(&inode_lock);
	if (!(inode->i_state & (I_FREEING|I_CLEAR|I_WILL_FREE)))
		__iget(inode);
	else
		/*
		 * Handle the case where s_op->clear_inode is not been
		 * called yet, and somebody is calling igrab
		 * while the inode is getting freed.
		 */
		inode = NULL;
	spin_unlock(&inode_lock);
	return inode;
}

EXPORT_SYMBOL(igrab);

/**
 * ifind - internal function, you want ilookup5() or iget5().
 * @sb:		super block of file system to search
 * @head:       the head of the list to search
 * @test:	callback used for comparisons between inodes
 * @data:	opaque data pointer to pass to @test
 * @wait:	if true wait for the inode to be unlocked, if false do not
 *
 * ifind() searches for the inode specified by @data in the inode
 * cache. This is a generalized version of ifind_fast() for file systems where
 * the inode number is not sufficient for unique identification of an inode.
 *
 * If the inode is in the cache, the inode is returned with an incremented
 * reference count.
 *
 * Otherwise NULL is returned.
 *
 * Note, @test is called with the inode_lock held, so can't sleep.
 */
static struct inode *ifind(struct super_block *sb,
		struct hlist_head *head, int (*test)(struct inode *, void *),
		void *data, const int wait)
{
	struct inode *inode;

	spin_lock(&inode_lock);
	inode = find_inode(sb, head, test, data);
	if (inode) {
		__iget(inode);
		spin_unlock(&inode_lock);
		if (likely(wait))
			//等待这个inode激活或者删除完成
			wait_on_inode(inode);
		return inode;
	}
	spin_unlock(&inode_lock);
	return NULL;
}

/**
 * ifind_fast - internal function, you want ilookup() or iget().
 * @sb:		super block of file system to search
 * @head:       head of the list to search
 * @ino:	inode number to search for
 *
 * ifind_fast() searches for the inode @ino in the inode cache. This is for
 * file systems where the inode number is sufficient for unique identification
 * of an inode.
 *
 * If the inode is in the cache, the inode is returned with an incremented
 * reference count.
 *
 * Otherwise NULL is returned.
 */
static struct inode *ifind_fast(struct super_block *sb,
		struct hlist_head *head, unsigned long ino)
{
	struct inode *inode;

	spin_lock(&inode_lock);
	inode = find_inode_fast(sb, head, ino);
	if (inode) {
		__iget(inode);
		spin_unlock(&inode_lock);
		wait_on_inode(inode);
		return inode;
	}
	spin_unlock(&inode_lock);
	return NULL;
}

/**
 * ilookup5_nowait - search for an inode in the inode cache
 * @sb:		super block of file system to search
 * @hashval:	hash value (usually inode number) to search for
 * @test:	callback used for comparisons between inodes
 * @data:	opaque data pointer to pass to @test
 *
 * ilookup5() uses ifind() to search for the inode specified by @hashval and
 * @data in the inode cache. This is a generalized version of ilookup() for
 * file systems where the inode number is not sufficient for unique
 * identification of an inode.
 *
 * If the inode is in the cache, the inode is returned with an incremented
 * reference count.  Note, the inode lock is not waited upon so you have to be
 * very careful what you do with the returned inode.  You probably should be
 * using ilookup5() instead.
 *
 * Otherwise NULL is returned.
 *
 * Note, @test is called with the inode_lock held, so can't sleep.
 */
struct inode *ilookup5_nowait(struct super_block *sb, unsigned long hashval,
		int (*test)(struct inode *, void *), void *data)
{
	struct hlist_head *head = inode_hashtable + hash(sb, hashval);

	return ifind(sb, head, test, data, 0);
}

EXPORT_SYMBOL(ilookup5_nowait);

/**
 * ilookup5 - search for an inode in the inode cache
 * @sb:		super block of file system to search
 * @hashval:	hash value (usually inode number) to search for
 * @test:	callback used for comparisons between inodes
 * @data:	opaque data pointer to pass to @test
 *
 * ilookup5() uses ifind() to search for the inode specified by @hashval and
 * @data in the inode cache. This is a generalized version of ilookup() for
 * file systems where the inode number is not sufficient for unique
 * identification of an inode.
 *
 * If the inode is in the cache, the inode lock is waited upon and the inode is
 * returned with an incremented reference count.
 *
 * Otherwise NULL is returned.
 *
 * Note, @test is called with the inode_lock held, so can't sleep.
 */
struct inode *ilookup5(struct super_block *sb, unsigned long hashval,
		int (*test)(struct inode *, void *), void *data)
{
	struct hlist_head *head = inode_hashtable + hash(sb, hashval);

	return ifind(sb, head, test, data, 1);
}

EXPORT_SYMBOL(ilookup5);

/**
 * ilookup - search for an inode in the inode cache
 * @sb:		super block of file system to search
 * @ino:	inode number to search for
 *
 * ilookup() uses ifind_fast() to search for the inode @ino in the inode cache.
 * This is for file systems where the inode number is sufficient for unique
 * identification of an inode.
 *
 * If the inode is in the cache, the inode is returned with an incremented
 * reference count.
 *
 * Otherwise NULL is returned.
 */
struct inode *ilookup(struct super_block *sb, unsigned long ino)
{
	struct hlist_head *head = inode_hashtable + hash(sb, ino);

	return ifind_fast(sb, head, ino);
}

EXPORT_SYMBOL(ilookup);

/**
 * iget5_locked - obtain an inode from a mounted file system
 * @sb:		super block of file system
 * @hashval:	hash value (usually inode number) to get
 * @test:	callback used for comparisons between inodes
 * @set:	callback used to initialize a new struct inode
 * @data:	opaque data pointer to pass to @test and @set
 *
 * iget5_locked() uses ifind() to search for the inode specified by @hashval
 * and @data in the inode cache and if present it is returned with an increased
 * reference count. This is a generalized version of iget_locked() for file
 * systems where the inode number is not sufficient for unique identification
 * of an inode.
 *
 * If the inode is not in cache, get_new_inode() is called to allocate a new
 * inode and this is returned locked, hashed, and with the I_NEW flag set. The
 * file system gets to fill it in before unlocking it via unlock_new_inode().
 *
 * Note both @test and @set are called with the inode_lock held, so can't sleep.
 */

/**
 * iget5_locked和iget_locked最大的区别是iget5_locked提供了test和set的方法， 
 * 其中test是用于比较的方法，这样的话寻找inode就不需要知道ino号，提供的test方法来 
 * 比较inode是否是我们正在寻找的inode 
 */
/*
 * inode number不足以用于唯一标识inode时候使用iget5_locked还会调用set将设备
 * 号放入新产生的block_device对象的bd_dev成员中.注意调用ifind获取inode后,这个
 * inode没有LOCK的标志,只有调用get_new_inode分配的(get_new_inode也会搜索,这里需要是“分配”)
 * inode有LOCK标志.正如注释所说的,如果inode不在cache中,get_new_inode()被调用,
 * 用于分配一个新的inode,而这个inode的被设置了LOCK,NEW的标志,并被置于hashtable上
*/
struct inode *iget5_locked(struct super_block *sb, unsigned long hashval,
		int (*test)(struct inode *, void *),
		int (*set)(struct inode *, void *), void *data)
{
	struct hlist_head *head = inode_hashtable + hash(sb, hashval);
	struct inode *inode;

	//在高速缓存中搜索inode
	inode = ifind(sb, head, test, data, 1);
	if (inode)
		return inode;
	/*
	 * get_new_inode() will do the right thing, re-trying the search
	 * in case it had to block at any point.
	 */
	//如果调用ifind没有找到inode
	return get_new_inode(sb, head, test, set, data);
}

EXPORT_SYMBOL(iget5_locked);

/**
 * iget_locked - obtain an inode from a mounted file system
 * @sb:		super block of file system
 * @ino:	inode number to get
 *
 * iget_locked() uses ifind_fast() to search for the inode specified by @ino in
 * the inode cache and if present it is returned with an increased reference
 * count. This is for file systems where the inode number is sufficient for
 * unique identification of an inode.
 *
 * If the inode is not in cache, get_new_inode_fast() is called to allocate a
 * new inode and this is returned locked, hashed, and with the I_NEW flag set.
 * The file system gets to fill it in before unlocking it via
 * unlock_new_inode().
 */
struct inode *iget_locked(struct super_block *sb, unsigned long ino)
{
	struct hlist_head *head = inode_hashtable + hash(sb, ino);
	struct inode *inode;

	inode = ifind_fast(sb, head, ino);
	if (inode)
		return inode;
	/*
	 * get_new_inode_fast() will do the right thing, re-trying the search
	 * in case it had to block at any point.
	 */
	return get_new_inode_fast(sb, head, ino);
}

EXPORT_SYMBOL(iget_locked);

/**
 *	__insert_inode_hash - hash an inode
 *	@inode: unhashed inode
 *	@hashval: unsigned long value used to locate this object in the
 *		inode_hashtable.
 *
 *	Add an inode to the inode hash for this superblock.
 */
void __insert_inode_hash(struct inode *inode, unsigned long hashval)
{
	/* 很多情况hashval是ino,不同的文件系统中的ino会有相同的,故计算hash时,需要另外的变量,这里选取sb */
	struct hlist_head *head = inode_hashtable + hash(inode->i_sb, hashval);
	spin_lock(&inode_lock);
	hlist_add_head(&inode->i_hash, head);
	spin_unlock(&inode_lock);
}

EXPORT_SYMBOL(__insert_inode_hash);

/**
 *	remove_inode_hash - remove an inode from the hash
 *	@inode: inode to unhash
 *
 *	Remove an inode from the superblock.
 */
void remove_inode_hash(struct inode *inode)
{
	spin_lock(&inode_lock);
	hlist_del_init(&inode->i_hash);
	spin_unlock(&inode_lock);
}

EXPORT_SYMBOL(remove_inode_hash);

/*
 * Tell the filesystem that this inode is no longer of any interest and should
 * be completely destroyed.
 *
 * We leave the inode in the inode hash table until *after* the filesystem's
 * ->delete_inode completes.  This ensures that an iget (such as nfsd might
 * instigate) will always find up-to-date information either in the hash or on
 * disk.
 *
 * I_FREEING is set so that no-one will take a new reference to the inode while
 * it is being deleted.
 */
void generic_delete_inode(struct inode *inode)
{
	const struct super_operations *op = inode->i_sb->s_op;

	/* 从inode必定从属的状态链表(unused,in_use,dirty)中移除 */
    /* delete the inode from usage list */
	list_del_init(&inode->i_list);
	/* 从sb管理的inode链表中移除 */
    /* delete the inode from sb->s_inodes */
	list_del_init(&inode->i_sb_list);
    /* the state of this inode is freeing */
	inode->i_state |= I_FREEING;
    /* decrease the number of inodes in system */
	inodes_stat.nr_inodes--;
	spin_unlock(&inode_lock);

	security_inode_delete(inode);

	if (op->delete_inode) {
		void (*delete)(struct inode *) = op->delete_inode;
		if (!is_bad_inode(inode))
			DQUOT_INIT(inode);
		/* Filesystems implementing their own
		 * s_op->delete_inode are required to call
		 * truncate_inode_pages and clear_inode()
		 * internally */
		delete(inode);
	} else {
		/* 释放inode的页面资源 */
		truncate_inode_pages(&inode->i_data, 0);
		clear_inode(inode);
	}
	spin_lock(&inode_lock);
	hlist_del_init(&inode->i_hash);
	spin_unlock(&inode_lock);
	wake_up_inode(inode);
	BUG_ON(inode->i_state != I_CLEAR);
	/* 将inode从inode_cachep中删除 */
	destroy_inode(inode);
}

EXPORT_SYMBOL(generic_delete_inode);

static void generic_forget_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;

	if (!hlist_unhashed(&inode->i_hash)) {
		if (!(inode->i_state & (I_DIRTY|I_SYNC)))
			list_move(&inode->i_list, &inode_unused);
		inodes_stat.nr_unused++;
		if (sb->s_flags & MS_ACTIVE) {
			spin_unlock(&inode_lock);
			return;
		}
		inode->i_state |= I_WILL_FREE;
		spin_unlock(&inode_lock);
		write_inode_now(inode, 1);
		spin_lock(&inode_lock);
		inode->i_state &= ~I_WILL_FREE;
		inodes_stat.nr_unused--;
		hlist_del_init(&inode->i_hash);
	}
	list_del_init(&inode->i_list);
	list_del_init(&inode->i_sb_list);
	inode->i_state |= I_FREEING;
	inodes_stat.nr_inodes--;
	spin_unlock(&inode_lock);
	if (inode->i_data.nrpages)
		truncate_inode_pages(&inode->i_data, 0);
	clear_inode(inode);
	wake_up_inode(inode);
	destroy_inode(inode);
}

/*
 * Normal UNIX filesystem behaviour: delete the
 * inode when the usage count drops to zero, and
 * i_nlink is zero.
 */
void generic_drop_inode(struct inode *inode)
{
	if (!inode->i_nlink)
		/* 如果硬链接数为0 */
		generic_delete_inode(inode);
	else
		/* 如果硬链接数不为0 */
		generic_forget_inode(inode);
}

EXPORT_SYMBOL_GPL(generic_drop_inode);

/*
 * Called when we're dropping the last reference
 * to an inode. 
 *
 * Call the FS "drop()" function, defaulting to
 * the legacy UNIX filesystem behaviour..
 *
 * NOTE! NOTE! NOTE! We're called with the inode lock
 * held, and the drop function is supposed to release
 * the lock!
 */
static inline void iput_final(struct inode *inode)
{
	const struct super_operations *op = inode->i_sb->s_op;
	void (*drop)(struct inode *) = generic_drop_inode;

	if (op && op->drop_inode)
		drop = op->drop_inode;
	drop(inode);
}

/**
 *	iput	- put an inode 
 *	@inode: inode to put
 *
 *	Puts an inode, dropping its usage count. If the inode use count hits
 *	zero, the inode is then freed and may also be destroyed.
 *
 *	Consequently, iput() can sleep.
 */
void iput(struct inode *inode)
{
	if (inode) {
		BUG_ON(inode->i_state == I_CLEAR);

		/* 如果没有其他地方引用这个inode,就释放inode资源 */
		if (atomic_dec_and_lock(&inode->i_count, &inode_lock))
			iput_final(inode);
	}
}

EXPORT_SYMBOL(iput);

/**
 *	bmap	- find a block number in a file
 *	@inode: inode of file
 *	@block: block to find
 *
 *	Returns the block number on the device holding the inode that
 *	is the disk block number for the block of the file requested.
 *	That is, asked for block 4 of inode 1 the function will return the
 *	disk block relative to the disk start that holds that block of the 
 *	file.
 */
sector_t bmap(struct inode * inode, sector_t block)
{
	sector_t res = 0;
	if (inode->i_mapping->a_ops->bmap)
		res = inode->i_mapping->a_ops->bmap(inode->i_mapping, block);
	return res;
}
EXPORT_SYMBOL(bmap);

/**
 *	touch_atime	-	update the access time
 *	@mnt: mount the inode is accessed on
 *	@dentry: dentry accessed
 *
 *	Update the accessed time on an inode and mark it for writeback.
 *	This function automatically handles read only file systems and media,
 *	as well as the "noatime" flag and inode specific "noatime" markers.
 */
void touch_atime(struct vfsmount *mnt, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	struct timespec now;

	if (mnt_want_write(mnt))
		return;
	if (inode->i_flags & S_NOATIME)
		goto out;
	if (IS_NOATIME(inode))
		goto out;
	if ((inode->i_sb->s_flags & MS_NODIRATIME) && S_ISDIR(inode->i_mode))
		goto out;

	if (mnt->mnt_flags & MNT_NOATIME)
		goto out;
	if ((mnt->mnt_flags & MNT_NODIRATIME) && S_ISDIR(inode->i_mode))
		goto out;
	if (mnt->mnt_flags & MNT_RELATIME) {
		/*
		 * With relative atime, only update atime if the previous
		 * atime is earlier than either the ctime or mtime.
		 */
		if (timespec_compare(&inode->i_mtime, &inode->i_atime) < 0 &&
		    timespec_compare(&inode->i_ctime, &inode->i_atime) < 0)
			goto out;
	}

	now = current_fs_time(inode->i_sb);
	if (timespec_equal(&inode->i_atime, &now))
		goto out;

	inode->i_atime = now;
	mark_inode_dirty_sync(inode);
out:
	mnt_drop_write(mnt);
}
EXPORT_SYMBOL(touch_atime);

/**
 *	file_update_time	-	update mtime and ctime time
 *	@file: file accessed
 *
 *	Update the mtime and ctime members of an inode and mark the inode
 *	for writeback.  Note that this function is meant exclusively for
 *	usage in the file write path of filesystems, and filesystems may
 *	choose to explicitly ignore update via this function with the
 *	S_NOCTIME inode flag, e.g. for network filesystem where these
 *	timestamps are handled by the server.
 */

void file_update_time(struct file *file)
{
	struct inode *inode = file->f_path.dentry->d_inode;
	struct timespec now;
	int sync_it = 0;
	int err;

	if (IS_NOCMTIME(inode))
		return;

	err = mnt_want_write(file->f_path.mnt);
	if (err)
		return;

	now = current_fs_time(inode->i_sb);
	if (!timespec_equal(&inode->i_mtime, &now)) {
		inode->i_mtime = now;
		sync_it = 1;
	}

	if (!timespec_equal(&inode->i_ctime, &now)) {
		inode->i_ctime = now;
		sync_it = 1;
	}

	if (IS_I_VERSION(inode)) {
		inode_inc_iversion(inode);
		sync_it = 1;
	}

	if (sync_it)
		mark_inode_dirty_sync(inode);
	mnt_drop_write(file->f_path.mnt);
}

EXPORT_SYMBOL(file_update_time);

int inode_needs_sync(struct inode *inode)
{
	if (IS_SYNC(inode))
		return 1;
	if (S_ISDIR(inode->i_mode) && IS_DIRSYNC(inode))
		return 1;
	return 0;
}

EXPORT_SYMBOL(inode_needs_sync);

int inode_wait(void *word)
{
	schedule();
	return 0;
}

/*
 * If we try to find an inode in the inode hash while it is being
 * deleted, we have to wait until the filesystem completes its
 * deletion before reporting that it isn't found.  This function waits
 * until the deletion _might_ have completed.  Callers are responsible
 * to recheck inode state.
 *
 * It doesn't matter if I_LOCK is not set initially, a call to
 * wake_up_inode() after removing from the hash list will DTRT.
 *
 * This is called with inode_lock held.
 */
static void __wait_on_freeing_inode(struct inode *inode)
{
	wait_queue_head_t *wq;
	DEFINE_WAIT_BIT(wait, &inode->i_state, __I_LOCK);
	wq = bit_waitqueue(&inode->i_state, __I_LOCK);
	prepare_to_wait(wq, &wait.wait, TASK_UNINTERRUPTIBLE);
	//在schedule之前unlock
	spin_unlock(&inode_lock);
	schedule();
	finish_wait(wq, &wait.wait);
	spin_lock(&inode_lock);
}

/*
 * We rarely want to lock two inodes that do not have a parent/child
 * relationship (such as directory, child inode) simultaneously. The
 * vast majority of file systems should be able to get along fine
 * without this. Do not use these functions except as a last resort.
 */
void inode_double_lock(struct inode *inode1, struct inode *inode2)
{
	if (inode1 == NULL || inode2 == NULL || inode1 == inode2) {
		if (inode1)
			mutex_lock(&inode1->i_mutex);
		else if (inode2)
			mutex_lock(&inode2->i_mutex);
		return;
	}

	if (inode1 < inode2) {
		mutex_lock_nested(&inode1->i_mutex, I_MUTEX_PARENT);
		mutex_lock_nested(&inode2->i_mutex, I_MUTEX_CHILD);
	} else {
		mutex_lock_nested(&inode2->i_mutex, I_MUTEX_PARENT);
		mutex_lock_nested(&inode1->i_mutex, I_MUTEX_CHILD);
	}
}
EXPORT_SYMBOL(inode_double_lock);

void inode_double_unlock(struct inode *inode1, struct inode *inode2)
{
	if (inode1)
		mutex_unlock(&inode1->i_mutex);

	if (inode2 && inode2 != inode1)
		mutex_unlock(&inode2->i_mutex);
}
EXPORT_SYMBOL(inode_double_unlock);

static __initdata unsigned long ihash_entries;
static int __init set_ihash_entries(char *str)
{
	if (!str)
		return 0;
	ihash_entries = simple_strtoul(str, &str, 0);
	return 1;
}
__setup("ihash_entries=", set_ihash_entries);

/*
 * Initialize the waitqueues and inode hash table.
 */
void __init inode_init_early(void)
{
	int loop;

	/* If hashes are distributed across NUMA nodes, defer
	 * hash allocation until vmalloc space is available.
	 */
	if (hashdist)
		return;

	inode_hashtable =
		alloc_large_system_hash("Inode-cache",
					sizeof(struct hlist_head),
					ihash_entries,
					14,
					HASH_EARLY,
					&i_hash_shift,
					&i_hash_mask,
					0);

	for (loop = 0; loop < (1 << i_hash_shift); loop++)
		INIT_HLIST_HEAD(&inode_hashtable[loop]);
}

void __init inode_init(void)
{
	int loop;

	/* inode slab cache */
	inode_cachep = kmem_cache_create("inode_cache",
					 sizeof(struct inode),
					 0,
					 (SLAB_RECLAIM_ACCOUNT|SLAB_PANIC|
					 SLAB_MEM_SPREAD),
					 init_once);
	register_shrinker(&icache_shrinker);

	/* Hash may have been set up in inode_init_early */
	if (!hashdist)
		return;

	inode_hashtable =
		alloc_large_system_hash("Inode-cache",
					sizeof(struct hlist_head),
					ihash_entries,
					14,
					0,
					&i_hash_shift,
					&i_hash_mask,
					0);

	for (loop = 0; loop < (1 << i_hash_shift); loop++)
		INIT_HLIST_HEAD(&inode_hashtable[loop]);
}

void init_special_inode(struct inode *inode, umode_t mode, dev_t rdev)
{
	inode->i_mode = mode;
	if (S_ISCHR(mode)) {
		inode->i_fop = &def_chr_fops;
		inode->i_rdev = rdev;
	} else if (S_ISBLK(mode)) {
		inode->i_fop = &def_blk_fops;
		inode->i_rdev = rdev;
	} else if (S_ISFIFO(mode))
		inode->i_fop = &def_fifo_fops;
	else if (S_ISSOCK(mode))
		inode->i_fop = &bad_sock_fops;
	else
		printk(KERN_DEBUG "init_special_inode: bogus i_mode (%o)\n",
		       mode);
}
EXPORT_SYMBOL(init_special_inode);
