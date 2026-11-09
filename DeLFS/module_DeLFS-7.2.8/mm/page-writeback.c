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
 * Difference from 6.6.8 DeLFS: it also set current->nr_dirtied_pause to the
 * value computed inside balance_dirty_pages() (a local variable we cannot
 * see).  Here nr_dirtied_pause keeps its previous value.  That only changes
 * how many pages pass before the next check.  While the wb is over its limit
 * the kernel caps that interval at 8 pages anyway (wb->dirty_exceeded).
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
