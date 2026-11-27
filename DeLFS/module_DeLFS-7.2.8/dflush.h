/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DeLFS dflush (ScaleCache "direct flush", DeLFS paper §3.2.1)
 *
 * Stock writeback: when a writer crosses the dirty limit it sleeps in
 * balance_dirty_pages() and a single flusher kworker per device does all the
 * writeback.  dflush lets the writer threads do the writeback themselves, so
 * writeback scales with the number of writers.
 *
 * It runs in two steps, because the writer holds inode->i_rwsem while it
 * copies data into the page cache:
 *
 *  1. df_generic_perform_write() (inode locked): dirty balancing runs in
 *     async mode.  Over the limit, it wakes the flusher and returns -EAGAIN
 *     instead of sleeping.  The writer keeps copying.
 *  2. df_flush_deferred_writeback() (after inode_unlock): if the wb is over
 *     the background threshold, the writer itself writes back up to
 *     DF_DEFERRED_WRITEBACK_PAGES (64MB) through df_wb_writeback().
 *     This runs after every write(), whatever its size.  Only a "below the
 *     threshold" result is cached, per CPU and for at most
 *     DF_BG_BELOW_CACHE_JIFFIES; while it is valid the writer skips the wb
 *     lookup and the check.  Over the threshold, every writer still does
 *     the full check and the writeback (with approach B this is the
 *     writer's only brake).
 *
 * Doing step 2 with i_rwsem held can deadlock or hang (writeback touches
 * other inodes and may run foreground GC), so the order matters.
 */
#ifndef _DELFS_DFLUSH_H
#define _DELFS_DFLUSH_H

#include <linux/fs.h>
#include <linux/uio.h>

/* mm/filemap.c */
ssize_t df_generic_perform_write(struct kiocb *iocb, struct iov_iter *i);

/* mm/page-writeback.c */
int df_balance_dirty_pages_ratelimited_async(struct address_space *mapping);

/* fs/fs-writeback.c */
long df_flush_deferred_writeback(struct address_space *mapping);

#endif /* _DELFS_DFLUSH_H */
