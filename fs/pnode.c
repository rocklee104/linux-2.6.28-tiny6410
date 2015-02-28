/*
 *  linux/fs/pnode.c
 *
 * (C) Copyright IBM Corporation 2005.
 *	Released under GPL v2.
 *	Author : Ram Pai (linuxram@us.ibm.com)
 *
 */
#include <linux/mnt_namespace.h>
#include <linux/mount.h>
#include <linux/fs.h>
#include "internal.h"
#include "pnode.h"

/* return the next shared peer mount of @p */
static inline struct vfsmount *next_peer(struct vfsmount *p)
{
	return list_entry(p->mnt_share.next, struct vfsmount, mnt_share);
}

static inline struct vfsmount *first_slave(struct vfsmount *p)
{
	return list_entry(p->mnt_slave_list.next, struct vfsmount, mnt_slave);
}

static inline struct vfsmount *next_slave(struct vfsmount *p)
{
	return list_entry(p->mnt_slave.next, struct vfsmount, mnt_slave);
}

/*
 * Return true if path is reachable from root
 *
 * namespace_sem is held, and mnt is attached
 */
static bool is_path_reachable(struct vfsmount *mnt, struct dentry *dentry,
			 const struct path *root)
{
	while (mnt != root->mnt && mnt->mnt_parent != mnt) {
		dentry = mnt->mnt_mountpoint;
		mnt = mnt->mnt_parent;
	}
	return mnt == root->mnt && is_subdir(dentry, root->dentry);
}

static struct vfsmount *get_peer_under_root(struct vfsmount *mnt,
					    struct mnt_namespace *ns,
					    const struct path *root)
{
	struct vfsmount *m = mnt;

	do {
		/* Check the namespace first for optimization */
		if (m->mnt_ns == ns && is_path_reachable(m, m->mnt_root, root))
			return m;

		m = next_peer(m);
	} while (m != mnt);

	return NULL;
}

/*
 * Get ID of closest dominating peer group having a representative
 * under the given root.
 *
 * Caller must hold namespace_sem
 */
int get_dominating_id(struct vfsmount *mnt, const struct path *root)
{
	struct vfsmount *m;

	for (m = mnt->mnt_master; m != NULL; m = m->mnt_master) {
		struct vfsmount *d = get_peer_under_root(m, mnt->mnt_ns, root);
		if (d)
			return d->mnt_group_id;
	}

	return 0;
}

static int do_make_slave(struct vfsmount *mnt)
{
	struct vfsmount *peer_mnt = mnt, *master = mnt->mnt_master;
	//slave_mount用于控制循环
	struct vfsmount *slave_mnt;

	/*
	 * slave 'mnt' to a peer mount that has the
	 * same root dentry. If none is available than
	 * slave it to anything that is available.
	 */
	//找到第一个peer,并且peer->mnt_root和mnt的相同
	while ((peer_mnt = next_peer(peer_mnt)) != mnt &&
	       peer_mnt->mnt_root != mnt->mnt_root) ;

	//如果mnt没有peer,或者其peer和mnt的mnt->mnt_root不相同
	if (peer_mnt == mnt) {
		peer_mnt = next_peer(mnt);
		if (peer_mnt == mnt)
			//确定没有peer
			peer_mnt = NULL;
	}
	if (IS_MNT_SHARED(mnt) && list_empty(&mnt->mnt_share))
		//虽然mnt有共享属性,但是mnt没有peer
		mnt_release_group_id(mnt);

	list_del_init(&mnt->mnt_share);
	mnt->mnt_group_id = 0;

	if (peer_mnt)
		//mnt如果设置成slave属性,它的peer就会变成其master
		master = peer_mnt;

	if (master) {
		list_for_each_entry(slave_mnt, &mnt->mnt_slave_list, mnt_slave)
			//mnt的slave的master指向mnt的peer
			slave_mnt->mnt_master = master;
		//将mnt加入其peer的管理slave的链表
		list_move(&mnt->mnt_slave, &master->mnt_slave_list);
		//将mnt的slave也加入其peer的管理slave的链表
		list_splice(&mnt->mnt_slave_list, master->mnt_slave_list.prev);
		//mnt不再拥有任何slave
		INIT_LIST_HEAD(&mnt->mnt_slave_list);
	} else {
		//如果mnt没有任何peer
		struct list_head *p = &mnt->mnt_slave_list;
		while (!list_empty(p)) {
			//释放mnt所有的slave,清空这些slave的mnt_slave
                        slave_mnt = list_first_entry(p,
					struct vfsmount, mnt_slave);
			list_del_init(&slave_mnt->mnt_slave);
			slave_mnt->mnt_master = NULL;
		}
	}
	//mnt的peer最终变成mnt的master
	mnt->mnt_master = master;
	//mnt不再拥有shared属性
	CLEAR_MNT_SHARED(mnt);
	return 0;
}

