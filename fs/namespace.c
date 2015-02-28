﻿/*
 *  linux/fs/namespace.c
 *
 * (C) Copyright Al Viro 2000, 2001
 *	Released under GPL v2.
 *
 * Based on code from fs/super.c, copyright Linus Torvalds and others.
 * Heavily rewritten.
 */

#include <linux/syscalls.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/smp_lock.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/acct.h>
#include <linux/capability.h>
#include <linux/cpumask.h>
#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/seq_file.h>
#include <linux/mnt_namespace.h>
#include <linux/namei.h>
#include <linux/security.h>
#include <linux/mount.h>
#include <linux/ramfs.h>
#include <linux/log2.h>
#include <linux/idr.h>
#include <asm/uaccess.h>
#include <asm/unistd.h>
#include "pnode.h"
#include "internal.h"

#define HASH_SHIFT ilog2(PAGE_SIZE / sizeof(struct list_head))
#define HASH_SIZE (1UL << HASH_SHIFT)

/* spinlock for vfsmount related operations, inplace of dcache_lock */
__cacheline_aligned_in_smp DEFINE_SPINLOCK(vfsmount_lock);

static int event;
static DEFINE_IDA(mnt_id_ida);
static DEFINE_IDA(mnt_group_ida);

//mount_hashtable桶是双向链表,桶的偏移为以父vfsmount及子vfsmount的dentry算出的hash
static struct list_head *mount_hashtable __read_mostly;
static struct kmem_cache *mnt_cache __read_mostly;
static struct rw_semaphore namespace_sem;

/* /sys/fs */
struct kobject *fs_kobj;
EXPORT_SYMBOL_GPL(fs_kobj);

static inline unsigned long hash(struct vfsmount *mnt, struct dentry *dentry)
{
	//L1_CACHE_BYTES是128个字节
	unsigned long tmp = ((unsigned long)mnt / L1_CACHE_BYTES);
	tmp += ((unsigned long)dentry / L1_CACHE_BYTES);
	tmp = tmp + (tmp >> HASH_SHIFT);
	return tmp & (HASH_SIZE - 1);
}

#define MNT_WRITER_UNDERFLOW_LIMIT -(1<<16)

/* allocation is serialized by namespace_sem */
static int mnt_alloc_id(struct vfsmount *mnt)
{
	int res;

retry:
	ida_pre_get(&mnt_id_ida, GFP_KERNEL);
	spin_lock(&vfsmount_lock);
	res = ida_get_new(&mnt_id_ida, &mnt->mnt_id);
	spin_unlock(&vfsmount_lock);
	if (res == -EAGAIN)
		goto retry;

	return res;
}

static void mnt_free_id(struct vfsmount *mnt)
{
	spin_lock(&vfsmount_lock);
	ida_remove(&mnt_id_ida, mnt->mnt_id);
	spin_unlock(&vfsmount_lock);
}

/*
 * Allocate a new peer group ID
 *
 * mnt_group_ida is protected by namespace_sem
 */
static int mnt_alloc_group_id(struct vfsmount *mnt)
{
	if (!ida_pre_get(&mnt_group_ida, GFP_KERNEL))
		return -ENOMEM;

	return ida_get_new_above(&mnt_group_ida, 1, &mnt->mnt_group_id);
}

/*
 * Release a peer group ID
 */
void mnt_release_group_id(struct vfsmount *mnt)
{
	ida_remove(&mnt_group_ida, mnt->mnt_group_id);
	mnt->mnt_group_id = 0;
}

struct vfsmount *alloc_vfsmnt(const char *name)
{
	struct vfsmount *mnt = kmem_cache_zalloc(mnt_cache, GFP_KERNEL);
	if (mnt) {
		int err;

		err = mnt_alloc_id(mnt);
		if (err)
			goto out_free_cache;

		if (name) {
			mnt->mnt_devname = kstrdup(name, GFP_KERNEL);
			if (!mnt->mnt_devname)
				goto out_free_id;
		}

		atomic_set(&mnt->mnt_count, 1);
		//所有链表相关初始化一遍
		INIT_LIST_HEAD(&mnt->mnt_hash);
		INIT_LIST_HEAD(&mnt->mnt_child);
		INIT_LIST_HEAD(&mnt->mnt_mounts);
		INIT_LIST_HEAD(&mnt->mnt_list);
		INIT_LIST_HEAD(&mnt->mnt_expire);
        //初始化时,mnt_share链表中的pre和next都指向自己
		INIT_LIST_HEAD(&mnt->mnt_share);
		INIT_LIST_HEAD(&mnt->mnt_slave_list);
		INIT_LIST_HEAD(&mnt->mnt_slave);
		atomic_set(&mnt->__mnt_writers, 0);
	}
	return mnt;

out_free_id:
	mnt_free_id(mnt);
out_free_cache:
	kmem_cache_free(mnt_cache, mnt);
	return NULL;
}

/*
 * Most r/o checks on a fs are for operations that take
 * discrete amounts of time, like a write() or unlink().
 * We must keep track of when those operations start
 * (for permission checks) and when they end, so that
 * we can determine when writes are able to occur to
 * a filesystem.
 */
/*
 * __mnt_is_readonly: check whether a mount is read-only
 * @mnt: the mount to check for its write status
 *
 * This shouldn't be used directly ouside of the VFS.
 * It does not guarantee that the filesystem will stay
 * r/w, just that it is right *now*.  This can not and
 * should not be used in place of IS_RDONLY(inode).
 * mnt_want/drop_write() will _keep_ the filesystem
 * r/w.
 */
int __mnt_is_readonly(struct vfsmount *mnt)
{
	if (mnt->mnt_flags & MNT_READONLY)
		return 1;
	if (mnt->mnt_sb->s_flags & MS_RDONLY)
		return 1;
	return 0;
}
EXPORT_SYMBOL_GPL(__mnt_is_readonly);

struct mnt_writer {
	/*
	 * If holding multiple instances of this lock, they
	 * must be ordered by cpu number.
	 */
	spinlock_t lock;
	struct lock_class_key lock_class; /* compiles out with !lockdep */
	unsigned long count;
	struct vfsmount *mnt;
} ____cacheline_aligned_in_smp;
static DEFINE_PER_CPU(struct mnt_writer, mnt_writers);

static int __init init_mnt_writers(void)
{
	int cpu;
	for_each_possible_cpu(cpu) {
		struct mnt_writer *writer = &per_cpu(mnt_writers, cpu);
		spin_lock_init(&writer->lock);
		lockdep_set_class(&writer->lock, &writer->lock_class);
		writer->count = 0;
	}
	return 0;
}
fs_initcall(init_mnt_writers);

static void unlock_mnt_writers(void)
{
	int cpu;
	struct mnt_writer *cpu_writer;

	for_each_possible_cpu(cpu) {
		cpu_writer = &per_cpu(mnt_writers, cpu);
		spin_unlock(&cpu_writer->lock);
	}
}

static inline void __clear_mnt_count(struct mnt_writer *cpu_writer)
{
	if (!cpu_writer->mnt)
		return;
	/*
	 * This is in case anyone ever leaves an invalid,
	 * old ->mnt and a count of 0.
	 */
	if (!cpu_writer->count)
		return;
	atomic_add(cpu_writer->count, &cpu_writer->mnt->__mnt_writers);
	cpu_writer->count = 0;
}
 /*
 * must hold cpu_writer->lock
 */
static inline void use_cpu_writer_for_mount(struct mnt_writer *cpu_writer,
					  struct vfsmount *mnt)
{
	if (cpu_writer->mnt == mnt)
		return;
	__clear_mnt_count(cpu_writer);
	cpu_writer->mnt = mnt;
}

/*
 * Most r/o checks on a fs are for operations that take
 * discrete amounts of time, like a write() or unlink().
 * We must keep track of when those operations start
 * (for permission checks) and when they end, so that
 * we can determine when writes are able to occur to
 * a filesystem.
 */
/**
 * mnt_want_write - get write access to a mount
 * @mnt: the mount on which to take a write
 *
 * This tells the low-level filesystem that a write is
 * about to be performed to it, and makes sure that
 * writes are allowed before returning success.  When
 * the write operation is finished, mnt_drop_write()
 * must be called.  This is effectively a refcount.
 */
int mnt_want_write(struct vfsmount *mnt)
{
	int ret = 0;
	struct mnt_writer *cpu_writer;

	cpu_writer = &get_cpu_var(mnt_writers);
	spin_lock(&cpu_writer->lock);
	if (__mnt_is_readonly(mnt)) {
		ret = -EROFS;
		goto out;
	}
	use_cpu_writer_for_mount(cpu_writer, mnt);
	cpu_writer->count++;
out:
	spin_unlock(&cpu_writer->lock);
	put_cpu_var(mnt_writers);
	return ret;
}
EXPORT_SYMBOL_GPL(mnt_want_write);

static void lock_mnt_writers(void)
{
	int cpu;
	struct mnt_writer *cpu_writer;

	for_each_possible_cpu(cpu) {
		cpu_writer = &per_cpu(mnt_writers, cpu);
		spin_lock(&cpu_writer->lock);
		__clear_mnt_count(cpu_writer);
		cpu_writer->mnt = NULL;
	}
}

/*
 * These per-cpu write counts are not guaranteed to have
 * matched increments and decrements on any given cpu.
 * A file open()ed for write on one cpu and close()d on
 * another cpu will imbalance this count.  Make sure it
 * does not get too far out of whack.
 */
static void handle_write_count_underflow(struct vfsmount *mnt)
{
	if (atomic_read(&mnt->__mnt_writers) >=
	    MNT_WRITER_UNDERFLOW_LIMIT)
		return;
	/*
	 * It isn't necessary to hold all of the locks
	 * at the same time, but doing it this way makes
	 * us share a lot more code.
	 */
	lock_mnt_writers();
	/*
	 * vfsmount_lock is for mnt_flags.
	 */
	spin_lock(&vfsmount_lock);
	/*
	 * If coalescing the per-cpu writer counts did not
	 * get us back to a positive writer count, we have
	 * a bug.
	 */
	if ((atomic_read(&mnt->__mnt_writers) < 0) &&
	    !(mnt->mnt_flags & MNT_IMBALANCED_WRITE_COUNT)) {
		WARN(1, KERN_DEBUG "leak detected on mount(%p) writers "
				"count: %d\n",
			mnt, atomic_read(&mnt->__mnt_writers));
		/* use the flag to keep the dmesg spam down */
		mnt->mnt_flags |= MNT_IMBALANCED_WRITE_COUNT;
	}
	spin_unlock(&vfsmount_lock);
	unlock_mnt_writers();
}

/**
 * mnt_drop_write - give up write access to a mount
 * @mnt: the mount on which to give up write access
 *
 * Tells the low-level filesystem that we are done
 * performing writes to it.  Must be matched with
 * mnt_want_write() call above.
 */
