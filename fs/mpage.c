/*
 * fs/mpage.c
 *
 * Copyright (C) 2002, Linus Torvalds.
 *
 * Contains functions related to preparing and submitting BIOs which contain
 * multiple pagecache pages.
 *
 * 15May2002	Andrew Morton
 *		Initial version
 * 27Jun2002	axboe@suse.de
 *		use bio_add_page() to build bio's just the right size
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/kdev_t.h>
#include <linux/bio.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/highmem.h>
#include <linux/prefetch.h>
#include <linux/mpage.h>
#include <linux/writeback.h>
#include <linux/backing-dev.h>
#include <linux/pagevec.h>

/*
 * I/O completion handler for multipage BIOs.
 *
 * The mpage code never puts partial pages into a BIO (except for end-of-file).
 * If a page does not map to a contiguous run of blocks then it simply falls
 * back to block_read_full_page().
 *
 * Why is this?  If a page's completion depends on a number of different BIOs
 * which can complete in any order (or at the same time) then determining the
 * status of that page is hard.  See end_buffer_async_read() for the details.
 * There is no point in duplicating all that complexity.
 */
static void mpage_end_io_read(struct bio *bio, int err)
{
	const int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	struct bio_vec *bvec = bio->bi_io_vec + bio->bi_vcnt - 1;

	do {
		struct page *page = bvec->bv_page;

		if (--bvec >= bio->bi_io_vec)
			prefetchw(&bvec->bv_page->flags);

		if (uptodate) {
			SetPageUptodate(page);
		} else {
			ClearPageUptodate(page);
			SetPageError(page);
		}
		unlock_page(page);
	} while (bvec >= bio->bi_io_vec);
	bio_put(bio);
}

static void mpage_end_io_write(struct bio *bio, int err)
{
	const int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	struct bio_vec *bvec = bio->bi_io_vec + bio->bi_vcnt - 1;

	do {
		struct page *page = bvec->bv_page;

		if (--bvec >= bio->bi_io_vec)
			prefetchw(&bvec->bv_page->flags);

		if (!uptodate){
			SetPageError(page);
			if (page->mapping)
				set_bit(AS_EIO, &page->mapping->flags);
		}
		end_page_writeback(page);
	} while (bvec >= bio->bi_io_vec);
	bio_put(bio);
}

struct bio *mpage_bio_submit(int rw, struct bio *bio)
{
	bio->bi_end_io = mpage_end_io_read;
	if (rw == WRITE)
		bio->bi_end_io = mpage_end_io_write;
	submit_bio(rw, bio);
	return NULL;
}
EXPORT_SYMBOL(mpage_bio_submit);

static struct bio *
mpage_alloc(struct block_device *bdev,
		sector_t first_sector, int nr_vecs,
		gfp_t gfp_flags)
{
	struct bio *bio;

	bio = bio_alloc(gfp_flags, nr_vecs);

	if (bio == NULL && (current->flags & PF_MEMALLOC)) {
		while (!bio && (nr_vecs /= 2))
			bio = bio_alloc(gfp_flags, nr_vecs);
	}

	if (bio) {
		bio->bi_bdev = bdev;
		bio->bi_sector = first_sector;
	}
	return bio;
}

/*
 * support function for mpage_readpages.  The fs supplied get_block might
 * return an up to date buffer.  This is used to map that buffer into
 * the page, which allows readpage to avoid triggering a duplicate call
 * to get_block.
 *
 * The idea is to avoid adding buffers to pages that don't already have
 * them.  So when the buffer is up to date and the page size == block size,
 * this marks the page up to date instead of adding new buffers.
 */
static void 
map_buffer_to_page(struct page *page, struct buffer_head *bh, int page_block) 
{
	struct inode *inode = page->mapping->host;
	struct buffer_head *page_bh, *head;
	int block = 0;

	if (!page_has_buffers(page)) {
		/*
		 * don't make any buffers if there is only one buffer on
		 * the page and the page just needs to be set up to date
		 */
		if (inode->i_blkbits == PAGE_CACHE_SHIFT && 
		    buffer_uptodate(bh)) {
            /*
             *如果这个buffer的大小刚好是一个page，并且buffer is uptodate, 
             *就将这个page设置成uptodate 
            */
			SetPageUptodate(page);    
			return;
		}
		create_empty_buffers(page, 1 << inode->i_blkbits, 0);
	}
	head = page_buffers(page);
	page_bh = head;
	do {
		if (block == page_block) {
			page_bh->b_state = bh->b_state;
			page_bh->b_bdev = bh->b_bdev;
			page_bh->b_blocknr = bh->b_blocknr;
			break;
		}
		page_bh = page_bh->b_this_page;
		block++;
	} while (page_bh != head);
}

