/*
 *  linux/fs/pnode.h
 *
 * (C) Copyright IBM Corporation 2005.
 *	Released under GPL v2.
 *
 */
#ifndef _LINUX_PNODE_H
#define _LINUX_PNODE_H

#include <linux/list.h>
#include <linux/mount.h>

#define IS_MNT_SHARED(mnt) (mnt->mnt_flags & MNT_SHARED)
#define IS_MNT_SLAVE(mnt) (mnt->mnt_master)
#define IS_MNT_NEW(mnt)  (!mnt->mnt_ns)
#define CLEAR_MNT_SHARED(mnt) (mnt->mnt_flags &= ~MNT_SHARED)
#define IS_MNT_UNBINDABLE(mnt) (mnt->mnt_flags & MNT_UNBINDABLE)

//clone标志
#define CL_EXPIRE    		0x01
#define CL_SLAVE     		0x02
#define CL_COPY_ALL 		0x04
#define CL_MAKE_SHARED 		0x08
#define CL_PROPAGATION 		0x10
#define CL_PRIVATE 		0x20

//即使一个mnt是unbindable的,也会被给予共享属性
static inline void set_mnt_shared(struct vfsmount *mnt)
{
	//清空共享子树的所有属性 
	mnt->mnt_flags &= ~MNT_PNODE_MASK;
	//添加共享属性
	mnt->mnt_flags |= MNT_SHARED;
}

void change_mnt_propagation(struct vfsmount *, int);
int propagate_mnt(struct vfsmount *, struct dentry *, struct vfsmount *,
		struct list_head *);
int propagate_umount(struct list_head *);
int propagate_mount_busy(struct vfsmount *, int);
void mnt_release_group_id(struct vfsmount *);
int get_dominating_id(struct vfsmount *mnt, const struct path *root);
#endif /* _LINUX_PNODE_H */
