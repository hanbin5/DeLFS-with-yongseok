// SPDX-License-Identifier: GPL-2.0
/*
 * DeLFS dflush, step 2: writeback run by the writer thread itself.
 *
 * df_wb_writeback() is a copy of wb_writeback() from Linux 7.2.8
 * fs/fs-writeback.c with the DeLFS changes on top:
 *   - tracepoints removed (the module cannot call the kernel's
 *     writeback tracepoints);
 *   - the 6.6.8 DeLFS re-check of the background threshold after filling
 *     b_io is not ported (see the comment in the loop);
 *   - a pass that wrote nothing ends the loop as in 6.6.8 DeLFS; the
 *     "queued once" retry of 7.2.8 is not used (see the comment in the loop).
 *
 * The kernel patch (DeLFS/linux-7.2.8) exports what this file needs:
 * struct wb_writeback_work (moved to <linux/writeback.h>), wb_queue_io()
 * (renamed from queue_io()), writeback_sb_inodes(), __writeback_inodes_wb(),
 * inode_sleep_on_writeback(), wb_over_bg_thresh(), dirty_expire_interval,
 * wb_get_create() and the wb->nr_threads field.
 */
#include <linux/atomic.h>
#include <linux/backing-dev.h>
#include <linux/blkdev.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/spinlock.h>
#include <linux/writeback.h>

#include "../dflush.h"
#include "../calc/calclock.h"
KTDEF(wb_check_start_all);
KTDEF(wb_check_old_data_flush);
KTDEF(df_wb_check_background_flush);

/* 4MB minimal write chunk size (same as the kernel's MIN_WRITEBACK_PAGES) */
#define MIN_WRITEBACK_PAGES		(4096UL >> (PAGE_SHIFT - 10))
/* One deferred dflush writes back at most 64MB */
#define DF_DEFERRED_WRITEBACK_PAGES	(MIN_WRITEBACK_PAGES * 16)

/* copy of the static inline in fs/fs-writeback.c */
static inline struct inode *wb_inode(struct list_head *head)
{
	return list_entry(head, struct inode, i_io_list);
}

KTDEF(writeback_sb_inodes);
KTDEF(__writeback_inodes_wb);
KTDEF(inode_sleep_on_writeback);

static long df_wb_writeback(struct bdi_writeback *wb,
			    struct wb_writeback_work *work)
{
	ktime_t stopwatch[2];
	long nr_pages = work->nr_pages;
	unsigned long dirtied_before = jiffies;
	struct inode *inode;
	long progress;
	struct blk_plug plug;

	blk_start_plug(&plug);
	for (;;) {
		/*
		 * Stop writeback when nr_pages has been consumed
		 */
		if (work->nr_pages <= 0)
			break;

		/*
		 * Background writeout and kupdate-style writeback may
		 * run forever. Stop them if there is other work to do
		 * so that e.g. sync can proceed. They'll be restarted
		 * after the other works are all done.
		 */
		if ((work->for_background || work->for_kupdate) &&
		    !list_empty(&wb->work_list))
			break;

		/*
		 * For background writeout, stop when we are below the
		 * background dirty threshold
		 */
		if (work->for_background && !wb_over_bg_thresh(wb))
			break;


		spin_lock(&wb->list_lock);

		if (list_empty(&wb->b_io)) {
			/*
			 * Kupdate and background works are special and we want
			 * to include all inodes that need writing. Livelock
			 * avoidance is handled by these works yielding to any
			 * other work so we are safe.
			 */
			if (work->for_kupdate) {
				dirtied_before = jiffies -
					msecs_to_jiffies(dirty_expire_interval *
							 10);
			} else if (work->for_background)
				dirtied_before = jiffies;

			wb_queue_io(wb, work, dirtied_before);
		}

		/*
		 * DeLFS: 6.6.8 DeLFS re-checked wb_over_bg_thresh() here, with
		 * wb->list_lock held, so a writer stops once others have brought
		 * the wb below the background threshold.  It is not ported:
		 * - the loop-top check already does this; only wb_queue_io(),
		 *   which writes nothing, runs in between;
		 * - it can sleep (memcg stats flush on cgroup v2), so it cannot
		 *   run under the spinlock, and dropping the lock for it lets
		 *   another thread empty b_io in the meantime.
		 * Measured with the lock-dropped re-check (4 writers,
		 * dflush-probe.sh): it stopped the loop 4 times in ~3,650 calls.
		 */

		if (work->sb) {
			ktget(&stopwatch[0]);
			progress = writeback_sb_inodes(work->sb, wb, work);
			ktget(&stopwatch[1]);
			ktput(stopwatch, writeback_sb_inodes);
		} else {
			ktget(&stopwatch[0]);
			progress = __writeback_inodes_wb(wb, work);
			ktget(&stopwatch[1]);
			ktput(stopwatch, __writeback_inodes_wb);
		}

		/*
		 * Did we write something? Try for more
		 *
		 * Dirty inodes are moved to b_io for writeback in batches.
		 * The completion of the current batch does not necessarily
		 * mean the overall work is done. So we keep looping as long
		 * as made some progress on cleaning pages or inodes.
		 *
		 * DeLFS: 7.2.8 wb_writeback() also loops again when this call
		 * has not refilled b_io yet ("|| !queued"), so that the single
		 * flusher does not give up before looking at b_dirty.  dflush
		 * does not need that: it is called again after every write(),
		 * and the flusher still runs the kernel's own loop.  With many
		 * writer threads the retry is expensive.  While a checkpoint
		 * holds cp_rwsem no data page can be written, so a pass walks
		 * every dirty page of every free inode and puts it back; the
		 * retry makes each thread that found b_io non-empty do that
		 * walk twice.  6.6.8 DeLFS stops (or sleeps on a busy inode)
		 * after one pass without progress, and so does this.
		 */
		if (progress) {
			spin_unlock(&wb->list_lock);
			continue;
		}

		/*
		 * No more inodes for IO, bail
		 */
		if (list_empty(&wb->b_more_io)) {
			spin_unlock(&wb->list_lock);
			break;
		}

		/*
		 * Nothing written. Wait for some inode to
		 * become available for writeback. Otherwise
		 * we'll just busyloop.
		 */
		inode = wb_inode(wb->b_more_io.prev);
		spin_lock(&inode->i_lock);
		spin_unlock(&wb->list_lock);
		/* This function drops i_lock... */
		ktget(&stopwatch[0]);
		inode_sleep_on_writeback(inode);
		ktget(&stopwatch[1]);
		ktput(stopwatch, inode_sleep_on_writeback);
	}
	blk_finish_plug(&plug);