/*
 * This is the worker routine which does all the work of mapping the disk
 * blocks and constructs largest possible bios, submits them for IO if the
 * blocks are not contiguous on the disk.
 *
 * We pass a buffer_head back and forth and use its buffer_mapped() flag to
 * represent the validity of its disk mapping and to decide when to do the next
 * get_block() call.
 */

/*
 * 这个函数试图读取文件中的一个page大小的数据,最理想的情况下就是这个page大小
 * 的数据都是在连续的物理磁盘上面的,然后函数只需要提交一个bio请求就可以获取
 * 所有的数据,这个函数大部分工作在检查page上所有的物理块是否连续,检查的方法
 * 就是调用文件系统提供的get_block函数,如果不连续,需要调用block_read_full_page
 * 函数采用buffer缓冲区的形式来逐个块获取数据
 */
/* 
 * 1、调用get_block函数检查page中是不是所有的物理块都连续
 * 2、如果连续调用mpage_bio_submit函数请求整个page的数据
 * 3、如果不连续调用block_read_full_page逐个block读取
 */
/*
 * 参数说明:
 * bio:指向通用块层请求描述符的指针,可能将多个页面组织在一个bio中提交.
 *
 * nr_pages:剩余的页面数目,这主要用在多个页面的场合下,指导分配包含多少个请求段的bio.
 *		实际上,一次调用do_mpage_readpage,只处理一个页面.
 *
 * last_block_in_bio:输入/输出参数,仅在bio不为NULL时有效,记录它在磁盘上最后一个逻辑块的编号,
 *		处理新的页面时可根据它来判断是否和以前的请求在磁盘上连续.
 *
 * map_bh:指向用于实现从文件逻辑块编号到磁盘逻辑块编号映射到缓冲头描述符的指针.
 *
 * first_logical_block:输入输出参数,仅在map_bh被映射时有效,表示映射的第一个文件逻辑块的编号.
 *		映射长度保存在缓冲头描述符的b_size域.
 *
 * 在构造的bio已提交,或者采用缓冲页面方式处理的情况下,返回NUL
 */