void mnt_drop_write(struct vfsmount *mnt)
{
	int must_check_underflow = 0;
	struct mnt_writer *cpu_writer;

	cpu_writer = &get_cpu_var(mnt_writers);
	spin_lock(&cpu_writer->lock);

	use_cpu_writer_for_mount(cpu_writer, mnt);
	if (cpu_writer->count > 0) {
		cpu_writer->count--;
	} else {
		must_check_underflow = 1;
		atomic_dec(&mnt->__mnt_writers);
	}

	spin_unlock(&cpu_writer->lock);
	/*
	 * Logically, we could call this each time,
	 * but the __mnt_writers cacheline tends to
	 * be cold, and makes this expensive.
	 */
	if (must_check_underflow)
		handle_write_count_underflow(mnt);
	/*
	 * This could be done right after the spinlock
	 * is taken because the spinlock keeps us on
	 * the cpu, and disables preemption.  However,
	 * putting it here bounds the amount that
	 * __mnt_writers can underflow.  Without it,
	 * we could theoretically wrap __mnt_writers.
	 */
	put_cpu_var(mnt_writers);
}
EXPORT_SYMBOL_GPL(mnt_drop_write);

static int mnt_make_readonly(struct vfsmount *mnt)
{
	int ret = 0;

	lock_mnt_writers();
	/*
	 * With all the locks held, this value is stable
	 */
	if (atomic_read(&mnt->__mnt_writers) > 0) {
		ret = -EBUSY;
		goto out;
	}
	/*
	 * nobody can do a successful mnt_want_write() with all
	 * of the counts in MNT_DENIED_WRITE and the locks held.
	 */
	spin_lock(&vfsmount_lock);
	if (!ret)
		mnt->mnt_flags |= MNT_READONLY;
	spin_unlock(&vfsmount_lock);
out:
	unlock_mnt_writers();
	return ret;
}

static void __mnt_unmake_readonly(struct vfsmount *mnt)
{
	spin_lock(&vfsmount_lock);
	mnt->mnt_flags &= ~MNT_READONLY;
	spin_unlock(&vfsmount_lock);
}

int simple_set_mnt(struct vfsmount *mnt, struct super_block *sb)
{
	mnt->mnt_sb = sb;
	mnt->mnt_root = dget(sb->s_root);
	return 0;
}

EXPORT_SYMBOL(simple_set_mnt);

void free_vfsmnt(struct vfsmount *mnt)
{
	kfree(mnt->mnt_devname);
	mnt_free_id(mnt);
	kmem_cache_free(mnt_cache, mnt);
}

/*
 * find the first or last mount at @dentry on vfsmount @mnt depending on
 * @dir. If @dir is set return the first mount else return the last mount.
 */
struct vfsmount *__lookup_mnt(struct vfsmount *mnt, struct dentry *dentry,
			      int dir)
{
	struct list_head *head = mount_hashtable + hash(mnt, dentry);
	struct list_head *tmp = head;
	struct vfsmount *p, *found = NULL;
	//p是当前vfsmount的子vfsmount

	for (;;) {
		//从头搜索(最先挂载), 还是从尾搜索(最后挂载)
		tmp = dir ? tmp->next : tmp->prev;
		p = NULL;
		if (tmp == head)
			break;
		p = list_entry(tmp, struct vfsmount, mnt_hash);
		if (p->mnt_parent == mnt && p->mnt_mountpoint == dentry) {
			found = p;
			break;
		}
	}
	return found;
}

/*
 * lookup_mnt increments the ref count before returning
 * the vfsmount struct.
 */
struct vfsmount *lookup_mnt(struct vfsmount *mnt, struct dentry *dentry)
{
	struct vfsmount *child_mnt;
	spin_lock(&vfsmount_lock);
	if ((child_mnt = __lookup_mnt(mnt, dentry, 1)))
		mntget(child_mnt);
	spin_unlock(&vfsmount_lock);
	return child_mnt;
}

static inline int check_mnt(struct vfsmount *mnt)
{
	return mnt->mnt_ns == current->nsproxy->mnt_ns;
}

static void touch_mnt_namespace(struct mnt_namespace *ns)
{
	if (ns) {
		ns->event = ++event;
		wake_up_interruptible(&ns->poll);
	}
}

static void __touch_mnt_namespace(struct mnt_namespace *ns)
{
	if (ns && ns->event != event) {
		ns->event = event;
		wake_up_interruptible(&ns->poll);
	}
}

//old_path用来记录mnt的parent
static void detach_mnt(struct vfsmount *mnt, struct path *old_path)
{
	old_path->dentry = mnt->mnt_mountpoint;
	old_path->mnt = mnt->mnt_parent;
	mnt->mnt_parent = mnt;
	mnt->mnt_mountpoint = mnt->mnt_root;
	list_del_init(&mnt->mnt_child);
	list_del_init(&mnt->mnt_hash);
    //将mnt从其原来的挂载点上移除,所以挂载点的d_mounted要--
	old_path->dentry->d_mounted--;
}

void mnt_set_mountpoint(struct vfsmount *mnt, struct dentry *dentry,
			struct vfsmount *child_mnt)
{
    //在clone_mnt中child_mnt->mnt_parent指向了自己,这里就改变parent为mnt
	child_mnt->mnt_parent = mntget(mnt);
	child_mnt->mnt_mountpoint = dget(dentry);
	dentry->d_mounted++;
}

static void attach_mnt(struct vfsmount *mnt, struct path *path)
{
	mnt_set_mountpoint(path->mnt, path->dentry, mnt);
    //通过父mnt及mnt的挂载点,将这个mnt加入mount_hashtable
	list_add_tail(&mnt->mnt_hash, mount_hashtable +
			hash(path->mnt, path->dentry));
    //将mnt作为子mnt加入其父mnt
	list_add_tail(&mnt->mnt_child, &path->mnt->mnt_mounts);
}

/*
 * the caller must hold vfsmount_lock
 */
static void commit_tree(struct vfsmount *mnt)
{
	struct vfsmount *parent = mnt->mnt_parent;
	struct vfsmount *m;
	LIST_HEAD(head);
	//父vfsmount的命名空间
	struct mnt_namespace *n = parent->mnt_ns;

	BUG_ON(parent == mnt);

	list_add_tail(&head, &mnt->mnt_list);
	//将链表中的所有vfsmount的namespace全都置成和父vfsmount的namespace
	list_for_each_entry(m, &head, mnt_list)
    //将mnt->mnt_list所在链表中的元素的mnt_ns全部设置成父mnt的namespace
		m->mnt_ns = n;
    //将head所在链表中的元素头插入n(head这个元素不插入)
	list_splice(&head, n->list.prev);

    //将mnt插入mount_hashtable
	list_add_tail(&mnt->mnt_hash, mount_hashtable +
				hash(parent, mnt->mnt_mountpoint));
    //将mnt插入其parent的管理子mnt的链表中
	list_add_tail(&mnt->mnt_child, &parent->mnt_mounts);
	touch_mnt_namespace(n);
}

//遍历mnt tree
static struct vfsmount *next_mnt(struct vfsmount *p, struct vfsmount *root)
{
	//取p的第一个子mnt
	struct list_head *next = p->mnt_mounts.next;
	if (next == &p->mnt_mounts) {
        //没有子vfsmount
		while (1) {
			if (p == root)
				//整个mnt tree遍历完成
				return NULL;
            //向其兄弟节点移动
			next = p->mnt_child.next;
			if (next != &p->mnt_parent->mnt_mounts)
				break;
            //向父节点移动
			p = p->mnt_parent;
		}
	}
	//返回子mnt
	return list_entry(next, struct vfsmount, mnt_child);
}

static struct vfsmount *skip_mnt_tree(struct vfsmount *p)
{
    /*
     * 这里的做法非常巧妙,在while循环中一直定位到p的挂载树中最后一个子孙节点.
     * 返回后,在copy_tree的循环中next_mnt回去寻找这个子孙节点的next mnt.
     * 由于这个子孙节点是p最后一个,所以在next_mnt中会从这个子孙节点一直向其
     * 父节点追溯,直到p的下一个兄弟节点为止.这样就跳过了以p为root的挂载树的copy.
    */
	struct list_head *prev = p->mnt_mounts.prev;
	while (prev != &p->mnt_mounts) {
		p = list_entry(prev, struct vfsmount, mnt_child);
		prev = p->mnt_mounts.prev;
	}
	return p;
}

//重新分配一个vfsmount,这个mnt的sb是old的sb,但是root被限制在参数@root
static struct vfsmount *clone_mnt(struct vfsmount *old, struct dentry *root,
					int flag)
{
	//指向同一个sb, 不需要分配空间
	struct super_block *sb = old->mnt_sb;
	struct vfsmount *mnt = alloc_vfsmnt(old->mnt_devname);

	if (mnt) {
		if (flag & (CL_SLAVE | CL_PRIVATE))
			mnt->mnt_group_id = 0; /* not a peer of original */
		else
			mnt->mnt_group_id = old->mnt_group_id;

		if ((flag & CL_MAKE_SHARED) && !mnt->mnt_group_id) {
			int err = mnt_alloc_group_id(mnt);
			if (err)
				goto out_free;
		}

        //新分配的mnt继承old的挂载标志
		mnt->mnt_flags = old->mnt_flags;
        //因为这个mnt和old共用sb,并且有root,故sb->s_active自加
		atomic_inc(&sb->s_active);
        //mnt和old共用sb
		mnt->mnt_sb = sb;
        //如果mnt->mnt_root = dget(old->mnt_root);那么进入b后可以看到old的根目录
		mnt->mnt_root = dget(root);
        //目前只是clone,并没有把mnt和具体的目录关联起来,故这里设置mnt_mountpoint指向其mnt_root
		mnt->mnt_mountpoint = mnt->mnt_root;
		//父vfsmount指向自己
		mnt->mnt_parent = mnt;

		if (flag & CL_SLAVE) {
            //clone时要求mnt是old的slave
			list_add(&mnt->mnt_slave, &old->mnt_slave_list);
			mnt->mnt_master = old;
            //slave默认没有共享属性
			CLEAR_MNT_SHARED(mnt);
		} else if (!(flag & CL_PRIVATE)) {
            //clone时要求不是private,也不是slave
			if ((flag & CL_PROPAGATION) || IS_MNT_SHARED(old))
                //clone时要求propagate或者old(一般是设备)本身就具有shared属性,将mnt加入old的peer group
				list_add(&mnt->mnt_share, &old->mnt_share);
			if (IS_MNT_SLAVE(old))
                //若old本身就是slave,那么mnt加入slave链表,并且和old拥有共同的master
                list_add(&mnt->mnt_slave, &old->mnt_slave);
			mnt->mnt_master = old->mnt_master;
		}
		if (flag & CL_MAKE_SHARED)
            //为mnt设置共享属性
			set_mnt_shared(mnt);

		/* stick the duplicate mount on the same expiry list
		 * as the original if that was on one */
		if (flag & CL_EXPIRE) {
			if (!list_empty(&old->mnt_expire))
				list_add(&mnt->mnt_expire, &old->mnt_expire);
		}
	}
	return mnt;

 out_free:
	free_vfsmnt(mnt);
	return NULL;
}

