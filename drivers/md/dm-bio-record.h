/*
 * Copyright (C) 2004-2005 Red Hat, Inc. All rights reserved.
 *
 * This file is released under the GPL.
 */

#ifndef DM_BIO_RECORD_H
#define DM_BIO_RECORD_H

#include <linux/bio.h>

/*
 * There are lots of mutable fields in the bio struct that get
 * changed by the lower levels of the block layer.  Some targets,
 * such as multipath, may wish to resubmit a bio on error.  The
 * functions in this file help the target record and restore the
 * original bio state.
 */
struct dm_bio_details {
	sector_t bi_sector;
	struct block_device *bi_bdev;
	unsigned int bi_size;
	unsigned short bi_idx;
	unsigned long bi_flags;
};

static inline void dm_bio_record(struct dm_bio_details *bd, struct bio *bio)
{
	bd->bi_sector = bio->bi_sector;
	bd->bi_bdev = bio->bi_bdev;
	bd->bi_size = bio->bi_size;
	bd->bi_idx = bio->bi_idx;
	bd->bi_flags = bio->bi_flags;
}

static inline void dm_bio_restore(struct dm_bio_details *bd, struct bio *bio)
{
	bio->bi_sector = bd->bi_sector;
	bio->bi_bdev = bd->bi_bdev;
	bio->bi_size = bd->bi_size;
	bio->bi_idx = bd->bi_idx;
	bio->bi_flags = bd->bi_flags;
}

#endif
