#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/minix_fs.h>

#define INODE_VERSION(inode)	minix_sb(inode->i_sb)->s_version
#define MINIX_V1		0x0001		/* original minix fs */
#define MINIX_V2		0x0002		/* minix V2 fs */
#define MINIX_V3		0x0003		/* minix V3 fs */

/*
 * minix fs inode data in memory
 */
struct minix_inode_info {
	union {
		/* v1 inode记录的数据块 */
		__u16 i1_data[16];
		__u32 i2_data[16];
	} u;
	/* 指向vfs中的inode */
	struct inode vfs_inode;
};

/*
 * minix super-block data in memory
 */
struct minix_sb_info {
	unsigned long s_ninodes;
	/* 含有zone的个数,对于v1 sb的s_nzones,对应v2 sb的s_zones */
	unsigned long s_nzones;
	unsigned long s_imap_blocks;
	unsigned long s_zmap_blocks;
	unsigned long s_firstdatazone;
	unsigned long s_log_zone_size;
	unsigned long s_max_size;
	/* 目录大小 */
	int s_dirsize;
	/* 文件名长度 */
	int s_namelen;
	/* 硬链接个数上限 */
	int s_link_max;
	/* 指针数组,成员是imap中每一个block的buffer指针 */
	struct buffer_head ** s_imap;
	/* 指针数组,成员是zmap中每一个block的buffer指针 */
	struct buffer_head ** s_zmap;
	/* 指向含有super block的buffer */
	struct buffer_head * s_sbh;
	struct minix_super_block * s_ms;
	/* 挂载状态 */
	unsigned short s_mount_state;
	/* minix fs版本号 */
	unsigned short s_version;
};

/* 获取vfs的inode */
extern struct inode *minix_iget(struct super_block *, unsigned long);
/* 获取minix fs的inode */
extern struct minix_inode * minix_V1_raw_inode(struct super_block *, ino_t, struct buffer_head **);
extern struct minix2_inode * minix_V2_raw_inode(struct super_block *, ino_t, struct buffer_head **);
/* 给vfs层分配一个inode */
extern struct inode * minix_new_inode(const struct inode * dir, int * error);
/* vfs调用的释放inode */
extern void minix_free_inode(struct inode * inode);
/* 统计minix fs中的inode个数 */
extern unsigned long minix_count_free_inodes(struct minix_sb_info *sbi);
extern int minix_new_block(struct inode * inode);
extern void minix_free_block(struct inode *inode, unsigned long block);
/* 统计minix fs中的block个数 */
extern unsigned long minix_count_free_blocks(struct minix_sb_info *sbi);
extern int minix_getattr(struct vfsmount *, struct dentry *, struct kstat *);
extern int __minix_write_begin(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned flags,
			struct page **pagep, void **fsdata);

extern void V1_minix_truncate(struct inode *);
extern void V2_minix_truncate(struct inode *);
extern void minix_truncate(struct inode *);
extern int minix_sync_inode(struct inode *);
extern void minix_set_inode(struct inode *, dev_t);
extern int V1_minix_get_block(struct inode *, long, struct buffer_head *, int);
extern int V2_minix_get_block(struct inode *, long, struct buffer_head *, int);
extern unsigned V1_minix_blocks(loff_t, struct super_block *);
extern unsigned V2_minix_blocks(loff_t, struct super_block *);

extern struct minix_dir_entry *minix_find_entry(struct dentry*, struct page**);
extern int minix_add_link(struct dentry*, struct inode*);
extern int minix_delete_entry(struct minix_dir_entry*, struct page*);
extern int minix_make_empty(struct inode*, struct inode*);
extern int minix_empty_dir(struct inode*);
extern void minix_set_link(struct minix_dir_entry*, struct page*, struct inode*);
extern struct minix_dir_entry *minix_dotdot(struct inode*, struct page**);
extern ino_t minix_inode_by_name(struct dentry*);
extern int minix_sync_file(struct file *, struct dentry *, int);

extern const struct inode_operations minix_file_inode_operations;
extern const struct inode_operations minix_dir_inode_operations;
extern const struct file_operations minix_file_operations;
extern const struct file_operations minix_dir_operations;

/* 通过super block获取minix super block */
static inline struct minix_sb_info *minix_sb(struct super_block *sb)
{
	return sb->s_fs_info;
}

/* 通过inode获取minix inode info */
static inline struct minix_inode_info *minix_i(struct inode *inode)
{
	return list_entry(inode, struct minix_inode_info, vfs_inode);
}
