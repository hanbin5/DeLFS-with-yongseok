// SPDX-License-Identifier: GPL-2.0
/*
 * DeLFS dflush: copy of generic_perform_write() from Linux 7.2.8
 * mm/filemap.c.
 *
 * The only change: balance_dirty_pages_ratelimited() (which may sleep while
 * the caller holds inode->i_rwsem) is replaced by
 * df_balance_dirty_pages_ratelimited_async(), which never sleeps.  The actual
 * writeback runs later in df_flush_deferred_writeback(), after inode_unlock.
 *
 * The call stays where 7.2.8 put it (top of the loop, before write_begin).
 * 6.6.8 called it at the bottom.  The async version only checks and resets
 * counters, so the position does not change what gets written back.
 */
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/sched/signal.h>
#include <linux/uio.h>

#include "../dflush.h"
#include "../calc/calclock.h"

KTDEF(write_begin);
KTDEF(write_end);

ssize_t df_generic_perform_write(struct kiocb *iocb, struct iov_iter *i)
{
	ktime_t stopwatch[2];
	struct file *file = iocb->ki_filp;
	loff_t pos = iocb->ki_pos;
	struct address_space *mapping = file->f_mapping;
	const struct address_space_operations *a_ops = mapping->a_ops;
	size_t chunk = mapping_max_folio_size(mapping);
	long status = 0;
	ssize_t written = 0;

	do {
		struct folio *folio;
		size_t offset;		/* Offset into folio */
		size_t bytes;		/* Bytes to write to folio */
		size_t copied;		/* Bytes copied from user */
		void *fsdata = NULL;

		bytes = iov_iter_count(i);
retry:
		offset = pos & (chunk - 1);
		bytes = min(chunk - offset, bytes);
		/* DeLFS dflush: was balance_dirty_pages_ratelimited(mapping) */
		df_balance_dirty_pages_ratelimited_async(mapping);

		if (fatal_signal_pending(current)) {
			status = -EINTR;
			break;
		}

		ktget(&stopwatch[0]);
		status = a_ops->write_begin(iocb, mapping, pos, bytes,
						&folio, &fsdata);
		ktget(&stopwatch[1]);
		ktput(stopwatch, write_begin);
		if (unlikely(status < 0))
			break;

		offset = offset_in_folio(folio, pos);
		if (bytes > folio_size(folio) - offset)
			bytes = folio_size(folio) - offset;

		if (mapping_writably_mapped(mapping))
			flush_dcache_folio(folio);

		/*
		 * Faults here on mmap()s can recurse into arbitrary
		 * filesystem code. Lots of locks are held that can
		 * deadlock. Use an atomic copy to avoid deadlocking
		 * in page fault handling.
		 */
		copied = copy_folio_from_iter_atomic(folio, offset, bytes, i);
		flush_dcache_folio(folio);

		ktget(&stopwatch[0]);
		status = a_ops->write_end(iocb, mapping, pos, bytes, copied,
						folio, fsdata);
		ktget(&stopwatch[1]);
		ktput(stopwatch, write_end);
		if (unlikely(status != copied)) {
			iov_iter_revert(i, copied - max(status, 0L));
			if (unlikely(status < 0))
				break;
		}
		cond_resched();

		if (unlikely(status == 0)) {
			/*
			 * A short copy made ->write_end() reject the
			 * thing entirely.  Might be memory poisoning
			 * halfway through, might be a race with munmap,
			 * might be severe memory pressure.
			 */
			if (chunk > PAGE_SIZE)
				chunk /= 2;
			if (copied) {
				bytes = copied;
				goto retry;
			}

			/*
			 * 'folio' is now unlocked and faults on it can be
			 * handled. Ensure forward progress by trying to
			 * fault it in now.
			 */
			if (fault_in_iov_iter_readable(i, bytes) == bytes) {
				status = -EFAULT;
				break;
			}
		} else {
			pos += status;
			written += status;
		}
	} while (iov_iter_count(i));

	if (!written)
		return status;
	iocb->ki_pos += written;
	return written;
}