static struct bio *
do_mpage_readpage(struct bio *bio, struct page *page, unsigned nr_pages,
		sector_t *last_block_in_bio, struct buffer_head *map_bh,
		unsigned long *first_logical_block, get_block_t get_block)
{
	struct inode *inode = page->mapping->host;
	/* 逻辑块的位数 */
	const unsigned blkbits = inode->i_blkbits;
    /* 每个页面包含逻辑块的数目 */
	const unsigned blocks_per_page = PAGE_CACHE_SIZE >> blkbits;
	/* 逻辑块的字节数 */
	const unsigned blocksize = 1 << blkbits;
	/* 在文件中逻辑块的编号 */
	sector_t block_in_file;
	/* 页面最后一个字节的逻辑块编号 */
	sector_t last_block;
	/* 文件最后一个逻辑块编号 */
	sector_t last_block_in_file;
	/*
	 * 映射数组,数组元素个数取决于每个页面包含的逻辑块数目,
	 * 每个元素表示这个页面给定索引的逻辑块对应的磁盘块编号
	 */
	sector_t blocks[MAX_BUF_PER_PAGE];
	/* 页面中正在处理的逻辑块编号 */
	unsigned page_block;
	/* 空洞起始位置在页面对应的数据块编号 */
	unsigned first_hole = blocks_per_page;
	struct block_device *bdev = NULL;
	int length;
	int fully_mapped = 1;
	/* 每个映射数据块所包含的逻辑块的数目 */
	unsigned nblocks;
	unsigned relative_block;

	/* 当前函数针对页面构造IO,而非基于缓冲页面构造IO */
	if (page_has_buffers(page))
		goto confused;

    /* 当前中的第一个block number */
	block_in_file = (sector_t)page->index << (PAGE_CACHE_SHIFT - blkbits);
    /* 本次传输的最后一个block */
	last_block = block_in_file + nr_pages * blocks_per_page;
    /* 文件最后一个block */
	last_block_in_file = (i_size_read(inode) + blocksize - 1) >> blkbits;
	if (last_block > last_block_in_file)
		last_block = last_block_in_file;
	page_block = 0;

	/*
	 * Map blocks using the result from the previous get_blocks call first.
	 */
	/* 每个映射数据块所包含的逻辑块的数目 */
	nblocks = map_bh->b_size >> blkbits;
	/*
	 * 本函数调用时,可能前面的调用已有的有效的映射关系可继续使用.对于在该范围内
	 * 的文件逻辑块编号,将对应的磁盘上逻辑块编号记录在blocks数组中.如果映射全部用光,
	 * 清除缓冲头的映射标志,以便下次重新调用get_block回调函数获取后面的映射信息.
	 */
	if (buffer_mapped(map_bh) && block_in_file > *first_logical_block &&
			block_in_file < (*first_logical_block + nblocks)) {
		unsigned map_offset = block_in_file - *first_logical_block;
		unsigned last = nblocks - map_offset;

		for (relative_block = 0; ; relative_block++) {
			if (relative_block == last) {
				clear_buffer_mapped(map_bh);
				break;
			}
			if (page_block == blocks_per_page)
				break;
			blocks[page_block] = map_bh->b_blocknr + map_offset +
						relative_block;
			page_block++;
			block_in_file++;
		}
		bdev = map_bh->b_bdev;
	}

	/*
	 * Then do more get_blocks calls until we are done with this page.
	 */
	map_bh->b_page = page;
    /*
    1、page_block从0开始循环，它表示在这个page内的block index 
    2、调用get_block 函数查找对应逻辑块的物理块号是多少 
    3、如果遇到了文件空洞、page上的物理块不连续就会跳转到confused 
    4、将这个page中每个逻辑块对应的物理块都保存到临时的数组blocks[]中 
    */ 
	while (page_block < blocks_per_page) {
		map_bh->b_state = 0;
		map_bh->b_size = 0;

		if (block_in_file < last_block) {
            //如果当前page中第一个block小于本次传输的最后一个block
			map_bh->b_size = (last_block-block_in_file) << blkbits;
			if (get_block(inode, block_in_file, map_bh, 0))
				goto confused;
			*first_logical_block = block_in_file;
		}

		if (!buffer_mapped(map_bh)) {
		   /*
			* 有两种情况会进入这个条件判断中:
			* 1.文件存在空洞,block_in_file < last_block,调用了get_block但是没有获取到映射.
			* 2.文件大小未以page size对齐,page中只有部分逻辑块属于文件.
			*/
			fully_mapped = 0;
			if (first_hole == blocks_per_page)
                //只有第一次调用时会修改first_hole,后续调用不会修改这个值。
				first_hole = page_block;
			page_block++;
			block_in_file++;
			clear_buffer_mapped(map_bh);
			continue;
		}

		/* some filesystems will copy data into the page during
		 * the get_block call, in which case we don't want to
		 * read it again.  map_buffer_to_page copies the data
		 * we just collected from get_block into the page's buffers
		 * so readpage doesn't have to repeat the get_block call
		 */
        /*
         * 检查buffer_head是否为buffer_uptodate,因为个别fs会在get_block期间将整页读出来,
         * 而我们在下面也会读出数据到buffer中,为了避免重复的读取,这里需要再次检查buffer标志
         * 是否最新
         */
		if (buffer_uptodate(map_bh)) {
			/* 建立页面内的buffer */
			map_buffer_to_page(page, map_bh, page_block);
			goto confused;
		}

         /*
          * 这个时候,缓冲头已经有映射.如果first_hole已经设置,说明这个页面在经过一个"空洞"
          * 后又重新被映射,没有办法用bio方式来提交,跳转到confused标号处
          */
		if (first_hole != blocks_per_page)
			goto confused;		/* hole -> non-hole */

		/* Contiguous blocks? */
		/*
		 * 如果这个逻辑块不是页面的第一个,判断它是否和前一个逻辑块在磁盘上也是连续的.
		 * 如果不是.没有办法用bio方式来提交,跳转到confused标号处.
		 */
		if (page_block && blocks[page_block-1] != map_bh->b_blocknr-1)
			goto confused;
        //计算出这个buffer_head指向的连续的块的块数
		nblocks = map_bh->b_size >> blkbits;
		/*
		 * 根据缓冲头的映射信息,记录页面每个逻辑块在磁盘上对应的逻辑块编号.
		 * 如果映射信息用光,清除缓冲头的BH_mapped标志,退出循环;
		 * 如果这个页面的所有逻辑块都已经处理完;也退出循环;
		 * 如果此时映射信息可能还没有用光,则留给下一个页面.
		 */
		for (relative_block = 0; ; relative_block++) {
			if (relative_block == nblocks) {
				clear_buffer_mapped(map_bh);
				break;
			} else if (page_block == blocks_per_page)
				break;
            /* 为page中连续的块计算在磁盘中的编号,放入blocks数组中 */
			blocks[page_block] = map_bh->b_blocknr+relative_block;
			page_block++;
			block_in_file++;
		}
		bdev = map_bh->b_bdev;
	}

	if (first_hole != blocks_per_page) {
		zero_user_segment(page, first_hole << blkbits, PAGE_CACHE_SIZE);
		if (first_hole == 0) {
			SetPageUptodate(page);
			unlock_page(page);
			goto out;
		}
	/* 页面的逻辑块被映射到磁盘上连续的逻辑块,这时设置页面的映射标志 */
	} else if (fully_mapped) {
		SetPageMappedToDisk(page);
	}

	/*
	 * This page will go to BIO.  Do we need to send this BIO off first?
	 */
	 /*
     * 首次调用时bio为NULL.
	 * 如果传入了一个bio,需要判断本页面第一个逻辑块和传入bio的最后一个逻辑块在磁盘上是否连续,
	 * 也就是看是否可以将本页面作为一个请求段加入传入的bio中.如果不连续,那么mpage_bio_submit
	 * 调用提交前面的bio，这个函数恒返回NULL
	 */
	if (bio && (*last_block_in_bio != blocks[0] - 1))
		bio = mpage_bio_submit(READ, bio);

alloc_new:
	/*
	 * 如果传入的bio为NULL,或者传入的bio已提交.那么,我们需要调用mpage_alloc
	 * 重新一个bio.该bio的请求段数目依赖于nr_pages参数,这是基于后面所有页面
	 * 都可以作为请求段被合并到这个bio的假设.当然bio的请求段数目也受限于块设备.
	 * 如果bio分配失败,跳转到confused标号处.因为我们没有办法按bio方式处理.
	 */
	if (bio == NULL) {
		bio = mpage_alloc(bdev, blocks[0] << (blkbits - 9),
			  	min_t(int, nr_pages, bio_get_nr_vecs(bdev)),
				GFP_KERNEL);
		if (bio == NULL)
			goto confused;
	}

	/*
	 * 现在,我们已经有一个bio,它可能是通过参数传入的,或者是由函数新分配的.
	 * 现在将这个页面作为请求段添加到bio,具体调用了bio_add_page函数.该请求段在页
	 * 面中的偏移为0,而长度取决于first_hole的值,在没有空洞时,它被设置成页面所包含
	 * 的逻辑块数.
	 */
	length = first_hole << blkbits;
	if (bio_add_page(bio, page, length, 0) < length) {
		/*
		 * 如果bio_add_page函数返回0,那么没办法将页面添加到现有bio中,调用
		 * mpage_bio_submit函数提交这个bio,然后重新分配一个bio,重复上面的过程.
		 */
		bio = mpage_bio_submit(READ, bio);
		goto alloc_new;
	}

	if (buffer_boundary(map_bh) || (first_hole != blocks_per_page))
		bio = mpage_bio_submit(READ, bio);
	else
		*last_block_in_bio = blocks[blocks_per_page - 1];
out:
	return bio;

confused:
	/* 如果已经构成了bio,就提交bio,否则下面的函数会提交bh */
	if (bio)
		bio = mpage_bio_submit(READ, bio);
	if (!PageUptodate(page))
			/* page不是最新的,使用buffer的形式读取数据 */
	        block_read_full_page(page, get_block);
	else
		unlock_page(page);
	goto out;
}