static inline void __mntput(struct vfsmount *mnt)
{
	int cpu;
	struct super_block *sb = mnt->mnt_sb;
	/*
	 * We don't have to hold all of the locks at the
	 * same time here because we know that we're the
	 * last reference to mnt and that no new writers
	 * can come in.
	 */
	for_each_possible_cpu(cpu) {
		struct mnt_writer *cpu_writer = &per_cpu(mnt_writers, cpu);
		if (cpu_writer->mnt != mnt)
			continue;
		spin_lock(&cpu_writer->lock);
		atomic_add(cpu_writer->count, &mnt->__mnt_writers);
		cpu_writer->count = 0;
		/*
		 * Might as well do this so that no one
		 * ever sees the pointer and expects
		 * it to be valid.
		 */
		cpu_writer->mnt = NULL;
		spin_unlock(&cpu_writer->lock);
	}
	/*
	 * This probably indicates that somebody messed
	 * up a mnt_want/drop_write() pair.  If this
	 * happens, the filesystem was probably unable
	 * to make r/w->r/o transitions.
	 */
	WARN_ON(atomic_read(&mnt->__mnt_writers));
	dput(mnt->mnt_root);
	free_vfsmnt(mnt);
	deactivate_super(sb);
}

void mntput_no_expire(struct vfsmount *mnt)
{
repeat:
	if (atomic_dec_and_lock(&mnt->mnt_count, &vfsmount_lock)) {
		if (likely(!mnt->mnt_pinned)) {
			spin_unlock(&vfsmount_lock);
			__mntput(mnt);
			return;
		}
		atomic_add(mnt->mnt_pinned + 1, &mnt->mnt_count);
		mnt->mnt_pinned = 0;
		spin_unlock(&vfsmount_lock);
		acct_auto_close_mnt(mnt);
		security_sb_umount_close(mnt);
		goto repeat;
	}
}

EXPORT_SYMBOL(mntput_no_expire);

void mnt_pin(struct vfsmount *mnt)
{
	spin_lock(&vfsmount_lock);
	mnt->mnt_pinned++;
	spin_unlock(&vfsmount_lock);
}

EXPORT_SYMBOL(mnt_pin);

void mnt_unpin(struct vfsmount *mnt)
{
	spin_lock(&vfsmount_lock);
	if (mnt->mnt_pinned) {
		atomic_inc(&mnt->mnt_count);
		mnt->mnt_pinned--;
	}
	spin_unlock(&vfsmount_lock);
}

EXPORT_SYMBOL(mnt_unpin);

static inline void mangle(struct seq_file *m, const char *s)
{
	seq_escape(m, s, " \t\n\\");
}

/*
 * Simple .show_options callback for filesystems which don't want to
 * implement more complex mount option showing.
 *
 * See also save_mount_options().
 */
int generic_show_options(struct seq_file *m, struct vfsmount *mnt)
{
	const char *options = mnt->mnt_sb->s_options;

	if (options != NULL && options[0]) {
		seq_putc(m, ',');
		mangle(m, options);
	}

	return 0;
}
EXPORT_SYMBOL(generic_show_options);

/*
 * If filesystem uses generic_show_options(), this function should be
 * called from the fill_super() callback.
 *
 * The .remount_fs callback usually needs to be handled in a special
 * way, to make sure, that previous options are not overwritten if the
 * remount fails.
 *
 * Also note, that if the filesystem's .remount_fs function doesn't
 * reset all options to their default value, but changes only newly
 * given options, then the displayed options will not reflect reality
 * any more.
 */
void save_mount_options(struct super_block *sb, char *options)
{
	kfree(sb->s_options);
	sb->s_options = kstrdup(options, GFP_KERNEL);
}
EXPORT_SYMBOL(save_mount_options);

#ifdef CONFIG_PROC_FS
/* iterator */
static void *m_start(struct seq_file *m, loff_t *pos)
{
	struct proc_mounts *p = m->private;

	down_read(&namespace_sem);
	return seq_list_start(&p->ns->list, *pos);
}

static void *m_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct proc_mounts *p = m->private;

	return seq_list_next(v, &p->ns->list, pos);
}

static void m_stop(struct seq_file *m, void *v)
{
	up_read(&namespace_sem);
}

struct proc_fs_info {
	int flag;
	const char *str;
};

static int show_sb_opts(struct seq_file *m, struct super_block *sb)
{
	static const struct proc_fs_info fs_info[] = {
		{ MS_SYNCHRONOUS, ",sync" },
		{ MS_DIRSYNC, ",dirsync" },
		{ MS_MANDLOCK, ",mand" },
		{ 0, NULL }
	};
	const struct proc_fs_info *fs_infop;

	for (fs_infop = fs_info; fs_infop->flag; fs_infop++) {
		if (sb->s_flags & fs_infop->flag)
			seq_puts(m, fs_infop->str);
	}

	return security_sb_show_options(m, sb);
}

static void show_mnt_opts(struct seq_file *m, struct vfsmount *mnt)
{
	static const struct proc_fs_info mnt_info[] = {
		{ MNT_NOSUID, ",nosuid" },
		{ MNT_NODEV, ",nodev" },
		{ MNT_NOEXEC, ",noexec" },
		{ MNT_NOATIME, ",noatime" },
		{ MNT_NODIRATIME, ",nodiratime" },
		{ MNT_RELATIME, ",relatime" },
		{ 0, NULL }
	};
	const struct proc_fs_info *fs_infop;

	for (fs_infop = mnt_info; fs_infop->flag; fs_infop++) {
		if (mnt->mnt_flags & fs_infop->flag)
			seq_puts(m, fs_infop->str);
	}
}

static void show_type(struct seq_file *m, struct super_block *sb)
{
	mangle(m, sb->s_type->name);
	if (sb->s_subtype && sb->s_subtype[0]) {
		seq_putc(m, '.');
		mangle(m, sb->s_subtype);
	}
}

static int show_vfsmnt(struct seq_file *m, void *v)
{
	struct vfsmount *mnt = list_entry(v, struct vfsmount, mnt_list);
	int err = 0;
	struct path mnt_path = { .dentry = mnt->mnt_root, .mnt = mnt };

	mangle(m, mnt->mnt_devname ? mnt->mnt_devname : "none");
	seq_putc(m, ' ');
	seq_path(m, &mnt_path, " \t\n\\");
	seq_putc(m, ' ');
	show_type(m, mnt->mnt_sb);
	seq_puts(m, __mnt_is_readonly(mnt) ? " ro" : " rw");
	err = show_sb_opts(m, mnt->mnt_sb);
	if (err)
		goto out;
	show_mnt_opts(m, mnt);
	if (mnt->mnt_sb->s_op->show_options)
		err = mnt->mnt_sb->s_op->show_options(m, mnt);
	seq_puts(m, " 0 0\n");
out:
	return err;
}

const struct seq_operations mounts_op = {
	.start	= m_start,
	.next	= m_next,
	.stop	= m_stop,
	.show	= show_vfsmnt
};

static int show_mountinfo(struct seq_file *m, void *v)
{
	struct proc_mounts *p = m->private;
	struct vfsmount *mnt = list_entry(v, struct vfsmount, mnt_list);
	struct super_block *sb = mnt->mnt_sb;
	struct path mnt_path = { .dentry = mnt->mnt_root, .mnt = mnt };
	struct path root = p->root;
	int err = 0;

	seq_printf(m, "%i %i %u:%u ", mnt->mnt_id, mnt->mnt_parent->mnt_id,
		   MAJOR(sb->s_dev), MINOR(sb->s_dev));
	seq_dentry(m, mnt->mnt_root, " \t\n\\");
	seq_putc(m, ' ');
	seq_path_root(m, &mnt_path, &root, " \t\n\\");
	if (root.mnt != p->root.mnt || root.dentry != p->root.dentry) {
		/*
		 * Mountpoint is outside root, discard that one.  Ugly,
		 * but less so than trying to do that in iterator in a
		 * race-free way (due to renames).
		 */
		return SEQ_SKIP;
	}
	seq_puts(m, mnt->mnt_flags & MNT_READONLY ? " ro" : " rw");
	show_mnt_opts(m, mnt);

	/* Tagged fields ("foo:X" or "bar") */
	if (IS_MNT_SHARED(mnt))
		seq_printf(m, " shared:%i", mnt->mnt_group_id);
	if (IS_MNT_SLAVE(mnt)) {
		int master = mnt->mnt_master->mnt_group_id;
		int dom = get_dominating_id(mnt, &p->root);
		seq_printf(m, " master:%i", master);
		if (dom && dom != master)
			seq_printf(m, " propagate_from:%i", dom);
	}
	if (IS_MNT_UNBINDABLE(mnt))
		seq_puts(m, " unbindable");

	/* Filesystem specific data */
	seq_puts(m, " - ");
	show_type(m, sb);
	seq_putc(m, ' ');
	mangle(m, mnt->mnt_devname ? mnt->mnt_devname : "none");
	seq_puts(m, sb->s_flags & MS_RDONLY ? " ro" : " rw");
	err = show_sb_opts(m, sb);
	if (err)
		goto out;
	if (sb->s_op->show_options)
		err = sb->s_op->show_options(m, mnt);
	seq_putc(m, '\n');
out:
	return err;
}

const struct seq_operations mountinfo_op = {
	.start	= m_start,
	.next	= m_next,
	.stop	= m_stop,
	.show	= show_mountinfo,
};

static int show_vfsstat(struct seq_file *m, void *v)
{
	struct vfsmount *mnt = list_entry(v, struct vfsmount, mnt_list);
	struct path mnt_path = { .dentry = mnt->mnt_root, .mnt = mnt };
	int err = 0;

	/* device */
	if (mnt->mnt_devname) {
		seq_puts(m, "device ");
		mangle(m, mnt->mnt_devname);
	} else
		seq_puts(m, "no device");

	/* mount point */
	seq_puts(m, " mounted on ");
	seq_path(m, &mnt_path, " \t\n\\");
	seq_putc(m, ' ');

	/* file system type */
	seq_puts(m, "with fstype ");
	show_type(m, mnt->mnt_sb);

	/* optional statistics */
	if (mnt->mnt_sb->s_op->show_stats) {
		seq_putc(m, ' ');
		err = mnt->mnt_sb->s_op->show_stats(m, mnt);
	}

	seq_putc(m, '\n');
	return err;
}

const struct seq_operations mountstats_op = {
	.start	= m_start,
	.next	= m_next,
	.stop	= m_stop,
	.show	= show_vfsstat,
};
#endif  /* CONFIG_PROC_FS */

/**
 * may_umount_tree - check if a mount tree is busy
 * @mnt: root of mount tree
 *
 * This is called to check if a tree of mounts has any
 * open files, pwds, chroots or sub mounts that are
 * busy.
 */