	return nr_pages - work->nr_pages;
}

/*
 * Background writeback of at most @nr_pages, run by the calling thread.
 *
 * WB_writeback_running normally belongs to the single flusher: it sets the
 * bit when it starts and clears it when it ends.  With dflush many threads
 * write back the same wb at once, so wb->nr_threads counts them.  The first
 * one sets the bit and the last one clears it.
 *
 * Known limitation (from 6.6.8 DeLFS, not fixed): the bit can be clear while
 * dflush threads are still running.
 * - The flusher's wb_do_writeback() sets and clears the bit without looking
 *   at nr_threads, so when it ends it clears the bit under running dflush
 *   threads.
 * - Between threads: A's atomic_dec_and_test() hits 0, then B's
 *   atomic_inc_return() returns 1 and B sets the bit, then A's clear_bit()
 *   clears B's bit.
 * The bit is only read through writeback_in_progress(): balance_dirty_pages()
 * wakes the flusher when it is clear, and skip_if_busy in
 * try_to_writeback_inodes_sb() skips queueing work when it is set.  So a
 * wrong clear bit only causes extra flusher wakeups or extra queued work; no
 * dirty data is lost or skipped.
 * Measured (dflush-probe.sh): with 4 writers the bit was clear in 43-49% of
 * calls just before the decrement, with 1 writer in 0.1-0.3%.  Even with 1
 * writer the flusher ran 1.3k-12.6k times (stock f2fs: 453), so the extra
 * runs come mostly from the bit being clear between dflush calls, not from
 * these races.
 * A fix needs a kernel patch: wb_do_writeback() must clear the bit only when
 * nr_threads is 0, and the dec/clear vs inc/set pair must be made atomic
 * (e.g. under a lock).
 */
static long df_wb_do_writeback_limited(struct bdi_writeback *wb, long nr_pages)
{
	ktime_t stopwatch[2];
	long wrote = 0;

	if (nr_pages <= 0)
		nr_pages = DF_DEFERRED_WRITEBACK_PAGES;

	if (atomic_inc_return(&wb->nr_threads) == 1)
		set_bit(WB_writeback_running, &wb->state);

	/* calclock: df_wb_check_background_flush() in 6.6.8 DeLFS */
	ktget(&stopwatch[0]);
	if (wb_over_bg_thresh(wb)) {
		struct wb_writeback_work work = {
			.nr_pages	= nr_pages,
			.sync_mode	= WB_SYNC_NONE,
			.for_background	= 1,
			.range_cyclic	= 1,
			.reason		= WB_REASON_BACKGROUND,
		};

		wrote += df_wb_writeback(wb, &work);
	}
	ktget(&stopwatch[1]);
	ktput(stopwatch, df_wb_check_background_flush);

	if (atomic_dec_and_test(&wb->nr_threads))
		clear_bit(WB_writeback_running, &wb->state);

	return wrote;
}

/*
 * Called from f2fs_file_write_iter() right after inode_unlock().
 * Must not be called with inode->i_rwsem held (see dflush.h).
 */
KTDEC(df_wb_do_writeback);

long df_flush_deferred_writeback(struct address_space *mapping)
{
	ktime_t stopwatch[2];
	struct inode *inode;
	struct backing_dev_info *bdi;
	struct bdi_writeback *wb = NULL;
	long wrote;

	if (!mapping)
		return 0;

	inode = mapping->host;
	if (!inode)
		return 0;

	bdi = inode_to_bdi(inode);
	if (!(bdi->capabilities & BDI_CAP_WRITEBACK))
		return 0;

	if (inode_cgwb_enabled(inode))
		wb = wb_get_create_current(bdi, GFP_KERNEL);
	if (!wb)
		wb = &bdi->wb;

	/* calclock: df_wb_do_writeback() call in 6.6.8 df_balance_dirty_pages() */
	ktget(&stopwatch[0]);
	wrote = df_wb_do_writeback_limited(wb, DF_DEFERRED_WRITEBACK_PAGES);
	ktget(&stopwatch[1]);
	ktput(stopwatch, df_wb_do_writeback);

	wb_put(wb);
	return wrote;
}