/**
 * mpage_readpages - populate an address space with some pages & start reads against them
 * @mapping: the address_space
 * @pages: The address of a list_head which contains the target pages.  These
 *   pages have their ->index populated and are otherwise uninitialised.
 *   The page at @pages->prev has the lowest file offset, and reads should be
 *   issued in @pages->prev to @pages->next order.
 * @nr_pages: The number of pages at *@pages
 * @get_block: The filesystem's block mapper function.
 *
 * This function walks the pages and the blocks within each page, building and
 * emitting large BIOs.
 *
 * If anything unusual happens, such as:
 *
 * - encountering a page which has buffers
 * - encountering a page which has a non-hole after a hole
 * - encountering a page with non-contiguous blocks
 *
 * then this code just gives up and calls the buffer_head-based read function.
 * It does handle a page which has holes at the end - that is a common case:
 * the end-of-file on blocksize < PAGE_CACHE_SIZE setups.
 *
 * BH_Boundary explanation:
 *
 * There is a problem.  The mpage read code assembles several pages, gets all
 * their disk mappings, and then submits them all.  That's fine, but obtaining
 * the disk mappings may require I/O.  Reads of indirect blocks, for example.
 *
 * So an mpage read of the first 16 blocks of an ext2 file will cause I/O to be
 * submitted in the following order:
 * 	12 0 1 2 3 4 5 6 7 8 9 10 11 13 14 15 16
 *
 * because the indirect block has to be read to get the mappings of blocks
 * 13,14,15,16.  Obviously, this impacts performance.
 *
 * So what we do it to allow the filesystem's get_block() function to set
 * BH_Boundary when it maps block 11.  BH_Boundary says: mapping of the block
 * after this one will require I/O against a block which is probably close to
 * this one.  So you should push what I/O you have currently accumulated.
 *
 * This all causes the disk requests to be issued in the correct order.
 */