int may_umount_tree(struct vfsmount *mnt)
{
	int actual_refs = 0;
	int minimum_refs = 0;
	struct vfsmount *p;

	spin_lock(&vfsmount_lock);
	for (p = mnt; p; p = next_mnt(p, mnt)) {
		actual_refs += atomic_read(&p->mnt_count);
		minimum_refs += 2;
	}
	spin_unlock(&vfsmount_lock);

	if (actual_refs > minimum_refs)
		return 0;

	return 1;
}

EXPORT_SYMBOL(may_umount_tree);

/**
 * may_umount - check if a mount point is busy
 * @mnt: root of mount
 *
 * This is called to check if a mount point has any
 * open files, pwds, chroots or sub mounts. If the
 * mount has sub mounts this will return busy
 * regardless of whether the sub mounts are busy.
 *
 * Doesn't take quota and stuff into account. IOW, in some cases it will
 * give false negatives. The main reason why it's here is that we need
 * a non-destructive way to look for easily umountable filesystems.
 */
int may_umount(struct vfsmount *mnt)
{
	int ret = 1;
	spin_lock(&vfsmount_lock);
	if (propagate_mount_busy(mnt, 2))
		ret = 0;
	spin_unlock(&vfsmount_lock);
	return ret;
}

EXPORT_SYMBOL(may_umount);

void release_mounts(struct list_head *head)
{
	struct vfsmount *mnt;
	while (!list_empty(head)) {
		mnt = list_first_entry(head, struct vfsmount, mnt_hash);
		list_del_init(&mnt->mnt_hash);
		if (mnt->mnt_parent != mnt) {
            //如果不mnt不是系统根mnt,才能释放
			struct dentry *dentry;
			struct vfsmount *m;
			spin_lock(&vfsmount_lock);
			dentry = mnt->mnt_mountpoint;
			m = mnt->mnt_parent;
			mnt->mnt_mountpoint = mnt->mnt_root;
			mnt->mnt_parent = mnt;
            //mnt马上要释放,其parent下记录处理处于umount_tree及release_mounts之间的子mnt的计数需要减少
			m->mnt_ghosts--;
			spin_unlock(&vfsmount_lock);
			dput(dentry);
			mntput(m);
		}
		mntput(mnt);
	}
}

void umount_tree(struct vfsmount *mnt, int propagate, struct list_head *kill)
{
	struct vfsmount *p;

	for (p = mnt; p; p = next_mnt(p, mnt))
		//将mnt整个挂载树从原来的hash表中删除,然后加入kill链表
        list_move(&p->mnt_hash, kill);

	if (propagate)
		/*
		 * 下面这个函数完成后,kill中就有了mnt的挂载树,及mnt父mnt propagate tree中对应于mnt
		 * 的子mnt
		 */
		propagate_umount(kill);

	list_for_each_entry(p, kill, mnt_hash) {
		list_del_init(&p->mnt_expire);
		list_del_init(&p->mnt_list);
		__touch_mnt_namespace(p->mnt_ns);
		p->mnt_ns = NULL;
        //从挂载树的链表中取下来
		list_del_init(&p->mnt_child);
		if (p->mnt_parent != p) {
			/*
			* 如果不是系统根mnt,其父mnt的mnt_ghosts++, 表示其父mnt又有一个子mnt
			* 将要调用release_mounts去处理mnt->mnt_parent及mnt->mnt_mountpoint
			*/
			p->mnt_parent->mnt_ghosts++;
            //递减其挂载点的d_mounted
			p->mnt_mountpoint->d_mounted--;
		}
        //修改mnt属性为private
		change_mnt_propagation(p, MS_PRIVATE);
	}
}

static void shrink_submounts(struct vfsmount *mnt, struct list_head *umounts);

static int do_umount(struct vfsmount *mnt, int flags)
{
	struct super_block *sb = mnt->mnt_sb;
	int retval;
	LIST_HEAD(umount_list);

	retval = security_sb_umount(mnt, flags);
	if (retval)
		return retval;

	/*
	 * Allow userspace to request a mountpoint be expired rather than
	 * unmounting unconditionally. Unmount only happens if:
	 *  (1) the mark is already set (the mark is cleared by mntput())
	 *  (2) the usage count == 1 [parent vfsmount] + 1 [sys_umount]
	 */
	if (flags & MNT_EXPIRE) {
		if (mnt == current->fs->root.mnt ||
		    flags & (MNT_FORCE | MNT_DETACH))
			return -EINVAL;

		if (atomic_read(&mnt->mnt_count) != 2)
			return -EBUSY;

		if (!xchg(&mnt->mnt_expiry_mark, 1))
			return -EAGAIN;
	}

	/*
	 * If we may have to abort operations to get out of this
	 * mount, and they will themselves hold resources we must
	 * allow the fs to do things. In the Unix tradition of
	 * 'Gee thats tricky lets do it in userspace' the umount_begin
	 * might fail to complete on the first run through as other tasks
	 * must return, and the like. Thats for the mount program to worry
	 * about for the moment.
	 */

	if (flags & MNT_FORCE && sb->s_op->umount_begin) {
		lock_kernel();
		sb->s_op->umount_begin(sb);
		unlock_kernel();
	}

	/*
	 * No sense to grab the lock for this test, but test itself looks
	 * somewhat bogus. Suggestions for better replacement?
	 * Ho-hum... In principle, we might treat that as umount + switch
	 * to rootfs. GC would eventually take care of the old vfsmount.
	 * Actually it makes sense, especially if rootfs would contain a
	 * /reboot - static binary that would close all descriptors and
	 * call reboot(9). Then init(8) could umount root and exec /reboot.
	 */
	if (mnt == current->fs->root.mnt && !(flags & MNT_DETACH)) {
		/*
		 * Special case for "unmounting" root ...
		 * we just try to remount it readonly.
		 */
		down_write(&sb->s_umount);
		if (!(sb->s_flags & MS_RDONLY)) {
			lock_kernel();
			retval = do_remount_sb(sb, MS_RDONLY, NULL, 0);
			unlock_kernel();
		}
		up_write(&sb->s_umount);
		return retval;
	}

	down_write(&namespace_sem);
	spin_lock(&vfsmount_lock);
	event++;

	if (!(flags & MNT_DETACH))
        //将mnt中所有可以shrink的子mnt加入umount_list链表
        shrink_submounts(mnt, &umount_list);

	retval = -EBUSY;
    /* 
     * 一个没有其他进程使用的mnt在这里mnt_count应该为2,因为在创建的时候mnt_count已经为1,
     * SYSCALL_DEFINE2中通过user_path -> lookup_mnt -> mntget 增加了mnt_count,
     * mnt_count为2
     */
	if (flags & MNT_DETACH || !propagate_mount_busy(mnt, 2)) {
		if (!list_empty(&mnt->mnt_list))
			umount_tree(mnt, 1, &umount_list);
		retval = 0;
	}
	spin_unlock(&vfsmount_lock);
	if (retval)
		security_sb_umount_busy(mnt);
	up_write(&namespace_sem);
    //将umount_list上所有的mnt释放
	release_mounts(&umount_list);
	return retval;
}

/*
 * Now umount can handle mount points as well as block devices.
 * This is important for filesystems which use unnamed block devices.
 *
 * We now support a flag for forced unmount like the other 'big iron'
 * unixes. Our API is identical to OSF/1 to avoid making a mess of AMD
 */

SYSCALL_DEFINE2(umount, char __user *, name, int, flags)
{
	struct path path;
	int retval;

	retval = user_path(name, &path);
	if (retval)
		goto out;
	retval = -EINVAL;
	if (path.dentry != path.mnt->mnt_root)
		goto dput_and_out;
	if (!check_mnt(path.mnt))
		goto dput_and_out;

	retval = -EPERM;
	if (!capable(CAP_SYS_ADMIN))
		goto dput_and_out;

	retval = do_umount(path.mnt, flags);
dput_and_out:
	/* we mustn't call path_put() as that would clear mnt_expiry_mark */
	dput(path.dentry);
	mntput_no_expire(path.mnt);
out:
	return retval;
}

#ifdef __ARCH_WANT_SYS_OLDUMOUNT

/*
 *	The 2.0 compatible umount. No flags.
 */
SYSCALL_DEFINE1(oldumount, char __user *, name)
{
	return sys_umount(name, 0);
}

#endif

static int mount_is_safe(struct path *path)
{
	if (capable(CAP_SYS_ADMIN))
		return 0;
	return -EPERM;
#ifdef notyet
	if (S_ISLNK(path->dentry->d_inode->i_mode))
		return -EPERM;
	if (path->dentry->d_inode->i_mode & S_ISVTX) {
		if (current->uid != path->dentry->d_inode->i_uid)
			return -EPERM;
	}
	if (inode_permission(path->dentry->d_inode, MAY_WRITE))
		return -EPERM;
	return 0;
#endif
}

//clone以mnt为根节点的挂载树(仅clone父子关系的mnt)
struct vfsmount *copy_tree(struct vfsmount *mnt, struct dentry *dentry,
					int flag)
{
    /* 
     * res用于记录clone出来的tree的root节点
     * p,s用于遍历mnt的挂载树,其中s记录当前节点,p记录s的父节点
     * r用于遍历mnt的一级子mnt
     * q用于记录clone当前正在clone的mnt
    */
	struct vfsmount *res, *p, *q, *r, *s;
	struct path path;

	if (!(flag & CL_COPY_ALL) && IS_MNT_UNBINDABLE(mnt))
        /* 
         * 如果clone要求除CL_COPY_ALL之外的标志并且mnt有不可绑定属性,就返回NULL.
         * 简而言之就是如果clone要求CL_COPY_ALL,就可以不管mnt是否有不可绑定属性,向下执行.
         * 或者,如果mnt可绑定,这样也不需要关心clone标志了.
         */
        //在do_loopback直接调用中,flag == 0,故只要mnt不可绑定就返回NULL.
        return NULL;

    //首先clone mnt挂载树的根节点
	res = q = clone_mnt(mnt, dentry, flag);
	if (!q)
		goto Enomem;
	q->mnt_mountpoint = mnt->mnt_mountpoint;

	p = mnt;
	list_for_each_entry(r, &mnt->mnt_mounts, mnt_child) {
        //遍历一级子mnt
		if (!is_subdir(r->mnt_mountpoint, dentry))
            //r是mnt的子mnt,故r的挂载点也必须是dentry的子目录
			continue;

        //遍历mnt的二级及以下的子mnt
		for (s = r; s; s = next_mnt(s, r)) {
			if (!(flag & CL_COPY_ALL) && IS_MNT_UNBINDABLE(s)) {
                 /* 
                  * 如果clone要求除CL_COPY_ALL之外的标志并且s有不可绑定属性,
                  * clone的时候就跳过以这个mnt为root的挂载树
                  */
				s = skip_mnt_tree(s);
				continue;
			}
			while (p != s->mnt_parent) {
                /* 
                 * 条件成立的情况是上一次遍历的时候,s,p指向了一个节点的最后一个子节点,
                 * 本次遍历的时候next_mnt(s, r)返回的s是p的父节点(也有可能是ancestor),
                 * 为了保证q一直记录clone tree中当前节点的父节点,故这里需要一直向parent追溯
                 */
				p = p->mnt_parent;
				q = q->mnt_parent;
			}
			p = s;
            //因为q在下面会改变,这里先记录q的父mnt
			path.mnt = q;
			path.dentry = p->mnt_mountpoint;
			q = clone_mnt(p, p->mnt_root, flag);
			if (!q)
				goto Enomem;
			spin_lock(&vfsmount_lock);
            //q和clone tree的根节点--res同一命名空间
			list_add_tail(&q->mnt_list, &res->mnt_list);
			attach_mnt(q, &path);
			spin_unlock(&vfsmount_lock);
		}
	}
    //返回clone tree的root节点
	return res;
Enomem:
	if (res) {
		LIST_HEAD(umount_list);
		spin_lock(&vfsmount_lock);
		umount_tree(res, 0, &umount_list);
		spin_unlock(&vfsmount_lock);
		release_mounts(&umount_list);
	}
	return NULL;
}

