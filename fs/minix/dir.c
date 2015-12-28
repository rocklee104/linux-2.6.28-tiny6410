/*
 *  linux/fs/minix/dir.c
 *
 *  Copyright (C) 1991, 1992 Linus Torvalds
 *
 *  minix directory handling functions
 *
 *  Updated to filesystem version 3 by Daniel Aragones
 */

#include "minix.h"
#include <linux/buffer_head.h>
#include <linux/highmem.h>
#include <linux/smp_lock.h>
#include <linux/swap.h>

typedef struct minix_dir_entry minix_dirent;
typedef struct minix3_dir_entry minix3_dirent;

static int minix_readdir(struct file *, void *, filldir_t);

const struct file_operations minix_dir_operations = {
	.read		= generic_read_dir,
	.readdir	= minix_readdir,
	.fsync		= minix_sync_file,
};

static inline void dir_put_page(struct page *page)
{
	kunmap(page);
	page_cache_release(page);
}

/*
 * Return the offset into page `page_nr' of the last valid
 * byte in that page, plus one.
 */
/* 返回第page_nr个page的最后一个有效的字节,page_nr从0开始 */
static unsigned
minix_last_byte(struct inode *inode, unsigned long page_nr)
{
	unsigned last_byte = PAGE_CACHE_SIZE;

	if (page_nr == (inode->i_size >> PAGE_CACHE_SHIFT))
		last_byte = inode->i_size & (PAGE_CACHE_SIZE - 1);
	return last_byte;
}

/* 目录占用多少个page */
static inline unsigned long dir_pages(struct inode *inode)
{
	return (inode->i_size+PAGE_CACHE_SIZE-1)>>PAGE_CACHE_SHIFT;
}

static int dir_commit_chunk(struct page *page, loff_t pos, unsigned len)
{
	struct address_space *mapping = page->mapping;
	struct inode *dir = mapping->host;
	int err = 0;
	block_write_end(NULL, mapping, pos, len, len, page, NULL);

	if (pos+len > dir->i_size) {
		i_size_write(dir, pos+len);
		mark_inode_dirty(dir);
	}
	if (IS_DIRSYNC(dir))
		err = write_one_page(page, 1);
	else
		unlock_page(page);
	return err;
}

static struct page * dir_get_page(struct inode *dir, unsigned long n)
{
	struct address_space *mapping = dir->i_mapping;
	struct page *page = read_mapping_page(mapping, n, NULL);
	if (!IS_ERR(page)) {
		kmap(page);
		if (!PageUptodate(page))
			goto fail;
	}
	return page;

fail:
	dir_put_page(page);
	return ERR_PTR(-EIO);
}

static inline void *minix_next_entry(void *de, struct minix_sb_info *sbi)
{
	return (void*)((char*)de + sbi->s_dirsize);
}

/* 目录inode的大小= 目录中的文件(或目录)个数 * 32(v1 filename 30) */
static int minix_readdir(struct file * filp, void * dirent, filldir_t filldir)
{
	unsigned long pos = filp->f_pos;
	/* 从filp中取出inode */
	struct inode *inode = filp->f_path.dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	/* 页内偏移 */
	unsigned offset = pos & ~PAGE_CACHE_MASK;
	/* 起始位置是从第几个page开始的 */
	unsigned long n = pos >> PAGE_CACHE_SHIFT;
	/* 目录占用的page个数 */
	unsigned long npages = dir_pages(inode);
	struct minix_sb_info *sbi = minix_sb(sb);
	/* 磁盘上的目录项大小 */
	unsigned chunk_size = sbi->s_dirsize;
	char *name;
	__u32 inumber;

	lock_kernel();

	/* pos以目录项大小对齐 */
	pos = (pos + chunk_size-1) & ~(chunk_size-1);
	/* 文件指针的位置不能超过目录大小 */
	if (pos >= inode->i_size)
		goto done;

	/* 遍历所有page */
	for ( ; n < npages; n++, offset = 0) {
		char *p, *kaddr, *limit;
		/* 从inode地址空间中获取第n个page */
		struct page *page = dir_get_page(inode, n);

		if (IS_ERR(page))
			continue;
		/* page转换成虚拟地址 */
		kaddr = (char *)page_address(page);
		p = kaddr+offset;
		limit = kaddr + minix_last_byte(inode, n) - chunk_size;
		/* 遍历page中所有的目录项 */
		for ( ; p <= limit; p = minix_next_entry(p, sbi)) {
			if (sbi->s_version == MINIX_V3) {
				minix3_dirent *de3 = (minix3_dirent *)p;
				name = de3->name;
				inumber = de3->inode;
	 		} else {
				minix_dirent *de = (minix_dirent *)p;
				name = de->name;
				inumber = de->inode;
			}
			/* 有效的目录项 */
			if (inumber) {
				int over;

				unsigned l = strnlen(name, sbi->s_namelen);
				offset = p - kaddr;
				/* 填充用户空间的linux_dirent数组,dirent在vfs_readdir中已经被赋值了,minix不支持file_type */
				over = filldir(dirent, name, l,
					(n << PAGE_CACHE_SHIFT) | offset,
					inumber, DT_UNKNOWN);
				if (over) {
					dir_put_page(page);
					goto done;
				}
			}
		}
		dir_put_page(page);
	}

done:
	filp->f_pos = (n << PAGE_CACHE_SHIFT) | offset;
	unlock_kernel();
	return 0;
}