/* page以链表的形式传参,mapping是相关的地址空间,get_block用于查找匹配的块地址 */
int
mpage_readpages(struct address_space *mapping, struct list_head *pages,
				unsigned nr_pages, get_block_t get_block)
{
	struct bio *bio = NULL;
	unsigned page_idx;
	sector_t last_block_in_bio = 0;
	struct buffer_head map_bh;
	unsigned long first_logical_block = 0;

	clear_buffer_mapped(&map_bh);
	for (page_idx = 0; page_idx < nr_pages; page_idx++) {
		struct page *page = list_entry(pages->prev, struct page, lru);

		prefetchw(&page->flags);
        //将page从lru链表中取下
		list_del(&page->lru);
        //将从lru上取下的page加入mapping中
		if (!add_to_page_cache_lru(page, mapping,
					page->index, GFP_KERNEL)) {
			bio = do_mpage_readpage(bio, page,
					nr_pages - page_idx,
					&last_block_in_bio, &map_bh,
					&first_logical_block,
					get_block);
		}
		page_cache_release(page);
	}
	BUG_ON(!list_empty(pages));
	if (bio)
		mpage_bio_submit(READ, bio);
	return 0;
}
EXPORT_SYMBOL(mpage_readpages);

/*
 * This isn't called much at all
 */
/* 针对页面构造IO请求 */
int mpage_readpage(struct page *page, get_block_t get_block)
{
	struct bio *bio = NULL;
	sector_t last_block_in_bio = 0;
	struct buffer_head map_bh;
	unsigned long first_logical_block = 0;
	struct inode *inode = page->mapping->host;

    /* 清除map_bh的BH_Mapped标志 */
	clear_buffer_mapped(&map_bh);
	/*
	 * 它选择两种不同的策略从磁盘上读取一个页面.
	 * 如果包含了所请求数据的逻辑块在磁盘上连续,则这个函数可以在单个bio中向通用块层提交读I/O请求.
	 * 否则,页面中的每个逻辑块要使用不同的bio.
	 */
	bio = do_mpage_readpage(bio, page, 1, &last_block_in_bio,
			&map_bh, &first_logical_block, get_block);
	if (bio)
		mpage_bio_submit(READ, bio);
	return 0;
}
EXPORT_SYMBOL(mpage_readpage);

/*
 * Writing is not so simple.
 *
 * If the page has buffers then they will be used for obtaining the disk
 * mapping.  We only support pages which are fully mapped-and-dirty, with a
 * special case for pages which are unmapped at the end: end-of-file.
 *
 * If the page has no buffers (preferred) then the page is mapped here.
 *
 * If all blocks are found to be contiguous then the page can go into the
 * BIO.  Otherwise fall back to the mapping's writepage().
 * 
 * FIXME: This code wants an estimate of how many pages are still to be
 * written, so it can intelligently allocate a suitably-sized BIO.  For now,
 * just allocate full-size (16-page) BIOs.
 */