struct vfsmount *collect_mounts(struct vfsmount *mnt, struct dentry *dentry)
{
	struct vfsmount *tree;
	down_write(&namespace_sem);
	tree = copy_tree(mnt, dentry, CL_COPY_ALL | CL_PRIVATE);
	up_write(&namespace_sem);
	return tree;
}

void drop_collected_mounts(struct vfsmount *mnt)
{
	LIST_HEAD(umount_list);
	down_write(&namespace_sem);
	spin_lock(&vfsmount_lock);
	umount_tree(mnt, 0, &umount_list);
	spin_unlock(&vfsmount_lock);
	up_write(&namespace_sem);
	release_mounts(&umount_list);
}

static void cleanup_group_ids(struct vfsmount *mnt, struct vfsmount *end)
{
	struct vfsmount *p;

	for (p = mnt; p != end; p = next_mnt(p, mnt)) {
		if (p->mnt_group_id && !IS_MNT_SHARED(p))
			mnt_release_group_id(p);
	}
}

static int invent_group_ids(struct vfsmount *mnt, bool recurse)
{
	struct vfsmount *p;

	for (p = mnt; p; p = recurse ? next_mnt(p, mnt) : NULL) {
		if (!p->mnt_group_id && !IS_MNT_SHARED(p)) {
			int err = mnt_alloc_group_id(p);
			if (err) {
				cleanup_group_ids(mnt, p);
				return err;
			}
		}
	}

	return 0;
}

/*
 *  @source_mnt : mount tree to be attached
 *  @nd         : place the mount tree @source_mnt is attached
 *  @parent_nd  : if non-null, detach the source_mnt from its parent and
 *  		   store the parent mount and mountpoint dentry.
 *  		   (done when source_mnt is moved)
 *
 *  NOTE: in the table below explains the semantics when a source mount
 *  of a given type is attached to a destination mount of a given type.
 * ---------------------------------------------------------------------------
 * |         BIND MOUNT OPERATION                                            |
 * |**************************************************************************
 * | source-->| shared        |       private  |       slave    | unbindable |
 * | dest     |               |                |                |            |
 * |   |      |               |                |                |            |
 * |   v      |               |                |                |            |
 * |**************************************************************************
 * |  shared  | shared (++)   |     shared (+) |     shared(+++)|  invalid   |
 * |          |               |                |                |            |
 * |non-shared| shared (+)    |      private   |      slave (*) |  invalid   |
 * ***************************************************************************
 * A bind operation clones the source mount and mounts the clone on the
 * destination mount.
 *
 * (++)  the cloned mount is propagated to all the mounts in the propagation
 * 	 tree of the destination mount and the cloned mount is added to
 * 	 the peer group of the source mount.
 * (+)   the cloned mount is created under the destination mount and is marked
 *       as shared. The cloned mount is added to the peer group of the source
 *       mount.
 * (+++) the mount is propagated to all the mounts in the propagation tree
 *       of the destination mount and the cloned mount is made slave
 *       of the same master as that of the source mount. The cloned mount
 *       is marked as 'shared and slave'.
 * (*)   the cloned mount is made a slave of the same master as that of the
 * 	 source mount.
 *
 * ---------------------------------------------------------------------------
 * |         		MOVE MOUNT OPERATION                                 |
 * |**************************************************************************
 * | source-->| shared        |       private  |       slave    | unbindable |
 * | dest     |               |                |                |            |
 * |   |      |               |                |                |            |
 * |   v      |               |                |                |            |
 * |**************************************************************************
 * |  shared  | shared (+)    |     shared (+) |    shared(+++) |  invalid   |
 * |          |               |                |                |            |
 * |non-shared| shared (+*)   |      private   |    slave (*)   | unbindable |
 * ***************************************************************************
 *
 * (+)  the mount is moved to the destination. And is then propagated to
 * 	all the mounts in the propagation tree of the destination mount.
 * (+*)  the mount is moved to the destination.
 * (+++)  the mount is moved to the destination and is then propagated to
 * 	all the mounts belonging to the destination mount's propagation tree.
 * 	the mount is marked as 'shared and slave'.
 * (*)	the mount continues to be a slave at the new location.
 *
 * if the source mount is a tree, the operations explained above is
 * applied to each mount in the tree.
 * Must be called without spinlocks held, since this function can sleep
 * in allocations.
 */
//将source_mnt整个挂载树(如果有子mnt,否则就只挂source_mnt)挂到path
static int attach_recursive_mnt(struct vfsmount *source_mnt,
			struct path *path, struct path *parent_path)
{
    //tree_list中记录propagation的mnt,这个这个链表头是局部变量,操作链表时就不需要加锁
	LIST_HEAD(tree_list);
	struct vfsmount *dest_mnt = path->mnt;
	struct dentry *dest_dentry = path->dentry;
	struct vfsmount *child, *p;
	int err;

	if (IS_MNT_SHARED(dest_mnt)) {
		err = invent_group_ids(source_mnt, true);
		if (err)
			goto out;
	}
    //将所有propagation的mnt加入tree_list
	err = propagate_mnt(dest_mnt, dest_dentry, source_mnt, &tree_list);
	if (err)
		goto out_cleanup_ids;

	if (IS_MNT_SHARED(dest_mnt)) {
        /* 
         * 如果dest_mnt是share mnt,那么将source_mnt(新创建的mnt)的
         * 所有子mnt(包括source_mnt本身)全部添加MNT_SHARED标志,source_mnt的peer的共享标志
         * 在propagate_mnt中已经处理完成
         */
		for (p = source_mnt; p; p = next_mnt(p, source_mnt))
			set_mnt_shared(p);
	}

	spin_lock(&vfsmount_lock);
    //单独处理source_mnt
	if (parent_path) {
        //如果移动一个旧的mnt到新的路径,需要将其从旧的挂载树上取下
		detach_mnt(source_mnt, parent_path);
        //path记录新的挂载点的路径信息,将source_mnt挂载到path
		attach_mnt(source_mnt, path);
		touch_mnt_namespace(current->nsproxy->mnt_ns);
	} else {
        //将新创建的mnt和挂载点关联起来
		mnt_set_mountpoint(dest_mnt, dest_dentry, source_mnt);
		commit_tree(source_mnt);
	}

	list_for_each_entry_safe(child, p, &tree_list, mnt_hash) {
        //集中处理source_mnt的propagation 
		list_del_init(&child->mnt_hash);
		commit_tree(child);
	}
	spin_unlock(&vfsmount_lock);
	return 0;

 out_cleanup_ids:
	if (IS_MNT_SHARED(dest_mnt))
		cleanup_group_ids(source_mnt, NULL);
 out:
	return err;
}

//mnt是新创建的mnt,path中保存挂载点的路径信息,将mnt的整个挂载树嫁接到path上
static int graft_tree(struct vfsmount *mnt, struct path *path)
{
	int err;
	//建立mnt tree时不能有MS_NOUSER,即不能被用户挂载
	if (mnt->mnt_sb->s_flags & MS_NOUSER)
        //如果mnt中的sb禁止用户挂载的话
		return -EINVAL;

	if (S_ISDIR(path->dentry->d_inode->i_mode) !=
	      S_ISDIR(mnt->mnt_root->d_inode->i_mode))
        /* 
         * path->dentry中记录了挂载点的路径信息,mnt是设备所在的mnt的clone,
         * 但其mnt->root已经指向了设备目录的denty.
         * 
         * NOTE:这里的隐含意思就是文件和文件也能挂载,文件也可以是挂载点!!!
         */
		return -ENOTDIR;

	err = -ENOENT;
	mutex_lock(&path->dentry->d_inode->i_mutex);
	if (IS_DEADDIR(path->dentry->d_inode))
		goto out_unlock;

	err = security_sb_check_sb(mnt, path);
	if (err)
		goto out_unlock;

	err = -ENOENT;
	if (IS_ROOT(path->dentry) || !d_unhashed(path->dentry))
        /*
         * 如果path->dentry是mount的根目录或者path->dentry已经在hash表中了,
         * 正常情况下path->dentry一定是mount的根目录
        */
		err = attach_recursive_mnt(mnt, path, NULL);
out_unlock:
	mutex_unlock(&path->dentry->d_inode->i_mutex);
	if (!err)
		security_sb_post_addmount(mnt, path);
	return err;
}

/*
 * recursively change the type of the mountpoint.
 */
static int do_change_type(struct path *path, int flag)
{
	struct vfsmount *m, *mnt = path->mnt;
    //recursive标志单独处理
	int recurse = flag & MS_REC;
    //清除recursive标志
	int type = flag & ~MS_REC;
	int err = 0;

	if (!capable(CAP_SYS_ADMIN))
        //只有root才能执行
		return -EPERM;

	if (path->dentry != path->mnt->mnt_root)
        /* 
         * Path must be a mount point,if whithout this snippet,any directory or file can
         * be operated by mount --make-shared or other similar opertions
         */
		return -EINVAL;

	down_write(&namespace_sem);
	if (type == MS_SHARED) {
		err = invent_group_ids(mnt, recurse);
		if (err)
			goto out_unlock;
	}

	spin_lock(&vfsmount_lock);
	for (m = mnt; m; m = (recurse ? next_mnt(m, mnt) : NULL))
        //改变mnt_flags
		change_mnt_propagation(m, type);
	spin_unlock(&vfsmount_lock);

 out_unlock:
	up_write(&namespace_sem);
	return err;
}

/*
 * do loopback mount.
 */