/*
 * type有下面4种可能：
 * 1.MS_SHARED:这种情况直接设置共享属性即可,mnt_flag置位MNT_SHARED.
 * 2.MS_SLAVE:如果A存在peer及slave,A及A的slave变成其第一个peer的slave;
 *  		  如果A没有peer,就将其slave的master置空;最后清除A的共享属性.
 * 3.MS_PRIVATE:如果A存在peer及slave,A的属性变为private,A的slave变成其第一个peer的slave;
 *  	      如果A没有peer,就将其slave的master置空;最后清除A的共享属性.
 * 4.MS_UNBINDABLE:如果A存在peer及slave,A的属性变为unbindable,A的slave变成其第一个peer的slave;
 * 		      如果A没有peer,就将其slave的master置空;最后清除A的共享属性,mnt_flag置位MNT_UNBINDABLE.
*/
void change_mnt_propagation(struct vfsmount *mnt, int type)
{
	if (type == MS_SHARED) {
		set_mnt_shared(mnt);
		return;
	}
	do_make_slave(mnt);
	//不可绑定挂载和私有挂载
	if (type != MS_SLAVE) {
		//将mnt从从属挂载中删除
		list_del_init(&mnt->mnt_slave);
		mnt->mnt_master = NULL;
		if (type == MS_UNBINDABLE)
			mnt->mnt_flags |= MNT_UNBINDABLE;
		else
			mnt->mnt_flags &= ~MNT_UNBINDABLE;
	}
}

/*
 * get the next mount in the propagation tree.
 * @m: the mount seen last
 * @origin: the original mount from where the tree walk initiated
 */
/* 
 * origin中的挂载能够传播的是origin的peer,origin的slave及slave的n级slave.
 * peer的slave及peer的n级slave.
 * 
 * NOTE:origin的slave的peer一定也是origin的slave.
 * 		如果m不在origin的propagation tree中,外部使用propagation_next循环就可能出现死循环
 */
static struct vfsmount *propagation_next(struct vfsmount *m,
					 struct vfsmount *origin)
{
	/* are there any slaves of this mount? */
	//先找最底层的first slave
	if (!IS_MNT_NEW(m) && !list_empty(&m->mnt_slave_list))
		//如果这个挂载不是new并且有slave,就返回第一个slave的mount
		return first_slave(m);

	while (1) {
		struct vfsmount *next;
		struct vfsmount *master = m->mnt_master;

		if (master == origin->mnt_master) {
            /* 
             * Note:以下m表示最开始调用propagation_next时传入的m,m'表示最后进入这个if语句中的m.
             * 
             * 如果m在slave tree中一直向master追溯,最终找到一个节点m'的master和origin的master
             * 指针指向同一个master,这时候m'和origin可能存在三种关系:
             *   1.m'->master和origin的master都为NULL,m'和origin是peer的关系
             *   2.m'->master和origin的master不为NULL,m'和origin都是其master的slave
             *   3.m'和origin都是master的slave并m'和origin同为peer
            */
            //如果是上述的第1,3种关系m的peer指向其peer, 如果是第2种,peer指向自己
			next = next_peer(m);
            //如果next == origin表示peer遍历完成
			return ((next == origin) ? NULL : next);
		} else if (m->mnt_slave.next != &master->mnt_slave_list)
            //如果m不是其master的mnt_slave_list链表中最后一个slave,就返回其master的下个slave
			return next_slave(m);

		/* back at master */
		m = master;
	}
}

/*
 * return the source mount to be used for cloning
 *
 * @dest 	the current destination mount
 * @last_dest  	the last seen destination mount
 * @last_src  	the last seen source mount
 * @type	return CL_SLAVE if the new mount has to be
 * 		cloned as a slave.
 */