int __mpage_writepage(struct page *page, struct writeback_control *wbc,
		      void *data)
{
	struct mpage_data *mpd = data;
	struct bio *bio = mpd->bio;
	struct address_space *mapping = page->mapping;
	struct inode *inode = page->mapping->host;
	const unsigned blkbits = inode->i_blkbits;
	unsigned long end_index;
	const unsigned blocks_per_page = PAGE_CACHE_SIZE >> blkbits;
	sector_t last_block;
	sector_t block_in_file;
	sector_t blocks[MAX_BUF_PER_PAGE];
	unsigned page_block;
	unsigned first_unmapped = blocks_per_page;
	struct block_device *bdev = NULL;
	int boundary = 0;
	sector_t boundary_block = 0;
	struct block_device *boundary_bdev = NULL;
	int length;
	struct buffer_head map_bh;
	loff_t i_size = i_size_read(inode);
	int ret = 0;

	if (page_has_buffers(page)) {
		struct buffer_head *head = page_buffers(page);
		struct buffer_head *bh = head;

		/* If they're all mapped and dirty, do it */
		page_block = 0;
		do {
			BUG_ON(buffer_locked(bh));
			if (!buffer_mapped(bh)) {
				/*
				 * unmapped dirty buffers are created by
				 * __set_page_dirty_buffers -> mmapped data
				 */
				if (buffer_dirty(bh))
					goto confused;
				if (first_unmapped == blocks_per_page)
					first_unmapped = page_block;
				continue;
			}

			if (first_unmapped != blocks_per_page)
				goto confused;	/* hole -> non-hole */

			if (!buffer_dirty(bh) || !buffer_uptodate(bh))
				goto confused;
			if (page_block) {
				if (bh->b_blocknr != blocks[page_block-1] + 1)
					goto confused;
			}
			blocks[page_block++] = bh->b_blocknr;
			boundary = buffer_boundary(bh);
			if (boundary) {
				boundary_block = bh->b_blocknr;
				boundary_bdev = bh->b_bdev;
			}
			bdev = bh->b_bdev;
		} while ((bh = bh->b_this_page) != head);

		if (first_unmapped)
			goto page_is_mapped;

		/*
		 * Page has buffers, but they are all unmapped. The page was
		 * created by pagein or read over a hole which was handled by
		 * block_read_full_page().  If this address_space is also
		 * using mpage_readpages then this can rarely happen.
		 */
		goto confused;
	}

	/*
	 * The page has no buffers: map it to disk
	 */
	BUG_ON(!PageUptodate(page));
	block_in_file = (sector_t)page->index << (PAGE_CACHE_SHIFT - blkbits);
	last_block = (i_size - 1) >> blkbits;
	map_bh.b_page = page;
	for (page_block = 0; page_block < blocks_per_page; ) {

		map_bh.b_state = 0;
		map_bh.b_size = 1 << blkbits;
		if (mpd->get_block(inode, block_in_file, &map_bh, 1))
			goto confused;
		if (buffer_new(&map_bh))
			unmap_underlying_metadata(map_bh.b_bdev,
						map_bh.b_blocknr);
		if (buffer_boundary(&map_bh)) {
			boundary_block = map_bh.b_blocknr;
			boundary_bdev = map_bh.b_bdev;
		}
		if (page_block) {
			if (map_bh.b_blocknr != blocks[page_block-1] + 1)
				goto confused;
		}
		blocks[page_block++] = map_bh.b_blocknr;
		boundary = buffer_boundary(&map_bh);
		bdev = map_bh.b_bdev;
		if (block_in_file == last_block)
			break;
		block_in_file++;
	}
	BUG_ON(page_block == 0);

	first_unmapped = page_block;

page_is_mapped:
	end_index = i_size >> PAGE_CACHE_SHIFT;
	if (page->index >= end_index) {
		/*
		 * The page straddles i_size.  It must be zeroed out on each
		 * and every writepage invokation because it may be mmapped.
		 * "A file is mapped in multiples of the page size.  For a file
		 * that is not a multiple of the page size, the remaining memory
		 * is zeroed when mapped, and writes to that region are not
		 * written out to the file."
		 */
		unsigned offset = i_size & (PAGE_CACHE_SIZE - 1);

		if (page->index > end_index || !offset)
			goto confused;
		zero_user_segment(page, offset, PAGE_CACHE_SIZE);
	}

	/*
	 * This page will go to BIO.  Do we need to send this BIO off first?
	 */
	if (bio && mpd->last_block_in_bio != blocks[0] - 1)
		bio = mpage_bio_submit(WRITE, bio);

alloc_new:
	if (bio == NULL) {
		bio = mpage_alloc(bdev, blocks[0] << (blkbits - 9),
				bio_get_nr_vecs(bdev), GFP_NOFS|__GFP_HIGH);
		if (bio == NULL)
			goto confused;
	}

	/*
	 * Must try to add the page before marking the buffer clean or
	 * the confused fail path above (OOM) will be very confused when
	 * it finds all bh marked clean (i.e. it will not write anything)
	 */
	length = first_unmapped << blkbits;
	if (bio_add_page(bio, page, length, 0) < length) {
		bio = mpage_bio_submit(WRITE, bio);
		goto alloc_new;
	}

	/*
	 * OK, we have our BIO, so we can now mark the buffers clean.  Make
	 * sure to only clean buffers which we know we'll be writing.
	 */
	if (page_has_buffers(page)) {
		struct buffer_head *head = page_buffers(page);
		struct buffer_head *bh = head;
		unsigned buffer_counter = 0;

		do {
			if (buffer_counter++ == first_unmapped)
				break;
			clear_buffer_dirty(bh);
			bh = bh->b_this_page;
		} while (bh != head);

		/*
		 * we cannot drop the bh if the page is not uptodate
		 * or a concurrent readpage would fail to serialize with the bh
		 * and it would read from disk before we reach the platter.
		 */
		if (buffer_heads_over_limit && PageUptodate(page))
			try_to_free_buffers(page);
	}

	BUG_ON(PageWriteback(page));
	set_page_writeback(page);
	unlock_page(page);
	if (boundary || (first_unmapped != blocks_per_page)) {
		bio = mpage_bio_submit(WRITE, bio);
		if (boundary_block) {
			write_boundary_block(boundary_bdev,
					boundary_block, 1 << blkbits);
		}
	} else {
		mpd->last_block_in_bio = blocks[blocks_per_page - 1];
	}
	goto out;

confused:
	if (bio)
		bio = mpage_bio_submit(WRITE, bio);

	if (mpd->use_writepage) {
		ret = mapping->a_ops->writepage(page, wbc);
	} else {
		ret = -EAGAIN;
		goto out;
	}
	/*
	 * The caller has a ref on the inode, so *mapping is stable
	 */
	mapping_set_error(mapping, ret);
out:
	mpd->bio = bio;
	return ret;
}
EXPORT_SYMBOL(__mpage_writepage);