//nd中保存了挂载点的名称,old_name是设备名称 
static int do_loopback(struct path *path, char *old_name,
				int recurse)
{
	struct path old_path;
	struct vfsmount *mnt = NULL;
    //确保只有root才能挂载
	int err = mount_is_safe(path);
	if (err)
		return err;
	if (!old_name || !*old_name)
		return -EINVAL;
    //old_path中保存了设备的路径信息
	err = kern_path(old_name, LOOKUP_FOLLOW, &old_path);
	if (err)
		return err;

	down_write(&namespace_sem);
	err = -EINVAL;
	if (IS_MNT_UNBINDABLE(old_path.mnt))
        //如果设备不可绑定,直接退出
		goto out;

	if (!check_mnt(path->mnt) || !check_mnt(old_path.mnt))
        //检查挂载点的mnt和设备的mnt的namespace是否和当前进程相同
		goto out;

	err = -ENOMEM;
	if (recurse)
        //如果mount --rbind,就copy old_path.mnt及其所有的子mount
		mnt = copy_tree(old_path.mnt, old_path.dentry, 0);
	else
        //如果mount --bind,只clone old_path.mnt本身
		mnt = clone_mnt(old_path.mnt, old_path.dentry, 0);

	if (!mnt)
		goto out;

    //将clone出来的mnt嫁接到挂载点所在的mnt及这个mnt的所有peer及slave的mnt上去
	err = graft_tree(mnt, path);
	if (err) {
		LIST_HEAD(umount_list);
		spin_lock(&vfsmount_lock);
		umount_tree(mnt, 0, &umount_list);
		spin_unlock(&vfsmount_lock);
		release_mounts(&umount_list);
	}

out:
	up_write(&namespace_sem);
	path_put(&old_path);
	return err;
}

static int change_mount_flags(struct vfsmount *mnt, int ms_flags)
{
	int error = 0;
	int readonly_request = 0;

	if (ms_flags & MS_RDONLY)
		readonly_request = 1;
	if (readonly_request == __mnt_is_readonly(mnt))
		return 0;

	if (readonly_request)
		error = mnt_make_readonly(mnt);
	else
		__mnt_unmake_readonly(mnt);
	return error;
}

/*
 * change filesystem flags. dir should be a physical root of filesystem.
 * If you've mounted a non-root directory somewhere and want to do remount
 * on it - tough luck.
 */
static int do_remount(struct path *path, int flags, int mnt_flags,
		      void *data)
{
	int err;
	struct super_block *sb = path->mnt->mnt_sb;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (!check_mnt(path->mnt))
		return -EINVAL;

	if (path->dentry != path->mnt->mnt_root)
		return -EINVAL;

	down_write(&sb->s_umount);
	if (flags & MS_BIND)
		err = change_mount_flags(path->mnt, flags);
	else
		err = do_remount_sb(sb, flags, data, 0);
	if (!err)
		path->mnt->mnt_flags = mnt_flags;
	up_write(&sb->s_umount);
	if (!err) {
		security_sb_post_remount(path->mnt, flags, data);

		spin_lock(&vfsmount_lock);
		touch_mnt_namespace(path->mnt->mnt_ns);
		spin_unlock(&vfsmount_lock);
	}
	return err;
}

static inline int tree_contains_unbindable(struct vfsmount *mnt)
{
	struct vfsmount *p;
	for (p = mnt; p; p = next_mnt(p, mnt)) {
		if (IS_MNT_UNBINDABLE(p))
			return 1;
	}
	return 0;
}

//path中记录挂载点的路径信息,old_name是设备名
static int do_move_mount(struct path *path, char *old_name)
{
	struct path old_path, parent_path;
	struct vfsmount *p;
	int err = 0;
	if (!capable(CAP_SYS_ADMIN))
        //只有root能够操作
		return -EPERM;
	if (!old_name || !*old_name)
        //如果设备名称为NULL及其解引用后为NULL
		return -EINVAL;
    //old_path中记录设备的原路径信息
	err = kern_path(old_name, LOOKUP_FOLLOW, &old_path);
	if (err)
		return err;

	down_write(&namespace_sem);
    //找到最后一个挂载点
	while (d_mountpoint(path->dentry) &&
	       follow_down(&path->mnt, &path->dentry))
		;
	err = -EINVAL;
	if (!check_mnt(path->mnt) || !check_mnt(old_path.mnt))
		goto out;

	err = -ENOENT;
	mutex_lock(&path->dentry->d_inode->i_mutex);
	if (IS_DEADDIR(path->dentry->d_inode))
		goto out1;

	if (!IS_ROOT(path->dentry) && d_unhashed(path->dentry))
        //如果path->dentry是整个系统的根目录或者这个dentry已经在hash表中了,这时候才不会跳转到out1
		goto out1;

	err = -EINVAL;
	if (old_path.dentry != old_path.mnt->mnt_root)
        //确保设备的dentry是其mnt的根目录
		goto out1;

	if (old_path.mnt == old_path.mnt->mnt_parent)
        //设备的mnt不是根文件系统,也就是说不能移动系统的根目录
		goto out1;

	if (S_ISDIR(path->dentry->d_inode->i_mode) !=
	      S_ISDIR(old_path.dentry->d_inode->i_mode))
        //设备和挂载点的文件类型必须相同
		goto out1;
	/*
	 * Don't move a mount residing in a shared parent.
	 */
	if (old_path.mnt->mnt_parent &&
	    IS_MNT_SHARED(old_path.mnt->mnt_parent))
        /*
         * 如果一个mnt的parent具有共享属性,这个mnt不允许move,因为这种情况下,mnt的clone也是
         * 其parent的peer的子mnt.如果移动这样一个mnt可能需要两种处理方式:
         * 
         * 1.仅仅将mnt移动.但这种情况下造成了mnt的parent和parent的peer挂载不同步.违背了共享挂载的特性.
         * 2.移动mnt,并且销毁其clone(也就是其parent的peer下和mnt对应的子mnt).这又影响其parent
         *   peer的挂载树
         * 
         * 根据共享子树设计者Ram Pai的回复,这种情况太复杂,就没有实现
        */
            goto out1;
	/*
	 * Don't move a mount tree containing unbindable mounts to a destination
	 * mount which is shared.
	 */
	if (IS_MNT_SHARED(path->mnt) &&
	    tree_contains_unbindable(old_path.mnt))
        /* 
         * 如果挂载点是共享的,但是设备的挂载树中有些挂载有不可挂载属性,这种情况是不允许
         * mount --move的.
         */
        goto out1;
	err = -ELOOP;
    /* 
     * 挂载点的mnt不能是设备mnt的子mnt, 这是因为将一个父mnt移动到子mnt会造成整个挂载树紊乱
     */
	for (p = path->mnt; p->mnt_parent != p; p = p->mnt_parent)
		if (p == old_path.mnt)
            goto out1;

	err = attach_recursive_mnt(old_path.mnt, path, &parent_path);
	if (err)
		goto out1;

	/* if the mount is moved, it should no longer be expire
	 * automatically */
	list_del_init(&old_path.mnt->mnt_expire);
out1:
	mutex_unlock(&path->dentry->d_inode->i_mutex);
out:
	up_write(&namespace_sem);
	if (!err)
        //原挂载的父mnt的mnt及dentry计数要--
		path_put(&parent_path);
	path_put(&old_path);
	return err;
}

/*
 * create a new mount for userspace and request it to be added into the
 * namespace's tree
 */
//nd中保存了挂载点的名称,name是设备名称
static int do_new_mount(struct path *path, char *type, int flags,
			int mnt_flags, char *name, void *data)
{
	struct vfsmount *mnt;

	//type为空,或者type超过page size
	if (!type || !memchr(type, 0, PAGE_SIZE))
		return -EINVAL;

	/* we need capabilities... */
	//检查进程的权能,只有root能mount,如果将下面的snippet去掉,普通用户也能mount 
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	mnt = do_kern_mount(type, flags, name, data);
	if (IS_ERR(mnt))
		return PTR_ERR(mnt);

	return do_add_mount(mnt, path, mnt_flags, NULL);
}

/*
 * add a mount into a namespace's mount tree
 * - provide the option of adding the new mount to an expiration list
 */
 //将安装点挂在目录树上
int do_add_mount(struct vfsmount *newmnt, struct path *path,
		 int mnt_flags, struct list_head *fslist)
{
	int err;

	down_write(&namespace_sem);
	/* Something was mounted here while we slept */
	while (d_mountpoint(path->dentry) &&
	       follow_down(&path->mnt, &path->dentry))
        //找到挂载到这个挂载点的最后一个vfsmount
		;
	err = -EINVAL;
	if (!check_mnt(path->mnt))
		goto unlock;

	/* Refuse the same filesystem on the same mount point */
	err = -EBUSY;
	if (path->mnt->mnt_sb == newmnt->mnt_sb &&
	    path->mnt->mnt_root == path->dentry)
		goto unlock;

	err = -EINVAL;
	if (S_ISLNK(newmnt->mnt_root->d_inode->i_mode))
		goto unlock;

	newmnt->mnt_flags = mnt_flags;
	if ((err = graft_tree(newmnt, path)))
		goto unlock;

	if (fslist) /* add to the specified expiration list */
		list_add_tail(&newmnt->mnt_expire, fslist);

	up_write(&namespace_sem);
	return 0;

unlock:
	up_write(&namespace_sem);
	mntput(newmnt);
	return err;
}

EXPORT_SYMBOL_GPL(do_add_mount);

/*
 * process a list of expirable mountpoints with the intent of discarding any
 * mountpoints that aren't in use and haven't been touched since last we came
 * here
 */
void mark_mounts_for_expiry(struct list_head *mounts)
{
	struct vfsmount *mnt, *next;
	LIST_HEAD(graveyard);
	LIST_HEAD(umounts);

	if (list_empty(mounts))
		return;

	down_write(&namespace_sem);
	spin_lock(&vfsmount_lock);

	/* extract from the expiration list every vfsmount that matches the
	 * following criteria:
	 * - only referenced by its parent vfsmount
	 * - still marked for expiry (marked on the last call here; marks are
	 *   cleared by mntput())
	 */
	list_for_each_entry_safe(mnt, next, mounts, mnt_expire) {
		if (!xchg(&mnt->mnt_expiry_mark, 1) ||
			propagate_mount_busy(mnt, 1))
			continue;
		list_move(&mnt->mnt_expire, &graveyard);
	}
	while (!list_empty(&graveyard)) {
		mnt = list_first_entry(&graveyard, struct vfsmount, mnt_expire);
		touch_mnt_namespace(mnt->mnt_ns);
		umount_tree(mnt, 1, &umounts);
	}
	spin_unlock(&vfsmount_lock);
	up_write(&namespace_sem);

	release_mounts(&umounts);
}

EXPORT_SYMBOL_GPL(mark_mounts_for_expiry);

/*
 * Ripoff of 'select_parent()'
 *
 * search the list of submounts for a given mountpoint, and move any
 * shrinkable submounts to the 'graveyard' list.
 */