/* 
 * 返回一个vfsmnt,供dest的peer clone, 被clone对象的要求:
 * 1.如果dest和last_dest是peer关系(slave & shared也属于这种情况),那么被clone的对象就是last_src.
 * 2.如果last_dest是dest的master,那么被clone的对象就是last_src.
 * 3.如果dest和last_dest仅仅只是slave,那么被clone的对象是master的那个last_src
*/
static struct vfsmount *get_source(struct vfsmount *dest,
					struct vfsmount *last_dest,
					struct vfsmount *last_src,
					int *type)
{
	struct vfsmount *p_last_src = NULL;
	struct vfsmount *p_last_dest = NULL;
	*type = CL_PROPAGATION;

	if (IS_MNT_SHARED(dest))
        /* 
         * 如果dest有共享标志,返回的type中也需要添加共享标志,接下使用copy_tree时,
         * 即使source中没有共享标志,在copy_tree返回的mnt也会有被设置此标志
         */
		*type |= CL_MAKE_SHARED;

	while (last_dest != dest->mnt_master) {
        //last_dest和dest是peer,或者last_dest和dest只是slave
		p_last_dest = last_dest;
		p_last_src = last_src;
		last_dest = last_dest->mnt_master;
		last_src = last_src->mnt_master;
	}

	if (p_last_dest) {
		do {
            //找到下一个不为new的peer
			p_last_dest = next_peer(p_last_dest);
		} while (IS_MNT_NEW(p_last_dest));
	}

	if (dest != p_last_dest) {
        /* 
         * 如果dest和p_last_dest不相等,说明dest和p_last_dest只是单纯的slave,不具有share属性.
		 * type设置CL_SLAVE标志,在后续clone_mnt调用时,新创建的mnt就是last_src的slave了  					  .
		 *  																								  .
		 * master下的子mnt也是这个master的slave对应子mnt的master											  .
         */
		*type |= CL_SLAVE;
		return last_src;
	} else
		return p_last_src;
}

/*
 * mount 'source_mnt' under the destination 'dest_mnt' at
 * dentry 'dest_dentry'. And propagate that mount to
 * all the peer and slave mounts of 'dest_mnt'.
 * Link all the new mounts into a propagation tree headed at
 * source_mnt. Also link all the new mounts using ->mnt_list
 * headed at source_mnt's ->mnt_list
 *
 * @dest_mnt: destination mount.
 * @dest_dentry: destination dentry.
 * @source_mnt: source mount.
 * @tree_list : list of heads of trees to be attached.
 */
//tree_list用于记录那些新生成并且用于propagate的mnt
int propagate_mnt(struct vfsmount *dest_mnt, struct dentry *dest_dentry,
		    struct vfsmount *source_mnt, struct list_head *tree_list)
{
	struct vfsmount *m, *child;
	int ret = 0;
	struct vfsmount *prev_dest_mnt = dest_mnt;
	struct vfsmount *prev_src_mnt  = source_mnt;
    /* 
     * tmp_list的作用: 记录需要释放的mnt,主要在一下两种情况下使用
     * 1.在copy tree时发生错误时,记录之前分配成功的mnt.
     * 2.在为propagate tree中一个节点分配mnt后,但是检测到这个节点不存在mnt需要挂载上的目录时,
     *   这时,这个新创建的mnt就需要释放,先把mnt暂存在tmp_list中
    */ 
	LIST_HEAD(tmp_list);
    /*
     * umount_list记录tmp_list中的成员及这个成员所有的子mnt,后续操作中,需要将这个链表中的成员全部释放 
    */
	LIST_HEAD(umount_list);

	for (m = propagation_next(dest_mnt, dest_mnt); m;
			m = propagation_next(m, dest_mnt)) {
		int type;
		struct vfsmount *source;

		if (IS_MNT_NEW(m))
			continue;

       /* 
        * 获取需要clone的mnt,如果prev_dest_mnt是m的peer,source就是prev_src_mnt.
        * 如果prev_dest_mnt是m的master,source就是prev_src_mnt.
        * 如果prev_dest_mnt和m仅仅是slave,那么需要找到其master的“prev_src_mnt”
        */
        source =  get_source(m, prev_dest_mnt, prev_src_mnt, &type);

        //clone source的整个mnt tree
		if (!(child = copy_tree(source, source->mnt_root, type))) {
			ret = -ENOMEM;
            //只要copy_tree失败一次,就需要将以前copy成功的mnt tree全部释放掉
			list_splice(tree_list, tmp_list.prev);
			goto out;
		}

		if (is_subdir(dest_dentry, m->mnt_root)) {
            /* 
             * 一般情况下m是dest_mnt的peer或者slave,这样的话m->mnt_root和dest_mnt->mnt_root指向
             * 同一个dentry,dest_dentry是dest_mnt->mnt_root的子目录,故dest_dentry也应该是m->mnt_root
             * 的子目录
             */

			mnt_set_mountpoint(m, dest_dentry, child);
            //添加到处理propagation的链表中
			list_add_tail(&child->mnt_hash, tree_list);
		} else {
			/*
			 * This can happen if the parent mount was bind mounted
			 * on some subdirectory of a shared/slave mount.
			 */
             /*
             * 当child的父mnt是由另一个共享/从属mnt通过绑定其子目录生成的,当父目录的传播树中出现一个新的子mnt,
             * 而这个子mnt的挂载点是在child的父mnt所在挂载点的统计目录或者上级目录,这时候child在其父mnt中就找不到
             * 对应的挂载点,这时候这个child就应该废弃
             */
			list_add_tail(&child->mnt_hash, &tmp_list);
		}
        //向下一个peer/slave移动
		prev_dest_mnt = m;
		prev_src_mnt  = child;
	}
out:
	spin_lock(&vfsmount_lock);
	while (!list_empty(&tmp_list)) {
		child = list_first_entry(&tmp_list, struct vfsmount, mnt_hash);
		umount_tree(child, 0, &umount_list);
	}
	spin_unlock(&vfsmount_lock);
	release_mounts(&umount_list);
	return ret;
}