/**
 * mpage_writepages - walk the list of dirty pages of the given address space & writepage() all of them
 * @mapping: address space structure to write
 * @wbc: subtract the number of written pages from *@wbc->nr_to_write
 * @get_block: the filesystem's block mapper function.
 *             If this is NULL then use a_ops->writepage.  Otherwise, go
 *             direct-to-BIO.
 *
 * This is a library function, which implements the writepages()
 * address_space_operation.
 *
 * If a page is already under I/O, generic_writepages() skips it, even
 * if it's dirty.  This is desirable behaviour for memory-cleaning writeback,
 * but it is INCORRECT for data-integrity system calls such as fsync().  fsync()
 * and msync() need to guarantee that all the data which was dirty at the time
 * the call was made get new I/O started against them.  If wbc->sync_mode is
 * WB_SYNC_ALL then we were called for data integrity and we must wait for
 * existing IO to complete.
 */
int
mpage_writepages(struct address_space *mapping,
		struct writeback_control *wbc, get_block_t get_block)
{
	int ret;

	if (!get_block)
		ret = generic_writepages(mapping, wbc);
	else {
		struct mpage_data mpd = {
			.bio = NULL,
			.last_block_in_bio = 0,
			.get_block = get_block,
			.use_writepage = 1,
		};

		ret = write_cache_pages(mapping, wbc, __mpage_writepage, &mpd);
		if (mpd.bio)
			mpage_bio_submit(WRITE, mpd.bio);
	}
	return ret;
}
EXPORT_SYMBOL(mpage_writepages);

int mpage_writepage(struct page *page, get_block_t get_block,
	struct writeback_control *wbc)
{
	struct mpage_data mpd = {
		.bio = NULL,
		.last_block_in_bio = 0,
		.get_block = get_block,
		.use_writepage = 0,
	};
	int ret = __mpage_writepage(page, wbc, &mpd);
	if (mpd.bio)
		mpage_bio_submit(WRITE, mpd.bio);
	return ret;
}
EXPORT_SYMBOL(mpage_writepage);