static int select_submounts(struct vfsmount *parent, struct list_head *graveyard)
{
	struct vfsmount *this_parent = parent;
	struct list_head *next;
	int found = 0;

repeat:
    //找到第一个子mount
	next = this_parent->mnt_mounts.next;
resume:
	while (next != &this_parent->mnt_mounts) {
		struct list_head *tmp = next;
		struct vfsmount *mnt = list_entry(tmp, struct vfsmount, mnt_child);

		next = tmp->next;
		if (!(mnt->mnt_flags & MNT_SHRINKABLE))
            //只有拥有MNT_SHRINKABLE标志的mnt才能shrink
			continue;
		/*
		 * Descend a level if the d_mounts list is non-empty.
		 */
        //向下一级子mount遍历
		if (!list_empty(&mnt->mnt_mounts)) {
			this_parent = mnt;
			goto repeat;
		}

        //如果已经到了挂载树的leave
		if (!propagate_mount_busy(mnt, 1)) {
			list_move_tail(&mnt->mnt_expire, graveyard);
			found++;
		}
	}
	/*
	 * All done at this level ... ascend and resume the search
	 */
	if (this_parent != parent) {
		next = this_parent->mnt_child.next;
		this_parent = this_parent->mnt_parent;
		goto resume;
	}
	return found;
}

/*
 * process a list of expirable mountpoints with the intent of discarding any
 * submounts of a specific parent mountpoint
 */
static void shrink_submounts(struct vfsmount *mnt, struct list_head *umounts)
{
    /* 
     * graveyard只是临时用于记录mnt中可以shrink的子mnt,最终还需要将这些mnt加入umounts中,
     * umount中还记录了共享子树中需要释放的部分
     */
	LIST_HEAD(graveyard);
	struct vfsmount *m;

	/* extract submounts of 'mountpoint' from the expiration list */
	while (select_submounts(mnt, &graveyard)) {
        //当mnt有子mnt的时候才会进入这个循环
		while (!list_empty(&graveyard)) {
			m = list_first_entry(&graveyard, struct vfsmount,
						mnt_expire);
			touch_mnt_namespace(m->mnt_ns);
			umount_tree(m, 1, umounts);
		}
	}
}

/*
 * Some copy_from_user() implementations do not return the exact number of
 * bytes remaining to copy on a fault.  But copy_mount_options() requires that.
 * Note that this function differs from copy_from_user() in that it will oops
 * on bad values of `to', rather than returning a short copy.
 */
static long exact_copy_from_user(void *to, const void __user * from,
				 unsigned long n)
{
	char *t = to;
	const char __user *f = from;
	char c;

	//检查用户空间指针是否可读
	if (!access_ok(VERIFY_READ, from, n))
		return n;

	while (n) {
		//user和kernel传递数据时不能用memcpy函数,因为kernel和user内存不能直接互访
		//__get_user,kernel从用户空间获得一个值
		//__put_user,kernel向用户空间传递一个值
		//__get_user(x,ptr),Returns zero on success, or -EFAULT on error. On error, the variable x is set to zero. 
		if (__get_user(c, f)) {
			memset(t, 0, n);
			break;
		}
		*t++ = c;
		f++;
		n--;
	}
	return n;
}

int copy_mount_options(const void __user * data, unsigned long *where)
{
	int i;
	unsigned long page;
	unsigned long size;

	*where = 0;
	if (!data)
		return 0;

	if (!(page = __get_free_page(GFP_KERNEL)))
		return -ENOMEM;

	/* We only care that *some* data at the address the user
	 * gave us is valid.  Just in case, we'll zero
	 * the remainder of the page.
	 */
	/* copy_from_user cannot cross TASK_SIZE ! */
	//从data开始，拷贝一个page的数据。如果data+PAGE_SIZE超出TASK_SIZE,也就是到了内核空间,就不能拷贝
	size = TASK_SIZE - (unsigned long)data;
	if (size > PAGE_SIZE)
		size = PAGE_SIZE;

	i = size - exact_copy_from_user((void *)page, data, size);
	//如果一个字节都没copy
	if (!i) {
		free_page(page);
		return -EFAULT;
	}
	//如果不满一个page,将剩下的部分填0
	if (i != PAGE_SIZE)
		memset((char *)page + i, 0, PAGE_SIZE - i);
	*where = page;
	return 0;
}

/*
 * Flags is a 32-bit value that allows up to 31 non-fs dependent flags to
 * be given to the mount() call (ie: read-only, no-dev, no-suid etc).
 *
 * data is a (void *) that can point to any structure up to
 * PAGE_SIZE-1 bytes, which can contain arbitrary fs-dependent
 * information (or be NULL).
 *
 * Pre-0.97 versions of mount() didn't have a flags word.
 * When the flags word was introduced its top half was required
 * to have the magic value 0xC0ED, and this remained so until 2.4.0-test9.
 * Therefore, if this magic number is present, it carries no information
 * and must be discarded.
 */
long do_mount(char *dev_name, char *dir_name, char *type_page,
		  unsigned long flags, void *data_page)
{
	struct path path;
	int retval = 0;
	int mnt_flags = 0;
    /*
     * mountflags：指定文件系统的读写访问标志，可能值有以下
     * MS_BIND：执行bind挂载，使文件或者子目录树在文件系统内的另一个点上可视。
     * MS_DIRSYNC：同步目录的更新。
     * MS_MANDLOCK：允许在文件上执行强制锁。
     * MS_MOVE：移动子目录树。
     * MS_NOATIME：不要更新文件上的访问时间。
     * MS_NODEV：不允许访问设备文件。
     * MS_NODIRATIME：不允许更新目录上的访问时间。
     * MS_NOEXEC：不允许在挂上的文件系统上执行程序。
     * MS_NOSUID：执行程序时，不遵照set-user-ID和set-group-ID位。
     * MS_RDONLY：指定文件系统为只读。
     * MS_REMOUNT：重新加载文件系统。这允许你改变现存文件系统的mountflag和数据，而无需使用先卸载，再挂上文件系统的方式。
     * MS_SYNCHRONOUS：同步文件的更新。
     * MNT_FORCE：强制卸载，即使文件系统处于忙状态。
     * MNT_EXPIRE：将挂载点标志为过时。
     */


	/* Discard magic */
	//MS_MGC_VAL 和 MS_MGC_MSK是在以前的版本中定义的安装标志和掩码，现在的安装标志中已经不使用这些魔数了
	//因此，当还有这个魔数时，则丢弃它。
	if ((flags & MS_MGC_MSK) == MS_MGC_VAL)
		flags &= ~MS_MGC_MSK;

	/* Basic sanity checks */
	//如果挂载字符串为空，或者字符串过长，超过PAGE_SIZE大小，则返回失败
	if (!dir_name || !*dir_name || !memchr(dir_name, 0, PAGE_SIZE))
		return -EINVAL;
	//对于基于网络的文件系统dev_name可以为空
	if (dev_name && !memchr(dev_name, 0, PAGE_SIZE))
		return -EINVAL;
	//如果data_page超过PAGE_SIZE长度，将超出部分截断
	if (data_page)
		((char *)data_page)[PAGE_SIZE - 1] = 0;

	/* Separate the per-mountpoint flags */
    /* 如果已安装文件系统对象中的安装标志MS_NOSUID、MS_NODEV、MS_NOATIME、MS_NODIRATIME、MS_NODEV或MS_NOEXEC
	 * 中任一个被设置，则清除它们，并在已安装文件系统对象中设置相应的标志
     *（MNT_NOSUID、MNT_NODEV、MNT_NOEXEC、MNT_NOATIME、MNT_NODIRATIME)
     */
	if (flags & MS_NOSUID)
		mnt_flags |= MNT_NOSUID;
	if (flags & MS_NODEV)
		mnt_flags |= MNT_NODEV;
	if (flags & MS_NOEXEC)
		mnt_flags |= MNT_NOEXEC;
	if (flags & MS_NOATIME)
		mnt_flags |= MNT_NOATIME;
	if (flags & MS_NODIRATIME)
		mnt_flags |= MNT_NODIRATIME;
	if (flags & MS_RELATIME)
		mnt_flags |= MNT_RELATIME;
	if (flags & MS_RDONLY)
		mnt_flags |= MNT_READONLY;

	flags &= ~(MS_NOSUID | MS_NOEXEC | MS_NODEV | MS_ACTIVE |
		   MS_NOATIME | MS_NODIRATIME | MS_RELATIME| MS_KERNMOUNT);

	/* ... and get the mountpoint */
	retval = kern_path(dir_name, LOOKUP_FOLLOW, &path);
	if (retval)
		return retval;

	retval = security_sb_mount(dev_name, &path,
				   type_page, flags, data_page);
	if (retval)
		goto dput_out;

	//如果MS_REMOUNT标志被指定，其目的通常是改变超级块对象s_flags字段的安装标志，
	if (flags & MS_REMOUNT)
		//修改挂载选项
		retval = do_remount(&path, flags & ~MS_REMOUNT, mnt_flags,
				    data_page);
	else if (flags & MS_BIND)
        //path中记录挂载点的路径信息
		retval = do_loopback(&path, dev_name, flags & MS_REC);
	else if (flags & (MS_SHARED | MS_PRIVATE | MS_SLAVE | MS_UNBINDABLE))
		//改变挂载标志或涉及的各个vfsmount实例之间建立所需的数据结构的关联
		retval = do_change_type(&path, flags);
	else if (flags & MS_MOVE)
		//移动一个已经挂载的文件系统
		retval = do_move_mount(&path, dev_name);
	else
		retval = do_new_mount(&path, type_page, flags, mnt_flags,
				      dev_name, data_page);
dput_out:
	path_put(&path);
	return retval;
}

/*
 * Allocate a new namespace structure and populate it with contents
 * copied from the namespace of the passed in task structure.
 */
static struct mnt_namespace *dup_mnt_ns(struct mnt_namespace *mnt_ns,
		struct fs_struct *fs)
{
	struct mnt_namespace *new_ns;
	struct vfsmount *rootmnt = NULL, *pwdmnt = NULL;
	struct vfsmount *p, *q;

	new_ns = kmalloc(sizeof(struct mnt_namespace), GFP_KERNEL);
	if (!new_ns)
		return ERR_PTR(-ENOMEM);

	atomic_set(&new_ns->count, 1);
	INIT_LIST_HEAD(&new_ns->list);
	init_waitqueue_head(&new_ns->poll);
	new_ns->event = 0;

	down_write(&namespace_sem);
	/* First pass: copy the tree topology */
	new_ns->root = copy_tree(mnt_ns->root, mnt_ns->root->mnt_root,
					CL_COPY_ALL | CL_EXPIRE);
	if (!new_ns->root) {
		up_write(&namespace_sem);
		kfree(new_ns);
		return ERR_PTR(-ENOMEM);;
	}
	spin_lock(&vfsmount_lock);
	list_add_tail(&new_ns->list, &new_ns->root->mnt_list);
	spin_unlock(&vfsmount_lock);

	/*
	 * Second pass: switch the tsk->fs->* elements and mark new vfsmounts
	 * as belonging to new namespace.  We have already acquired a private
	 * fs_struct, so tsk->fs->lock is not needed.
	 */
	p = mnt_ns->root;
	q = new_ns->root;
	while (p) {
		q->mnt_ns = new_ns;
		if (fs) {
			if (p == fs->root.mnt) {
				rootmnt = p;
				fs->root.mnt = mntget(q);
			}
			if (p == fs->pwd.mnt) {
				pwdmnt = p;
				fs->pwd.mnt = mntget(q);
			}
		}
		p = next_mnt(p, mnt_ns->root);
		q = next_mnt(q, new_ns->root);
	}
	up_write(&namespace_sem);

