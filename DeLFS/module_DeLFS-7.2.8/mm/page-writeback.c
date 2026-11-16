// SPDX-License-Identifier: GPL-2.0
/*
 * DeLFS dflush, step 1: dirty balancing without sleeping.
 *
 * 6.6.8 DeLFS copied the whole balance_dirty_pages() (~290 lines) into the
 * module.  In the async mode it actually uses, the only change from the
 * kernel's code was resetting three per-task counters before returning
 * -EAGAIN.  The kernel already exports the entry point
 * balance_dirty_pages_ratelimited_flags() (6.6.8 and 7.2.8 both), so this
 * port calls it and resets the counters here instead of copying.
 *
 * Why reset: without it, current->nr_dirtied stays above the ratelimit, so
 * every following page re-enters balance_dirty_pages() and redoes the whole
 * limit calculation.  The stock -EAGAIN path keeps the counters because its
 * io_uring caller is expected to wait and retry.
 *
 * Difference from 6.6.8 DeLFS: on -EAGAIN it also set
 * current->nr_dirtied_pause to the value wb_min_pause() computes inside
 * balance_dirty_pages() (a local variable we cannot see).  Here
 * nr_dirtied_pause is not touched.  BDP_ASYNC returns at the "pause:" label,
 * before the kernel's own "nr_dirtied_pause = ..." after the sleep, so the
 * value changes only in two places:
 *  - the freerun path sets it to dirty_poll_interval();
 *  - the "pause < min_pause" path adds pages_dirtied to it (about doubles
 *    it) when period == 0.
 * Otherwise the old value stays.  The kernel lowers the interval to 8 pages
 * only while wb->dirty_exceeded is set, i.e. above the hard limit (dirty >
 * thresh and wb_dirty > wb_thresh) or with strictlimit.  Throttling starts
 * earlier, at the freerun ceiling (bg_thresh + thresh) / 2, and between the
 * two there is no 8-page cap.
 * Measured (dflush-probe.sh, ~466k balance_dirty_pages tracepoint events):
 * ~99% were above the hard limit (task_ratelimit 0, which goes straight to
 * "pause:" and never reaches the doubling).  In the ~6.9k inside the
 * throttled band period == 0 never happened.  nr_dirtied_pause did not grow;
 * it stayed at the freerun value (128, or 32-64 with bg_thresh close to
 * thresh).  period == 0 is still possible with a fast enough task_ratelimit.
 * This only changes how often balance_dirty_pages() runs, not what gets
 * written back.
 */
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/writeback.h>

#include "../dflush.h"

int df_balance_dirty_pages_ratelimited_async(struct address_space *mapping)
{
	int ret = balance_dirty_pages_ratelimited_flags(mapping, BDP_ASYNC);

	if (ret == -EAGAIN) {
		current->dirty_paused_when = jiffies;
		current->nr_dirtied = 0;
	}
	return ret;
}