static inline int namecompare(int len, int maxlen,
	const char * name, const char * buffer)
{
	if (len < maxlen && buffer[len])
		return 0;
	return !memcmp(name, buffer, len);
}

/*
 *	minix_find_entry()
 *
 * finds an entry in the specified directory with the wanted name. It
 * returns the cache buffer in which the entry was found, and the entry
 * itself (as a parameter - res_dir). It does NOT read the inode of the
 * entry - you'll have to do that yourself if you want to.
 */
minix_dirent *minix_find_entry(struct dentry *dentry, struct page **res_page)
{
	const char * name = dentry->d_name.name;
	int namelen = dentry->d_name.len;
	/* 目标文件父目录的inode */
	struct inode * dir = dentry->d_parent->d_inode;
	struct super_block * sb = dir->i_sb;
	struct minix_sb_info * sbi = minix_sb(sb);
	unsigned long n;
	/* 父目录的占用的page个数 */
	unsigned long npages = dir_pages(dir);
	struct page *page = NULL;
	char *p;

	char *namx;
	__u32 inumber;
	*res_page = NULL;

	/* 遍历父目录的page */
	for (n = 0; n < npages; n++) {
		char *kaddr, *limit;

		page = dir_get_page(dir, n);
		if (IS_ERR(page))
			continue;

		kaddr = (char*)page_address(page);
		limit = kaddr + minix_last_byte(dir, n) - sbi->s_dirsize;
		/* 遍历page中的每个目录项 */
		for (p = kaddr; p <= limit; p = minix_next_entry(p, sbi)) {
			if (sbi->s_version == MINIX_V3) {
				minix3_dirent *de3 = (minix3_dirent *)p;
				namx = de3->name;
				inumber = de3->inode;
 			} else {
				minix_dirent *de = (minix_dirent *)p;
				namx = de->name;
				inumber = de->inode;
			}
			if (!inumber)
				continue;
			if (namecompare(namelen, sbi->s_namelen, name, namx))
				goto found;
		}
		dir_put_page(page);
	}
	return NULL;

found:
	/* 获取到含有目标目录项的page */
	*res_page = page;
	/* 返回目标目录项 */
	return (minix_dirent *)p;
}