	if (rootmnt)
		mntput(rootmnt);
	if (pwdmnt)
		mntput(pwdmnt);

	return new_ns;
}

struct mnt_namespace *copy_mnt_ns(unsigned long flags, struct mnt_namespace *ns,
		struct fs_struct *new_fs)
{
	struct mnt_namespace *new_ns;

	BUG_ON(!ns);
	get_mnt_ns(ns);

	if (!(flags & CLONE_NEWNS))
		return ns;

	new_ns = dup_mnt_ns(ns, new_fs);

	put_mnt_ns(ns);
	return new_ns;
}

/*
 *data这个参数是具体文件系统在挂载时的特定选项，对于ext2来说有sb=n这个参数可以指定sb的位置， 
 *虽然sb的默认位置是1。 
*/
SYSCALL_DEFINE5(mount, char __user *, dev_name, char __user *, dir_name,
		char __user *, type, unsigned long, flags, void __user *, data)
{
	int retval;
	unsigned long data_page;
	unsigned long type_page;
	unsigned long dev_page;
	char *dir_page;

	//复制挂载选项
	retval = copy_mount_options(type, &type_page);
	if (retval < 0)
		return retval;

	//getname()在复制时遇到字符串结尾符“\0”就停止
	dir_page = getname(dir_name);
	retval = PTR_ERR(dir_page);
	if (IS_ERR(dir_page))
		goto out1;

	//copy_mount_option拷贝整个页面，并返回该页面的起始地址。 
	retval = copy_mount_options(dev_name, &dev_page);
	if (retval < 0)
		goto out2;

	retval = copy_mount_options(data, &data_page);
	if (retval < 0)
		goto out3;

	lock_kernel();
	retval = do_mount((char *)dev_page, dir_page, (char *)type_page,
			  flags, (void *)data_page);
	unlock_kernel();
	free_page(data_page);

out3:
	free_page(dev_page);
out2:
	putname(dir_page);
out1:
	free_page(type_page);
	return retval;
}

/*
 * Replace the fs->{rootmnt,root} with {mnt,dentry}. Put the old values.
 * It can block. Requires the big lock held.
 */
void set_fs_root(struct fs_struct *fs, struct path *path)
{
	struct path old_root;

	write_lock(&fs->lock);
	old_root = fs->root;
	fs->root = *path;
	path_get(path);
	write_unlock(&fs->lock);
	if (old_root.dentry)
		path_put(&old_root);
}

/*
 * Replace the fs->{pwdmnt,pwd} with {mnt,dentry}. Put the old values.
 * It can block. Requires the big lock held.
 */
void set_fs_pwd(struct fs_struct *fs, struct path *path)
{
	struct path old_pwd;

	write_lock(&fs->lock);
	old_pwd = fs->pwd;
	fs->pwd = *path;
	path_get(path);
	write_unlock(&fs->lock);

	if (old_pwd.dentry)
		path_put(&old_pwd);
}

static void chroot_fs_refs(struct path *old_root, struct path *new_root)
{
	struct task_struct *g, *p;
	struct fs_struct *fs;

	read_lock(&tasklist_lock);
	do_each_thread(g, p) {
		task_lock(p);
		fs = p->fs;
		if (fs) {
			atomic_inc(&fs->count);
			task_unlock(p);
			if (fs->root.dentry == old_root->dentry
			    && fs->root.mnt == old_root->mnt)
				set_fs_root(fs, new_root);
			if (fs->pwd.dentry == old_root->dentry
			    && fs->pwd.mnt == old_root->mnt)
				set_fs_pwd(fs, new_root);
			put_fs_struct(fs);
		} else
			task_unlock(p);
	} while_each_thread(g, p);
	read_unlock(&tasklist_lock);
}

/*
 * pivot_root Semantics:
 * Moves the root file system of the current process to the directory put_old,
 * makes new_root as the new root file system of the current process, and sets
 * root/cwd of all processes which had them on the current root to new_root.
 *
 * Restrictions:
 * The new_root and put_old must be directories, and  must not be on the
 * same file  system as the current process root. The put_old  must  be
 * underneath new_root,  i.e. adding a non-zero number of /.. to the string
 * pointed to by put_old must yield the same directory as new_root. No other
 * file system may be mounted on put_old. After all, new_root is a mountpoint.
 *
 * Also, the current root cannot be on the 'rootfs' (initial ramfs) filesystem.
 * See Documentation/filesystems/ramfs-rootfs-initramfs.txt for alternatives
 * in this situation.
 *
 * Notes:
 *  - we don't move root/cwd if they are not at the root (reason: if something
 *    cared enough to change them, it's probably wrong to force them elsewhere)
 *  - it's okay to pick a root that isn't the root of a file system, e.g.
 *    /nfs/my_root where /nfs is the mount point. It must be a mountpoint,
 *    though, so you may need to say mount --bind /nfs/my_root /nfs/my_root
 *    first.
 */
SYSCALL_DEFINE2(pivot_root, const char __user *, new_root,
		const char __user *, put_old)
{
	struct vfsmount *tmp;
	struct path new, old, parent_path, root_parent, root;
	int error;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	error = user_path_dir(new_root, &new);
	if (error)
		goto out0;
	error = -EINVAL;
	if (!check_mnt(new.mnt))
		goto out1;

	error = user_path_dir(put_old, &old);
	if (error)
		goto out1;

	error = security_sb_pivotroot(&old, &new);
	if (error) {
		path_put(&old);
		goto out1;
	}

	read_lock(&current->fs->lock);
	root = current->fs->root;
	path_get(&current->fs->root);
	read_unlock(&current->fs->lock);
	down_write(&namespace_sem);
	mutex_lock(&old.dentry->d_inode->i_mutex);
	error = -EINVAL;
	if (IS_MNT_SHARED(old.mnt) ||
		IS_MNT_SHARED(new.mnt->mnt_parent) ||
		IS_MNT_SHARED(root.mnt->mnt_parent))
		goto out2;
	if (!check_mnt(root.mnt))
		goto out2;
	error = -ENOENT;
	if (IS_DEADDIR(new.dentry->d_inode))
		goto out2;
	if (d_unhashed(new.dentry) && !IS_ROOT(new.dentry))
		goto out2;
	if (d_unhashed(old.dentry) && !IS_ROOT(old.dentry))
		goto out2;
	error = -EBUSY;
	if (new.mnt == root.mnt ||
	    old.mnt == root.mnt)
		goto out2; /* loop, on the same file system  */
	error = -EINVAL;
	if (root.mnt->mnt_root != root.dentry)
		goto out2; /* not a mountpoint */
	if (root.mnt->mnt_parent == root.mnt)
		goto out2; /* not attached */
	if (new.mnt->mnt_root != new.dentry)
		goto out2; /* not a mountpoint */
	if (new.mnt->mnt_parent == new.mnt)
		goto out2; /* not attached */
	/* make sure we can reach put_old from new_root */
	tmp = old.mnt;
	spin_lock(&vfsmount_lock);
	if (tmp != new.mnt) {
		for (;;) {
			if (tmp->mnt_parent == tmp)
				goto out3; /* already mounted on put_old */
			if (tmp->mnt_parent == new.mnt)
				break;
			tmp = tmp->mnt_parent;
		}
		if (!is_subdir(tmp->mnt_mountpoint, new.dentry))
			goto out3;
	} else if (!is_subdir(old.dentry, new.dentry))
		goto out3;
	detach_mnt(new.mnt, &parent_path);
	detach_mnt(root.mnt, &root_parent);
	/* mount old root on put_old */
	attach_mnt(root.mnt, &old);
	/* mount new_root on / */
	attach_mnt(new.mnt, &root_parent);
	touch_mnt_namespace(current->nsproxy->mnt_ns);
	spin_unlock(&vfsmount_lock);
	chroot_fs_refs(&root, &new);
	security_sb_post_pivotroot(&root, &new);
	error = 0;
	path_put(&root_parent);
	path_put(&parent_path);
out2:
	mutex_unlock(&old.dentry->d_inode->i_mutex);
	up_write(&namespace_sem);
	path_put(&root);
	path_put(&old);
out1:
	path_put(&new);
out0:
	return error;
out3:
	spin_unlock(&vfsmount_lock);
	goto out2;
}

static void __init init_mount_tree(void)
{
	struct vfsmount *mnt;
	struct mnt_namespace *ns;
	struct path root;

	mnt = do_kern_mount("rootfs", 0, "rootfs", NULL);
	if (IS_ERR(mnt))
		panic("Can't create rootfs");
	ns = kmalloc(sizeof(*ns), GFP_KERNEL);
	if (!ns)
		panic("Can't allocate initial namespace");
	atomic_set(&ns->count, 1);
	INIT_LIST_HEAD(&ns->list);
	init_waitqueue_head(&ns->poll);
	ns->event = 0;
	list_add(&mnt->mnt_list, &ns->list);
	//namespace的根vfsmount
	ns->root = mnt;
	mnt->mnt_ns = ns;

	init_task.nsproxy->mnt_ns = ns;
	get_mnt_ns(ns);

	root.mnt = ns->root;
	root.dentry = ns->root->mnt_root;

	set_fs_pwd(current->fs, &root);
	set_fs_root(current->fs, &root);
}

void __init mnt_init(void)
{
	unsigned u;
	int err;

	init_rwsem(&namespace_sem);

	mnt_cache = kmem_cache_create("mnt_cache", sizeof(struct vfsmount),
			0, SLAB_HWCACHE_ALIGN | SLAB_PANIC, NULL);

	//mount_hashtable只占用一个页面
	mount_hashtable = (struct list_head *)__get_free_page(GFP_ATOMIC);

	if (!mount_hashtable)
		panic("Failed to allocate mount hash table\n");

	printk("Mount-cache hash table entries: %lu\n", HASH_SIZE);

	for (u = 0; u < HASH_SIZE; u++)
		INIT_LIST_HEAD(&mount_hashtable[u]);

	err = sysfs_init();
	if (err)
		printk(KERN_WARNING "%s: sysfs_init error: %d\n",
			__func__, err);
	fs_kobj = kobject_create_and_add("fs", NULL);
	if (!fs_kobj)
		printk(KERN_WARNING "%s: kobj create error\n", __func__);
	init_rootfs();
	init_mount_tree();
}

void __put_mnt_ns(struct mnt_namespace *ns)
{
	struct vfsmount *root = ns->root;
	LIST_HEAD(umount_list);
	ns->root = NULL;
	spin_unlock(&vfsmount_lock);
	down_write(&namespace_sem);
	spin_lock(&vfsmount_lock);
	umount_tree(root, 0, &umount_list);
	spin_unlock(&vfsmount_lock);
	up_write(&namespace_sem);
	release_mounts(&umount_list);
	kfree(ns);
}
