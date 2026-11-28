// SPDX-License-Identifier: GPL-2.0
/*
 * DeLFS dflush, step 2: writeback run by the writer thread itself.
 *
 * df_wb_writeback() is a copy of wb_writeback() from Linux 7.2.8
 * fs/fs-writeback.c with the DeLFS changes on top:
 *   - tracepoints removed (the module cannot call the kernel's
 *     writeback tracepoints);
 *   - the 6.6.8 DeLFS re-check of the background threshold after filling
 *     b_io is not ported (see the comment in the loop).
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
#include <linux/cgroup.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/percpu.h>
#include <linux/rcupdate.h>
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
	bool queued = false;

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
			queued = true;
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
		 */
		if (progress || !queued) {
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
static long df_wb_do_writeback_limited(struct bdi_writeback *wb, long nr_pages,
				       bool *over)
{
	long wrote = 0;

	if (nr_pages <= 0)
		nr_pages = DF_DEFERRED_WRITEBACK_PAGES;

	if (atomic_inc_return(&wb->nr_threads) == 1)
		set_bit(WB_writeback_running, &wb->state);

	*over = wb_over_bg_thresh(wb);
	if (*over) {
		ktime_t stopwatch[2];
		struct wb_writeback_work work = {
			.nr_pages	= nr_pages,
			.sync_mode	= WB_SYNC_NONE,
			.for_background	= 1,
			.range_cyclic	= 1,
			.reason		= WB_REASON_BACKGROUND,
		};

		/*
		 * calclock: time only real writer-assisted writeback.  In
		 * 6.6.8 DeLFS the probes of this name sit on a path the async
		 * write never reaches, so timing every poll here made each
		 * write() pay two clock-read pairs the original does not.
		 */
		ktget(&stopwatch[0]);
		wrote += df_wb_writeback(wb, &work);
		ktget(&stopwatch[1]);
		ktput(stopwatch, df_wb_check_background_flush);
	}

	if (atomic_dec_and_test(&wb->nr_threads))
		clear_bit(WB_writeback_running, &wb->state);

	return wrote;
}

/*
 * DeLFS: short-lived cache of "this wb is below the background threshold".
 *
 * Like 6.6.8 DeLFS, this port ran the cgroup wb lookup + reference and
 * wb_over_bg_thresh() after every write().  On 128 cores (4 KiB buffered
 * randwrite, 128 jobs, RAM disk) that path took 14.36% of all CPU time here
 * vs 5.01% in 6.6.8 DeLFS: per-call work, not contention
 * (wb_over_bg_thresh() zeroes two dirty_throttle_control structs and does
 * 64-bit divisions).
 *
 * Only a "below" result is cached, for at most DF_BG_BELOW_CACHE_JIFFIES.
 * While it is valid, the writer returns before the wb lookup and the check.
 * An "over" result is never cached: every writer keeps doing the full lookup,
 * the check and the writeback, exactly as before.  With approach B the
 * writer never sleeps in balance_dirty_pages(), so this writeback is its only
 * brake, and the brake must stay on for every writer regardless of write
 * size (skipping by write size, 00d31d7, cost YCSB A 0.61x).
 *
 * Where it lives: per CPU, keyed by what selects the wb without looking it
 * up.  wb_find_current() picks the wb from (bdi, current's memcg css, and
 * current's blkcg css); the key holds the same three pointers (the two css
 * are NULL when cgroup writeback is off for the inode, i.e. &bdi->wb).
 * Reading them is two task_css() loads; no radix tree lookup, no wb
 * reference.  A per-wb or per-bdi field would need the lookup first, or
 * would mix up the verdicts of different cgroups' wbs.  Per CPU also means
 * no cache line is written by more than one CPU.
 *
 * Cost of a stale entry: a writer may skip the check for up to
 * DF_BG_BELOW_CACHE_JIFFIES after its wb crossed the background threshold
 * (the async balance in step 1 still wakes the flusher meanwhile).  Once the
 * entry expires, the next write() on that CPU does the full check.  The
 * pointers are compared, never dereferenced; if a bdi or css is freed and
 * its address reused within the window, the effect is the same bounded skip.
 *
 * TODO(measure): the saving is only on calls made while the wb is below the
 * background threshold.  The share of such calls in fio rand-write-600m /
 * YCSB A is not measured; if the wb is mostly over the threshold, this
 * changes little.
 * TODO(measure): 1 jiffy is 1 ms at HZ=1000 and 4 ms at HZ=250.  The HZ of
 * the 128-core server kernel was not checked here.
 */
#define DF_BG_BELOW_CACHE_JIFFIES	1	/* 0 disables the cache */

struct df_bg_key {
	const struct backing_dev_info	*bdi;
	const void			*memcg_css;
	const void			*blkcg_css;
};

struct df_bg_below_cache {
	struct df_bg_key	key;
	unsigned long		stamp;	/* jiffies of the "below" check */
	/* pages written through this CPU, see df_cpu_is_light() */
	unsigned long		win;		/* the jiffy win_pages is for */
	unsigned int		win_pages;
	unsigned int		prev_pages;	/* the jiffy before, or 0 */
};
static DEFINE_PER_CPU(struct df_bg_below_cache, df_bg_below_cache);

/*
 * DeLFS: a CPU that hardly dirties anything does not do the writeback.
 *
 * Whoever finds the wb over the background threshold writes back up to
 * 64MB, whatever it dirtied itself.  Without the cache above that was close
 * to fair: every write() checked, the wb was brought back below within
 * microseconds of crossing, and almost always by the threads that write all
 * the time.  With the cache the heavy writers look again only at the next
 * jiffy, so the wb stays over for milliseconds, and a thread that writes
 * now and then walks into nearly every one of those windows and gets the
 * 64MB.  Measured in a 16-vCPU VM (8 fio 4K random writers plus one process
 * writing 4 KiB every 2 ms, build/perf/smallwrite-test.sh): that process
 * spent 9.5 of 15 s inside write() with the cache (single calls up to
 * 2.3 s), 0.4 s without it.
 *
 * So the pages written through each CPU are counted per jiffy, and a CPU
 * that wrote fewer than DF_LIGHT_PAGES in the current and the previous jiffy
 * returns before the check: it has not caused the dirty pages and it is not
 * the one to brake.  The kernel's own throttling has the same notion (a task
 * below its ratelimit never enters balance_dirty_pages()).  Counting per CPU
 * rather than per task also catches many small writers on one CPU.  A heavy
 * writer comes back from a long writeback with an empty count and gets
 * DF_LIGHT_PAGES pages for free; that is nothing next to the 64MB it wrote.
 *
 * The bound on what can stay unbraked is DF_LIGHT_PAGES per two jiffies and
 * CPU (about 4MB/s per CPU at HZ=250); the flusher still runs for that.
 */
#define DF_LIGHT_PAGES	max(2, 2000 / HZ)	/* 8 pages at HZ=250 */

/* account @pages to this CPU; true if it wrote too little to be braked */
static inline bool df_cpu_is_light(struct df_bg_below_cache *c,
				   unsigned long pages)
{
	unsigned long now = jiffies;

	if (c->win != now) {
		c->prev_pages = (c->win + 1 == now) ? c->win_pages : 0;
		c->win = now;
		c->win_pages = 0;
	}
	if (c->win_pages < DF_LIGHT_PAGES)
		c->win_pages += min_t(unsigned long, pages, DF_LIGHT_PAGES);

	return c->win_pages + c->prev_pages < DF_LIGHT_PAGES;
}

static inline void df_bg_key_init(struct df_bg_key *key,
				  struct backing_dev_info *bdi, bool cgwb)
{
	key->bdi = bdi;
	key->memcg_css = NULL;
	key->blkcg_css = NULL;
#ifdef CONFIG_CGROUP_WRITEBACK
	if (cgwb) {
		/* same css as wb_find_current() uses; pointers only */
		rcu_read_lock();
		key->memcg_css = task_css(current, memory_cgrp_id);
		key->blkcg_css = task_css(current, io_cgrp_id);
		rcu_read_unlock();
	}
#endif
}

/* true if this write() needs no check: light CPU, or a fresh "below" result */
static inline bool df_bg_below_cached(const struct df_bg_key *key,
				      unsigned long pages)
{
	struct df_bg_below_cache *c;
	bool hit;

	if (!DF_BG_BELOW_CACHE_JIFFIES)
		return false;

	c = get_cpu_ptr(&df_bg_below_cache);
	hit = df_cpu_is_light(c, pages) ||
	      (c->key.bdi == key->bdi &&
	       c->key.memcg_css == key->memcg_css &&
	       c->key.blkcg_css == key->blkcg_css &&
	       time_before(jiffies, c->stamp + DF_BG_BELOW_CACHE_JIFFIES));
	put_cpu_ptr(&df_bg_below_cache);
	return hit;
}

static inline void df_bg_below_store(const struct df_bg_key *key,
				     unsigned long stamp)
{
	struct df_bg_below_cache *c;

	if (!DF_BG_BELOW_CACHE_JIFFIES)
		return;

	/*
	 * May run on another CPU than the lookup if the task migrated; the
	 * key describes the task, not the CPU, so the entry is still right.
	 */
	c = get_cpu_ptr(&df_bg_below_cache);
	c->key = *key;
	c->stamp = stamp;
	put_cpu_ptr(&df_bg_below_cache);
}

/*
 * Called from f2fs_file_write_iter() right after inode_unlock().
 * Must not be called with inode->i_rwsem held (see dflush.h).
 */
KTDEC(df_wb_do_writeback);

long df_flush_deferred_writeback(struct address_space *mapping, size_t bytes)
{
	struct inode *inode;
	struct backing_dev_info *bdi;
	struct bdi_writeback *wb = NULL;
	struct df_bg_key key;
	unsigned long stamp;
	bool cgwb, over;
	long wrote;

	if (!mapping)
		return 0;

	inode = mapping->host;
	if (!inode)
		return 0;

	bdi = inode_to_bdi(inode);
	if (!(bdi->capabilities & BDI_CAP_WRITEBACK))
		return 0;

	cgwb = inode_cgwb_enabled(inode);
	df_bg_key_init(&key, bdi, cgwb);

	/* fast path: a light CPU, or recently below the background threshold */
	if (df_bg_below_cached(&key, DIV_ROUND_UP(bytes, PAGE_SIZE)))
		return 0;

	stamp = jiffies;
	if (cgwb)
		wb = wb_get_create_current(bdi, GFP_KERNEL);
	if (!wb)
		wb = &bdi->wb;

	/* not timed per poll: see df_wb_do_writeback_limited() */
	wrote = df_wb_do_writeback_limited(wb, DF_DEFERRED_WRITEBACK_PAGES,
					   &over);

	/*
	 * Cache only a "below" result.  After writeback the wb may be below
	 * too, but the loop also stops for other reasons (nr_pages used up,
	 * queued work, no inodes), so the next write() checks again.
	 */
	if (!over)
		df_bg_below_store(&key, stamp);

	wb_put(wb);
	return wrote;
}