/*
 * return true if the refcount is greater than count
 */
static inline int do_refcount_check(struct vfsmount *mnt, int count)
{
	int mycount = atomic_read(&mnt->mnt_count) - mnt->mnt_ghosts;
	return (mycount > count);
}

/*
 * check if the mount 'mnt' can be unmounted successfully.
 * @mnt: the mount to be checked for unmount
 * NOTE: unmounting 'mnt' would naturally propagate to all
 * other mounts its parent propagates to.
 * Check if any of these mounts that **do not have submounts**
 * have more references than 'refcnt'. If so return busy.
 */
int propagate_mount_busy(struct vfsmount *mnt, int refcnt)
{
	struct vfsmount *m, *child;
	struct vfsmount *parent = mnt->mnt_parent;
	int ret = 0;

	if (mnt == parent)
        //如果mnt是系统的根mnt
		return do_refcount_check(mnt, refcnt);

	/*
	 * quickly check if the current mount can be unmounted.
	 * If not, we don't have to go checking for all other
	 * mounts
	 */
	if (!list_empty(&mnt->mnt_mounts) || do_refcount_check(mnt, refcnt))
        //如果一个mnt具有子mnt,或者the refcount is greater than count,就返回busy
        return 1;

    //遍历父mnt所在的propagate tree
	for (m = propagation_next(parent, parent); m;
	     		m = propagation_next(m, parent)) {
        //找到mnt的父mnt的propagate tree中对应mnt的子mnt
		child = __lookup_mnt(m, mnt->mnt_mountpoint, 0);
        if (child && list_empty(&child->mnt_mounts) &&
		    (ret = do_refcount_check(child, 1)))
            /*
             * 当child(也就是参数mnt的父mnt的propagate tree中对应mnt的子mnt)下没有子mnt,
             * 但是计数大于1,返回busy.
             * 
             * 但如果child存在子mnt,就去寻找下一个propagate tree中的节点.在系统中的表现就是:
             * A, B, C同为peer.D是A的slave.Z,Z1,Z2,Z3分别是A,B,C,D的子mnt.E是Z3的子mnt.
             * 如果umount Z的时候,Z,Z1,Z2会被卸载掉,但是Z3会继续保留在系统中.最后这些mnt只留
             * 下了A,B,C,D,Z3,E
            */
			break;
	}
	return ret;
}

/*
 * NOTE: unmounting 'mnt' naturally propagates to all other mounts its
 * parent propagates to.
 */
static void __propagate_umount(struct vfsmount *mnt)
{
	struct vfsmount *parent = mnt->mnt_parent;
	struct vfsmount *m;

	BUG_ON(parent == mnt);

	for (m = propagation_next(parent, parent); m;
			m = propagation_next(m, parent)) {

		//遍历需要释放的mnt的parent的propagate tree,找到这些节点与mnt对应的子mnt 
		struct vfsmount *child = __lookup_mnt(m,
					mnt->mnt_mountpoint, 0);
		/*
		 * umount the child only if the child has no
		 * other children
		 */
		 /*
		  * 如果这些子mnt没有子mnt了,就将他们加入mnt->mnt_hash为首的链表中,
		  * 其实也就是propagate_umount的参数 -- list
		  */
		if (child && list_empty(&child->mnt_mounts))
			list_move_tail(&child->mnt_hash, &mnt->mnt_hash);
	}
}

/*
 * collect all mounts that receive propagation from the mount in @list,
 * and return these additional mounts in the same list.
 * @list: the list of mounts to be unmounted.
 */
int propagate_umount(struct list_head *list)
{
	struct vfsmount *mnt;

	//list中搜集了需要释放的mnt
	list_for_each_entry(mnt, list, mnt_hash)
        /* 
         * 遍历这些需要释放的mnt, 将这些mnt父目录的propagate tree中
         * 对应这些mnt的子mnt加入mnt_hash中,实际上也就是参数list中
         */
		__propagate_umount(mnt);
	return 0;
}