/* 在目录中增加一个目录项,dentry和inode代表同一个文件,要插入dentry的parent下 */
int minix_add_link(struct dentry *dentry, struct inode *inode)
{
	struct inode *dir = dentry->d_parent->d_inode;
	const char * name = dentry->d_name.name;
	int namelen = dentry->d_name.len;
	struct super_block * sb = dir->i_sb;
	struct minix_sb_info * sbi = minix_sb(sb);
	struct page *page = NULL;
	unsigned long npages = dir_pages(dir);
	unsigned long n;
	char *kaddr, *p;
	minix_dirent *de;
	minix3_dirent *de3;
	loff_t pos;
	int err;
	char *namx = NULL;
	__u32 inumber;

	/*
	 * We take care of directory expansion in the same loop
	 * This code plays outside i_size, so it locks the page
	 * to protect that region.
	 */
	/* 遍历dentry父目录地址空间中的所有page */
	for (n = 0; n <= npages; n++) {
		char *limit, *dir_end;

		page = dir_get_page(dir, n);
		err = PTR_ERR(page);
		if (IS_ERR(page))
			goto out;
		lock_page(page);
		kaddr = (char*)page_address(page);
		/* 每一个page中有效数据之后第一个无效数据 */
		dir_end = kaddr + minix_last_byte(dir, n);
		/* 每一个page最后一个目录项 */
		limit = kaddr + PAGE_CACHE_SIZE - sbi->s_dirsize;
		/* 遍历每个page中的目录项 */
		for (p = kaddr; p <= limit; p = minix_next_entry(p, sbi)) {
			de = (minix_dirent *)p;
			de3 = (minix3_dirent *)p;
			if (sbi->s_version == MINIX_V3) {
				namx = de3->name;
				inumber = de3->inode;
		 	} else {
  				namx = de->name;
				inumber = de->inode;
			}
			if (p == dir_end) {
				/*
				 * 目录项的起始地址等于当前page中有效数据之后第一个无效数据,
				 * 说明当前这个目录项是当前目录第一个无效的目录,也就是我们要插入的位置.
				 */
				/* We hit i_size */
				if (sbi->s_version == MINIX_V3)
					de3->inode = 0;
		 		else
					de->inode = 0;
				goto got_it;
			}
			/* ino为0表示这个目录项无效,可能之前这里有个目录项,但是被删除了，然后空出来的位置 */
			if (!inumber)
				goto got_it;
			err = -EEXIST;
			/* 判断要插入的文件是否已经在其父目录下 */
			if (namecompare(namelen, sbi->s_namelen, name, namx))
				goto out_unlock;
		}
		unlock_page(page);
		dir_put_page(page);
	}
	BUG();
	return -EINVAL;

got_it:
	/*
	 * page_offset(page)获取到page起始地址在地址空间的位置.
	 * p - (char *)page_address(page)得到在page中的偏移.
	 */
	pos = page_offset(page) + p - (char *)page_address(page);
	/* 没弄懂什么意思 */
	err = __minix_write_begin(NULL, page->mapping, pos, sbi->s_dirsize,
					AOP_FLAG_UNINTERRUPTIBLE, &page, NULL);
	if (err)
		goto out_unlock;
	/* 相当于给de->name或者de3->name赋值 */
	memcpy (namx, name, namelen);
	if (sbi->s_version == MINIX_V3) {
		/* 将de3->name之后的字符清除 */
		memset (namx + namelen, 0, sbi->s_dirsize - namelen - 4);
		de3->inode = inode->i_ino;
	} else {
		/* 将de->name之后的字符清除 */
		memset (namx + namelen, 0, sbi->s_dirsize - namelen - 2);
		de->inode = inode->i_ino;
	}
	/* 将目录项写入磁盘 */
	err = dir_commit_chunk(page, pos, sbi->s_dirsize);
	/* 更新父目录的时间戳 */
	dir->i_mtime = dir->i_ctime = CURRENT_TIME_SEC;
	/* 将父目录的inode标记为dirty */
	mark_inode_dirty(dir);
out_put:
	dir_put_page(page);
out:
	return err;
out_unlock:
	unlock_page(page);
	goto out_put;
}

int minix_delete_entry(struct minix_dir_entry *de, struct page *page)
{
	struct address_space *mapping = page->mapping;
	struct inode *inode = (struct inode*)mapping->host;
	char *kaddr = page_address(page);
	/* de在地址空间的相对位置 */
	loff_t pos = page_offset(page) + (char*)de - kaddr;
	unsigned len = minix_sb(inode->i_sb)->s_dirsize;
	int err;

	lock_page(page);
	err = __minix_write_begin(NULL, mapping, pos, len,
					AOP_FLAG_UNINTERRUPTIBLE, &page, NULL);
	if (err == 0) {
		/* 将inode number改为0后写入磁盘 */
		de->inode = 0;
		err = dir_commit_chunk(page, pos, len);
	} else {
		unlock_page(page);
	}
	dir_put_page(page);
	/* 更新文件访问时间和修改时间 */
	inode->i_ctime = inode->i_mtime = CURRENT_TIME_SEC;
	mark_inode_dirty(inode);
	return err;
}

int minix_make_empty(struct inode *inode, struct inode *dir)
{
	struct address_space *mapping = inode->i_mapping;
	/* 目录是空的,先获取一个page */
	struct page *page = grab_cache_page(mapping, 0);
	struct minix_sb_info *sbi = minix_sb(inode->i_sb);
	char *kaddr;
	int err;

	if (!page)
		return -ENOMEM;
	/* 需要写入2个目录项,__minix_write_begin将会获取要写入的block */
	err = __minix_write_begin(NULL, mapping, 0, 2 * sbi->s_dirsize,
					AOP_FLAG_UNINTERRUPTIBLE, &page, NULL);
	if (err) {
		unlock_page(page);
		goto fail;
	}

	kaddr = kmap_atomic(page, KM_USER0);
	memset(kaddr, 0, PAGE_CACHE_SIZE);

	if (sbi->s_version == MINIX_V3) {
		minix3_dirent *de3 = (minix3_dirent *)kaddr;

		de3->inode = inode->i_ino;
		strcpy(de3->name, ".");
		de3 = minix_next_entry(de3, sbi);
		/* 记录父目录的目录项 */
		de3->inode = dir->i_ino;
		strcpy(de3->name, "..");
	} else {
		minix_dirent *de = (minix_dirent *)kaddr;

		de->inode = inode->i_ino;
		strcpy(de->name, ".");
		de = minix_next_entry(de, sbi);
		de->inode = dir->i_ino;
		strcpy(de->name, "..");
	}
	kunmap_atomic(kaddr, KM_USER0);

	/* 将写入的目录项提交给磁盘 */
	err = dir_commit_chunk(page, 0, 2 * sbi->s_dirsize);
fail:
	page_cache_release(page);
	return err;
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
/* 检查目录是否为空,当目录数据块中只有".",".."的目录项时,目录为空 */
int minix_empty_dir(struct inode * inode)
{
	struct page *page = NULL;
	unsigned long i, npages = dir_pages(inode);
	struct minix_sb_info *sbi = minix_sb(inode->i_sb);
	char *name;
	__u32 inumber;

	for (i = 0; i < npages; i++) {
		char *p, *kaddr, *limit;

		page = dir_get_page(inode, i);
		if (IS_ERR(page))
			continue;

		kaddr = (char *)page_address(page);
		limit = kaddr + minix_last_byte(inode, i) - sbi->s_dirsize;
		for (p = kaddr; p <= limit; p = minix_next_entry(p, sbi)) {
			if (sbi->s_version == MINIX_V3) {
				minix3_dirent *de3 = (minix3_dirent *)p;
				name = de3->name;
				inumber = de3->inode;
			} else {
				minix_dirent *de = (minix_dirent *)p;
				name = de->name;
				inumber = de->inode;
			}

			/* 有效inode */
			if (inumber != 0) {
				/* check for . and .. */
				if (name[0] != '.')
					goto not_empty;

				if (!name[1]) {
					/* 当目录中存在".",但是这个目录(或者文件)的inode number和其父目录不同,父目录非空 */
					if (inumber != inode->i_ino)
						goto not_empty;
				/* 当文件名以"."开头,但是文件名不是"..",目录非空 */
				} else if (name[1] != '.')
					goto not_empty;
				else if (name[2])
					goto not_empty;
			}
		}
		dir_put_page(page);
	}
	return 1;

not_empty:
	dir_put_page(page);
	return 0;
}

/* Releases the page */
void minix_set_link(struct minix_dir_entry *de, struct page *page,
	struct inode *inode)
{
	struct address_space *mapping = page->mapping;
	struct inode *dir = mapping->host;
	struct minix_sb_info *sbi = minix_sb(dir->i_sb);
	loff_t pos = page_offset(page) +
			(char *)de-(char*)page_address(page);
	int err;

	lock_page(page);

	err = __minix_write_begin(NULL, mapping, pos, sbi->s_dirsize,
					AOP_FLAG_UNINTERRUPTIBLE, &page, NULL);
	if (err == 0) {
		de->inode = inode->i_ino;
		err = dir_commit_chunk(page, pos, sbi->s_dirsize);
	} else {
		unlock_page(page);
	}
	dir_put_page(page);
	dir->i_mtime = dir->i_ctime = CURRENT_TIME_SEC;
	mark_inode_dirty(dir);
}

/* 返回dir这个目录中的".."指向的目录项 */
struct minix_dir_entry * minix_dotdot (struct inode *dir, struct page **p)
{
	struct page *page = dir_get_page(dir, 0);
	struct minix_sb_info *sbi = minix_sb(dir->i_sb);
	struct minix_dir_entry *de = NULL;

	if (!IS_ERR(page)) {
		de = minix_next_entry(page_address(page), sbi);
		*p = page;
	}
	return de;
}

/* 从vfs dentry找到minix dentry,最后获取到inode number */
ino_t minix_inode_by_name(struct dentry *dentry)
{
	struct page *page;
	/* 通过vfs dentry 获取到minix 目录项 */
	struct minix_dir_entry *de = minix_find_entry(dentry, &page);
	ino_t res = 0;

	if (de) {
		/* 从minix目录项中获取到inode number后就可以释放含有minix目录项的page了 */
		res = de->inode;
		dir_put_page(page);
	}
	return res;
}
