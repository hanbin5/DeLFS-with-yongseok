// SPDX-License-Identifier: GPL-2.0
/*
 * fs/f2fs/segment.c
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 */
#include <linux/fs.h>
#include "delfs_fs.h"
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/sched/mm.h>
#include <linux/prefetch.h>
#include <linux/kthread.h>
#include <linux/swap.h>
#include <linux/timer.h>
#include <linux/freezer.h>
#include <linux/sched/signal.h>
#include <linux/random.h>
#include <linux/fserror.h>

#include "f2fs.h"
#include "segment.h"
#include "node.h"
#include "gc.h"
#include "iostat.h"
#include "oldinv.h"	/* DeLFS deLLC (B) */
#include <trace/events/f2fs.h>

#define __reverse_ffz(x) __reverse_ffs(~(x))

static struct kmem_cache *discard_entry_slab;
static struct kmem_cache *discard_cmd_slab;
static struct kmem_cache *sit_entry_set_slab;
static struct kmem_cache *revoke_entry_slab;

/* DeLFS DDL exception: take/release every core region's SIT lock */
static void f2fs_down_write_all_sentry_locks(struct f2fs_sb_info *sbi)
{
        struct sit_info *sit_i = SIT_I(sbi);
        int cpu;

        for_each_online_cpu(cpu)
                down_write(per_cpu_ptr(sit_i->percore_sentry_lock, cpu));
}

static void f2fs_up_write_all_sentry_locks(struct f2fs_sb_info *sbi)
{
        struct sit_info *sit_i = SIT_I(sbi);
        int cpu;

        for_each_online_cpu(cpu)
                up_write(per_cpu_ptr(sit_i->percore_sentry_lock, cpu));
}

/*
 * DeLFS: the main area is split into num_online_cpus() equal ranges of
 * segments; core c "owns" (takes the sentry lock of) segments in its range,
 * and the last core also owns the remainder.
 */
int segno_to_sentry_cpu(struct f2fs_sb_info *sbi, unsigned int segno)
{
	int max_cpu = num_online_cpus();
	unsigned int segs_per_core = MAIN_SEGS(sbi) / max_cpu;
	int sentry_cpu;

	/* main_segs < max_cpu: everything belongs to the last core */
	if (!segs_per_core)
		return max_cpu - 1;

	sentry_cpu = segno / segs_per_core;
	if (sentry_cpu >= max_cpu)
		sentry_cpu = max_cpu - 1;
	return sentry_cpu;
}

/* DeLFS: [*start, *end] segment range owned by core @c (inclusive) */
static inline void __maybe_unused sentry_cpu_to_seg_range(int c, int max_cpu,
			int main_segs, unsigned int *start, unsigned int *end)
{
	int segs_per_core = main_segs / max_cpu;

	if (segs_per_core == 0) {
		*start = 0;
		*end = main_segs ? main_segs - 1 : 0;
		return;
	}

	if (c < 0)
		c = 0;
	if (c >= max_cpu)
		c = max_cpu - 1;

	*start = (unsigned int)(c * segs_per_core);
	if (c == max_cpu - 1)
		*end = (unsigned int)(main_segs - 1);
	else
		*end = (unsigned int)((c + 1) * segs_per_core - 1);
}

void get_sentry_range_from_cpu(struct f2fs_sb_info *sbi, int cpu,
				unsigned int *sentry_begin,
				unsigned int *sentry_end)
{
	int max_cpu = num_online_cpus();
	int main_segs = MAIN_SEGS(sbi);
	int segs_per_core;

	if (!sentry_begin || !sentry_end)
		return;

	if (max_cpu <= 0 || main_segs <= 0) {
		*sentry_begin = 0;
		*sentry_end = 0;
		return;
	}

	segs_per_core = main_segs / max_cpu;

	/* same clamping rule as segno_to_sentry_cpu() */
	if (cpu < 0)
		cpu = 0;
	if (cpu >= max_cpu)
		cpu = max_cpu - 1;

	if (segs_per_core == 0) {
		/* the last core owns all segments, the others own none */
		*sentry_begin = 0;
		*sentry_end = (cpu == max_cpu - 1) ? main_segs - 1 :
							(unsigned int)-1;
		return;
	}

	*sentry_begin = (unsigned int)(cpu * segs_per_core);
	if (cpu == max_cpu - 1)
		*sentry_end = (unsigned int)(main_segs - 1);
	else
		*sentry_end = (unsigned int)((cpu + 1) * segs_per_core - 1);
}

static unsigned long __reverse_ulong(unsigned char *str)
{
	unsigned long tmp = 0;
	int shift = 24, idx = 0;

#if BITS_PER_LONG == 64
	shift = 56;
#endif
	while (shift >= 0) {
		tmp |= (unsigned long)str[idx++] << shift;
		shift -= BITS_PER_BYTE;
	}
	return tmp;
}

/*
 * __reverse_ffs is copied from include/asm-generic/bitops/__ffs.h since
 * MSB and LSB are reversed in a byte by f2fs_set_bit.
 */
static inline unsigned long __reverse_ffs(unsigned long word)
{
	int num = 0;

#if BITS_PER_LONG == 64
	if ((word & 0xffffffff00000000UL) == 0)
		num += 32;
	else
		word >>= 32;
#endif
	if ((word & 0xffff0000) == 0)
		num += 16;
	else
		word >>= 16;

	if ((word & 0xff00) == 0)
		num += 8;
	else
		word >>= 8;

	if ((word & 0xf0) == 0)
		num += 4;
	else
		word >>= 4;

	if ((word & 0xc) == 0)
		num += 2;
	else
		word >>= 2;

	if ((word & 0x2) == 0)
		num += 1;
	return num;
}

/*
 * __find_rev_next(_zero)_bit is copied from lib/find_next_bit.c because
 * f2fs_set_bit makes MSB and LSB reversed in a byte.
 * @size must be integral times of unsigned long.
 * Example:
 *                             MSB <--> LSB
 *   f2fs_set_bit(0, bitmap) => 1000 0000
 *   f2fs_set_bit(7, bitmap) => 0000 0001
 */
static unsigned long __find_rev_next_bit(const unsigned long *addr,
			unsigned long size, unsigned long offset)
{
	const unsigned long *p = addr + BIT_WORD(offset);
	unsigned long result = size;
	unsigned long tmp;

	if (offset >= size)
		return size;

	size -= (offset & ~(BITS_PER_LONG - 1));
	offset %= BITS_PER_LONG;

	while (1) {
		if (*p == 0)
			goto pass;

		tmp = __reverse_ulong((unsigned char *)p);

		tmp &= ~0UL >> offset;
		if (size < BITS_PER_LONG)
			tmp &= (~0UL << (BITS_PER_LONG - size));
		if (tmp)
			goto found;
pass:
		if (size <= BITS_PER_LONG)
			break;
		size -= BITS_PER_LONG;
		offset = 0;
		p++;
	}
	return result;
found:
	return result - size + __reverse_ffs(tmp);
}

static unsigned long __find_rev_next_zero_bit(const unsigned long *addr,
			unsigned long size, unsigned long offset)
{
	const unsigned long *p = addr + BIT_WORD(offset);
	unsigned long result = size;
	unsigned long tmp;

	if (offset >= size)
		return size;

	size -= (offset & ~(BITS_PER_LONG - 1));
	offset %= BITS_PER_LONG;

	while (1) {
		if (*p == ~0UL)
			goto pass;

		tmp = __reverse_ulong((unsigned char *)p);

		if (offset)
			tmp |= ~0UL << (BITS_PER_LONG - offset);
		if (size < BITS_PER_LONG)
			tmp |= ~0UL >> size;
		if (tmp != ~0UL)
			goto found;
pass:
		if (size <= BITS_PER_LONG)
			break;
		size -= BITS_PER_LONG;
		offset = 0;
		p++;
	}
	return result;
found:
	return result - size + __reverse_ffz(tmp);
}

bool f2fs_need_SSR(struct f2fs_sb_info *sbi)
{
	int node_secs = get_blocktype_secs(sbi, F2FS_DIRTY_NODES);
	int dent_secs = get_blocktype_secs(sbi, F2FS_DIRTY_DENTS);
	int imeta_secs = get_blocktype_secs(sbi, F2FS_DIRTY_IMETA);

	if (f2fs_lfs_mode(sbi))
		return false;
	if (sbi->gc_mode == GC_URGENT_HIGH)
		return true;
	if (unlikely(is_sbi_flag_set(sbi, SBI_CP_DISABLED)))
		return true;

	return free_sections(sbi) <= (node_secs + 2 * dent_secs + imeta_secs +
			SM_I(sbi)->min_ssr_sections + reserved_sections(sbi));
}

void f2fs_abort_atomic_write(struct inode *inode, bool clean)
{
	struct f2fs_inode_info *fi = F2FS_I(inode);

	if (!f2fs_is_atomic_file(inode))
		return;

	if (clean)
		truncate_inode_pages_final(inode->i_mapping);

	release_atomic_write_cnt(inode);
	clear_inode_flag(inode, FI_ATOMIC_COMMITTED);
	clear_inode_flag(inode, FI_ATOMIC_REPLACE);
	clear_inode_flag(inode, FI_ATOMIC_FILE);
	if (is_inode_flag_set(inode, FI_ATOMIC_DIRTIED)) {
		clear_inode_flag(inode, FI_ATOMIC_DIRTIED);
		/*
		 * The vfs inode keeps clean during commit, but the f2fs inode
		 * doesn't. So clear the dirty state after commit and let
		 * f2fs_mark_inode_dirty_sync ensure a consistent dirty state.
		 */
		f2fs_inode_synced(inode);
		f2fs_mark_inode_dirty_sync(inode, true);
	}
	stat_dec_atomic_inode(inode);

	F2FS_I(inode)->atomic_write_task = NULL;

	if (clean) {
		f2fs_i_size_write(inode, fi->original_i_size);
		fi->original_i_size = 0;
	}
	/* avoid stale dirty inode during eviction */
	sync_inode_metadata(inode, 0);
}

static int __replace_atomic_write_block(struct inode *inode, pgoff_t index,
			block_t new_addr, block_t *old_addr, bool recover)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct dnode_of_data dn;
	struct node_info ni;
	int err;

retry:
	set_new_dnode(&dn, inode, NULL, NULL, 0);
	err = f2fs_get_dnode_of_data(&dn, index, ALLOC_NODE);
	if (err) {
		if (err == -ENOMEM) {
			memalloc_retry_wait(GFP_NOFS);
			goto retry;
		}
		return err;
	}

	err = f2fs_get_node_info(sbi, dn.nid, &ni, false);
	if (err) {
		f2fs_put_dnode(&dn);
		return err;
	}

	if (recover) {
		/* dn.data_blkaddr is always valid */
		if (!__is_valid_data_blkaddr(new_addr)) {
			if (new_addr == NULL_ADDR)
				dec_valid_block_count(sbi, inode, 1);
			f2fs_invalidate_blocks(sbi, dn.data_blkaddr, 1);
			f2fs_update_data_blkaddr(&dn, new_addr);
		} else {
			f2fs_replace_block(sbi, &dn, dn.data_blkaddr,
				new_addr, ni.version, true, true);
		}
	} else {
		blkcnt_t count = 1;

		err = inc_valid_block_count(sbi, inode, &count, true);
		if (err) {
			f2fs_put_dnode(&dn);
			return err;
		}

		*old_addr = dn.data_blkaddr;
		f2fs_truncate_data_blocks_range(&dn, 1);
		dec_valid_block_count(sbi, F2FS_I(inode)->cow_inode, count);

		f2fs_replace_block(sbi, &dn, dn.data_blkaddr, new_addr,
					ni.version, true, false);
	}

	f2fs_put_dnode(&dn);

	trace_f2fs_replace_atomic_write_block(inode, F2FS_I(inode)->cow_inode,
			index, old_addr ? *old_addr : 0, new_addr, recover);
	return 0;
}

static void __complete_revoke_list(struct inode *inode, struct list_head *head,
					bool revoke)
{
	struct revoke_entry *cur, *tmp;
	pgoff_t start_index = 0;
	bool truncate = is_inode_flag_set(inode, FI_ATOMIC_REPLACE);

	list_for_each_entry_safe(cur, tmp, head, list) {
		if (revoke) {
			__replace_atomic_write_block(inode, cur->index,
						cur->old_addr, NULL, true);
		} else if (truncate) {
			f2fs_truncate_hole(inode, start_index, cur->index);
			start_index = cur->index + 1;
		}

		list_del(&cur->list);
		kmem_cache_free(revoke_entry_slab, cur);
	}

	if (!revoke && truncate)
		f2fs_do_truncate_blocks(inode, start_index * PAGE_SIZE, false);
}

static int __f2fs_commit_atomic_write(struct inode *inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct f2fs_inode_info *fi = F2FS_I(inode);
	struct inode *cow_inode = fi->cow_inode;
	struct revoke_entry *new;
	struct list_head revoke_list;
	block_t blkaddr;
	struct dnode_of_data dn;
	pgoff_t len = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);
	pgoff_t off = 0, blen, index;
	int ret = 0, i;

	INIT_LIST_HEAD(&revoke_list);

	while (len) {
		blen = min_t(pgoff_t, ADDRS_PER_BLOCK(cow_inode), len);

		set_new_dnode(&dn, cow_inode, NULL, NULL, 0);
		ret = f2fs_get_dnode_of_data(&dn, off, LOOKUP_NODE_RA);
		if (ret && ret != -ENOENT) {
			goto out;
		} else if (ret == -ENOENT) {
			ret = 0;
			if (dn.max_level == 0)
				goto out;
			goto next;
		}

		blen = min((pgoff_t)ADDRS_PER_PAGE(dn.node_folio, cow_inode),
				len);
		index = off;
		for (i = 0; i < blen; i++, dn.ofs_in_node++, index++) {
			blkaddr = f2fs_data_blkaddr(&dn);

			if (!__is_valid_data_blkaddr(blkaddr)) {
				continue;
			} else if (!f2fs_is_valid_blkaddr(sbi, blkaddr,
					DATA_GENERIC_ENHANCE)) {
				f2fs_put_dnode(&dn);
				ret = -EFSCORRUPTED;
				goto out;
			}

			new = f2fs_kmem_cache_alloc(revoke_entry_slab, GFP_NOFS,
							true, NULL);

			ret = __replace_atomic_write_block(inode, index, blkaddr,
							&new->old_addr, false);
			if (ret) {
				f2fs_put_dnode(&dn);
				kmem_cache_free(revoke_entry_slab, new);
				goto out;
			}

			f2fs_update_data_blkaddr(&dn, NULL_ADDR);
			new->index = index;
			list_add_tail(&new->list, &revoke_list);
		}
		f2fs_put_dnode(&dn);
next:
		off += blen;
		len -= blen;
	}

out:
	if (time_to_inject(sbi, FAULT_ATOMIC_TIMEOUT))
		f2fs_schedule_timeout_killable(DEFAULT_FAULT_TIMEOUT, true);

	if (ret) {
		sbi->revoked_atomic_block += fi->atomic_write_cnt;
	} else {
		sbi->committed_atomic_block += fi->atomic_write_cnt;
		set_inode_flag(inode, FI_ATOMIC_COMMITTED);

		/*
		 * inode may has no FI_ATOMIC_DIRTIED flag due to no write
		 * before commit.
		 */
		if (is_inode_flag_set(inode, FI_ATOMIC_DIRTIED)) {
			/* clear atomic dirty status and set vfs dirty status */
			clear_inode_flag(inode, FI_ATOMIC_DIRTIED);
			f2fs_mark_inode_dirty_sync(inode, true);
		}
	}

	__complete_revoke_list(inode, &revoke_list, ret ? true : false);

	return ret;
}

int f2fs_commit_atomic_write(struct inode *inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct f2fs_inode_info *fi = F2FS_I(inode);
	struct f2fs_lock_context lc;
	int err;

	err = filemap_write_and_wait_range(inode->i_mapping, 0, LLONG_MAX);
	if (err)
		return err;

	f2fs_down_write(&fi->i_gc_rwsem[WRITE]);
	f2fs_lock_op(sbi, &lc);

	err = __f2fs_commit_atomic_write(inode);

	f2fs_unlock_op(sbi, &lc);
	f2fs_up_write(&fi->i_gc_rwsem[WRITE]);

	return err;
}

/*
 * This function balances dirty node and dentry pages.
 * In addition, it controls garbage collection.
 */
void f2fs_balance_fs(struct f2fs_sb_info *sbi, bool need)
{
	if (f2fs_cp_error(sbi))
		return;

	if (time_to_inject(sbi, FAULT_CHECKPOINT))
		f2fs_stop_checkpoint(sbi, false, STOP_CP_REASON_FAULT_INJECT);

	/* balance_fs_bg is able to be pending */
	if (need && excess_cached_nats(sbi))
		f2fs_balance_fs_bg(sbi, false);

	if (unlikely(is_sbi_flag_set(sbi, SBI_CP_DISABLED)))
		return;

	/*
	 * We should do GC or end up with checkpoint, if there are so many dirty
	 * dir/node pages without enough free segments.
	 */
	if (has_enough_free_secs(sbi, 0, 0)) {
		return;
	}

	/*
	 * We are entering critical free-space path.
	 * Before waking/doing FG_GC, make pending async old invalidations visible
	 * in SIT/dirty-seg state.  Otherwise GC may migrate blocks while their
	 * source valid bits are still set.
	 */
	if (oldinv_has_pending(sbi)) {
		oldinv_flush_all(sbi);

		/*
		 * oldinv_flush_all() itself does not necessarily create free
		 * sections immediately, but it can turn segments into PRE.
		 * Recheck anyway; if enough space became visible, skip GC.
		 */
		if (has_enough_free_secs(sbi, 0, 0)) {
			return;
		}
	}

	/*
	 * Submit all cached OPU/IPU DATA bios before triggering
	 * foreground GC to avoid potential deadlocks.
	 */
	f2fs_submit_merged_write(sbi, DATA);
	f2fs_submit_all_merged_ipu_writes(sbi);

	if (test_opt(sbi, GC_MERGE) && sbi->gc_thread.f2fs_gc_task) {
		DEFINE_WAIT(wait);

		prepare_to_wait(&sbi->gc_thread.fggc_wq, &wait,
					TASK_UNINTERRUPTIBLE);
		wake_up(&sbi->gc_thread.gc_wait_queue_head);
		io_schedule();
		finish_wait(&sbi->gc_thread.fggc_wq, &wait);
	} else {
		struct f2fs_gc_control gc_control = {
			.victim_segno = NULL_SEGNO,
			.init_gc_type = f2fs_sb_has_blkzoned(sbi) ?
				FG_GC : BG_GC,
			.no_bg_gc = true,
			.should_migrate_blocks = false,
			.err_gc_skipped = false,
			.nr_free_secs = 1,
		};

		/* f2fs_down_write(&sbi->gc_lock); */

		stat_inc_gc_call_count(sbi, FOREGROUND);
		f2fs_gc(sbi, &gc_control, smp_processor_id());
	}
}

static inline bool excess_dirty_threshold(struct f2fs_sb_info *sbi)
{
	int factor = f2fs_rwsem_is_locked(&sbi->cp_rwsem) ? 3 : 2;
	unsigned int dents = get_pages(sbi, F2FS_DIRTY_DENTS);
	unsigned int qdata = get_pages(sbi, F2FS_DIRTY_QDATA);
	unsigned int nodes = get_pages(sbi, F2FS_DIRTY_NODES);
	unsigned int meta = get_pages(sbi, F2FS_DIRTY_META);
	unsigned int imeta = get_pages(sbi, F2FS_DIRTY_IMETA);
	unsigned int threshold =
		SEGS_TO_BLKS(sbi, (factor * DEFAULT_DIRTY_THRESHOLD));
	unsigned int global_threshold = threshold * 3 / 2;

	if (dents >= threshold || qdata >= threshold ||
		nodes >= threshold || meta >= threshold ||
		imeta >= threshold)
		return true;
	return dents + qdata + nodes + meta + imeta >  global_threshold;
}

void f2fs_balance_fs_bg(struct f2fs_sb_info *sbi, bool from_bg)
{
	int nat_cpu;

	if (unlikely(is_sbi_flag_set(sbi, SBI_POR_DOING)))
		return;

	/* try to shrink extent cache when there is no enough memory */
	if (!f2fs_available_free_memory(sbi, READ_EXTENT_CACHE))
		f2fs_shrink_read_extent_tree(sbi,
				READ_EXTENT_CACHE_SHRINK_NUMBER);

	/* try to shrink age extent cache when there is no enough memory */
	if (!f2fs_available_free_memory(sbi, AGE_EXTENT_CACHE))
		f2fs_shrink_age_extent_tree(sbi,
				AGE_EXTENT_CACHE_SHRINK_NUMBER);

	/* check the # of cached NAT entries */
	if (!f2fs_available_free_memory(sbi, NAT_ENTRIES)) {
		for_each_online_cpu(nat_cpu)
			f2fs_try_to_free_nats(sbi, NAT_ENTRY_PER_BLOCK, nat_cpu);
	}

	if (!f2fs_available_free_memory(sbi, FREE_NIDS))
		f2fs_try_to_free_nids(sbi, MAX_FREE_NIDS);
	else
		f2fs_build_free_nids(sbi, false, false);

	if (excess_dirty_nats(sbi) || excess_dirty_threshold(sbi) ||
		excess_prefree_segs(sbi) || !f2fs_space_for_roll_forward(sbi))
		goto do_sync;

	/* there is background inflight IO or foreground operation recently */
	if (is_inflight_io(sbi, REQ_TIME) ||
		(!f2fs_time_over(sbi, REQ_TIME) && f2fs_rwsem_is_locked(&sbi->cp_rwsem)))
		return;

	/* exceed periodical checkpoint timeout threshold */
	if (f2fs_time_over(sbi, CP_TIME))
		goto do_sync;

	/* checkpoint is the only way to shrink partial cached entries */
	if (f2fs_available_free_memory(sbi, NAT_ENTRIES) &&
		f2fs_available_free_memory(sbi, INO_ENTRIES))
		return;

do_sync:
	if (test_opt(sbi, DATA_FLUSH) && from_bg) {
		struct blk_plug plug;

		mutex_lock(&sbi->flush_lock);

		blk_start_plug(&plug);
		f2fs_sync_dirty_inodes(sbi, FILE_INODE, false);
		blk_finish_plug(&plug);

		mutex_unlock(&sbi->flush_lock);
	}
	stat_inc_cp_call_count(sbi, BACKGROUND);
	f2fs_sync_fs(sbi->sb, 1);
}

static int __submit_flush_wait(struct f2fs_sb_info *sbi,
				struct block_device *bdev)
{
	int ret = blkdev_issue_flush(bdev);

	trace_f2fs_issue_flush(bdev, test_opt(sbi, NOBARRIER),
				test_opt(sbi, FLUSH_MERGE), ret);
	if (!ret)
		f2fs_update_iostat(sbi, NULL, FS_FLUSH_IO, 0);
	return ret;
}

static int submit_flush_wait(struct f2fs_sb_info *sbi, nid_t ino)
{
	int ret = 0;
	int i;

	if (!f2fs_is_multi_device(sbi))
		return __submit_flush_wait(sbi, sbi->sb->s_bdev);

	for (i = 0; i < sbi->s_ndevs; i++) {
		if (!f2fs_is_dirty_device(sbi, ino, i, FLUSH_INO))
			continue;
		ret = __submit_flush_wait(sbi, FDEV(i).bdev);
		if (ret)
			break;
	}
	return ret;
}

static int issue_flush_thread(void *data)
{
	struct f2fs_sb_info *sbi = data;
	struct flush_cmd_control *fcc = SM_I(sbi)->fcc_info;
	wait_queue_head_t *q = &fcc->flush_wait_queue;
repeat:
	if (kthread_should_stop())
		return 0;

	if (!llist_empty(&fcc->issue_list)) {
		struct flush_cmd *cmd, *next;
		int ret;

		fcc->dispatch_list = llist_del_all(&fcc->issue_list);
		fcc->dispatch_list = llist_reverse_order(fcc->dispatch_list);

		cmd = llist_entry(fcc->dispatch_list, struct flush_cmd, llnode);

		ret = submit_flush_wait(sbi, cmd->ino);
		atomic_inc(&fcc->issued_flush);

		llist_for_each_entry_safe(cmd, next,
					  fcc->dispatch_list, llnode) {
			cmd->ret = ret;
			complete(&cmd->wait);
		}
		fcc->dispatch_list = NULL;
	}

	wait_event_interruptible(*q,
		kthread_should_stop() || !llist_empty(&fcc->issue_list));
	goto repeat;
}

int f2fs_issue_flush(struct f2fs_sb_info *sbi, nid_t ino)
{
	struct flush_cmd_control *fcc = SM_I(sbi)->fcc_info;
	struct flush_cmd cmd;
	int ret;

	if (test_opt(sbi, NOBARRIER))
		return 0;

	if (!test_opt(sbi, FLUSH_MERGE)) {
		atomic_inc(&fcc->queued_flush);
		ret = submit_flush_wait(sbi, ino);
		atomic_dec(&fcc->queued_flush);
		atomic_inc(&fcc->issued_flush);
		return ret;
	}

	if (atomic_inc_return(&fcc->queued_flush) == 1 ||
	    f2fs_is_multi_device(sbi)) {
		ret = submit_flush_wait(sbi, ino);
		atomic_dec(&fcc->queued_flush);

		atomic_inc(&fcc->issued_flush);
		return ret;
	}

	cmd.ino = ino;
	init_completion(&cmd.wait);

	llist_add(&cmd.llnode, &fcc->issue_list);

	/*
	 * update issue_list before we wake up issue_flush thread, this
	 * smp_mb() pairs with another barrier in ___wait_event(), see
	 * more details in comments of waitqueue_active().
	 */
	smp_mb();

	if (waitqueue_active(&fcc->flush_wait_queue))
		wake_up(&fcc->flush_wait_queue);

	if (fcc->f2fs_issue_flush) {
		wait_for_completion(&cmd.wait);
		atomic_dec(&fcc->queued_flush);
	} else {
		struct llist_node *list;

		list = llist_del_all(&fcc->issue_list);
		if (!list) {
			wait_for_completion(&cmd.wait);
			atomic_dec(&fcc->queued_flush);
		} else {
			struct flush_cmd *tmp, *next;

			ret = submit_flush_wait(sbi, ino);

			llist_for_each_entry_safe(tmp, next, list, llnode) {
				if (tmp == &cmd) {
					cmd.ret = ret;
					atomic_dec(&fcc->queued_flush);
					continue;
				}
				tmp->ret = ret;
				complete(&tmp->wait);
			}
		}
	}

	return cmd.ret;
}

int f2fs_create_flush_cmd_control(struct f2fs_sb_info *sbi)
{
	dev_t dev = sbi->sb->s_bdev->bd_dev;
	struct flush_cmd_control *fcc;

	if (SM_I(sbi)->fcc_info) {
		fcc = SM_I(sbi)->fcc_info;
		if (fcc->f2fs_issue_flush)
			return 0;
		goto init_thread;
	}

	fcc = f2fs_kzalloc(sbi, sizeof(struct flush_cmd_control), GFP_KERNEL);
	if (!fcc)
		return -ENOMEM;
	atomic_set(&fcc->issued_flush, 0);
	atomic_set(&fcc->queued_flush, 0);
	init_waitqueue_head(&fcc->flush_wait_queue);
	init_llist_head(&fcc->issue_list);
	SM_I(sbi)->fcc_info = fcc;
	if (!test_opt(sbi, FLUSH_MERGE))
		return 0;

init_thread:
	fcc->f2fs_issue_flush = kthread_run(issue_flush_thread, sbi,
				"f2fs_flush-%u:%u", MAJOR(dev), MINOR(dev));
	if (IS_ERR(fcc->f2fs_issue_flush)) {
		int err = PTR_ERR(fcc->f2fs_issue_flush);

		fcc->f2fs_issue_flush = NULL;
		return err;
	}

	return 0;
}

void f2fs_destroy_flush_cmd_control(struct f2fs_sb_info *sbi, bool free)
{
	struct flush_cmd_control *fcc = SM_I(sbi)->fcc_info;

	if (fcc && fcc->f2fs_issue_flush) {
		struct task_struct *flush_thread = fcc->f2fs_issue_flush;

		fcc->f2fs_issue_flush = NULL;
		kthread_stop(flush_thread);
	}
	if (free) {
		kfree(fcc);
		SM_I(sbi)->fcc_info = NULL;
	}
}

int f2fs_flush_device_cache(struct f2fs_sb_info *sbi)
{
	int ret = 0, i;

	if (!f2fs_is_multi_device(sbi))
		return 0;

	if (test_opt(sbi, NOBARRIER))
		return 0;

	for (i = 1; i < sbi->s_ndevs; i++) {
		int count = DEFAULT_RETRY_IO_COUNT;

		if (!f2fs_test_bit(i, (char *)&sbi->dirty_device))
			continue;

		do {
			ret = __submit_flush_wait(sbi, FDEV(i).bdev);
			if (ret)
				f2fs_schedule_timeout(DEFAULT_SCHEDULE_TIMEOUT);
		} while (ret && --count);

		if (ret) {
			f2fs_stop_checkpoint(sbi, false,
					STOP_CP_REASON_FLUSH_FAIL);
			break;
		}

		spin_lock(&sbi->dev_lock);
		f2fs_clear_bit(i, (char *)&sbi->dirty_device);
		spin_unlock(&sbi->dev_lock);
	}

	return ret;
}

static void __locate_dirty_segment(struct f2fs_sb_info *sbi, unsigned int segno,
		enum dirty_type dirty_type)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	struct dirty_seg_percpu *p;
	struct seg_entry *sentry;
	unsigned int valid_blocks;
	unsigned int usable_blocks;
	int seglist_cpu;

	if (segno == NULL_SEGNO || dirty_type >= NR_DIRTY_TYPE)
		return;

	/*
	 * Current segment must never be inserted into dirty lists.
	 * locate_dirty_segment() is responsible for removing stale bits.
	 */
	if (is_curseg(sbi, segno))
		return;

	usable_blocks = f2fs_usable_blks_in_seg(sbi, segno);
	valid_blocks = get_valid_blocks(sbi, segno, false);

	/*
	 * Strong invariant:
	 *
	 * DIRTY means partially invalid:
	 *     0 < valid_blocks < usable_blocks
	 *
	 * PRE means fully invalid:
	 *     valid_blocks == 0
	 *
	 * Full-valid segment must never be inserted into DIRTY.
	 */
	if (dirty_type == DIRTY) {
		if (!usable_blocks || !valid_blocks ||
		    valid_blocks >= usable_blocks)
			return;
	} else if (dirty_type == PRE) {
		if (valid_blocks)
			return;
	}

	seglist_cpu = segno_to_sentry_cpu(sbi, segno);
	p = per_cpu_ptr(dirty_i->pcpu, seglist_cpu);

	if (!test_and_set_bit(segno, p->p_dirty_segmap[dirty_type])) {
		atomic_add(1, &dirty_i->nr_dirty[dirty_type]);
		p->p_nr_dirty[dirty_type]++;
	}

	if (dirty_type == DIRTY) {
		enum dirty_type t;

		sentry = get_seg_entry(sbi, segno);
		t = sentry->type;

		if (unlikely(t >= DIRTY)) {
			f2fs_bug_on(sbi, 1);
			return;
		}

		if (!test_and_set_bit(segno, p->p_dirty_segmap[t])) {
			atomic_add(1, &dirty_i->nr_dirty[t]);
			p->p_nr_dirty[t]++;
		}

		if (__is_large_section(sbi)) {
			/* large section path is disabled in your setup */
		}
	}
}

static void __remove_dirty_segment(struct f2fs_sb_info *sbi, unsigned int segno,
		enum dirty_type dirty_type)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	struct dirty_seg_percpu *p;
	unsigned int valid_blocks;
	int seglist_cpu;
	int t;

	if (segno == NULL_SEGNO || dirty_type >= NR_DIRTY_TYPE)
		return;

	seglist_cpu = segno_to_sentry_cpu(sbi, segno);
	p = per_cpu_ptr(dirty_i->pcpu, seglist_cpu);

	/*
	 * If a segment is removed from DIRTY, it must not remain as a
	 * background victim hint either.  The old code cleared victim_secmap
	 * only when valid_blocks == 0, which allowed full-valid stale victims.
	 */
	if (dirty_type == DIRTY)
		clear_bit(GET_SEC_FROM_SEG(sbi, segno), dirty_i->victim_secmap);

	if (test_and_clear_bit(segno, p->p_dirty_segmap[dirty_type])) {
		atomic_sub(1, &dirty_i->nr_dirty[dirty_type]);
		p->p_nr_dirty[dirty_type]--;
	}

	if (dirty_type != DIRTY)
		return;

	/*
	 * Clear all type-specific dirty maps, not only sentry->type.
	 *
	 * Reason:
	 * sentry->type can change when a segment is reused.  If we clear only
	 * the current type, a stale bit can remain in the old type-specific map.
	 */
	for (t = DIRTY_HOT_DATA; t < DIRTY; t++) {
		if (test_and_clear_bit(segno, p->p_dirty_segmap[t])) {
			atomic_sub(1, &dirty_i->nr_dirty[t]);
			p->p_nr_dirty[t]--;
		}
	}

	valid_blocks = get_valid_blocks(sbi, segno, true);
	if (valid_blocks == 0) {
#ifdef CONFIG_F2FS_CHECK_FS
		clear_bit(segno, SIT_I(sbi)->invalid_segmap);
#endif
	}

	if (__is_large_section(sbi)) {
		/* large section path is disabled in your setup */

	}
}

void f2fs_remove_dirty_segment_locked(struct f2fs_sb_info *sbi,
		unsigned int segno, enum dirty_type dirty_type)
{
	__remove_dirty_segment(sbi, segno, dirty_type);
}

/*
 * Should not occur error such as -ENOMEM.
 * Adding dirty entry into seglist is not critical operation.
 * If a given segment is one of current working segments, it won't be added.
 */
void locate_dirty_segment(struct f2fs_sb_info *sbi, unsigned int segno)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	struct dirty_seg_percpu *p;
	unsigned short valid_blocks, ckpt_valid_blocks;
	unsigned int usable_blocks;
	int seglist_cpu;

	if (segno == NULL_SEGNO)
		return;

	usable_blocks = f2fs_usable_blks_in_seg(sbi, segno);

	seglist_cpu = segno_to_sentry_cpu(sbi, segno);
	p = per_cpu_ptr(dirty_i->pcpu, seglist_cpu);

	mutex_lock(&p->p_seglist_lock);

	/*
	 * Current segment must not be in DIRTY or PRE.
	 *
	 * The old code returned immediately for curseg, which prevented new
	 * insertion but failed to clean stale dirty bits.  That stale bit can
	 * later become visible after this segment stops being current.
	 */
	if (is_curseg(sbi, segno)) {
		__remove_dirty_segment(sbi, segno, DIRTY);
		__remove_dirty_segment(sbi, segno, PRE);
		goto out_unlock;
	}

	valid_blocks = get_valid_blocks(sbi, segno, false);
	ckpt_valid_blocks = get_ckpt_valid_blocks(sbi, segno, false);

	if (unlikely(valid_blocks > usable_blocks)) {
		f2fs_bug_on(sbi, 1);
		__remove_dirty_segment(sbi, segno, DIRTY);
		__remove_dirty_segment(sbi, segno, PRE);
		goto out_unlock;
	}

	if (valid_blocks == 0 &&
	    (!is_sbi_flag_set(sbi, SBI_CP_DISABLED) ||
	     ckpt_valid_blocks == usable_blocks)) {
		/*
		 * Fully invalid segment.
		 */
		__locate_dirty_segment(sbi, segno, PRE);
		__remove_dirty_segment(sbi, segno, DIRTY);
	} else if (valid_blocks > 0 && valid_blocks < usable_blocks) {
		/*
		 * Real dirty segment: partially valid, partially invalid.
		 */
		__remove_dirty_segment(sbi, segno, PRE);
		__locate_dirty_segment(sbi, segno, DIRTY);
	} else {
		/*
		 * Full-valid segment or CP-disabled special case.
		 * It must not be in DIRTY or PRE.
		 */
		__remove_dirty_segment(sbi, segno, DIRTY);
		__remove_dirty_segment(sbi, segno, PRE);
	}

out_unlock:
	mutex_unlock(&p->p_seglist_lock);
}

/* This moves currently empty dirty blocks to prefree. Must hold seglist_lock */
void f2fs_dirty_to_prefree(struct f2fs_sb_info *sbi)
{
        struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
        unsigned int segno;
        int seglist_cpu;
        struct dirty_seg_percpu *p;

        for_each_online_cpu(seglist_cpu) {
                p = per_cpu_ptr(dirty_i->pcpu, seglist_cpu);
                mutex_lock(&p->p_seglist_lock);

                for_each_set_bit(segno, p->p_dirty_segmap[DIRTY], MAIN_SEGS(sbi)) {
                        if (get_valid_blocks(sbi, segno, false))
                                continue;
                        if (is_curseg(sbi, segno))
                                continue;

                        __locate_dirty_segment(sbi, segno, PRE);
                        __remove_dirty_segment(sbi, segno, DIRTY);
                }

                mutex_unlock(&p->p_seglist_lock);
        }
}

block_t f2fs_get_unusable_blocks(struct f2fs_sb_info *sbi)
{
	int ovp_hole_segs =
		(overprovision_segments(sbi) - reserved_segments(sbi));
	block_t ovp_holes = SEGS_TO_BLKS(sbi, ovp_hole_segs);
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	block_t holes[2] = {0, 0};	/* DATA and NODE */
	block_t unusable;
	struct seg_entry *se;
	unsigned int segno;


	unusable = max(holes[DATA], holes[NODE]);
	if (unusable > ovp_holes)
		return unusable - ovp_holes;
	return 0;
}

int f2fs_disable_cp_again(struct f2fs_sb_info *sbi, block_t unusable)
{
	int ovp_hole_segs =
		(overprovision_segments(sbi) - reserved_segments(sbi));

	if (F2FS_OPTION(sbi).unusable_cap_perc == 100)
		return 0;
	if (unusable > F2FS_OPTION(sbi).unusable_cap)
		return -EAGAIN;
	if (is_sbi_flag_set(sbi, SBI_CP_DISABLED_QUICK) &&
		dirty_segments(sbi) > ovp_hole_segs)
		return -EAGAIN;
	if (has_not_enough_free_secs(sbi, 0, 0))
		return -EAGAIN;
	return 0;
}

/* This is only used by SBI_CP_DISABLED */
static unsigned int get_free_segment(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	unsigned int segno = 0;

	return NULL_SEGNO;
}

static struct discard_cmd *__create_discard_cmd(struct f2fs_sb_info *sbi,
		struct block_device *bdev, block_t lstart,
		block_t start, block_t len)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct list_head *p_pend_list;
	struct discard_cmd *dc;

	f2fs_bug_on(sbi, !len);

	int dcc_cpu = segno_to_sentry_cpu(sbi, GET_SEGNO(sbi, lstart));
	struct discard_set *ds = per_cpu_ptr(dcc->ds, dcc_cpu);

	p_pend_list = &ds->p_pend_list[plist_idx(len)];

	dc = f2fs_kmem_cache_alloc(discard_cmd_slab, GFP_NOFS, true, NULL);
	INIT_LIST_HEAD(&dc->list);
	dc->bdev = bdev;
	dc->di.lstart = lstart;
	dc->di.start = start;
	dc->di.len = len;
	dc->ref = 0;
	dc->state = D_PREP;
	dc->queued = 0;
	dc->error = 0;
	init_completion(&dc->wait);
	list_add_tail(&dc->list, p_pend_list);
	spin_lock_init(&dc->lock);
	dc->bio_ref = 0;
	atomic_inc(&dcc->discard_cmd_cnt);
	dcc->undiscard_blks += len;

	return dc;
}

static bool f2fs_check_discard_tree(struct f2fs_sb_info *sbi)
{
#ifdef CONFIG_F2FS_CHECK_FS
#endif
	return true;
}

static struct discard_cmd *__lookup_discard_cmd(struct f2fs_sb_info *sbi,
						block_t blkaddr)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	
	int dcc_cpu = segno_to_sentry_cpu(sbi, GET_SEGNO(sbi, blkaddr));
	struct discard_set *ds = per_cpu_ptr(dcc->ds, dcc_cpu);

	struct rb_node *node = ds->p_root.rb_root.rb_node;
	struct discard_cmd *dc;

	while (node) {
		dc = rb_entry(node, struct discard_cmd, rb_node);

		if (blkaddr < dc->di.lstart)
			node = node->rb_left;
		else if (blkaddr >= dc->di.lstart + dc->di.len)
			node = node->rb_right;
		else
			return dc;
	}
	return NULL;
}

static struct discard_cmd *__lookup_discard_cmd_ret(struct rb_root_cached *root,
				block_t blkaddr,
				struct discard_cmd **prev_entry,
				struct discard_cmd **next_entry,
				struct rb_node ***insert_p,
				struct rb_node **insert_parent)
{
	struct rb_node **pnode = &root->rb_root.rb_node;
	struct rb_node *parent = NULL, *tmp_node;
	struct discard_cmd *dc;

	*insert_p = NULL;
	*insert_parent = NULL;
	*prev_entry = NULL;
	*next_entry = NULL;

	if (RB_EMPTY_ROOT(&root->rb_root))
		return NULL;

	while (*pnode) {
		parent = *pnode;
		dc = rb_entry(*pnode, struct discard_cmd, rb_node);

		if (blkaddr < dc->di.lstart)
			pnode = &(*pnode)->rb_left;
		else if (blkaddr >= dc->di.lstart + dc->di.len)
			pnode = &(*pnode)->rb_right;
		else
			goto lookup_neighbors;
	}

	*insert_p = pnode;
	*insert_parent = parent;

	dc = rb_entry(parent, struct discard_cmd, rb_node);
	tmp_node = parent;
	if (parent && blkaddr > dc->di.lstart)
		tmp_node = rb_next(parent);
	*next_entry = rb_entry_safe(tmp_node, struct discard_cmd, rb_node);

	tmp_node = parent;
	if (parent && blkaddr < dc->di.lstart)
		tmp_node = rb_prev(parent);
	*prev_entry = rb_entry_safe(tmp_node, struct discard_cmd, rb_node);
	return NULL;

lookup_neighbors:
	/* lookup prev node for merging backward later */
	tmp_node = rb_prev(&dc->rb_node);
	*prev_entry = rb_entry_safe(tmp_node, struct discard_cmd, rb_node);

	/* lookup next node for merging frontward later */
	tmp_node = rb_next(&dc->rb_node);
	*next_entry = rb_entry_safe(tmp_node, struct discard_cmd, rb_node);
	return dc;
}

static void __detach_discard_cmd(struct discard_cmd_control *dcc,
							struct discard_cmd *dc,
							struct f2fs_sb_info *sbi)
{
	int dcc_cpu = segno_to_sentry_cpu(sbi, GET_SEGNO(sbi, dc->di.lstart));
	struct discard_set *ds = per_cpu_ptr(dcc->ds, dcc_cpu);

	if (dc->state == D_DONE)
		atomic_sub(dc->queued, &dcc->queued_discard);

	list_del(&dc->list);
	rb_erase_cached(&dc->rb_node, &ds->p_root);
	dcc->undiscard_blks -= dc->di.len;

	kmem_cache_free(discard_cmd_slab, dc);

	atomic_dec(&dcc->discard_cmd_cnt);
}

static void __remove_discard_cmd(struct f2fs_sb_info *sbi,
							struct discard_cmd *dc)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	unsigned long flags;

	trace_f2fs_remove_discard(dc->bdev, dc->di.start, dc->di.len);

	spin_lock_irqsave(&dc->lock, flags);
	if (dc->bio_ref) {
		spin_unlock_irqrestore(&dc->lock, flags);
		return;
	}
	spin_unlock_irqrestore(&dc->lock, flags);

	f2fs_bug_on(sbi, dc->ref);

	if (dc->error == -EOPNOTSUPP)
		dc->error = 0;

	if (dc->error)
		f2fs_info_ratelimited(sbi,
			"Issue discard(%u, %u, %u) failed, ret: %d",
			dc->di.lstart, dc->di.start, dc->di.len, dc->error);
	__detach_discard_cmd(dcc, dc, sbi);
}

static void f2fs_submit_discard_endio(struct bio *bio)
{
	struct discard_cmd *dc = (struct discard_cmd *)bio->bi_private;
	unsigned long flags;

	spin_lock_irqsave(&dc->lock, flags);
	if (!dc->error)
		dc->error = blk_status_to_errno(bio->bi_status);
	dc->bio_ref--;
	if (!dc->bio_ref && dc->state == D_SUBMIT) {
		dc->state = D_DONE;
		complete_all(&dc->wait);
	}
	spin_unlock_irqrestore(&dc->lock, flags);
	bio_put(bio);
}

static void __check_sit_bitmap(struct f2fs_sb_info *sbi,
				block_t start, block_t end)
{
#ifdef CONFIG_F2FS_CHECK_FS
	struct seg_entry *sentry;
	unsigned int segno;
	block_t blk = start;
	unsigned long offset, size, *map;

	while (blk < end) {
		segno = GET_SEGNO(sbi, blk);
		sentry = get_seg_entry(sbi, segno);
		offset = GET_BLKOFF_FROM_SEG0(sbi, blk);

		if (end < START_BLOCK(sbi, segno + 1))
			size = GET_BLKOFF_FROM_SEG0(sbi, end);
		else
			size = BLKS_PER_SEG(sbi);
		map = (unsigned long *)(sentry->cur_valid_map);
		offset = __find_rev_next_bit(map, size, offset);
		f2fs_bug_on(sbi, offset != size);
		blk = START_BLOCK(sbi, segno + 1);
	}
#endif
}

static void __init_discard_policy(struct f2fs_sb_info *sbi,
				struct discard_policy *dpolicy,
				int discard_type, unsigned int granularity)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;

	/* common policy */
	dpolicy->type = discard_type;
	dpolicy->sync = true;
	dpolicy->ordered = false;
	dpolicy->granularity = granularity;

	dpolicy->max_requests = dcc->max_discard_request;
	dpolicy->io_aware_gran = dcc->discard_io_aware_gran;
	dpolicy->timeout = false;

	if (discard_type == DPOLICY_BG) {
		dpolicy->min_interval = dcc->min_discard_issue_time;
		dpolicy->mid_interval = dcc->mid_discard_issue_time;
		dpolicy->max_interval = dcc->max_discard_issue_time;
		if (dcc->discard_io_aware == DPOLICY_IO_AWARE_ENABLE)
			dpolicy->io_aware = true;
		else if (dcc->discard_io_aware == DPOLICY_IO_AWARE_DISABLE)
			dpolicy->io_aware = false;
		dpolicy->sync = false;
		dpolicy->ordered = true;
		if (utilization(sbi) > dcc->discard_urgent_util) {
			dpolicy->granularity = MIN_DISCARD_GRANULARITY;
			if (atomic_read(&dcc->discard_cmd_cnt))
				dpolicy->max_interval =
					dcc->min_discard_issue_time;
		}
	} else if (discard_type == DPOLICY_FORCE) {
		dpolicy->min_interval = dcc->min_discard_issue_time;
		dpolicy->mid_interval = dcc->mid_discard_issue_time;
		dpolicy->max_interval = dcc->max_discard_issue_time;
		dpolicy->io_aware = false;
	} else if (discard_type == DPOLICY_FSTRIM) {
		dpolicy->io_aware = false;
	} else if (discard_type == DPOLICY_UMOUNT) {
		dpolicy->io_aware = false;
		/* we need to issue all to keep CP_TRIMMED_FLAG */
		dpolicy->granularity = MIN_DISCARD_GRANULARITY;
		dpolicy->timeout = true;
	}
}

static void __update_discard_tree_range(struct f2fs_sb_info *sbi,
				struct block_device *bdev, block_t lstart,
				block_t start, block_t len);

#ifdef CONFIG_BLK_DEV_ZONED
static void __submit_zone_reset_cmd(struct f2fs_sb_info *sbi,
				   struct discard_cmd *dc, blk_opf_t flag,
				   struct list_head *wait_list,
				   unsigned int *issued)
{
}
#endif

/* this function is copied from blkdev_issue_discard from block/blk-lib.c */
static int __submit_discard_cmd(struct f2fs_sb_info *sbi,
				struct discard_policy *dpolicy,
				struct discard_cmd *dc, int *issued)
{
	struct block_device *bdev = dc->bdev;
	unsigned int max_discard_blocks =
			SECTOR_TO_BLOCK(bdev_max_discard_sectors(bdev));
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;

	struct list_head *p_wait_list;
	blk_opf_t flag = dpolicy->sync ? REQ_SYNC : 0;
	block_t lstart, start, len, total_len;

	if (dc->state != D_PREP)
		return 0;

	if (is_sbi_flag_set(sbi, SBI_NEED_FSCK))
		return 0;

#ifdef CONFIG_BLK_DEV_ZONED
	if (f2fs_sb_has_blkzoned(sbi) && bdev_is_zoned(bdev)) {
	}
#endif

	/*
	 * stop issuing discard for any of below cases:
	 * 1. device is conventional zone, but it doesn't support discard.
	 * 2. device is regulare device, after snapshot it doesn't support
	 * discard.
	 */
	if (!bdev_max_discard_sectors(bdev))
		return -EOPNOTSUPP;

	trace_f2fs_issue_discard(bdev, dc->di.start, dc->di.len);

	lstart = dc->di.lstart;
	start = dc->di.start;
	len = dc->di.len;
	total_len = len;

	dc->di.len = 0;

	int dcc_cpu = segno_to_sentry_cpu(sbi, GET_SEGNO(sbi, lstart));
	struct discard_set *ds = per_cpu_ptr(dcc->ds, dcc_cpu);

	p_wait_list = (dpolicy->type == DPOLICY_FSTRIM) ?
					&(ds->p_fstrim_list) : &(ds->p_wait_list);

	while (total_len && *issued < dpolicy->max_requests) {
		struct bio *bio = NULL;
		unsigned long flags;
		bool last = true;

		if (len > max_discard_blocks) {
			len = max_discard_blocks;
			last = false;
		}

		(*issued)++;
		if (*issued == dpolicy->max_requests)
			last = true;

		dc->di.len += len;

		__blkdev_issue_discard(bdev, SECTOR_FROM_BLOCK(start),
				SECTOR_FROM_BLOCK(len), GFP_NOFS, &bio);
		f2fs_bug_on(sbi, !bio);

		/*
		 * should keep before submission to avoid D_DONE
		 * right away
		 */
		spin_lock_irqsave(&dc->lock, flags);
		if (last)
			dc->state = D_SUBMIT;
		else
			dc->state = D_PARTIAL;
		dc->bio_ref++;
		spin_unlock_irqrestore(&dc->lock, flags);

		atomic_inc(&dcc->queued_discard);
		dc->queued++;
		list_move_tail(&dc->list, p_wait_list);

		/* sanity check on discard range */
		__check_sit_bitmap(sbi, lstart, lstart + len);

		bio->bi_private = dc;
		bio->bi_end_io = f2fs_submit_discard_endio;
		bio->bi_opf |= flag;
		submit_bio(bio);


		atomic_inc(&dcc->issued_discard);

		f2fs_update_iostat(sbi, NULL, FS_DISCARD_IO, len * F2FS_BLKSIZE);

		lstart += len;
		start += len;
		total_len -= len;
		len = total_len;
	}

	if (len) {
		dcc->undiscard_blks -= len;
		__update_discard_tree_range(sbi, bdev, lstart, start, len);
	}
	return 0;
}

static void __insert_discard_cmd(struct f2fs_sb_info *sbi,
				struct block_device *bdev, block_t lstart,
				block_t start, block_t len)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct rb_node **p;
	struct rb_node *parent = NULL;
	struct discard_cmd *dc;
	bool leftmost = true;

	int dcc_cpu = segno_to_sentry_cpu(sbi, GET_SEGNO(sbi, lstart));
	struct discard_set *ds = per_cpu_ptr(dcc->ds, dcc_cpu);

	p = &ds->p_root.rb_root.rb_node;

	/* look up rb tree to find parent node */
	while (*p) {
		parent = *p;
		dc = rb_entry(parent, struct discard_cmd, rb_node);

		if (lstart < dc->di.lstart) {
			p = &(*p)->rb_left;
		} else if (lstart >= dc->di.lstart + dc->di.len) {
			p = &(*p)->rb_right;
			leftmost = false;
		} else {
			/* Let's skip to add, if exists */
			return;
		}
	}

	dc = __create_discard_cmd(sbi, bdev, lstart, start, len);

	rb_link_node(&dc->rb_node, parent, p);
	rb_insert_color_cached(&dc->rb_node, &ds->p_root, leftmost);
}

static void __relocate_discard_cmd(struct discard_cmd_control *dcc,
						struct discard_cmd *dc,
						struct f2fs_sb_info *sbi)
{
	int dcc_cpu = segno_to_sentry_cpu(sbi, GET_SEGNO(sbi, dc->di.lstart));
	struct discard_set *ds = per_cpu_ptr(dcc->ds, dcc_cpu);

	list_move_tail(&dc->list, &ds->p_pend_list[plist_idx(dc->di.len)]);
}

static void __punch_discard_cmd(struct f2fs_sb_info *sbi,
				struct discard_cmd *dc, block_t blkaddr)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct discard_info di = dc->di;
	bool modified = false;

	if (dc->state == D_DONE || dc->di.len == 1) {
		__remove_discard_cmd(sbi, dc);
		return;
	}

	dcc->undiscard_blks -= di.len;

	if (blkaddr > di.lstart) {
		dc->di.len = blkaddr - dc->di.lstart;
		dcc->undiscard_blks += dc->di.len;
		__relocate_discard_cmd(dcc, dc, sbi);
		modified = true;
	}

	if (blkaddr < di.lstart + di.len - 1) {
		if (modified) {
			__insert_discard_cmd(sbi, dc->bdev, blkaddr + 1,
					di.start + blkaddr + 1 - di.lstart,
					di.lstart + di.len - 1 - blkaddr);
		} else {
			dc->di.lstart++;
			dc->di.len--;
			dc->di.start++;
			dcc->undiscard_blks += dc->di.len;
			__relocate_discard_cmd(dcc, dc, sbi);
		}
	}
}

static void __update_discard_tree_range(struct f2fs_sb_info *sbi,
				struct block_device *bdev, block_t lstart,
				block_t start, block_t len)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct discard_cmd *prev_dc = NULL, *next_dc = NULL;
	struct discard_cmd *dc;
	struct discard_info di = {0};
	struct rb_node **insert_p = NULL, *insert_parent = NULL;
	unsigned int max_discard_blocks =
			SECTOR_TO_BLOCK(bdev_max_discard_sectors(bdev));
	block_t end = lstart + len;

	int dcc_cpu = segno_to_sentry_cpu(sbi, GET_SEGNO(sbi, lstart));
	struct discard_set *ds = per_cpu_ptr(dcc->ds, dcc_cpu);

	dc = __lookup_discard_cmd_ret(&ds->p_root, lstart,
				&prev_dc, &next_dc, &insert_p, &insert_parent);
	if (dc)
		prev_dc = dc;

	if (!prev_dc) {
		di.lstart = lstart;
		di.len = next_dc ? next_dc->di.lstart - lstart : len;
		di.len = min(di.len, len);
		di.start = start;
	}

	while (1) {
		struct rb_node *node;
		bool merged = false;
		struct discard_cmd *tdc = NULL;

		if (prev_dc) {
			di.lstart = prev_dc->di.lstart + prev_dc->di.len;
			if (di.lstart < lstart)
				di.lstart = lstart;
			if (di.lstart >= end)
				break;

			if (!next_dc || next_dc->di.lstart > end)
				di.len = end - di.lstart;
			else
				di.len = next_dc->di.lstart - di.lstart;
			di.start = start + di.lstart - lstart;
		}

		if (!di.len)
			goto next;

		if (prev_dc && prev_dc->state == D_PREP &&
			prev_dc->bdev == bdev &&
			__is_discard_back_mergeable(&di, &prev_dc->di,
							max_discard_blocks)) {
			prev_dc->di.len += di.len;
			dcc->undiscard_blks += di.len;
			__relocate_discard_cmd(dcc, prev_dc, sbi);
			di = prev_dc->di;
			tdc = prev_dc;
			merged = true;
		}

		if (next_dc && next_dc->state == D_PREP &&
			next_dc->bdev == bdev &&
			__is_discard_front_mergeable(&di, &next_dc->di,
							max_discard_blocks)) {
			next_dc->di.lstart = di.lstart;
			next_dc->di.len += di.len;
			next_dc->di.start = di.start;
			dcc->undiscard_blks += di.len;
			__relocate_discard_cmd(dcc, next_dc, sbi);
			if (tdc)
				__remove_discard_cmd(sbi, tdc);
			merged = true;
		}

		if (!merged)
			__insert_discard_cmd(sbi, bdev,
						di.lstart, di.start, di.len);
 next:
		prev_dc = next_dc;
		if (!prev_dc)
			break;

		node = rb_next(&prev_dc->rb_node);
		next_dc = rb_entry_safe(node, struct discard_cmd, rb_node);
	}
}

#ifdef CONFIG_BLK_DEV_ZONED
static void __queue_zone_reset_cmd(struct f2fs_sb_info *sbi,
		struct block_device *bdev, block_t blkstart, block_t lblkstart,
		block_t blklen)
{
}
#endif

static void __queue_discard_cmd(struct f2fs_sb_info *sbi,
		struct block_device *bdev, block_t blkstart, block_t blklen)
{
	block_t lblkstart = blkstart;

	int dcc_cpu = segno_to_sentry_cpu(sbi, GET_SEGNO(sbi, lblkstart));
	struct discard_set *ds = per_cpu_ptr(SM_I(sbi)->dcc_info->ds, dcc_cpu);

	if (!f2fs_bdev_support_discard(bdev))
		return;

	trace_f2fs_queue_discard(bdev, blkstart, blklen);

	if (f2fs_is_multi_device(sbi)) {
		int devi = f2fs_target_device_index(sbi, blkstart);

		blkstart -= FDEV(devi).start_blk;
	}
	mutex_lock(&ds->p_cmd_lock);
	__update_discard_tree_range(sbi, bdev, lblkstart, blkstart, blklen);
	mutex_unlock(&ds->p_cmd_lock);
}

static void __issue_discard_cmd_orderly(struct f2fs_sb_info *sbi,
		struct discard_policy *dpolicy, int *issued)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct discard_cmd *prev_dc = NULL, *next_dc = NULL;
	struct rb_node **insert_p = NULL, *insert_parent = NULL;
	struct discard_cmd *dc;
	struct blk_plug plug;
	bool io_interrupted = false;

	int dcc_cpu;
	for_each_online_cpu(dcc_cpu) {
		struct discard_set *ds = per_cpu_ptr(dcc->ds, dcc_cpu);

		mutex_lock(&ds->p_cmd_lock);
		dc = __lookup_discard_cmd_ret(&ds->p_root, dcc->next_pos,
					&prev_dc, &next_dc, &insert_p, &insert_parent);
		if (!dc)
			dc = next_dc;

		blk_start_plug(&plug);

		while (dc) {
			struct rb_node *node;
			int err = 0;

			if (*issued > 0 && unlikely(freezing(current)))
				break;

			if (dc->state != D_PREP)
				goto next;

			if (dpolicy->io_aware && !is_idle(sbi, DISCARD_TIME)) {
				io_interrupted = true;
				break;
			}

			dcc->next_pos = dc->di.lstart + dc->di.len;
			err = __submit_discard_cmd(sbi, dpolicy, dc, issued);

			if (*issued >= dpolicy->max_requests)
				break;
next:
			node = rb_next(&dc->rb_node);
			if (err)
				__remove_discard_cmd(sbi, dc);
			dc = rb_entry_safe(node, struct discard_cmd, rb_node);
		}

		blk_finish_plug(&plug);

		if (!dc)
			dcc->next_pos = 0;

		mutex_unlock(&ds->p_cmd_lock);
	}


	if (!(*issued) && io_interrupted)
		*issued = -1;
}
static unsigned int __wait_all_discard_cmd(struct f2fs_sb_info *sbi,
					struct discard_policy *dpolicy);

static int __issue_discard_cmd(struct f2fs_sb_info *sbi,
					struct discard_policy *dpolicy)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct list_head *p_pend_list;
	struct discard_cmd *dc, *tmp;
	struct blk_plug plug;
	int i, issued;
	bool io_interrupted = false;
	bool suspended = false;

	if (dpolicy->timeout)
		f2fs_update_time(sbi, UMOUNT_DISCARD_TIMEOUT);

retry:
	issued = 0;
	for (i = MAX_PLIST_NUM - 1; i >= 0; i--) {
		if (dpolicy->timeout &&
				f2fs_time_over(sbi, UMOUNT_DISCARD_TIMEOUT))
			break;

		if (i + 1 < dpolicy->granularity)
			break;

		if (i + 1 < dcc->max_ordered_discard && dpolicy->ordered) {
			__issue_discard_cmd_orderly(sbi, dpolicy, &issued);
			return issued;
		}

		int dcc_cpu;
		for_each_online_cpu(dcc_cpu) {
			struct discard_set *ds = per_cpu_ptr(dcc->ds, dcc_cpu);

			p_pend_list = &ds->p_pend_list[i];

			mutex_lock(&ds->p_cmd_lock);
			if (list_empty(p_pend_list))
				goto next;
			if (unlikely(dcc->rbtree_check))
				f2fs_bug_on(sbi, !f2fs_check_discard_tree(sbi));
			blk_start_plug(&plug);
			list_for_each_entry_safe(dc, tmp, p_pend_list, list) {
				f2fs_bug_on(sbi, dc->state != D_PREP);


				if (issued > 0 && unlikely(freezing(current))) {
					suspended = true;
					break;
				}

				if (dpolicy->timeout &&
					f2fs_time_over(sbi, UMOUNT_DISCARD_TIMEOUT))
					break;

				if (dpolicy->io_aware && i < dpolicy->io_aware_gran &&
							!is_idle(sbi, DISCARD_TIME)) {
					io_interrupted = true;
					break;
				}

				__submit_discard_cmd(sbi, dpolicy, dc, &issued);

				if (issued >= dpolicy->max_requests)
					break;
			}
			blk_finish_plug(&plug);
	next:
			mutex_unlock(&ds->p_cmd_lock);
		}


		if (issued >= dpolicy->max_requests || io_interrupted ||
					suspended)
			break;
	}

	if (dpolicy->type == DPOLICY_UMOUNT && issued) {
		__wait_all_discard_cmd(sbi, dpolicy);
		goto retry;
	}

	if (!issued && io_interrupted)
		issued = -1;

	return issued;
}

static bool __drop_discard_cmd(struct f2fs_sb_info *sbi)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct list_head *p_pend_list;
	struct discard_cmd *dc, *tmp;
	int i;
	bool dropped = false;

	int dcc_cpu;
	for_each_online_cpu(dcc_cpu) {
		struct discard_set *ds = per_cpu_ptr(dcc->ds, dcc_cpu);

		mutex_lock(&ds->p_cmd_lock);
		for (i = MAX_PLIST_NUM - 1; i >= 0; i--) {
			p_pend_list = &ds->p_pend_list[i];
			list_for_each_entry_safe(dc, tmp, p_pend_list, list) {
				f2fs_bug_on(sbi, dc->state != D_PREP);
				__remove_discard_cmd(sbi, dc);
				dropped = true;
			}
		}
		mutex_unlock(&ds->p_cmd_lock);
	}


	return dropped;
}

void f2fs_drop_discard_cmd(struct f2fs_sb_info *sbi)
{
	__drop_discard_cmd(sbi);
}

static unsigned int __wait_one_discard_bio(struct f2fs_sb_info *sbi,
							struct discard_cmd *dc)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	unsigned int len = 0;

	int dcc_cpu = segno_to_sentry_cpu(sbi, GET_SEGNO(sbi, dc->di.lstart));
	struct discard_set *ds = per_cpu_ptr(dcc->ds, dcc_cpu);

	wait_for_completion_io(&dc->wait);
	mutex_lock(&ds->p_cmd_lock);
	f2fs_bug_on(sbi, dc->state != D_DONE);
	dc->ref--;
	if (!dc->ref) {
		if (!dc->error)
			len = dc->di.len;
		__remove_discard_cmd(sbi, dc);
	}
	mutex_unlock(&ds->p_cmd_lock);

	return len;
}

static unsigned int __wait_discard_cmd_range(struct f2fs_sb_info *sbi,
						struct discard_policy *dpolicy,
						block_t start, block_t end)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct list_head *p_wait_list;
	struct discard_cmd *dc = NULL, *iter, *tmp;
	unsigned int trimmed = 0;

next:
	int dcc_cpu;
	for_each_online_cpu(dcc_cpu) {
		struct discard_set *ds = per_cpu_ptr(dcc->ds, dcc_cpu);

		dc = NULL;
	
		p_wait_list = (dpolicy->type == DPOLICY_FSTRIM) ?
						&(ds->p_fstrim_list) : &(ds->p_wait_list);

		mutex_lock(&ds->p_cmd_lock);
		list_for_each_entry_safe(iter, tmp, p_wait_list, list) {
			if (iter->di.lstart + iter->di.len <= start ||
						end <= iter->di.lstart)
				continue;
			if (iter->di.len < dpolicy->granularity)
				continue;
			if (iter->state == D_DONE && !iter->ref) {
				wait_for_completion_io(&iter->wait);
				if (!iter->error)
					trimmed += iter->di.len;
				__remove_discard_cmd(sbi, iter);
			} else {
				iter->ref++;
				dc = iter;
				break;
			}
		}
		mutex_unlock(&ds->p_cmd_lock);

		if (dc) {
			trimmed += __wait_one_discard_bio(sbi, dc);
			goto next;
		}
	}


	return trimmed;
}

static unsigned int __wait_all_discard_cmd(struct f2fs_sb_info *sbi,
						struct discard_policy *dpolicy)
{
	struct discard_policy dp;
	unsigned int discard_blks;

	if (dpolicy)
		return __wait_discard_cmd_range(sbi, dpolicy, 0, UINT_MAX);

	/* wait all */
	__init_discard_policy(sbi, &dp, DPOLICY_FSTRIM, MIN_DISCARD_GRANULARITY);
	discard_blks = __wait_discard_cmd_range(sbi, &dp, 0, UINT_MAX);
	__init_discard_policy(sbi, &dp, DPOLICY_UMOUNT, MIN_DISCARD_GRANULARITY);
	discard_blks += __wait_discard_cmd_range(sbi, &dp, 0, UINT_MAX);

	return discard_blks;
}

/* This should be covered by global mutex, &sit_i->sentry_lock */
static void f2fs_wait_discard_bio(struct f2fs_sb_info *sbi, block_t blkaddr)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct discard_cmd *dc;
	bool need_wait = false;

	int dcc_cpu = segno_to_sentry_cpu(sbi, GET_SEGNO(sbi, blkaddr));
	struct discard_set *ds = per_cpu_ptr(dcc->ds, dcc_cpu);

	mutex_lock(&ds->p_cmd_lock);
	dc = __lookup_discard_cmd(sbi, blkaddr);
#ifdef CONFIG_BLK_DEV_ZONED
	if (dc && f2fs_sb_has_blkzoned(sbi) && bdev_is_zoned(dc->bdev)) {
	}
#endif
	if (dc) {
		if (dc->state == D_PREP) {
			__punch_discard_cmd(sbi, dc, blkaddr);
		} else {
			dc->ref++;
			need_wait = true;
		}
	}
	mutex_unlock(&ds->p_cmd_lock);

	if (need_wait)
		__wait_one_discard_bio(sbi, dc);
}

void f2fs_stop_discard_thread(struct f2fs_sb_info *sbi)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;

	if (dcc && dcc->f2fs_issue_discard) {
		struct task_struct *discard_thread = dcc->f2fs_issue_discard;

		dcc->f2fs_issue_discard = NULL;
		kthread_stop(discard_thread);
	}
}

/**
 * f2fs_issue_discard_timeout() - Issue all discard cmd within UMOUNT_DISCARD_TIMEOUT
 * @sbi: the f2fs_sb_info data for discard cmd to issue
 *
 * When UMOUNT_DISCARD_TIMEOUT is exceeded, all remaining discard commands will be dropped
 *
 * Return true if issued all discard cmd or no discard cmd need issue, otherwise return false.
 */
bool f2fs_issue_discard_timeout(struct f2fs_sb_info *sbi, bool need_check)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct discard_policy dpolicy;
	bool dropped;

	if (!atomic_read(&dcc->discard_cmd_cnt))
		return true;

	__init_discard_policy(sbi, &dpolicy, DPOLICY_UMOUNT,
					dcc->discard_granularity);
	__issue_discard_cmd(sbi, &dpolicy);
	dropped = __drop_discard_cmd(sbi);

	/* just to make sure there is no pending discard commands */
	__wait_all_discard_cmd(sbi, NULL);

	f2fs_bug_on(sbi, need_check && atomic_read(&dcc->discard_cmd_cnt));
	return !dropped;
}

static int issue_discard_thread(void *data)
{
	struct f2fs_sb_info *sbi = data;
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	wait_queue_head_t *q = &dcc->discard_wait_queue;
	struct discard_policy dpolicy;
	unsigned int wait_ms = dcc->min_discard_issue_time;
	int issued;

	set_freezable();

	do {
		wait_event_freezable_timeout(*q,
				kthread_should_stop() || dcc->discard_wake,
				msecs_to_jiffies(wait_ms));

		if (sbi->gc_mode == GC_URGENT_HIGH ||
			!f2fs_available_free_memory(sbi, DISCARD_CACHE))
			__init_discard_policy(sbi, &dpolicy, DPOLICY_FORCE,
						MIN_DISCARD_GRANULARITY);
		else
			__init_discard_policy(sbi, &dpolicy, DPOLICY_BG,
						dcc->discard_granularity);

		if (dcc->discard_wake)
			dcc->discard_wake = false;

		/* clean up pending candidates before going to sleep */
		if (atomic_read(&dcc->queued_discard))
			__wait_all_discard_cmd(sbi, NULL);

		if (f2fs_readonly(sbi->sb))
			continue;
		if (kthread_should_stop())
			return 0;
		if (is_sbi_flag_set(sbi, SBI_NEED_FSCK) ||
			!atomic_read(&dcc->discard_cmd_cnt)) {
			wait_ms = dpolicy.max_interval;
			continue;
		}

		sb_start_intwrite(sbi->sb);

		issued = __issue_discard_cmd(sbi, &dpolicy);
		if (issued > 0) {
			__wait_all_discard_cmd(sbi, &dpolicy);
			wait_ms = dpolicy.min_interval;
		} else if (issued == -1) {
			wait_ms = f2fs_time_to_wait(sbi, DISCARD_TIME);
			if (!wait_ms)
				wait_ms = dpolicy.mid_interval;
		} else {
			wait_ms = dpolicy.max_interval;
		}
		if (!atomic_read(&dcc->discard_cmd_cnt))
			wait_ms = dpolicy.max_interval;

		sb_end_intwrite(sbi->sb);

	} while (!kthread_should_stop());
	return 0;
}

#ifdef CONFIG_BLK_DEV_ZONED
static int __f2fs_issue_discard_zone(struct f2fs_sb_info *sbi,
		struct block_device *bdev, block_t blkstart, block_t blklen)
{
	sector_t sector, nr_sects;
	block_t lblkstart = blkstart;
	int devi = 0;
	u64 remainder = 0;

	if (f2fs_is_multi_device(sbi)) {
		devi = f2fs_target_device_index(sbi, blkstart);
		if (blkstart < FDEV(devi).start_blk ||
		    blkstart > FDEV(devi).end_blk) {
			f2fs_err(sbi, "Invalid block %x", blkstart);
			return -EIO;
		}
		blkstart -= FDEV(devi).start_blk;
	}

	/* For sequential zones, reset the zone write pointer */
	if (f2fs_blkz_is_seq(sbi, devi, blkstart)) {
		sector = SECTOR_FROM_BLOCK(blkstart);
		nr_sects = SECTOR_FROM_BLOCK(blklen);
		div64_u64_rem(sector, bdev_zone_sectors(bdev), &remainder);

		if (remainder || nr_sects != bdev_zone_sectors(bdev)) {
			f2fs_err(sbi, "(%d) %s: Unaligned zone reset attempted (block %x + %x)",
				 devi, sbi->s_ndevs ? FDEV(devi).path : "",
				 blkstart, blklen);
			return -EIO;
		}

		if (unlikely(is_sbi_flag_set(sbi, SBI_POR_DOING))) {
			unsigned int nofs_flags;
			int ret;

			trace_f2fs_issue_reset_zone(bdev, blkstart);
			nofs_flags = memalloc_nofs_save();
			ret = blkdev_zone_mgmt(bdev, REQ_OP_ZONE_RESET,
						sector, nr_sects);
			memalloc_nofs_restore(nofs_flags);
			return ret;
		}

		__queue_zone_reset_cmd(sbi, bdev, blkstart, lblkstart, blklen);
		return 0;
	}

	/* For conventional zones, use regular discard if supported */
	__queue_discard_cmd(sbi, bdev, lblkstart, blklen);
	return 0;
}
#endif

static int __issue_discard_async(struct f2fs_sb_info *sbi,
		struct block_device *bdev, block_t blkstart, block_t blklen)
{
#ifdef CONFIG_BLK_DEV_ZONED
	if (f2fs_sb_has_blkzoned(sbi) && bdev_is_zoned(bdev))
		return __f2fs_issue_discard_zone(sbi, bdev, blkstart, blklen);
#endif
	__queue_discard_cmd(sbi, bdev, blkstart, blklen);
	return 0;
}

static int f2fs_issue_discard(struct f2fs_sb_info *sbi,
				block_t blkstart, block_t blklen)
{
	sector_t start = blkstart, len = 0;
	struct block_device *bdev;
	struct seg_entry *se;
	unsigned int offset;
	block_t i;
	int err = 0;

	bdev = f2fs_target_device(sbi, blkstart, NULL);

	for (i = blkstart; i < blkstart + blklen; i++, len++) {
		if (i != start) {
			struct block_device *bdev2 =
				f2fs_target_device(sbi, i, NULL);

			if (bdev2 != bdev) {
				err = __issue_discard_async(sbi, bdev,
						start, len);
				if (err)
					return err;
				bdev = bdev2;
				start = i;
				len = 0;
			}
		}

		se = get_seg_entry(sbi, GET_SEGNO(sbi, i));
		offset = GET_BLKOFF_FROM_SEG0(sbi, i);

		if (f2fs_block_unit_discard(sbi) &&
				!f2fs_test_and_set_bit(offset, se->discard_map))
			atomic_sub(1, &sbi->discard_blks);
	}

	if (len)
		err = __issue_discard_async(sbi, bdev, start, len);
	return err;
}

static bool add_discard_addrs(struct f2fs_sb_info *sbi, struct cp_control *cpc,
							bool check_only)
{
	int entries = SIT_VBLOCK_MAP_SIZE / sizeof(unsigned long);
	struct seg_entry *se = get_seg_entry(sbi, cpc->trim_start);
	unsigned long *cur_map = (unsigned long *)se->cur_valid_map;
	unsigned long *ckpt_map = (unsigned long *)se->ckpt_valid_map;
	unsigned long *discard_map = (unsigned long *)se->discard_map;
	unsigned long *dmap = SIT_I(sbi)->tmp_map;
	unsigned int start = 0, end = -1;
	bool force = (cpc->reason & CP_DISCARD);
	struct discard_entry *de = NULL;
	struct list_head *head;
	int i;

	/* DeLFS: discard entries go to the discard set of the segment's core */
	int dcc_cpu = segno_to_sentry_cpu(sbi, cpc->trim_start);
	struct discard_set *ds = per_cpu_ptr(SM_I(sbi)->dcc_info->ds, dcc_cpu);

	head = &ds->p_entry_list;

	if (se->valid_blocks == BLKS_PER_SEG(sbi) ||
	    !f2fs_hw_support_discard(sbi) ||
	    !f2fs_block_unit_discard(sbi))
		return false;

	if (!force) {
		if (!f2fs_realtime_discard_enable(sbi) ||
			(!se->valid_blocks &&
				!is_curseg(sbi, cpc->trim_start)) ||
			SM_I(sbi)->dcc_info->nr_discards >=
				SM_I(sbi)->dcc_info->max_discards) {
			return false;
		}
	}

	/* SIT_VBLOCK_MAP_SIZE should be multiple of sizeof(unsigned long) */
	for (i = 0; i < entries; i++)
		dmap[i] = force ? ~ckpt_map[i] & ~discard_map[i] :
				(cur_map[i] ^ ckpt_map[i]) & ckpt_map[i];

	while (force || SM_I(sbi)->dcc_info->nr_discards <=
				SM_I(sbi)->dcc_info->max_discards) {
		start = __find_rev_next_bit(dmap, BLKS_PER_SEG(sbi), end + 1);
		if (start >= BLKS_PER_SEG(sbi))
			break;

		end = __find_rev_next_zero_bit(dmap,
						BLKS_PER_SEG(sbi), start + 1);
		if (force && start && end != BLKS_PER_SEG(sbi) &&
		    (end - start) < cpc->trim_minlen)
			continue;

		if (check_only)
			return true;

		if (!de) {
			de = f2fs_kmem_cache_alloc(discard_entry_slab,
						GFP_F2FS_ZERO, true, NULL);
			de->start_blkaddr = START_BLOCK(sbi, cpc->trim_start);
			list_add_tail(&de->list, head);
		}

		for (i = start; i < end; i++)
			__set_bit_le(i, (void *)de->discard_map);

		SM_I(sbi)->dcc_info->nr_discards += end - start;
	}
	return false;
}

static void release_discard_addr(struct discard_entry *entry)
{
	list_del(&entry->list);
	kmem_cache_free(discard_entry_slab, entry);
}

void f2fs_release_discard_addrs(struct f2fs_sb_info *sbi)
{
	struct discard_entry *entry, *this;

	int dcc_cpu;
	for_each_online_cpu(dcc_cpu) {
		struct discard_set *ds = per_cpu_ptr(SM_I(sbi)->dcc_info->ds, dcc_cpu);
		struct list_head *head = &ds->p_entry_list;

		/* drop caches */
		list_for_each_entry_safe(entry, this, head, list)
			release_discard_addr(entry);
	}

}

/*
 * Should call f2fs_clear_prefree_segments after checkpoint is done.
 */
static void set_prefree_as_free_segments(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	unsigned int segno;

	int seglist_cpu;
	struct dirty_seg_percpu *p;

	for_each_online_cpu(seglist_cpu) {
		p = per_cpu_ptr(dirty_i->pcpu, seglist_cpu);
		mutex_lock(&p->p_seglist_lock);
		for_each_set_bit(segno, p->p_dirty_segmap[PRE], MAIN_SEGS(sbi)) {
			__set_test_and_free(sbi, segno, false);
		}
		mutex_unlock(&p->p_seglist_lock);
	}

}

void f2fs_clear_prefree_segments(struct f2fs_sb_info *sbi,
						struct cp_control *cpc)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct list_head *head;
	struct discard_entry *entry, *this;
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	unsigned long *prefree_map;
	unsigned int start, end;
	unsigned int secno, start_segno;
	bool force = (cpc->reason & CP_DISCARD);
	bool section_alignment = F2FS_OPTION(sbi).discard_unit ==
						DISCARD_UNIT_SECTION;

	if (f2fs_lfs_mode(sbi) && __is_large_section(sbi)) {
		section_alignment = true;
	}

	int seglist_cpu;
	struct dirty_seg_percpu *p;

	/*
	 * DeLFS: each core keeps its own prefree list, walk the cores one by
	 * one, each under its own seglist lock, inside its segment range.
	 * DeLFS dropped the section_alignment rounding (one segment per
	 * section, so it would not change anything).
	 */
	for_each_online_cpu(seglist_cpu) {
		p = per_cpu_ptr(dirty_i->pcpu, seglist_cpu);
		prefree_map = p->p_dirty_segmap[PRE];

		mutex_lock(&p->p_seglist_lock);

		unsigned int sentry_begin, sentry_end;
		get_sentry_range_from_cpu(sbi, seglist_cpu, &sentry_begin, &sentry_end);

		start = sentry_begin;
		unsigned int limit = sentry_end + 1;

		while (1) {
			int i;

			start = find_next_bit(prefree_map, limit, start);
			if (start >= limit)
				break;
			end = find_next_zero_bit(prefree_map, limit, start + 1);

			for (i = start; i < end; i++) {
				if (test_and_clear_bit(i, prefree_map)) {
					atomic_sub(1, &dirty_i->nr_dirty[PRE]);
					p->p_nr_dirty[PRE]--;
				}
			}

			if (!f2fs_realtime_discard_enable(sbi))
				continue;

			if (force && start >= cpc->trim_start &&
						(end - 1) <= cpc->trim_end)
				continue;

			/* Should cover 2MB zoned device for zone-based reset */
			if (!f2fs_sb_has_blkzoned(sbi) &&
			    (!f2fs_lfs_mode(sbi) || !__is_large_section(sbi))) {
				f2fs_issue_discard(sbi, START_BLOCK(sbi, start),
					SEGS_TO_BLKS(sbi, end - start));
				continue;
			}
next:
			secno = GET_SEC_FROM_SEG(sbi, start);
			start_segno = GET_SEG_FROM_SEC(sbi, secno);
			if (!is_cursec(sbi, secno) &&
				!get_valid_blocks(sbi, start, true))
				f2fs_issue_discard(sbi, START_BLOCK(sbi, start_segno),
							BLKS_PER_SEC(sbi));

			start = start_segno + SEGS_PER_SEC(sbi);
			if (start < end)
				goto next;
			else
				end = start - 1;

			start = end + 1;
			if (start >= limit)
				break;
		}
		mutex_unlock(&p->p_seglist_lock);
	}

	if (!f2fs_block_unit_discard(sbi))
		goto wakeup;

	/* DeLFS: small discards are kept in each core's discard set */
	int dcc_cpu;
	for_each_online_cpu(dcc_cpu) {
		struct discard_set *ds = per_cpu_ptr(dcc->ds, dcc_cpu);
		head = &ds->p_entry_list;

		/* send small discards */
		list_for_each_entry_safe(entry, this, head, list) {
			unsigned int cur_pos = 0, next_pos, len, total_len = 0;
			bool is_valid = test_bit_le(0, entry->discard_map);

find_next:
			if (is_valid) {
				next_pos = find_next_zero_bit_le(entry->discard_map,
							BLKS_PER_SEG(sbi), cur_pos);
				len = next_pos - cur_pos;

				if (f2fs_sb_has_blkzoned(sbi) ||
				    (force && len < cpc->trim_minlen))
					goto skip;

				f2fs_issue_discard(sbi, entry->start_blkaddr + cur_pos,
										len);
				total_len += len;
			} else {
				next_pos = find_next_bit_le(entry->discard_map,
							BLKS_PER_SEG(sbi), cur_pos);
			}
skip:
			cur_pos = next_pos;
			is_valid = !is_valid;

			if (cur_pos < BLKS_PER_SEG(sbi))
				goto find_next;

			release_discard_addr(entry);
			dcc->nr_discards -= total_len;
		}
	}

wakeup:
	wake_up_discard_thread(sbi, false);
}

int f2fs_start_discard_thread(struct f2fs_sb_info *sbi)
{
	dev_t dev = sbi->sb->s_bdev->bd_dev;
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	int err = 0;

	if (f2fs_sb_has_readonly(sbi)) {
		f2fs_info(sbi,
			"Skip to start discard thread for readonly image");
		return 0;
	}

	if (!f2fs_realtime_discard_enable(sbi))
		return 0;

	dcc->f2fs_issue_discard = kthread_run(issue_discard_thread, sbi,
				"f2fs_discard-%u:%u", MAJOR(dev), MINOR(dev));
	if (IS_ERR(dcc->f2fs_issue_discard)) {
		err = PTR_ERR(dcc->f2fs_issue_discard);
		dcc->f2fs_issue_discard = NULL;
	}

	return err;
}

static int create_discard_cmd_control(struct f2fs_sb_info *sbi)
{
	struct discard_cmd_control *dcc;
	int err = 0, i;

	if (SM_I(sbi)->dcc_info) {
		dcc = SM_I(sbi)->dcc_info;
		goto init_thread;
	}

	dcc = f2fs_kzalloc(sbi, sizeof(struct discard_cmd_control), GFP_KERNEL);
	if (!dcc)
		return -ENOMEM;

	dcc->ds = alloc_percpu(struct discard_set);

	int dcc_cpu;
	for_each_online_cpu(dcc_cpu) {
		struct discard_set *ds = per_cpu_ptr(dcc->ds, dcc_cpu);

		INIT_LIST_HEAD(&ds->p_entry_list);
		for (i = 0; i < MAX_PLIST_NUM; i++)
			INIT_LIST_HEAD(&ds->p_pend_list[i]);
		INIT_LIST_HEAD(&ds->p_wait_list);
		INIT_LIST_HEAD(&ds->p_fstrim_list);

		mutex_init(&ds->p_cmd_lock);

		ds->p_root = RB_ROOT_CACHED;
	}

	dcc->discard_io_aware_gran = MAX_PLIST_NUM;
	dcc->discard_granularity = DEFAULT_DISCARD_GRANULARITY;
	dcc->max_ordered_discard = DEFAULT_MAX_ORDERED_DISCARD_GRANULARITY;
	dcc->discard_io_aware = DPOLICY_IO_AWARE_ENABLE;
	if (F2FS_OPTION(sbi).discard_unit == DISCARD_UNIT_SEGMENT ||
		F2FS_OPTION(sbi).discard_unit == DISCARD_UNIT_SECTION)
		dcc->discard_granularity = BLKS_PER_SEG(sbi);

	atomic_set(&dcc->issued_discard, 0);
	atomic_set(&dcc->queued_discard, 0);
	atomic_set(&dcc->discard_cmd_cnt, 0);
	dcc->nr_discards = 0;
	dcc->max_discards = SEGS_TO_BLKS(sbi, MAIN_SEGS(sbi));
	dcc->max_discard_request = DEF_MAX_DISCARD_REQUEST;
	dcc->min_discard_issue_time = DEF_MIN_DISCARD_ISSUE_TIME;
	dcc->mid_discard_issue_time = DEF_MID_DISCARD_ISSUE_TIME;
	dcc->max_discard_issue_time = DEF_MAX_DISCARD_ISSUE_TIME;
	dcc->discard_urgent_util = DEF_DISCARD_URGENT_UTIL;
	dcc->undiscard_blks = 0;
	dcc->next_pos = 0;
	dcc->rbtree_check = false;

	init_waitqueue_head(&dcc->discard_wait_queue);
	SM_I(sbi)->dcc_info = dcc;
init_thread:
	err = f2fs_start_discard_thread(sbi);
	if (err) {
		kfree(dcc);
		SM_I(sbi)->dcc_info = NULL;
	}

	return err;
}

static void destroy_discard_cmd_control(struct f2fs_sb_info *sbi)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;

	if (!dcc)
		return;

	f2fs_stop_discard_thread(sbi);

	/*
	 * Recovery can cache discard commands, so in error path of
	 * fill_super(), it needs to give a chance to handle them.
	 */
	f2fs_issue_discard_timeout(sbi, true);

	kfree(dcc);
	SM_I(sbi)->dcc_info = NULL;
}

static bool __mark_sit_entry_dirty(struct f2fs_sb_info *sbi, unsigned int segno)
{
	struct sit_info *sit_i = SIT_I(sbi);

	if (!test_and_set_bit(segno, sit_i->dirty_sentries_bitmap)) {
		atomic_inc(&sit_i->dirty_sentries);
		return false;
	}

	return true;
}

static void __set_sit_entry_type(struct f2fs_sb_info *sbi, int type,
					unsigned int segno, int modified)
{
	struct seg_entry *se = get_seg_entry(sbi, segno);

	se->type = type;
	if (modified)
		__mark_sit_entry_dirty(sbi, segno);
}

static inline unsigned long long get_segment_mtime(struct f2fs_sb_info *sbi,
								block_t blkaddr)
{
	unsigned int segno = GET_SEGNO(sbi, blkaddr);

	if (segno == NULL_SEGNO)
		return 0;
	return get_seg_entry(sbi, segno)->mtime;
}

void update_segment_mtime(struct f2fs_sb_info *sbi, block_t blkaddr,
						unsigned long long old_mtime)
{
	struct seg_entry *se;
	unsigned int segno = GET_SEGNO(sbi, blkaddr);
	unsigned long long ctime = get_mtime(sbi, false);
	unsigned long long mtime = old_mtime ? old_mtime : ctime;

	if (segno == NULL_SEGNO)
		return;

	se = get_seg_entry(sbi, segno);

	if (!se->mtime)
		se->mtime = mtime;
	else
		se->mtime = div_u64(se->mtime * se->valid_blocks + mtime,
						se->valid_blocks + 1);

	if (ctime > SIT_I(sbi)->max_mtime)
		SIT_I(sbi)->max_mtime = ctime;
}

/*
 * NOTE: when updating multiple blocks at the same time, please ensure
 * that the consecutive input blocks belong to the same segment.
 */
static int update_sit_entry_for_release(struct f2fs_sb_info *sbi, struct seg_entry *se,
				unsigned int segno, block_t blkaddr, unsigned int offset, int del)
{
	bool exist;
	int i;
	int del_count = -del;

	f2fs_bug_on(sbi, GET_SEGNO(sbi, blkaddr) != GET_SEGNO(sbi, blkaddr + del_count - 1));

	for (i = 0; i < del_count; i++) {
		exist = f2fs_test_and_clear_bit(offset + i, se->cur_valid_map);
		if (unlikely(!exist)) {
			f2fs_err(sbi, "Bitmap was wrongly cleared, blk:%u", blkaddr + i);
			f2fs_bug_on(sbi, 1);
			se->valid_blocks++;
			del += 1;
		} else if (unlikely(is_sbi_flag_set(sbi, SBI_CP_DISABLED))) {
			/*
			 * If checkpoints are off, we must not reuse data that
			 * was used in the previous checkpoint. If it was used
			 * before, we must track that to know how much space we
			 * really have.
			 */
			if (f2fs_test_bit(offset + i, se->ckpt_valid_map)) {
				spin_lock(&sbi->stat_lock);
				sbi->unusable_block_count++;
				spin_unlock(&sbi->stat_lock);
			}
		}

		if (f2fs_block_unit_discard(sbi) &&
				f2fs_test_and_clear_bit(offset + i, se->discard_map))
			atomic_add(1, &sbi->discard_blks);	/* DeLFS: atomic counter */

		if (!f2fs_test_bit(offset + i, se->ckpt_valid_map)) {
			se->ckpt_valid_blocks -= 1;
			if (__is_large_section(sbi))
				get_sec_entry(sbi, segno)->ckpt_valid_blocks -= 1;
		}
	}

	if (__is_large_section(sbi))
		sanity_check_valid_blocks(sbi, segno);

	return del;
}

static int update_sit_entry_for_alloc(struct f2fs_sb_info *sbi, struct seg_entry *se,
				unsigned int segno, block_t blkaddr, unsigned int offset, int del)
{
	bool exist;

	exist = f2fs_test_and_set_bit(offset, se->cur_valid_map);
	if (unlikely(exist)) {
		f2fs_err(sbi, "Bitmap was wrongly set, blk:%u", blkaddr);
		f2fs_bug_on(sbi, 1);
		se->valid_blocks--;
		del = 0;
	}

	if (f2fs_block_unit_discard(sbi) &&
			!f2fs_test_and_set_bit(offset, se->discard_map))
		atomic_sub(1, &sbi->discard_blks);	/* DeLFS: atomic counter */

	/*
	 * SSR should never reuse block which is checkpointed
	 * or newly invalidated.
	 */
	if (!is_sbi_flag_set(sbi, SBI_CP_DISABLED)) {
		if (!f2fs_test_and_set_bit(offset, se->ckpt_valid_map)) {
			se->ckpt_valid_blocks++;
			if (__is_large_section(sbi))
				get_sec_entry(sbi, segno)->ckpt_valid_blocks++;
		}
	}

	if (!f2fs_test_bit(offset, se->ckpt_valid_map)) {
		se->ckpt_valid_blocks += del;
		if (__is_large_section(sbi))
			get_sec_entry(sbi, segno)->ckpt_valid_blocks += del;
	}

	if (__is_large_section(sbi))
		sanity_check_valid_blocks(sbi, segno);

	return del;
}

/* DeLFS: not static, oldinv.c (deLLC) calls it for deferred invalidations */
void update_sit_entry(struct f2fs_sb_info *sbi, block_t blkaddr, int del)
{
	struct seg_entry *se;
	unsigned int segno, offset;
	long int new_vblocks;

	segno = GET_SEGNO(sbi, blkaddr);
	if (segno == NULL_SEGNO)
		return;

	se = get_seg_entry(sbi, segno);
	new_vblocks = se->valid_blocks + del;
	offset = GET_BLKOFF_FROM_SEG0(sbi, blkaddr);

	f2fs_bug_on(sbi, (new_vblocks < 0 ||
			(new_vblocks > f2fs_usable_blks_in_seg(sbi, segno))));

	se->valid_blocks = new_vblocks;

	/* Update valid block bitmap */
	if (del > 0) {
		del = update_sit_entry_for_alloc(sbi, se, segno, blkaddr, offset, del);
	} else {
		del = update_sit_entry_for_release(sbi, se, segno, blkaddr, offset, del);
	}

	__mark_sit_entry_dirty(sbi, segno);

	/* update total number of valid blocks to be written in ckpt area */
	atomic64_add(del, &SIT_I(sbi)->written_valid_blocks);	/* DeLFS */

	if (__is_large_section(sbi))
		get_sec_entry(sbi, segno)->valid_blocks += del;
}

void f2fs_invalidate_blocks(struct f2fs_sb_info *sbi, block_t addr,
				unsigned int len)
{
	unsigned int segno = GET_SEGNO(sbi, addr);
	struct sit_info *sit_i = SIT_I(sbi);
	block_t addr_start = addr, addr_end = addr + len - 1;
	unsigned int seg_num = GET_SEGNO(sbi, addr_end) - segno + 1;
	unsigned int i = 1, max_blocks = BLKS_PER_SEG(sbi), cnt;

	f2fs_bug_on(sbi, addr == NULL_ADDR);
	if (addr == NEW_ADDR || addr == COMPRESS_ADDR)
		return;

	f2fs_invalidate_internal_cache(sbi, addr, len);

	if (seg_num == 1)
		cnt = len;
	else
		cnt = max_blocks - GET_BLKOFF_FROM_SEG0(sbi, addr);

	do {
		/*
		 * DeLFS DDL: a segment's SIT entry is protected by the SIT lock
		 * of the core that owns the segment. 7.2.8 passes a range that
		 * can cross segments (and so cores), so take the owning core's
		 * lock per segment, one lock at a time (DeLFS 6.6.8 handled one
		 * block, so one lock was enough).
		 */
		struct rw_semaphore *se_lock =
			per_cpu_ptr(sit_i->percore_sentry_lock,
					segno_to_sentry_cpu(sbi, segno));

		/* add it into sit main buffer */
		down_write(se_lock);

		update_segment_mtime(sbi, addr_start, 0);
		update_sit_entry(sbi, addr_start, -cnt);

		/* add it into dirty seglist */
		locate_dirty_segment(sbi, segno);

		up_write(se_lock);

		/* update @addr_start and @cnt and @segno */
		addr_start = START_BLOCK(sbi, ++segno);
		if (++i == seg_num)
			cnt = GET_BLKOFF_FROM_SEG0(sbi, addr_end) + 1;
		else
			cnt = max_blocks;
	} while (i <= seg_num);
}

bool f2fs_is_checkpointed_data(struct f2fs_sb_info *sbi, block_t blkaddr)
{
	struct sit_info *sit_i = SIT_I(sbi);
	unsigned int segno, offset;
	struct seg_entry *se;
	bool is_cp = false;
	int sentry_cpu, max_cpu = num_online_cpus();

	if (!__is_valid_data_blkaddr(blkaddr))
		return true;

	for_each_online_cpu(sentry_cpu) {
		struct rw_semaphore *se_lock = per_cpu_ptr(sit_i->percore_sentry_lock, sentry_cpu);
		down_read(se_lock);
	}

	segno = GET_SEGNO(sbi, blkaddr);
	se = get_seg_entry(sbi, segno);
	offset = GET_BLKOFF_FROM_SEG0(sbi, blkaddr);

	if (f2fs_test_bit(offset, se->ckpt_valid_map))
		is_cp = true;

	for_each_online_cpu(sentry_cpu) {
		struct rw_semaphore *se_lock = per_cpu_ptr(sit_i->percore_sentry_lock, max_cpu-1-sentry_cpu);
		up_read(se_lock);
	}

	return is_cp;
}

/* DeLFS: valid summary entries of @cpu's curseg @type */
static unsigned short f2fs_curseg_valid_blocks(struct f2fs_sb_info *sbi,
						int type, int cpu)
{
	struct curseg_info_array *cpu_array = WRAP_CURSEG_I(sbi, cpu);
	struct curseg_info *curseg = cpu_array->array + type;

	if (sbi->ckpt->alloc_type[type] == SSR)
		return BLKS_PER_SEG(sbi);
	return curseg->next_blkoff;
}

/*
 * Calculate the number of current summary pages of @cpu for writing
 */
int percore_f2fs_npages_for_summary_flush(struct f2fs_sb_info *sbi,
						bool for_ra, int cpu)
{
	int valid_sum_count = 0;
	int i, sum_in_page;

	for (i = CURSEG_HOT_DATA; i <= CURSEG_COLD_DATA; i++) {
		if (sbi->ckpt->alloc_type[i] != SSR && for_ra)
			valid_sum_count +=
				le16_to_cpu(F2FS_CKPT(sbi)->cur_datas_blkoff[cpu][i]);
		else
			valid_sum_count += f2fs_curseg_valid_blocks(sbi, i, cpu);
	}

	sum_in_page = (sbi->blocksize - 2 * sbi->sum_journal_size -
			SUM_FOOTER_SIZE) / SUMMARY_SIZE;
	if (valid_sum_count <= sum_in_page)
		return 1;
	else if ((valid_sum_count - sum_in_page) <=
		(sbi->blocksize - SUM_FOOTER_SIZE) / SUMMARY_SIZE)
		return 2;
	return 3;
}

/*
 * Caller should put this summary folio
 */
struct folio *f2fs_get_sum_folio(struct f2fs_sb_info *sbi, unsigned int segno)
{
	if (unlikely(f2fs_cp_error(sbi)))
		return ERR_PTR(-EIO);
	return f2fs_get_meta_folio_retry(sbi, GET_SUM_BLOCK(sbi, segno));
}

void f2fs_update_meta_page(struct f2fs_sb_info *sbi,
					void *src, block_t blk_addr)
{
	struct folio *folio;

	if (!f2fs_sb_has_packed_ssa(sbi))
		folio = f2fs_grab_meta_folio(sbi, blk_addr);
	else
		folio = f2fs_get_meta_folio_retry(sbi, blk_addr);

	if (IS_ERR(folio))
		return;

	memcpy(folio_address(folio), src, PAGE_SIZE);
	folio_mark_dirty(folio);
	f2fs_folio_put(folio, true);
}

static void write_sum_page(struct f2fs_sb_info *sbi,
		struct f2fs_summary_block *sum_blk, unsigned int segno)
{
	struct folio *folio;

	if (!f2fs_sb_has_packed_ssa(sbi))
		return f2fs_update_meta_page(sbi, (void *)sum_blk,
				GET_SUM_BLOCK(sbi, segno));

	folio = f2fs_get_sum_folio(sbi, segno);
	if (IS_ERR(folio))
		return;

	memcpy(SUM_BLK_PAGE_ADDR(sbi, folio, segno), sum_blk,
			sbi->sum_blocksize);
	folio_mark_dirty(folio);
	f2fs_folio_put(folio, true);
}

static void write_current_sum_page(struct f2fs_sb_info *sbi,
						int type, block_t blk_addr,
						struct curseg_info_array *cpu_array)
{
	struct curseg_info *curseg = cpu_array->array + type;
	struct folio *folio = f2fs_grab_meta_folio(sbi, blk_addr);
	struct f2fs_summary_block *src = curseg->sum_blk;
	struct f2fs_summary_block *dst;

	dst = folio_address(folio);
	memset(dst, 0, PAGE_SIZE);

	mutex_lock(&curseg->curseg_mutex);

	down_read(&curseg->journal_rwsem);
	memcpy(sum_journal(sbi, dst), curseg->journal, sbi->sum_journal_size);
	up_read(&curseg->journal_rwsem);

	memcpy(sum_entries(dst), sum_entries(src), sbi->sum_entry_size);
	memcpy(sum_footer(sbi, dst), sum_footer(sbi, src), SUM_FOOTER_SIZE);

	mutex_unlock(&curseg->curseg_mutex);

	folio_mark_dirty(folio);
	f2fs_folio_put(folio, true);
}

/* DeLFS: unused since need_new_seg() always allocates a new segment (LFS only) */
static int __maybe_unused is_next_segment_free(struct f2fs_sb_info *sbi,
				struct curseg_info *curseg)
{
	unsigned int segno = curseg->segno + 1;
	struct free_segmap_info *free_i = FREE_I(sbi);

	if (segno < MAIN_SEGS(sbi) && segno % SEGS_PER_SEC(sbi))
		return !test_bit(segno, free_i->free_segmap);
	return 0;
}

/*
 * Find a new segment from the free segments bitmap to right order
 * This function should be returned with success, otherwise BUG
 */
/*
 * DeLFS: search to the right of the hint (@cpu's curseg->segno) and wrap to
 * the beginning. The search is not limited to @cpu's own segment range, so
 * a core borrows segments of other ranges once its own range is full.
 */
static int get_new_segment(struct f2fs_sb_info *sbi,
			unsigned int *newseg, bool new_sec, bool pinning, int cpu)
{
	struct curseg_info_array *cpu_array = WRAP_CURSEG_I(sbi, cpu);
	struct free_segmap_info *free_i = FREE_I(sbi);
	unsigned int segno, secno, zoneno;
	unsigned int total_zones = MAIN_SECS(sbi) / sbi->secs_per_zone;
	unsigned int hint = GET_SEC_FROM_SEG(sbi, *newseg);
	unsigned int old_zoneno = GET_ZONE_FROM_SEG(sbi, *newseg);
	unsigned int alloc_policy = sbi->allocate_section_policy;
	unsigned int alloc_hint = sbi->allocate_section_hint;
	bool init = true;
	int i;
	int ret = 0;

	spin_lock(&free_i->segmap_lock);

	if (time_to_inject(sbi, FAULT_NO_SEGMENT)) {
		ret = -ENOSPC;
		goto out_unlock;
	}

	if (!new_sec && ((*newseg + 1) % SEGS_PER_SEC(sbi))) {
		segno = find_next_zero_bit(free_i->free_segmap,
			GET_SEG_FROM_SEC(sbi, hint + 1), *newseg + 1);
		if (segno < GET_SEG_FROM_SEC(sbi, hint + 1))
			goto got_it;
	}

#ifdef CONFIG_BLK_DEV_ZONED
	/*
	 * If we format f2fs on zoned storage, let's try to get pinned sections
	 * from beginning of the storage, which should be a conventional one.
	 */
	if (f2fs_sb_has_blkzoned(sbi)) {
		/* Prioritize writing to conventional zones */
		if (sbi->blkzone_alloc_policy == BLKZONE_ALLOC_PRIOR_CONV || pinning)
			segno = 0;
		else
			segno = max(sbi->first_seq_zone_segno, *newseg);
		hint = GET_SEC_FROM_SEG(sbi, segno);
	}
#endif

	/*
	 * Prevent allocate_section_hint from exceeding MAIN_SECS()
	 * due to desynchronization.
	 */
	if (alloc_policy != ALLOCATE_FORWARD_NOHINT &&
		alloc_hint > MAIN_SECS(sbi))
		alloc_hint = MAIN_SECS(sbi);

	if (alloc_policy == ALLOCATE_FORWARD_FROM_HINT &&
		hint < alloc_hint)
		hint = alloc_hint;
	else if (alloc_policy == ALLOCATE_FORWARD_WITHIN_HINT &&
			hint >= alloc_hint)
		hint = 0;

find_other_zone:
	secno = find_next_zero_bit(free_i->free_secmap, MAIN_SECS(sbi), hint);

#ifdef CONFIG_BLK_DEV_ZONED
	if (secno >= MAIN_SECS(sbi) && f2fs_sb_has_blkzoned(sbi)) {
		/* Write only to sequential zones */
		if (sbi->blkzone_alloc_policy == BLKZONE_ALLOC_ONLY_SEQ) {
			hint = GET_SEC_FROM_SEG(sbi, sbi->first_seq_zone_segno);
			secno = find_next_zero_bit(free_i->free_secmap, MAIN_SECS(sbi), hint);
		} else
			secno = find_first_zero_bit(free_i->free_secmap,
								MAIN_SECS(sbi));
		if (secno >= MAIN_SECS(sbi)) {
			ret = -ENOSPC;
			f2fs_bug_on(sbi, 1);
			goto out_unlock;
		}
	}
#endif

	if (secno >= MAIN_SECS(sbi)) {
		secno = find_first_zero_bit(free_i->free_secmap,
							MAIN_SECS(sbi));
		if (secno >= MAIN_SECS(sbi)) {
			ret = -ENOSPC;
			f2fs_bug_on(sbi, !pinning);
			goto out_unlock;
		}
	}
	segno = GET_SEG_FROM_SEC(sbi, secno);
	zoneno = GET_ZONE_FROM_SEC(sbi, secno);

	/* give up on finding another zone */
	if (!init)
		goto got_it;
	if (sbi->secs_per_zone == 1)
		goto got_it;
	if (zoneno == old_zoneno)
		goto got_it;
	/*
	 * TODO(A): like DeLFS, only @cpu's own cursegs are checked here;
	 * other cores' cursegs may still use @zoneno (secs_per_zone > 1 only).
	 */
	for (i = 0; i < NR_CURSEG_TYPE; i++)
		if ((cpu_array->array + i)->zone == zoneno)
			break;

	if (i < NR_CURSEG_TYPE) {
		/* zone is in user, try another */
		if (zoneno + 1 >= total_zones)
			hint = 0;
		else
			hint = (zoneno + 1) * sbi->secs_per_zone;
		init = false;
		goto find_other_zone;
	}
got_it:
	/* set it as dirty segment in free segmap */
	if (test_bit(segno, free_i->free_segmap)) {
		ret = -EFSCORRUPTED;
		fserror_report_metadata(sbi->sb, -EFSCORRUPTED, GFP_NOFS);
		f2fs_stop_checkpoint(sbi, false, STOP_CP_REASON_CORRUPTED_FREE_BITMAP);
		goto out_unlock;
	}

	/* no free section in conventional device or conventional zone */
	if (new_sec && pinning &&
		f2fs_is_sequential_zone_area(sbi, START_BLOCK(sbi, segno))) {
		ret = -EAGAIN;
		goto out_unlock;
	}
	__set_inuse(sbi, segno);
	*newseg = segno;
out_unlock:
	spin_unlock(&free_i->segmap_lock);

	if (ret == -ENOSPC && !pinning)
		f2fs_stop_checkpoint(sbi, false, STOP_CP_REASON_NO_SEGMENT);
	return ret;
}

/*
 * DeLFS: switch @cpu's curseg @type to its next_segno. The new segment's
 * curseg ref is taken before curseg->segno is published, and the old
 * segment's ref is dropped only afterwards, so is_curseg()/is_cursec()
 * never see a window in which neither segment is current.
 */
static void reset_curseg(struct f2fs_sb_info *sbi, int type, int modified,
			int cpu, struct curseg_info_array *cpu_array)
{
	struct curseg_info *curseg = cpu_array->array + type;
	struct summary_footer *sum_footer;
	unsigned short seg_type = curseg->seg_type;
	unsigned int old_segno = curseg->segno;
	unsigned int new_segno = curseg->next_segno;

	/* only happen when get_new_segment() fails */
	if (new_segno == NULL_SEGNO)
		return;

	curseg->inited = true;

	/* 1) get ref on the new current segment */
	if (new_segno != old_segno)
		f2fs_curseg_get(sbi, new_segno);
	atomic_set(&get_seg_entry(sbi, new_segno)->curseg_cpu, cpu); /* debug only */

	/* 2) publish the new segno */
	WRITE_ONCE(curseg->segno, new_segno);

	/* 3) drop ref on the old current segment */
	if (old_segno != NULL_SEGNO && old_segno != new_segno) {
		atomic_set(&get_seg_entry(sbi, old_segno)->curseg_cpu, -1); /* debug only */
		f2fs_curseg_put(sbi, old_segno);
	}

	curseg->zone = GET_ZONE_FROM_SEG(sbi, curseg->segno);
	curseg->next_blkoff = 0;
	curseg->next_segno = NULL_SEGNO;

	sum_footer = sum_footer(sbi, curseg->sum_blk);
	memset(sum_footer, 0, sizeof(struct summary_footer));

	sanity_check_seg_type(sbi, seg_type);

	if (IS_DATASEG(seg_type))
		SET_SUM_TYPE(sum_footer, SUM_TYPE_DATA);
	if (IS_NODESEG(seg_type))
		SET_SUM_TYPE(sum_footer, SUM_TYPE_NODE);
	__set_sit_entry_type(sbi, seg_type, curseg->segno, modified);
}

/*
 * DeLFS: always use @cpu's current segno as the hint, so that every core
 * keeps allocating near its own position. (Vanilla returns 0 for HOT_DATA
 * and node logs, and last_victim[ALLOC_NEXT] / 0 for ALLOC_MODE_REUSE,
 * which would pile all cores up around segment 0.)
 */
static unsigned int __get_next_segno(struct f2fs_sb_info *sbi, int type,
								int cpu)
{
	struct curseg_info_array *cpu_array = WRAP_CURSEG_I(sbi, cpu);
	struct curseg_info *curseg = cpu_array->array + type;
	unsigned short seg_type = curseg->seg_type;

	sanity_check_seg_type(sbi, seg_type);
	if (__is_large_section(sbi)) {
		if (f2fs_need_rand_seg_blk(sbi, type)) {
			unsigned int hint = GET_SEC_FROM_SEG(sbi, curseg->segno);

			if (GET_SEC_FROM_SEG(sbi, curseg->segno + 1) != hint)
				return curseg->segno;
			return get_random_u32_inclusive(curseg->segno + 1,
					GET_SEG_FROM_SEC(sbi, hint + 1) - 1);
		}
		return curseg->segno;
	} else if (f2fs_need_rand_seg_blk(sbi, type)) {
		return get_random_u32_below(MAIN_SECS(sbi) * SEGS_PER_SEC(sbi));
	}

	/* inmem log may not locate on any segment after mount */
	if (!curseg->inited)
		return 0;

	if (unlikely(is_sbi_flag_set(sbi, SBI_CP_DISABLED)))
		return 0;

	return curseg->segno;
}

static void reset_curseg_fields(struct curseg_info *curseg)
{
	curseg->inited = false;
	curseg->segno = NULL_SEGNO;
	curseg->next_segno = 0;
}

/*
 * DeLFS: like reset_curseg_fields(), but for a live curseg: drop its curseg
 * ref first, so that the segment stops being "current" (curseg_refs).
 */
static void release_curseg(struct f2fs_sb_info *sbi, struct curseg_info *curseg)
{
	unsigned int segno = curseg->segno;

	if (curseg->inited && segno != NULL_SEGNO) {
		WRITE_ONCE(curseg->segno, NULL_SEGNO);
		atomic_set(&get_seg_entry(sbi, segno)->curseg_cpu, -1); /* debug only */
		f2fs_curseg_put(sbi, segno);
	}
	reset_curseg_fields(curseg);
}

/*
 * Allocate a current working segment.
 * This function always allocates a free segment in LFS manner.
 */
static int new_curseg(struct f2fs_sb_info *sbi, int type, bool new_sec, int cpu)
{
	struct curseg_info_array *cpu_array = WRAP_CURSEG_I(sbi, cpu);
	struct curseg_info *curseg = cpu_array->array + type;
	unsigned int segno = curseg->segno;
	bool pinning = type == CURSEG_COLD_DATA_PINNED;
	int ret;

	if (curseg->inited)
		write_sum_page(sbi, curseg->sum_blk, segno);

	segno = __get_next_segno(sbi, type, cpu);
	ret = get_new_segment(sbi, &segno, new_sec, pinning, cpu);
	if (ret) {
		/* DeLFS: drop the curseg ref of the abandoned segment as well */
		if (ret == -ENOSPC)
			release_curseg(sbi, curseg);
		return ret;
	}

	curseg->next_segno = segno;
	reset_curseg(sbi, type, 1, cpu, cpu_array);
	curseg->alloc_type = LFS;
	if (f2fs_need_rand_blk(sbi, type))
		curseg->fragment_remained_chunk =
				get_random_u32_inclusive(1, sbi->max_fragment_chunk);
	return 0;
}

static int __next_free_blkoff(struct f2fs_sb_info *sbi,
					int segno, block_t start)
{
	struct seg_entry *se = get_seg_entry(sbi, segno);
	int entries = SIT_VBLOCK_MAP_SIZE / sizeof(unsigned long);
	unsigned long *target_map = SIT_I(sbi)->tmp_map;
	unsigned long *ckpt_map = (unsigned long *)se->ckpt_valid_map;
	unsigned long *cur_map = (unsigned long *)se->cur_valid_map;
	int i;

	for (i = 0; i < entries; i++)
		target_map[i] = ckpt_map[i] | cur_map[i];

	return __find_rev_next_zero_bit(target_map, BLKS_PER_SEG(sbi), start);
}

static int f2fs_find_next_ssr_block(struct f2fs_sb_info *sbi,
		struct curseg_info *seg)
{
	return __next_free_blkoff(sbi, seg->segno, seg->next_blkoff + 1);
}

bool f2fs_segment_has_free_slot(struct f2fs_sb_info *sbi, int segno)
{
	return __next_free_blkoff(sbi, segno, 0) < BLKS_PER_SEG(sbi);
}

/*
 * This function always allocates a used segment(from dirty seglist) by SSR
 * manner, so it should recover the existing segment information of valid blocks
 */
static int change_curseg(struct f2fs_sb_info *sbi, int type, int cpu)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	struct curseg_info_array *cpu_array = WRAP_CURSEG_I(sbi, cpu);
	struct curseg_info *curseg = cpu_array->array + type;
	unsigned int new_segno = curseg->next_segno;
	struct f2fs_summary_block *sum_node;
	struct folio *sum_folio;

	if (curseg->inited)
		write_sum_page(sbi, curseg->sum_blk, curseg->segno);

	__set_test_and_inuse(sbi, new_segno);

	/*
	 * DeLFS (B): the dirty lists are per core region, take the seglist
	 * lock of the region that owns new_segno.
	 */
	{
		struct dirty_seg_percpu *p = per_cpu_ptr(dirty_i->pcpu,
					segno_to_sentry_cpu(sbi, new_segno));

		mutex_lock(&p->p_seglist_lock);
		__remove_dirty_segment(sbi, new_segno, PRE);
		__remove_dirty_segment(sbi, new_segno, DIRTY);
		mutex_unlock(&p->p_seglist_lock);
	}

	reset_curseg(sbi, type, 1, cpu, cpu_array);
	curseg->alloc_type = SSR;
	curseg->next_blkoff = __next_free_blkoff(sbi, curseg->segno, 0);

	sum_folio = f2fs_get_sum_folio(sbi, new_segno);
	if (IS_ERR(sum_folio)) {
		/* GC won't be able to use stale summary pages by cp_error */
		memset(curseg->sum_blk, 0, sbi->sum_entry_size);
		return PTR_ERR(sum_folio);
	}
	sum_node = SUM_BLK_PAGE_ADDR(sbi, sum_folio, new_segno);
	memcpy(curseg->sum_blk, sum_node, sbi->sum_entry_size);
	f2fs_folio_put(sum_folio, true);
	return 0;
}

static int get_ssr_segment(struct f2fs_sb_info *sbi, int type,
			int alloc_mode, unsigned long long age, int cpu);

static int get_atssr_segment(struct f2fs_sb_info *sbi, int type,
					int target_type, int alloc_mode,
					unsigned long long age, int cpu)
{
	struct curseg_info_array *cpu_array = WRAP_CURSEG_I(sbi, cpu);
	struct curseg_info *curseg = cpu_array->array + type;
	int ret = 0;

	curseg->seg_type = target_type;

	if (get_ssr_segment(sbi, type, alloc_mode, age, cpu)) {
		struct seg_entry *se = get_seg_entry(sbi, curseg->next_segno);

		curseg->seg_type = se->type;
		ret = change_curseg(sbi, type, cpu);
	} else {
		/* allocate cold segment by default */
		curseg->seg_type = CURSEG_COLD_DATA;
		ret = new_curseg(sbi, type, true, cpu);
	}
	stat_inc_seg_type(sbi, curseg);
	return ret;
}

/* DeLFS: every online core has its own ATGC curseg */
static int __f2fs_init_atgc_curseg(struct f2fs_sb_info *sbi, bool force)
{
	int cpu, ret = 0;

	if (!sbi->am.atgc_enabled && !force)
		return 0;

	f2fs_down_read(&SM_I(sbi)->curseg_lock);

	for_each_online_cpu(cpu) {
		struct curseg_info_array *cpu_array = WRAP_CURSEG_I(sbi, cpu);
		struct curseg_info *curseg =
				cpu_array->array + CURSEG_ALL_DATA_ATGC;

		mutex_lock(&curseg->curseg_mutex);
		/*
		 * DeLFS (B): no global sentry_lock. DeLFS 6.6.8 takes no SIT
		 * lock here (mount time, single thread).
		 */
		ret = get_atssr_segment(sbi, CURSEG_ALL_DATA_ATGC,
					CURSEG_COLD_DATA, SSR, 0, cpu);

		mutex_unlock(&curseg->curseg_mutex);
		if (ret)
			break;
	}

	f2fs_up_read(&SM_I(sbi)->curseg_lock);
	return ret;
}

int f2fs_init_inmem_curseg(struct f2fs_sb_info *sbi)
{
	return __f2fs_init_atgc_curseg(sbi, false);
}

int f2fs_reinit_atgc_curseg(struct f2fs_sb_info *sbi)
{
	int ret;

	if (!test_opt(sbi, ATGC))
		return 0;
	if (sbi->am.atgc_enabled)
		return 0;
	if (le64_to_cpu(F2FS_CKPT(sbi)->elapsed_time) <
			sbi->am.age_threshold)
		return 0;

	ret = __f2fs_init_atgc_curseg(sbi, true);
	if (!ret) {
		sbi->am.atgc_enabled = true;
		f2fs_info(sbi, "reenabled age threshold GC");
	}
	return ret;
}

/* DeLFS: inmem cursegs (pinned/ATGC) are per-core as well */
static void __f2fs_save_inmem_curseg(struct f2fs_sb_info *sbi, int type)
{
	int cpu;

	for_each_online_cpu(cpu) {
		struct curseg_info_array *cpu_array = WRAP_CURSEG_I(sbi, cpu);
		struct curseg_info *curseg = cpu_array->array + type;

		mutex_lock(&curseg->curseg_mutex);
		if (!curseg->inited)
			goto out;

		if (get_valid_blocks(sbi, curseg->segno, false)) {
			write_sum_page(sbi, curseg->sum_blk, curseg->segno);
		} else {
			/*
			 * DeLFS (B): no global seglist_lock any more (per-core
			 * dirty lists); as in DeLFS 6.6.8 it is not taken here.
			 */
			__set_test_and_free(sbi, curseg->segno, true);
		}
out:
		mutex_unlock(&curseg->curseg_mutex);
	}
}

void f2fs_save_inmem_curseg(struct f2fs_sb_info *sbi)
{
	__f2fs_save_inmem_curseg(sbi, CURSEG_COLD_DATA_PINNED);

	if (sbi->am.atgc_enabled)
		__f2fs_save_inmem_curseg(sbi, CURSEG_ALL_DATA_ATGC);
}

static void __f2fs_restore_inmem_curseg(struct f2fs_sb_info *sbi, int type)
{
	int cpu;

	for_each_online_cpu(cpu) {
		struct curseg_info_array *cpu_array = WRAP_CURSEG_I(sbi, cpu);
		struct curseg_info *curseg = cpu_array->array + type;

		mutex_lock(&curseg->curseg_mutex);
		if (!curseg->inited)
			goto out;
		if (get_valid_blocks(sbi, curseg->segno, false))
			goto out;

		/* DeLFS (B): no global seglist_lock, as in DeLFS 6.6.8 */
		__set_test_and_inuse(sbi, curseg->segno);
out:
		mutex_unlock(&curseg->curseg_mutex);
	}
}

void f2fs_restore_inmem_curseg(struct f2fs_sb_info *sbi)
{
	__f2fs_restore_inmem_curseg(sbi, CURSEG_COLD_DATA_PINNED);

	if (sbi->am.atgc_enabled)
		__f2fs_restore_inmem_curseg(sbi, CURSEG_ALL_DATA_ATGC);
}

static int get_ssr_segment(struct f2fs_sb_info *sbi, int type,
			int alloc_mode, unsigned long long age, int cpu)
{
	struct curseg_info_array *cpu_array = WRAP_CURSEG_I(sbi, cpu);
	struct curseg_info *curseg = cpu_array->array + type;
	unsigned segno = NULL_SEGNO;
	unsigned short seg_type = curseg->seg_type;
	int i, cnt;
	bool reversed = false;

	sanity_check_seg_type(sbi, seg_type);

	/* f2fs_need_SSR() already forces to do this */
	if (!f2fs_get_victim(sbi, &segno, BG_GC, seg_type,
				alloc_mode, age, false, cpu)) {
		curseg->next_segno = segno;
		return 1;
	}

	/* For node segments, let's do SSR more intensively */
	if (IS_NODESEG(seg_type)) {
		if (seg_type >= CURSEG_WARM_NODE) {
			reversed = true;
			i = CURSEG_COLD_NODE;
		} else {
			i = CURSEG_HOT_NODE;
		}
		cnt = NR_CURSEG_NODE_TYPE;
	} else {
		if (seg_type >= CURSEG_WARM_DATA) {
			reversed = true;
			i = CURSEG_COLD_DATA;
		} else {
			i = CURSEG_HOT_DATA;
		}
		cnt = NR_CURSEG_DATA_TYPE;
	}

	for (; cnt-- > 0; reversed ? i-- : i++) {
		if (i == seg_type)
			continue;
		if (!f2fs_get_victim(sbi, &segno, BG_GC, i,
					alloc_mode, age, false, cpu)) {
			curseg->next_segno = segno;
			return 1;
		}
	}

	/* find valid_blocks=0 in dirty list */
	if (unlikely(is_sbi_flag_set(sbi, SBI_CP_DISABLED))) {
		segno = get_free_segment(sbi);
		if (segno != NULL_SEGNO) {
			curseg->next_segno = segno;
			return 1;
		}
	}
	return 0;
}

/*
 * DeLFS: always allocate a new segment (LFS only, no SSR). ckpt->alloc_type[]
 * is shared by all cores, so SSR must not be re-enabled here.
 */
static bool need_new_seg(struct f2fs_sb_info *sbi, int type, int cpu)
{
	return true;
}

/*
 * DeLFS: move every core's curseg @type out of [start, end]. (DeLFS 6.6.8
 * only handled the curseg of the calling core.)
 */
int f2fs_allocate_segment_for_resize(struct f2fs_sb_info *sbi, int type,
					unsigned int start, unsigned int end)
{
	int cpu, ret = 0;

	f2fs_down_read(&SM_I(sbi)->curseg_lock);

	for_each_online_cpu(cpu) {
		struct curseg_info_array *cpu_array = WRAP_CURSEG_I(sbi, cpu);
		struct curseg_info *curseg = cpu_array->array + type;
		unsigned int segno;

		mutex_lock(&curseg->curseg_mutex);
		/*
		 * DeLFS (B): no global sentry_lock; as in DeLFS 6.6.8 no SIT
		 * lock is taken here (locate_dirty_segment() takes the region's
		 * seglist lock itself).
		 */
		segno = curseg->segno;
		if (segno < start || segno > end)
			goto unlock;

		if (f2fs_need_SSR(sbi) && get_ssr_segment(sbi, type, SSR, 0, cpu))
			ret = change_curseg(sbi, type, cpu);
		else
			ret = new_curseg(sbi, type, true, cpu);

		stat_inc_seg_type(sbi, curseg);

		locate_dirty_segment(sbi, segno);
unlock:

		if (segno != curseg->segno)
			f2fs_notice(sbi, "For resize: cpu %d curseg of type %d: %u ==> %u",
				    cpu, type, segno, curseg->segno);

		mutex_unlock(&curseg->curseg_mutex);
		if (ret)
			break;
	}

	f2fs_up_read(&SM_I(sbi)->curseg_lock);
	return ret;
}

/* DeLFS: allocate a new segment for @cpu's curseg @type */
static int __allocate_new_segment_cpu(struct f2fs_sb_info *sbi, int type,
				bool new_sec, bool force, int cpu)
{
	struct curseg_info_array *cpu_array = WRAP_CURSEG_I(sbi, cpu);
	struct curseg_info *curseg = cpu_array->array + type;
	unsigned int old_segno;
	int err = 0;

	if (type == CURSEG_COLD_DATA_PINNED && !curseg->inited)
		goto allocate;

	if (!force && curseg->inited &&
	    !curseg->next_blkoff &&
	    !get_valid_blocks(sbi, curseg->segno, new_sec) &&
	    !get_ckpt_valid_blocks(sbi, curseg->segno, new_sec))
		return 0;

allocate:
	old_segno = curseg->segno;
	err = new_curseg(sbi, type, true, cpu);
	if (err)
		return err;
	stat_inc_seg_type(sbi, curseg);
	locate_dirty_segment(sbi, old_segno);
	return 0;
}

/*
 * DeLFS: callers without a target core use the core they run on.
 * TODO(A): e.g. f2fs_allocate_pinning_section() allocates the pinned section
 * for the calling core only; the pinned file may later be written from
 * another core whose CURSEG_COLD_DATA_PINNED has no section.
 */
static int __allocate_new_segment(struct f2fs_sb_info *sbi, int type,
						bool new_sec, bool force)
{
	return __allocate_new_segment_cpu(sbi, type, new_sec, force,
						raw_smp_processor_id());
}

int f2fs_allocate_new_section(struct f2fs_sb_info *sbi, int type, bool force)
{
	int ret;

	/* DeLFS (B): no SIT lock here, as in DeLFS 6.6.8 */
	f2fs_down_read(&SM_I(sbi)->curseg_lock);
	ret = __allocate_new_segment(sbi, type, true, force);
	f2fs_up_read(&SM_I(sbi)->curseg_lock);

	return ret;
}

int f2fs_allocate_pinning_section(struct f2fs_sb_info *sbi)
{
	struct f2fs_lock_context lc;
	int err;
	bool gc_required = true;

retry:
	f2fs_lock_op(sbi, &lc);
	err = f2fs_allocate_new_section(sbi, CURSEG_COLD_DATA_PINNED, false);
	f2fs_unlock_op(sbi, &lc);

	if (f2fs_sb_has_blkzoned(sbi) && err == -EAGAIN && gc_required) {
		err = f2fs_gc_range(sbi, 0, sbi->first_seq_zone_segno - 1,
				true, ZONED_PIN_SEC_REQUIRED_COUNT, true);
		if (err)
			return err;
		err = f2fs_sync_fs(sbi->sb, 1);
		if (!err) {
			gc_required = false;
			goto retry;
		}
	}

	return err;
}

int f2fs_allocate_new_segments(struct f2fs_sb_info *sbi)
{
	int i, cpu;
	int err = 0;

	f2fs_down_read(&SM_I(sbi)->curseg_lock);
	/* DeLFS (B): every core's SIT lock (DDL exception, recovery only) */
	f2fs_down_write_all_sentry_locks(sbi);
	/* DeLFS: every online core's data cursegs */
	for_each_online_cpu(cpu)
		for (i = CURSEG_HOT_DATA; i <= CURSEG_COLD_DATA; i++)
			err += __allocate_new_segment_cpu(sbi, i, false, false, cpu);
	f2fs_up_write_all_sentry_locks(sbi);
	f2fs_up_read(&SM_I(sbi)->curseg_lock);

	return err;
}

bool f2fs_exist_trim_candidates(struct f2fs_sb_info *sbi,
						struct cp_control *cpc)
{
	__u64 trim_start = cpc->trim_start;
	bool has_candidate = false;

	for (; cpc->trim_start <= cpc->trim_end; cpc->trim_start++) {
		if (add_discard_addrs(sbi, cpc, true)) {
			has_candidate = true;
			break;
		}
	}

	cpc->trim_start = trim_start;
	return has_candidate;
}

static unsigned int __issue_discard_cmd_range(struct f2fs_sb_info *sbi,
					struct discard_policy *dpolicy,
					unsigned int start, unsigned int end)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct discard_cmd *prev_dc = NULL, *next_dc = NULL;
	struct rb_node **insert_p = NULL, *insert_parent = NULL;
	struct discard_cmd *dc;
	struct blk_plug plug;
	int issued;
	unsigned int trimmed = 0;

	int dcc_cpu = segno_to_sentry_cpu(sbi, GET_SEGNO(sbi, start));
	struct discard_set *ds = per_cpu_ptr(dcc->ds, dcc_cpu);
next:
	issued = 0;

	mutex_lock(&ds->p_cmd_lock);
	if (unlikely(dcc->rbtree_check))
		f2fs_bug_on(sbi, !f2fs_check_discard_tree(sbi));

	dc = __lookup_discard_cmd_ret(&ds->p_root, start,
				&prev_dc, &next_dc, &insert_p, &insert_parent);
	if (!dc)
		dc = next_dc;

	blk_start_plug(&plug);

	while (dc && dc->di.lstart <= end) {
		struct rb_node *node;
		int err = 0;

		if (dc->di.len < dpolicy->granularity)
			goto skip;

		if (dc->state != D_PREP) {
			list_move_tail(&dc->list, &ds->p_fstrim_list);
			goto skip;
		}

		err = __submit_discard_cmd(sbi, dpolicy, dc, &issued);

		if (issued >= dpolicy->max_requests) {
			start = dc->di.lstart + dc->di.len;

			if (err)
				__remove_discard_cmd(sbi, dc);

			blk_finish_plug(&plug);
			mutex_unlock(&ds->p_cmd_lock);
			trimmed += __wait_all_discard_cmd(sbi, NULL);
			f2fs_schedule_timeout(DEFAULT_DISCARD_INTERVAL);
			goto next;
		}
skip:
		node = rb_next(&dc->rb_node);
		if (err)
			__remove_discard_cmd(sbi, dc);
		dc = rb_entry_safe(node, struct discard_cmd, rb_node);

		if (fatal_signal_pending(current))
			break;
	}

	blk_finish_plug(&plug);
	mutex_unlock(&ds->p_cmd_lock);

	return trimmed;
}

int f2fs_trim_fs(struct f2fs_sb_info *sbi, struct fstrim_range *range)
{
	__u64 start = F2FS_BYTES_TO_BLK(range->start);
	__u64 end = start + F2FS_BYTES_TO_BLK(range->len) - 1;
	unsigned int start_segno, end_segno;
	block_t start_block, end_block;
	struct cp_control cpc;
	struct discard_policy dpolicy;
	struct f2fs_lock_context lc;
	unsigned long long trimmed = 0;
	int err = 0;
	bool need_align = f2fs_lfs_mode(sbi) && __is_large_section(sbi);

	if (start >= MAX_BLKADDR(sbi) || range->len < sbi->blocksize)
		return -EINVAL;

	if (end < MAIN_BLKADDR(sbi))
		goto out;

	if (is_sbi_flag_set(sbi, SBI_NEED_FSCK)) {
		f2fs_warn(sbi, "Found FS corruption, run fsck to fix.");
		return -EFSCORRUPTED;
	}

	/* start/end segment number in main_area */
	start_segno = (start <= MAIN_BLKADDR(sbi)) ? 0 : GET_SEGNO(sbi, start);
	end_segno = (end >= MAX_BLKADDR(sbi)) ? MAIN_SEGS(sbi) - 1 :
						GET_SEGNO(sbi, end);
	if (need_align) {
		start_segno = rounddown(start_segno, SEGS_PER_SEC(sbi));
		end_segno = roundup(end_segno + 1, SEGS_PER_SEC(sbi)) - 1;
	}

	cpc.reason = CP_DISCARD;
	cpc.trim_minlen = max_t(__u64, 1, F2FS_BYTES_TO_BLK(range->minlen));
	cpc.trim_start = start_segno;
	cpc.trim_end = end_segno;

	if (atomic_read(&sbi->discard_blks) == 0)
		goto out;

	f2fs_down_write_trace(&sbi->gc_lock, &lc);
	stat_inc_cp_call_count(sbi, TOTAL_CALL);
	err = f2fs_write_checkpoint(sbi, &cpc);
	f2fs_up_write_trace(&sbi->gc_lock, &lc);
	if (err)
		goto out;

	/*
	 * We filed discard candidates, but actually we don't need to wait for
	 * all of them, since they'll be issued in idle time along with runtime
	 * discard option. User configuration looks like using runtime discard
	 * or periodic fstrim instead of it.
	 */
	if (f2fs_realtime_discard_enable(sbi))
		goto out;

	start_block = START_BLOCK(sbi, start_segno);
	end_block = START_BLOCK(sbi, end_segno + 1);

	__init_discard_policy(sbi, &dpolicy, DPOLICY_FSTRIM, cpc.trim_minlen);
	trimmed = __issue_discard_cmd_range(sbi, &dpolicy,
					start_block, end_block);

	trimmed += __wait_discard_cmd_range(sbi, &dpolicy,
					start_block, end_block);
out:
	if (!err)
		range->len = F2FS_BLK_TO_BYTES(trimmed);
	return err;
}

int f2fs_rw_hint_to_seg_type(struct f2fs_sb_info *sbi, enum rw_hint hint)
{
	if (F2FS_OPTION(sbi).active_logs == 2)
		return CURSEG_HOT_DATA;
	else if (F2FS_OPTION(sbi).active_logs == 4)
		return CURSEG_COLD_DATA;

	/* active_log == 6 */
	switch (hint) {
	case WRITE_LIFE_SHORT:
		return CURSEG_HOT_DATA;
	case WRITE_LIFE_EXTREME:
		return CURSEG_COLD_DATA;
	default:
		return CURSEG_WARM_DATA;
	}
}

/*
 * This returns write hints for each segment type. This hints will be
 * passed down to block layer as below by default.
 *
 * User                  F2FS                     Block
 * ----                  ----                     -----
 *                       META                     WRITE_LIFE_NONE|REQ_META
 *                       HOT_NODE                 WRITE_LIFE_NONE
 *                       WARM_NODE                WRITE_LIFE_MEDIUM
 *                       COLD_NODE                WRITE_LIFE_LONG
 * ioctl(COLD)           COLD_DATA                WRITE_LIFE_EXTREME
 * extension list        "                        "
 *
 * -- buffered io
 *                       COLD_DATA                WRITE_LIFE_EXTREME
 *                       HOT_DATA                 WRITE_LIFE_SHORT
 *                       WARM_DATA                WRITE_LIFE_NOT_SET
 *
 * -- direct io
 * WRITE_LIFE_EXTREME    COLD_DATA                WRITE_LIFE_EXTREME
 * WRITE_LIFE_SHORT      HOT_DATA                 WRITE_LIFE_SHORT
 * WRITE_LIFE_NOT_SET    WARM_DATA                WRITE_LIFE_NOT_SET
 * WRITE_LIFE_NONE       "                        WRITE_LIFE_NONE
 * WRITE_LIFE_MEDIUM     "                        WRITE_LIFE_MEDIUM
 * WRITE_LIFE_LONG       "                        WRITE_LIFE_LONG
 */
enum rw_hint f2fs_io_type_to_rw_hint(struct f2fs_sb_info *sbi,
				enum page_type type, enum temp_type temp)
{
	switch (type) {
	case DATA:
		switch (temp) {
		case WARM:
			return WRITE_LIFE_NOT_SET;
		case HOT:
			return WRITE_LIFE_SHORT;
		case COLD:
			return WRITE_LIFE_EXTREME;
		default:
			return WRITE_LIFE_NONE;
		}
	case NODE:
		switch (temp) {
		case WARM:
			return WRITE_LIFE_MEDIUM;
		case HOT:
			return WRITE_LIFE_NONE;
		case COLD:
			return WRITE_LIFE_LONG;
		default:
			return WRITE_LIFE_NONE;
		}
	case META:
		return WRITE_LIFE_NONE;
	default:
		return WRITE_LIFE_NONE;
	}
}

u8 f2fs_io_type_to_write_stream(struct block_device *bdev,
				enum page_type type, enum temp_type temp)
{
	unsigned short nr = bdev_max_write_streams(bdev);

	if (type != DATA || !nr)
		return 0;
	if (nr < NR_TEMP_TYPE)
		return temp == COLD ? nr : HOT + 1;

	return temp + 1;
}

static int __get_segment_type_2(struct f2fs_io_info *fio)
{
	if (fio->type == DATA)
		return CURSEG_HOT_DATA;
	else
		return CURSEG_HOT_NODE;
}

static int __get_segment_type_4(struct f2fs_io_info *fio)
{
	if (fio->type == DATA) {
		struct inode *inode = fio_inode(fio);

		if (S_ISDIR(inode->i_mode))
			return CURSEG_HOT_DATA;
		else
			return CURSEG_COLD_DATA;
	} else {
		if (IS_DNODE(fio->folio) && is_cold_node(fio->folio))
			return CURSEG_WARM_NODE;
		else
			return CURSEG_COLD_NODE;
	}
}

static int __get_age_segment_type(struct inode *inode, pgoff_t pgofs)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct extent_info ei = {};

	if (f2fs_lookup_age_extent_cache(inode, pgofs, &ei)) {
		if (!ei.age)
			return NO_CHECK_TYPE;
		if (ei.age <= sbi->hot_data_age_threshold)
			return CURSEG_HOT_DATA;
		if (ei.age <= sbi->warm_data_age_threshold)
			return CURSEG_WARM_DATA;
		return CURSEG_COLD_DATA;
	}
	return NO_CHECK_TYPE;
}

static int __get_segment_type_6(struct f2fs_io_info *fio)
{
	if (fio->type == DATA) {
		struct inode *inode = fio_inode(fio);
		int type;

		if (is_inode_flag_set(inode, FI_ALIGNED_WRITE))
			return CURSEG_COLD_DATA_PINNED;

		if (page_private_gcing(fio->page)) {
			if (fio->sbi->am.atgc_enabled &&
				(fio->io_type == FS_DATA_IO) &&
				(fio->sbi->gc_mode != GC_URGENT_HIGH) &&
				__is_valid_data_blkaddr(fio->old_blkaddr) &&
				!is_inode_flag_set(inode, FI_OPU_WRITE))
				return CURSEG_ALL_DATA_ATGC;
			else
				return CURSEG_COLD_DATA;
		}
		if (file_is_cold(inode) || f2fs_need_compress_data(inode))
			return CURSEG_COLD_DATA;

		type = __get_age_segment_type(inode, fio->folio->index);
		if (type != NO_CHECK_TYPE)
			return type;

		if (file_is_hot(inode) ||
				is_inode_flag_set(inode, FI_HOT_DATA) ||
				f2fs_is_cow_file(inode) ||
				is_inode_flag_set(inode, FI_NEED_IPU))
			return CURSEG_HOT_DATA;
		return f2fs_rw_hint_to_seg_type(F2FS_I_SB(inode),
						inode->i_write_hint);
	} else {
		if (IS_DNODE(fio->folio))
			return is_cold_node(fio->folio) ? CURSEG_WARM_NODE :
						CURSEG_HOT_NODE;
		return CURSEG_COLD_NODE;
	}
}

enum temp_type f2fs_get_segment_temp(struct f2fs_sb_info *sbi,
						enum log_type type)
{
	/*
	 * DeLFS: seg_type of a log type is the same on every core except the
	 * ATGC log (set per core by get_atssr_segment()), so look at the
	 * calling core.
	 * TODO(A): for CURSEG_ALL_DATA_ATGC the result may differ from the core
	 * that eventually allocates the block.
	 */
	struct curseg_info_array *cpu_array =
			WRAP_CURSEG_I(sbi, raw_smp_processor_id());
	struct curseg_info *curseg = cpu_array->array + type;
	enum temp_type temp = COLD;

	switch (curseg->seg_type) {
	case CURSEG_HOT_NODE:
	case CURSEG_HOT_DATA:
		temp = HOT;
		break;
	case CURSEG_WARM_NODE:
	case CURSEG_WARM_DATA:
		temp = WARM;
		break;
	case CURSEG_COLD_NODE:
	case CURSEG_COLD_DATA:
		temp = COLD;
		break;
	default:
		f2fs_bug_on(sbi, 1);
	}

	return temp;
}

static int __get_segment_type(struct f2fs_io_info *fio)
{
	enum log_type type = CURSEG_HOT_DATA;

	switch (F2FS_OPTION(fio->sbi).active_logs) {
	case 2:
		type = __get_segment_type_2(fio);
		break;
	case 4:
		type = __get_segment_type_4(fio);
		break;
	case 6:
		type = __get_segment_type_6(fio);
		break;
	default:
		f2fs_bug_on(fio->sbi, true);
	}

	fio->temp = f2fs_get_segment_temp(fio->sbi, type);

	return type;
}

static void f2fs_randomize_chunk(struct f2fs_sb_info *sbi,
		struct curseg_info *seg)
{
	/* To allocate block chunks in different sizes, use random number */
	if (--seg->fragment_remained_chunk > 0)
		return;

	seg->fragment_remained_chunk =
		get_random_u32_inclusive(1, sbi->max_fragment_chunk);
	seg->next_blkoff +=
		get_random_u32_inclusive(1, sbi->max_fragment_hole);
}

/* DeLFS deLLC (oldinv.c) */
void oldinv_enqueue(struct f2fs_sb_info *sbi, block_t old_blkaddr);

static bool f2fs_fio_is_gc_data_write(struct f2fs_io_info *fio,
				      struct folio *folio, int type)
{
	if (type == CURSEG_ALL_DATA_ATGC)
		return true;

	if (fio && fio->io_type == FS_GC_DATA_IO)
		return true;

	if (folio && IS_DATASEG(type) && folio_test_f2fs_gcing(folio))
		return true;

	return false;
}

static bool f2fs_fio_is_gc_write(struct f2fs_io_info *fio,
				 struct folio *folio, int type)
{
	if (f2fs_fio_is_gc_data_write(fio, folio, type))
		return true;

	if (fio && fio->io_type == FS_GC_NODE_IO)
		return true;

	return false;
}

/*
 * Caller must hold the matching per-core sentry lock.
 * This helper is intentionally idempotent: if the old bit is already clear,
 * do nothing instead of triggering update_sit_entry(..., -1) bug path.
 */
static bool f2fs_try_invalidate_old_blkaddr_locked(struct f2fs_sb_info *sbi,
						   block_t old_blkaddr)
{
	unsigned int segno = GET_SEGNO(sbi, old_blkaddr);
	unsigned int offset;
	struct seg_entry *se;

	if (segno == NULL_SEGNO)
		return false;

	se = get_seg_entry(sbi, segno);
	offset = GET_BLKOFF_FROM_SEG0(sbi, old_blkaddr);

	if (!f2fs_test_bit(offset, se->cur_valid_map))
		return false;

	update_segment_mtime(sbi, old_blkaddr, 0);
	update_sit_entry(sbi, old_blkaddr, -1);
	locate_dirty_segment(sbi, segno);

	return true;
}

static void f2fs_invalidate_old_blkaddr_sync(struct f2fs_sb_info *sbi,
					     block_t old_blkaddr,
					     int sentry_cpu)
{
	struct sit_info *sit_i = SIT_I(sbi);
	struct rw_semaphore *se_lock;

	if (sentry_cpu < 0)
		return;

	se_lock = per_cpu_ptr(sit_i->percore_sentry_lock, sentry_cpu);

	down_write(se_lock);
	f2fs_try_invalidate_old_blkaddr_locked(sbi, old_blkaddr);
	up_write(se_lock);
}

int f2fs_allocate_data_block(struct f2fs_sb_info *sbi, struct folio *folio,
		block_t old_blkaddr, block_t *new_blkaddr,
		struct f2fs_summary *sum, int type,
		struct f2fs_io_info *fio)
{
	struct sit_info *sit_i = SIT_I(sbi);
	/*
	 * DeLFS: allocate from the cursegs of the core we run on. The task may
	 * migrate afterwards; curseg_mutex serializes users of this curseg.
	 */
	int cpu = raw_smp_processor_id();
	struct curseg_info_array *cpu_array = WRAP_CURSEG_I(sbi, cpu);
	struct curseg_info *curseg = cpu_array->array + type;
	unsigned long long old_mtime = 0;
	bool from_gc = (type == CURSEG_ALL_DATA_ATGC);
	/* DeLFS (B): GC writes, see f2fs_fio_is_gc_data_write/_gc_write() */
	bool gc_data_write = f2fs_fio_is_gc_data_write(fio, folio, type);
	bool gc_write = f2fs_fio_is_gc_write(fio, folio, type);
	struct seg_entry *se = NULL;
	bool segment_full = false;
	unsigned int old_segno, new_segno;
	int sentry_cpu_old, sentry_cpu_new;
	struct rw_semaphore *se_lock;
	int ret = 0;

	f2fs_down_read(&SM_I(sbi)->curseg_lock);

	/*
	 * DeLFS (B) DDL: no SIT lock while the curseg moves on. The SIT
	 * entries are updated after curseg_mutex is released, each under the
	 * SIT lock of the core that owns the segment, one lock at a time
	 * (curseg_lock(read) -> curseg_mutex -> unlock -> owner SIT lock).
	 */
	mutex_lock(&curseg->curseg_mutex);

	if (curseg->segno == NULL_SEGNO) {
		ret = -ENOSPC;
		goto out_err;
	}

	/*
	 * DeLFS (B): GC data migration keeps the source segment's mtime, and
	 * its old block must be invalidated before FG_GC checks the victim.
	 */
	if (gc_data_write) {
		f2fs_bug_on(sbi, GET_SEGNO(sbi, old_blkaddr) == NULL_SEGNO);
		se = get_seg_entry(sbi, GET_SEGNO(sbi, old_blkaddr));
		sanity_check_seg_type(sbi, se->type);
		f2fs_bug_on(sbi, IS_NODESEG(se->type));
		old_mtime = get_segment_mtime(sbi, old_blkaddr);
	}
	*new_blkaddr = NEXT_FREE_BLKADDR(sbi, curseg);

	f2fs_bug_on(sbi, curseg->next_blkoff >= BLKS_PER_SEG(sbi));

	f2fs_wait_discard_bio(sbi, *new_blkaddr);

	sum_entries(curseg->sum_blk)[curseg->next_blkoff] = *sum;
	if (curseg->alloc_type == SSR) {
		curseg->next_blkoff = f2fs_find_next_ssr_block(sbi, curseg);
	} else {
		curseg->next_blkoff++;
		if (f2fs_need_rand_blk(sbi, type))
			f2fs_randomize_chunk(sbi, curseg);
	}
	if (curseg->next_blkoff >= f2fs_usable_blks_in_seg(sbi, curseg->segno))
		segment_full = true;
	stat_inc_block_count(sbi, curseg);

	/*
	 * If the current segment is full, flush it out and replace it with a
	 * new segment.
	 */
	if (segment_full) {
		if (type == CURSEG_COLD_DATA_PINNED &&
		    !((curseg->segno + 1) % sbi->segs_per_sec)) {
			write_sum_page(sbi, curseg->sum_blk, curseg->segno);
			/* DeLFS: drop the curseg ref as well */
			release_curseg(sbi, curseg);
			goto skip_new_segment;
		}

		if (from_gc) {
			ret = get_atssr_segment(sbi, type, se->type,
						AT_SSR, se->mtime, cpu);
		} else {
			if (need_new_seg(sbi, type, cpu))
				ret = new_curseg(sbi, type, false, cpu);
			else
				ret = change_curseg(sbi, type, cpu);
			stat_inc_seg_type(sbi, curseg);
		}

		if (ret)
			goto out_err;
	}

skip_new_segment:
	if (IS_DATASEG(curseg->seg_type)) {
		unsigned long long new_val;

		new_val = atomic64_inc_return(&sbi->allocated_data_blocks);
		if (unlikely(new_val == ULLONG_MAX))
			atomic64_set(&sbi->allocated_data_blocks, 0);
	}

	if (folio && IS_NODESEG(curseg->seg_type)) {
		fill_node_footer_blkaddr(folio, NEXT_FREE_BLKADDR(sbi, curseg));

		f2fs_inode_chksum_set(sbi, folio);
	}

	/*
	 * DeLFS (B): GC's move_data_block() passes its READ fio only to mark
	 * the write as GC (fio->io_type); it submits the block by itself, so
	 * only WRITE fios are queued.
	 */
	if (fio && fio->op == REQ_OP_WRITE) {
		struct f2fs_bio_info *io;

		INIT_LIST_HEAD(&fio->list);
		fio->in_list = 1;
		/* DeLFS: remember the core whose cursegs served this IO */
		fio->cpu = cpu;
		/*
		 * TODO(A): DeLFS queues the fio on the per-core write_io
		 * (sbi->wrap_write_io of @cpu) and only for REQ_OP_WRITE fios;
		 * write_io is owned by another part, so it is kept global here.
		 */
		io = sbi->write_io[fio->type] + fio->temp;
		spin_lock(&io->io_lock);
		list_add_tail(&fio->list, &io->io_list);
		spin_unlock(&io->io_lock);
	}

	mutex_unlock(&curseg->curseg_mutex);

	/*
	 * DeLFS (B) DDL: publish the new block under the SIT lock of the core
	 * that owns its segment. An old block of the same core is invalidated
	 * under that same lock. An old block of another core is invalidated
	 * right away for GC writes (FG_GC checks the victim's valid blocks
	 * next), otherwise it goes to deLLC (oldinv_enqueue).
	 */
	old_segno = GET_SEGNO(sbi, old_blkaddr);
	new_segno = GET_SEGNO(sbi, *new_blkaddr);
	sentry_cpu_old = (old_segno == NULL_SEGNO) ? -1 :
				segno_to_sentry_cpu(sbi, old_segno);
	sentry_cpu_new = segno_to_sentry_cpu(sbi, new_segno);

	se_lock = per_cpu_ptr(sit_i->percore_sentry_lock, sentry_cpu_new);
	down_write(se_lock);
	update_segment_mtime(sbi, *new_blkaddr, old_mtime);
	update_sit_entry(sbi, *new_blkaddr, 1);
	locate_dirty_segment(sbi, new_segno);
	if (sentry_cpu_old == sentry_cpu_new)
		f2fs_try_invalidate_old_blkaddr_locked(sbi, old_blkaddr);
	up_write(se_lock);

	if (sentry_cpu_old != -1 && sentry_cpu_old != sentry_cpu_new) {
		if (gc_write)
			f2fs_invalidate_old_blkaddr_sync(sbi, old_blkaddr,
							 sentry_cpu_old);
		else
			oldinv_enqueue(sbi, old_blkaddr);
	}

	f2fs_up_read(&SM_I(sbi)->curseg_lock);
	return 0;

out_err:
	*new_blkaddr = NULL_ADDR;
	mutex_unlock(&curseg->curseg_mutex);
	f2fs_up_read(&SM_I(sbi)->curseg_lock);
	return ret;
}

void f2fs_update_device_state(struct f2fs_sb_info *sbi, nid_t ino,
					block_t blkaddr, unsigned int blkcnt)
{
	if (!f2fs_is_multi_device(sbi))
		return;

	while (1) {
		unsigned int devidx = f2fs_target_device_index(sbi, blkaddr);
		unsigned int blks = FDEV(devidx).end_blk - blkaddr + 1;

		/* update device state for fsync */
		f2fs_set_dirty_device(sbi, ino, devidx, FLUSH_INO);

		/* update device state for checkpoint */
		if (!f2fs_test_bit(devidx, (char *)&sbi->dirty_device)) {
			spin_lock(&sbi->dev_lock);
			f2fs_set_bit(devidx, (char *)&sbi->dirty_device);
			spin_unlock(&sbi->dev_lock);
		}

		if (blkcnt <= blks)
			break;
		blkcnt -= blks;
		blkaddr += blks;
	}
}

static int log_type_to_seg_type(enum log_type type)
{
	int seg_type = CURSEG_COLD_DATA;

	switch (type) {
	case CURSEG_HOT_DATA:
	case CURSEG_WARM_DATA:
	case CURSEG_COLD_DATA:
	case CURSEG_HOT_NODE:
	case CURSEG_WARM_NODE:
	case CURSEG_COLD_NODE:
		seg_type = (int)type;
		break;
	case CURSEG_COLD_DATA_PINNED:
	case CURSEG_ALL_DATA_ATGC:
		seg_type = CURSEG_COLD_DATA;
		break;
	default:
		break;
	}
	return seg_type;
}

static void do_write_page(struct f2fs_summary *sum, struct f2fs_io_info *fio)
{
	struct folio *folio = fio->folio;
	enum log_type type = __get_segment_type(fio);
	int seg_type = log_type_to_seg_type(type);
	bool keep_order = (f2fs_lfs_mode(fio->sbi) &&
				seg_type == CURSEG_COLD_DATA);
	int err;

	if (keep_order)
		f2fs_down_read(&fio->sbi->io_order_lock);

	err = f2fs_allocate_data_block(fio->sbi, folio, fio->old_blkaddr,
			&fio->new_blkaddr, sum, type, fio);
	if (unlikely(err)) {
		f2fs_err_ratelimited(fio->sbi,
			"%s Failed to allocate data block, ino:%u, index:%lu, type:%d, old_blkaddr:0x%x, new_blkaddr:0x%x, err:%d",
			__func__, fio->ino, folio->index, type,
			fio->old_blkaddr, fio->new_blkaddr, err);
		if (fscrypt_inode_uses_fs_layer_crypto(folio->mapping->host))
			fscrypt_finalize_bounce_page(&fio->encrypted_page);
		folio_end_writeback(folio);
		if (f2fs_in_warm_node_list(folio))
			f2fs_del_fsync_node_entry(fio->sbi, folio);
		f2fs_bug_on(fio->sbi, !is_set_ckpt_flags(fio->sbi,
							CP_ERROR_FLAG));
		goto out;
	}

	f2fs_bug_on(fio->sbi, !f2fs_is_valid_blkaddr_raw(fio->sbi,
				fio->new_blkaddr, DATA_GENERIC_ENHANCE));

	if (GET_SEGNO(fio->sbi, fio->old_blkaddr) != NULL_SEGNO)
		f2fs_invalidate_internal_cache(fio->sbi, fio->old_blkaddr, 1);

	/* writeout dirty page into bdev */
	f2fs_submit_page_write(fio);

	f2fs_update_device_state(fio->sbi, fio->ino, fio->new_blkaddr, 1);
out:
	if (keep_order)
		f2fs_up_read(&fio->sbi->io_order_lock);
}

void f2fs_do_write_meta_page(struct f2fs_sb_info *sbi, struct folio *folio,
					enum iostat_type io_type)
{
	struct f2fs_io_info fio = {
		.sbi = sbi,
		.type = META,
		.temp = HOT,
		.op = REQ_OP_WRITE,
		.op_flags = REQ_SYNC | REQ_META | REQ_PRIO,
		.old_blkaddr = folio->index,
		.new_blkaddr = folio->index,
		.folio = folio,
		.encrypted_page = NULL,
		.in_list = 0,
	};

	if (unlikely(folio->index >= MAIN_BLKADDR(sbi)))
		fio.op_flags &= ~REQ_META;

	folio_start_writeback(folio);
	f2fs_submit_page_write(&fio);

	stat_inc_meta_count(sbi, folio->index);
	f2fs_update_iostat(sbi, NULL, io_type, F2FS_BLKSIZE);
}

void f2fs_do_write_node_page(unsigned int nid, struct f2fs_io_info *fio)
{
	struct f2fs_summary sum;

	set_summary(&sum, nid, 0, 0);
	do_write_page(&sum, fio);

	f2fs_update_iostat(fio->sbi, NULL, fio->io_type, F2FS_BLKSIZE);
}

void f2fs_outplace_write_data(struct dnode_of_data *dn,
					struct f2fs_io_info *fio)
{
	struct f2fs_sb_info *sbi = fio->sbi;
	struct f2fs_summary sum;

	f2fs_bug_on(sbi, dn->data_blkaddr == NULL_ADDR);
	if (fio->io_type == FS_DATA_IO || fio->io_type == FS_CP_DATA_IO)
		f2fs_update_age_extent_cache(dn);
	set_summary(&sum, dn->nid, dn->ofs_in_node, fio->version);
	do_write_page(&sum, fio);
	f2fs_update_data_blkaddr(dn, fio->new_blkaddr);

	f2fs_update_iostat(sbi, dn->inode, fio->io_type, F2FS_BLKSIZE);
}

int f2fs_inplace_write_data(struct f2fs_io_info *fio)
{
	int err;
	struct f2fs_sb_info *sbi = fio->sbi;
	unsigned int segno;

	fio->new_blkaddr = fio->old_blkaddr;
	/* i/o temperature is needed for passing down write hints */
	__get_segment_type(fio);

	segno = GET_SEGNO(sbi, fio->new_blkaddr);

	if (!IS_DATASEG(get_seg_entry(sbi, segno)->type)) {
		set_sbi_flag(sbi, SBI_NEED_FSCK);
		f2fs_warn(sbi, "%s: incorrect segment(%u) type, run fsck to fix.",
			  __func__, segno);
		err = -EFSCORRUPTED;
		f2fs_handle_error(sbi, ERROR_INCONSISTENT_SUM_TYPE);
		goto drop_bio;
	}

	if (f2fs_cp_error(sbi)) {
		err = -EIO;
		goto drop_bio;
	}

	if (fio->meta_gc)
		f2fs_truncate_meta_inode_pages(sbi, fio->new_blkaddr, 1);

	stat_inc_inplace_blocks(fio->sbi);

	if (fio->bio && !IS_F2FS_IPU_NOCACHE(sbi))
		err = f2fs_merge_page_bio(fio);
	else
		err = f2fs_submit_page_bio(fio);
	if (!err) {
		f2fs_update_device_state(fio->sbi, fio->ino,
						fio->new_blkaddr, 1);
		f2fs_update_iostat(fio->sbi, fio_inode(fio),
						fio->io_type, F2FS_BLKSIZE);
	}

	return err;
drop_bio:
	if (fio->bio && *(fio->bio)) {
		struct bio *bio = *(fio->bio);

		bio->bi_status = BLK_STS_IOERR;
		bio_endio(bio);
		*(fio->bio) = NULL;
	}
	return err;
}

/*
 * DeLFS: find which core's curseg holds @segno. Returns the log type and
 * sets *cpu, or NO_CHECK_TYPE. (DeLFS 6.6.8 only searched the calling core,
 * which hits f2fs_bug_on() when another core owns the curseg.)
 */
static inline int __f2fs_get_curseg(struct f2fs_sb_info *sbi,
					unsigned int segno, int *cpu)
{
	int i, c;

	for_each_online_cpu(c) {
		struct curseg_info_array *cpu_array = WRAP_CURSEG_I(sbi, c);

		for (i = CURSEG_HOT_DATA; i < NO_CHECK_TYPE; i++) {
			if ((cpu_array->array + i)->segno == segno) {
				*cpu = c;
				return i;
			}
		}
	}
	return NO_CHECK_TYPE;
}

void f2fs_do_replace_block(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
				block_t old_blkaddr, block_t new_blkaddr,
				bool recover_curseg, bool recover_newaddr,
				bool from_gc)
{
	struct curseg_info *curseg;
	unsigned int segno, old_cursegno;
	struct seg_entry *se;
	int type;
	/* DeLFS: default to the calling core; curseg_lock(write) excludes allocators */
	int cpu = raw_smp_processor_id();
	unsigned short old_blkoff;
	unsigned char old_alloc_type;

	segno = GET_SEGNO(sbi, new_blkaddr);
	se = get_seg_entry(sbi, segno);
	type = se->type;

	f2fs_down_write(&SM_I(sbi)->curseg_lock);

	if (!recover_curseg) {
		/* for recovery flow */
		if (se->valid_blocks == 0 && !is_curseg(sbi, segno)) {
			if (old_blkaddr == NULL_ADDR)
				type = CURSEG_COLD_DATA;
			else
				type = CURSEG_WARM_DATA;
		}
	} else {
		if (is_curseg(sbi, segno)) {
			/* se->type is volatile as SSR allocation */
			type = __f2fs_get_curseg(sbi, segno, &cpu);
			f2fs_bug_on(sbi, type == NO_CHECK_TYPE);
		} else {
			type = CURSEG_WARM_DATA;
		}
	}

	curseg = WRAP_CURSEG_I(sbi, cpu)->array + type;
	f2fs_bug_on(sbi, !IS_DATASEG(curseg->seg_type));

	mutex_lock(&curseg->curseg_mutex);
	/*
	 * DeLFS (B): the old and new blocks can belong to different cores'
	 * regions, so every region's SIT lock is taken (DDL exception, rare
	 * path: recovery and f2fs_replace_block), as in DeLFS 6.6.8.
	 */
	f2fs_down_write_all_sentry_locks(sbi);

	old_cursegno = curseg->segno;
	old_blkoff = curseg->next_blkoff;
	old_alloc_type = curseg->alloc_type;

	/* change the current segment */
	if (segno != curseg->segno) {
		curseg->next_segno = segno;
		if (change_curseg(sbi, type, cpu))
			goto out_unlock;
	}

	curseg->next_blkoff = GET_BLKOFF_FROM_SEG0(sbi, new_blkaddr);
	sum_entries(curseg->sum_blk)[curseg->next_blkoff] = *sum;

	if (!recover_curseg || recover_newaddr) {
		if (!from_gc)
			update_segment_mtime(sbi, new_blkaddr, 0);
		update_sit_entry(sbi, new_blkaddr, 1);
	}
	if (GET_SEGNO(sbi, old_blkaddr) != NULL_SEGNO) {
		f2fs_invalidate_internal_cache(sbi, old_blkaddr, 1);
		if (!from_gc)
			update_segment_mtime(sbi, old_blkaddr, 0);
		update_sit_entry(sbi, old_blkaddr, -1);
	}

	locate_dirty_segment(sbi, GET_SEGNO(sbi, old_blkaddr));
	locate_dirty_segment(sbi, GET_SEGNO(sbi, new_blkaddr));

	locate_dirty_segment(sbi, old_cursegno);

	if (recover_curseg) {
		if (old_cursegno != curseg->segno) {
			curseg->next_segno = old_cursegno;
			if (change_curseg(sbi, type, cpu))
				goto out_unlock;
		}
		curseg->next_blkoff = old_blkoff;
		curseg->alloc_type = old_alloc_type;
	}

out_unlock:
	f2fs_up_write_all_sentry_locks(sbi);
	mutex_unlock(&curseg->curseg_mutex);
	f2fs_up_write(&SM_I(sbi)->curseg_lock);
}

void f2fs_replace_block(struct f2fs_sb_info *sbi, struct dnode_of_data *dn,
				block_t old_addr, block_t new_addr,
				unsigned char version, bool recover_curseg,
				bool recover_newaddr)
{
	struct f2fs_summary sum;

	set_summary(&sum, dn->nid, dn->ofs_in_node, version);

	f2fs_do_replace_block(sbi, &sum, old_addr, new_addr,
					recover_curseg, recover_newaddr, false);

	f2fs_update_data_blkaddr(dn, new_addr);
}

void f2fs_folio_wait_writeback(struct folio *folio, enum page_type type,
		bool ordered, bool locked)
{
	if (folio_test_writeback(folio)) {
		struct f2fs_sb_info *sbi = F2FS_F_SB(folio);

		/* submit cached LFS IO */
		f2fs_submit_merged_write_folio(sbi, folio, type);
		/* submit cached IPU IO */
		f2fs_submit_merged_ipu_write(sbi, NULL, folio);
		if (ordered) {
			folio_wait_writeback(folio);
			f2fs_bug_on(sbi, locked && folio_test_writeback(folio));
		} else {
			folio_wait_stable(folio);
		}
	}
}

void f2fs_wait_on_block_writeback(struct inode *inode, block_t blkaddr)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct folio *cfolio;

	if (!f2fs_meta_inode_gc_required(inode))
		return;

	if (!__is_valid_data_blkaddr(blkaddr))
		return;

	cfolio = filemap_lock_folio(META_MAPPING(sbi), blkaddr);
	if (!IS_ERR(cfolio)) {
		f2fs_folio_wait_writeback(cfolio, DATA, true, true);
		f2fs_folio_put(cfolio, true);
	}
}

void f2fs_wait_on_block_writeback_range(struct inode *inode, block_t blkaddr,
								block_t len)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	block_t i;

	if (!f2fs_meta_inode_gc_required(inode))
		return;

	for (i = 0; i < len; i++)
		f2fs_wait_on_block_writeback(inode, blkaddr + i);

	f2fs_truncate_meta_inode_pages(sbi, blkaddr, len);
}

/*
 * DeLFS: restore @cpu's compacted data summaries. Every core owns
 * percore_sum_step() blocks starting at start_sum_block(), and its compacted
 * data summaries occupy the first @blk_cnt (= max_npages) of them.
 *
 * TODO(B)/TODO(C): the SIT(COLD_DATA)/NAT(HOT_DATA) journals restored here
 * are per-core; their consumers (SIT/NAT journal flush/lookup) are B/C.
 */
static int percore_read_compacted_summaries(struct f2fs_sb_info *sbi,
			int blk_cnt, int cpu, struct curseg_info_array *cpu_array,
			int max_npages)
{
	struct f2fs_checkpoint *ckpt = F2FS_CKPT(sbi);
	struct curseg_info *seg_i;
	unsigned char *kaddr;
	struct folio *folio;
	block_t start, end;
	int i, j, offset;

	start = start_sum_block(sbi) + cpu * percore_sum_step(sbi, max_npages);
	end = start + blk_cnt;

	folio = f2fs_get_meta_folio(sbi, start++);
	if (IS_ERR(folio))
		return PTR_ERR(folio);
	kaddr = folio_address(folio);

	/* Step 1: restore nat cache */
	seg_i = cpu_array->array + CURSEG_HOT_DATA;
	memcpy(seg_i->journal, kaddr, sbi->sum_journal_size);

	/* Step 2: restore sit cache */
	seg_i = cpu_array->array + CURSEG_COLD_DATA;
	memcpy(seg_i->journal, kaddr + sbi->sum_journal_size, sbi->sum_journal_size);
	offset = 2 * sbi->sum_journal_size;

	/* Step 3: restore summary entries */
	for (i = CURSEG_HOT_DATA; i <= CURSEG_COLD_DATA; i++) {
		unsigned short blk_off;
		unsigned int segno;

		seg_i = cpu_array->array + i;
		segno = le32_to_cpu(ckpt->cur_datas_segno[cpu][i]);
		blk_off = le16_to_cpu(ckpt->cur_datas_blkoff[cpu][i]);
		seg_i->next_segno = segno;
		reset_curseg(sbi, i, 0, cpu, cpu_array);
		seg_i->alloc_type = ckpt->alloc_type[i];
		seg_i->next_blkoff = blk_off;

		if (seg_i->alloc_type == SSR)
			blk_off = BLKS_PER_SEG(sbi);

		for (j = 0; j < blk_off; j++) {
			struct f2fs_summary *s;

			s = (struct f2fs_summary *)(kaddr + offset);
			sum_entries(seg_i->sum_blk)[j] = *s;
			offset += SUMMARY_SIZE;
			if (offset + SUMMARY_SIZE <= sbi->blocksize -
						SUM_FOOTER_SIZE)
				continue;

			f2fs_folio_put(folio, true);

			/* never read into the next core's summary area */
			if (start >= end) {
				f2fs_err(sbi, "cpu %d: compacted summaries exceed %d blocks",
					 cpu, blk_cnt);
				return -EFSCORRUPTED;
			}
			folio = f2fs_get_meta_folio(sbi, start++);
			if (IS_ERR(folio))
				return PTR_ERR(folio);
			kaddr = folio_address(folio);
			offset = 0;
		}
	}
	f2fs_folio_put(folio, true);
	return 0;
}

static int percore_read_normal_summaries(struct f2fs_sb_info *sbi, int type,
			int cpu, struct curseg_info_array *cpu_array, int max_npages)
{
	struct f2fs_checkpoint *ckpt = F2FS_CKPT(sbi);
	struct f2fs_summary_block *sum;
	struct curseg_info *curseg;
	struct folio *new;
	unsigned short blk_off;
	unsigned int segno = 0;
	block_t blk_addr = 0;
	long num_cpus = num_online_cpus();
	int err = 0;

	/* get segment number and block addr */
	if (IS_DATASEG(type)) {
		segno = le32_to_cpu(ckpt->cur_datas_segno[cpu][type]);
		blk_off = le16_to_cpu(ckpt->cur_datas_blkoff[cpu][type -
							CURSEG_HOT_DATA]);
		if (__exist_node_summaries(sbi))
			blk_addr = sum_blk_addr(sbi, NR_CURSEG_PERSIST_TYPE, type,
						num_cpus, cpu, max_npages);
		else
			blk_addr = sum_blk_addr(sbi, NR_CURSEG_DATA_TYPE, type,
						num_cpus, cpu, max_npages);
	} else {
		segno = le32_to_cpu(ckpt->cur_nodes_segno[cpu][type -
							CURSEG_HOT_NODE]);
		blk_off = le16_to_cpu(ckpt->cur_nodes_blkoff[cpu][type -
							CURSEG_HOT_NODE]);
		if (__exist_node_summaries(sbi))
			blk_addr = sum_blk_addr(sbi, NR_CURSEG_NODE_TYPE,
						type - CURSEG_HOT_NODE,
						num_cpus, cpu, max_npages);
		else
			blk_addr = GET_SUM_BLOCK(sbi, segno);
	}

	new = f2fs_get_meta_folio(sbi, blk_addr);
	if (IS_ERR(new))
		return PTR_ERR(new);
	sum = folio_address(new);

	if (IS_NODESEG(type)) {
		if (__exist_node_summaries(sbi)) {
			struct f2fs_summary *ns = sum_entries(sum);
			int i;

			for (i = 0; i < BLKS_PER_SEG(sbi); i++, ns++) {
				ns->version = 0;
				ns->ofs_in_node = 0;
			}
		} else {
			err = f2fs_restore_node_summary(sbi, segno, sum);
			if (err)
				goto out;
		}
	}

	/* set uncompleted segment to curseg */
	curseg = cpu_array->array + type;
	mutex_lock(&curseg->curseg_mutex);

	/* update journal info */
	down_write(&curseg->journal_rwsem);
	memcpy(curseg->journal, sum_journal(sbi, sum), sbi->sum_journal_size);
	up_write(&curseg->journal_rwsem);

	memcpy(sum_entries(curseg->sum_blk), sum_entries(sum),
			sbi->sum_entry_size);
	memcpy(sum_footer(sbi, curseg->sum_blk), sum_footer(sbi, sum),
			SUM_FOOTER_SIZE);
	curseg->next_segno = segno;
	reset_curseg(sbi, type, 0, cpu, cpu_array);
	curseg->alloc_type = ckpt->alloc_type[type];
	curseg->next_blkoff = blk_off;
	mutex_unlock(&curseg->curseg_mutex);
out:
	f2fs_folio_put(new, true);
	return err;
}

/*
 * DeLFS: max_npages is the # of data summary blocks every core reserves in
 * the cp pack: max over cores of compacted pages, or NR_CURSEG_DATA_TYPE.
 */
static int get_max_sum_npages(struct f2fs_sb_info *sbi)
{
	int cpu, npages, max_npages = 0;

	if (!is_set_ckpt_flags(sbi, CP_COMPACT_SUM_FLAG))
		return NR_CURSEG_DATA_TYPE;

	for_each_online_cpu(cpu) {
		npages = percore_f2fs_npages_for_summary_flush(sbi, true, cpu);
		if (npages > max_npages)
			max_npages = npages;
	}
	return max_npages;
}

static int restore_curseg_summaries(struct f2fs_sb_info *sbi)
{
	long num_cpus = num_online_cpus();
	int max_npages = get_max_sum_npages(sbi);
	int type, err, cpu;

	for_each_online_cpu(cpu) {
		struct curseg_info_array *cpu_array = WRAP_CURSEG_I(sbi, cpu);
		struct f2fs_journal *sit_j, *nat_j;

		type = CURSEG_HOT_DATA;

		if (is_set_ckpt_flags(sbi, CP_COMPACT_SUM_FLAG)) {
			if (max_npages >= 2)
				f2fs_ra_meta_pages(sbi, start_sum_block(sbi) +
					cpu * percore_sum_step(sbi, max_npages),
					max_npages, META_CP, true);

			/* restore for compacted data summary */
			err = percore_read_compacted_summaries(sbi, max_npages,
						cpu, cpu_array, max_npages);
			if (err)
				return err;
			type = CURSEG_HOT_NODE;
		}

		if (__exist_node_summaries(sbi))
			f2fs_ra_meta_pages(sbi,
				sum_blk_addr(sbi, NR_CURSEG_PERSIST_TYPE, type,
						num_cpus, cpu, max_npages),
				NR_CURSEG_PERSIST_TYPE - type, META_CP, true);

		for (; type <= CURSEG_COLD_NODE; type++) {
			err = percore_read_normal_summaries(sbi, type, cpu,
							cpu_array, max_npages);
			if (err)
				return err;
		}

		/* sanity check for summary blocks */
		sit_j = (cpu_array->array + CURSEG_COLD_DATA)->journal;
		nat_j = (cpu_array->array + CURSEG_HOT_DATA)->journal;
		if (nats_in_cursum(nat_j) > sbi->nat_journal_entries ||
				sits_in_cursum(sit_j) > sbi->sit_journal_entries) {
			f2fs_err(sbi, "cpu %d: invalid journal entries nats %u sits %u",
				 cpu, nats_in_cursum(nat_j), sits_in_cursum(sit_j));
			return -EINVAL;
		}
	}

	return 0;
}

/*
 * DeLFS: write @cpu's compacted data summaries into exactly @blk_cnt
 * (= max_npages) blocks. All blocks are grabbed and zeroed up front so that
 * every core's summary area has a fixed size.
 */
static void write_compacted_summaries(struct f2fs_sb_info *sbi, block_t blkaddr,
			int blk_cnt, int cpu, struct curseg_info_array *cpu_array)
{
	struct folio *folio[NR_CURSEG_DATA_TYPE];
	unsigned char *kaddr;
	struct f2fs_summary *summary;
	struct curseg_info *seg_i;
	int written_size = 0;
	int i, j, cur_idx = 0;

	f2fs_bug_on(sbi, blk_cnt < 1 || blk_cnt > NR_CURSEG_DATA_TYPE);

	for (i = 0; i < blk_cnt; i++) {
		folio[i] = f2fs_grab_meta_folio(sbi, blkaddr + i);
		memset(folio_address(folio[i]), 0, PAGE_SIZE);
	}
	kaddr = folio_address(folio[cur_idx]);

	/* Step 1: write nat cache */
	seg_i = cpu_array->array + CURSEG_HOT_DATA;
	memcpy(kaddr, seg_i->journal, sbi->sum_journal_size);
	written_size += sbi->sum_journal_size;

	/* Step 2: write sit cache */
	seg_i = cpu_array->array + CURSEG_COLD_DATA;
	memcpy(kaddr + written_size, seg_i->journal, sbi->sum_journal_size);
	written_size += sbi->sum_journal_size;

	/* Step 3: write summary entries */
	for (i = CURSEG_HOT_DATA; i <= CURSEG_COLD_DATA; i++) {
		seg_i = cpu_array->array + i;
		for (j = 0; j < f2fs_curseg_valid_blocks(sbi, i, cpu); j++) {
			if (!kaddr) {
				cur_idx++;
				f2fs_bug_on(sbi, cur_idx >= blk_cnt);
				kaddr = folio_address(folio[cur_idx]);
				written_size = 0;
			}
			summary = (struct f2fs_summary *)(kaddr + written_size);
			*summary = sum_entries(seg_i->sum_blk)[j];
			written_size += SUMMARY_SIZE;

			if (written_size + SUMMARY_SIZE <= sbi->blocksize -
							SUM_FOOTER_SIZE)
				continue;

			kaddr = NULL;
		}
	}

	for (i = 0; i < blk_cnt; i++) {
		folio_mark_dirty(folio[i]);
		f2fs_folio_put(folio[i], true);
	}
}

static void write_normal_summaries(struct f2fs_sb_info *sbi,
		block_t blkaddr, int type, struct curseg_info_array *cpu_array)
{
	int i, end;

	if (IS_DATASEG(type))
		end = type + NR_CURSEG_DATA_TYPE;
	else
		end = type + NR_CURSEG_NODE_TYPE;

	for (i = type; i < end; i++)
		write_current_sum_page(sbi, i, blkaddr + (i - type), cpu_array);
}

void f2fs_write_data_summaries(struct f2fs_sb_info *sbi, block_t start_blk,
		int blk_cnt, int cpu, struct curseg_info_array *cpu_array)
{
	if (is_set_ckpt_flags(sbi, CP_COMPACT_SUM_FLAG))
		write_compacted_summaries(sbi, start_blk, blk_cnt, cpu, cpu_array);
	else
		write_normal_summaries(sbi, start_blk, CURSEG_HOT_DATA, cpu_array);
}

void f2fs_write_node_summaries(struct f2fs_sb_info *sbi, block_t start_blk,
		int cpu, struct curseg_info_array *cpu_array)
{
	write_normal_summaries(sbi, start_blk, CURSEG_HOT_NODE, cpu_array);
}

int f2fs_lookup_journal_in_cursum(struct f2fs_sb_info *sbi,
			struct f2fs_journal *journal, int type,
			unsigned int val, int alloc)
{
	int i;

	if (type == NAT_JOURNAL) {
		for (i = 0; i < nats_in_cursum(journal); i++) {
			if (le32_to_cpu(nid_in_journal(journal, i)) == val)
				return i;
		}
		if (alloc && __has_cursum_space(sbi, journal, 1, NAT_JOURNAL))
			return update_nats_in_cursum(journal, 1);
	} else if (type == SIT_JOURNAL) {
		for (i = 0; i < sits_in_cursum(journal); i++)
			if (le32_to_cpu(segno_in_journal(journal, i)) == val)
				return i;
		if (alloc && __has_cursum_space(sbi, journal, 1, SIT_JOURNAL))
			return update_sits_in_cursum(journal, 1);
	}
	return -1;
}

static struct folio *get_current_sit_folio(struct f2fs_sb_info *sbi,
					unsigned int segno)
{
	return f2fs_get_meta_folio(sbi, current_sit_addr(sbi, segno));
}

static struct folio *get_next_sit_folio(struct f2fs_sb_info *sbi,
					unsigned int start)
{
	struct sit_info *sit_i = SIT_I(sbi);
	struct folio *folio;
	pgoff_t src_off, dst_off;

	src_off = current_sit_addr(sbi, start);
	dst_off = next_sit_addr(sbi, src_off);

	folio = f2fs_grab_meta_folio(sbi, dst_off);
	seg_info_to_sit_folio(sbi, folio, start);

	folio_mark_dirty(folio);
	set_to_next_sit(sit_i, start);

	return folio;
}

static struct sit_entry_set *grab_sit_entry_set(void)
{
	struct sit_entry_set *ses =
			f2fs_kmem_cache_alloc(sit_entry_set_slab,
						GFP_NOFS, true, NULL);

	ses->entry_cnt = 0;
	INIT_LIST_HEAD(&ses->set_list);
	return ses;
}

static void release_sit_entry_set(struct sit_entry_set *ses)
{
	list_del(&ses->set_list);
	kmem_cache_free(sit_entry_set_slab, ses);
}

static void adjust_sit_entry_set(struct sit_entry_set *ses,
						struct list_head *head)
{
	struct sit_entry_set *next = ses;

	if (list_is_last(&ses->set_list, head))
		return;

	list_for_each_entry_continue(next, head, set_list)
		if (ses->entry_cnt <= next->entry_cnt) {
			list_move_tail(&ses->set_list, &next->set_list);
			return;
		}

	list_move_tail(&ses->set_list, head);
}

static void add_sit_entry(unsigned int segno, struct list_head *head)
{
	struct sit_entry_set *ses;
	unsigned int start_segno = START_SEGNO(segno);

	list_for_each_entry(ses, head, set_list) {
		if (ses->start_segno == start_segno) {
			ses->entry_cnt++;
			adjust_sit_entry_set(ses, head);
			return;
		}
	}

	ses = grab_sit_entry_set();

	ses->start_segno = start_segno;
	ses->entry_cnt++;
	list_add(&ses->set_list, head);
}

static void add_sits_in_set(struct f2fs_sb_info *sbi)
{
	struct f2fs_sm_info *sm_info = SM_I(sbi);
	struct list_head *set_list = &sm_info->sit_entry_set;
	unsigned long *bitmap = SIT_I(sbi)->dirty_sentries_bitmap;
	unsigned int segno;

	for_each_set_bit(segno, bitmap, MAIN_SEGS(sbi))
		add_sit_entry(segno, set_list);
}

static void remove_sits_in_journal(struct f2fs_sb_info *sbi, int cpu)
{
	int i;

	struct curseg_info_array *cpu_array = WRAP_CURSEG_I(sbi, cpu);
	struct curseg_info *curseg = cpu_array->array + CURSEG_COLD_DATA;
	struct f2fs_journal *journal = curseg->journal;

	down_write(&curseg->journal_rwsem);
	for (i = 0; i < sits_in_cursum(journal); i++) {
		unsigned int segno;
		bool dirtied;

		segno = le32_to_cpu(segno_in_journal(journal, i));
		dirtied = __mark_sit_entry_dirty(sbi, segno);

		if (!dirtied)
			add_sit_entry(segno, &SM_I(sbi)->sit_entry_set);
	}
	update_sits_in_cursum(journal, -i);
	up_write(&curseg->journal_rwsem);
}

/*
 * CP calls this function, which flushes SIT entries including sit_journal,
 * and moves prefree segs to free segs.
 */
void f2fs_flush_sit_entries(struct f2fs_sb_info *sbi, struct cp_control *cpc)
{
	struct sit_info *sit_i = SIT_I(sbi);
	unsigned long *bitmap = sit_i->dirty_sentries_bitmap;
	int i, cpu, sentry_cpu, max_cpu = num_online_cpus();
	struct curseg_info_array *cpu_array;
	struct curseg_info *curseg;
	struct f2fs_journal *journal;

	struct sit_entry_set *ses, *tmp;
	struct list_head *head = &SM_I(sbi)->sit_entry_set;
	struct seg_entry *se;
	bool to_journal[max_cpu];
	for (i = 0; i < max_cpu; i++)
		to_journal[i] = !is_sbi_flag_set(sbi, SBI_IS_RESIZEFS);

	struct rw_semaphore *se_lock;
	for_each_online_cpu(sentry_cpu) {
		se_lock = per_cpu_ptr(sit_i->percore_sentry_lock, sentry_cpu);
		down_write(se_lock);
	}

	if (!atomic_read(&sit_i->dirty_sentries)) {
		goto out;
	}

	/*
	 * add and account sit entries of dirty bitmap in sit entry
	 * set temporarily
	 */
	add_sits_in_set(sbi);

	/*
	 * if there are no enough space in journal to store dirty sit
	 * entries, remove all entries from journal and add and account
	 * them in sit entry set.
	 */
	/* DeLFS: each core has its own SIT journal (its cold data curseg) */
	for_each_online_cpu(cpu) {
		cpu_array = WRAP_CURSEG_I(sbi, cpu);
		curseg = cpu_array->array + CURSEG_COLD_DATA;
		journal = curseg->journal;
		if (!__has_cursum_space(sbi, journal,
				atomic_read(&sit_i->dirty_sentries), SIT_JOURNAL) ||
							!to_journal[cpu]) {
			remove_sits_in_journal(sbi, cpu);
		}
	}

	/*
	 * there are two steps to flush sit entries:
	 * #1, flush sit entries to journal in current cold data summary block.
	 * #2, flush sit entries to sit page.
	 */
	list_for_each_entry_safe(ses, tmp, head, set_list) {
		struct folio *folio = NULL;
		struct f2fs_sit_block *raw_sit = NULL;
		unsigned int start_segno = ses->start_segno;
		unsigned int end = min(start_segno + SIT_ENTRY_PER_BLOCK,
						(unsigned long)MAIN_SEGS(sbi));
		unsigned int segno = start_segno;
		int sit_blks_per_core = sit_i->sit_blocks / max_cpu;

		int journal_cpu = segno_to_sentry_cpu(sbi, segno);

		cpu_array = WRAP_CURSEG_I(sbi, journal_cpu);
		curseg = cpu_array->array + CURSEG_COLD_DATA;
		journal = curseg->journal;

		if (to_journal[journal_cpu] &&
			!__has_cursum_space(sbi, journal, ses->entry_cnt,
				SIT_JOURNAL))
			to_journal[journal_cpu] = false;

		if (to_journal[journal_cpu]) {
			down_write(&curseg->journal_rwsem);
		} else {
			folio = get_next_sit_folio(sbi, start_segno);
			raw_sit = folio_address(folio);
		}


		/* flush dirty sit entries in region of current sit set */
		for_each_set_bit_from(segno, bitmap, end) {
			int offset, sit_offset;

			se = get_seg_entry(sbi, segno);

			/* add discard candidates */
			if (!(cpc->reason & CP_DISCARD)) {
				cpc->trim_start = segno;
				add_discard_addrs(sbi, cpc, false);
			}

			if (to_journal[journal_cpu]) {
				offset = f2fs_lookup_journal_in_cursum(sbi, journal,
							SIT_JOURNAL, segno, 1);
				f2fs_bug_on(sbi, offset < 0);
				segno_in_journal(journal, offset) =
							cpu_to_le32(segno);
				seg_info_to_raw_sit(se,
					&sit_in_journal(journal, offset));
				check_block_count(sbi, segno,
					&sit_in_journal(journal, offset));
			} else {
				sit_offset = SIT_ENTRY_OFFSET(sit_i, segno);
				seg_info_to_raw_sit(se,
						&raw_sit->entries[sit_offset]);
				check_block_count(sbi, segno,
						&raw_sit->entries[sit_offset]);
			}

			/* update ckpt_valid_block */
			if (__is_large_section(sbi))
				set_ckpt_valid_blocks(sbi, segno);

			__clear_bit(segno, bitmap);
			atomic_sub(1, &sit_i->dirty_sentries);
			ses->entry_cnt--;
		}

		if (to_journal[journal_cpu])
			up_write(&curseg->journal_rwsem);
		else
			f2fs_folio_put(folio, true);

		f2fs_bug_on(sbi, ses->entry_cnt);
		release_sit_entry_set(ses);
	}

	f2fs_bug_on(sbi, !list_empty(head));
out:
	if (cpc->reason & CP_DISCARD) {
		__u64 trim_start = cpc->trim_start;

		for (; cpc->trim_start <= cpc->trim_end; cpc->trim_start++) {
			add_discard_addrs(sbi, cpc, false);
		}

		cpc->trim_start = trim_start;
	}
	for_each_online_cpu(sentry_cpu) {
		se_lock = per_cpu_ptr(sit_i->percore_sentry_lock, max_cpu-1-sentry_cpu);
		up_write(se_lock);
	}

	set_prefree_as_free_segments(sbi);
}

static int build_sit_info(struct f2fs_sb_info *sbi)
{
	struct f2fs_super_block *raw_super = F2FS_RAW_SUPER(sbi);
	struct sit_info *sit_i;
	unsigned int sit_segs, start;
	char *src_bitmap, *bitmap;
	unsigned int bitmap_size, main_bitmap_size, sit_bitmap_size;
	unsigned int discard_map = f2fs_block_unit_discard(sbi) ? 1 : 0;
	struct rw_semaphore *lock;
	int sentry_cpu;

	/* allocate memory for SIT information */
	sit_i = f2fs_kzalloc(sbi, sizeof(struct sit_info), GFP_KERNEL);
	if (!sit_i)
		return -ENOMEM;

	SM_I(sbi)->sit_info = sit_i;

	sit_i->sentries =
		f2fs_kvzalloc(sbi, array_size(sizeof(struct seg_entry),
					      MAIN_SEGS(sbi)),
			      GFP_KERNEL);
	if (!sit_i->sentries)
		return -ENOMEM;

	main_bitmap_size = f2fs_bitmap_size(MAIN_SEGS(sbi));
	sit_i->dirty_sentries_bitmap = f2fs_kvzalloc(sbi, main_bitmap_size,
								GFP_KERNEL);
	if (!sit_i->dirty_sentries_bitmap)
		return -ENOMEM;

	bitmap_size = MAIN_SEGS(sbi) * SIT_VBLOCK_MAP_SIZE * (2 + discard_map);
	sit_i->bitmap = f2fs_kvzalloc(sbi, bitmap_size, GFP_KERNEL);
	if (!sit_i->bitmap)
		return -ENOMEM;

	bitmap = sit_i->bitmap;

	for (start = 0; start < MAIN_SEGS(sbi); start++) {
		sit_i->sentries[start].cur_valid_map = bitmap;
		bitmap += SIT_VBLOCK_MAP_SIZE;

		sit_i->sentries[start].ckpt_valid_map = bitmap;
		bitmap += SIT_VBLOCK_MAP_SIZE;

		if (discard_map) {
			sit_i->sentries[start].discard_map = bitmap;
			bitmap += SIT_VBLOCK_MAP_SIZE;
		}

		atomic_set(&sit_i->sentries[start].curseg_cpu, -1);
	        atomic_set(&sit_i->sentries[start].curseg_refs, 0);
	}

	sit_i->tmp_map = f2fs_kzalloc(sbi, SIT_VBLOCK_MAP_SIZE, GFP_KERNEL);
	if (!sit_i->tmp_map)
		return -ENOMEM;

	sit_i->cursec_refs =
		f2fs_kvzalloc(sbi,
			      array_size(MAIN_SECS(sbi), sizeof(atomic_t)),
			      GFP_KERNEL);
	if (!sit_i->cursec_refs)
		return -ENOMEM;

	for (start = 0; start < MAIN_SECS(sbi); start++)
		atomic_set(&sit_i->cursec_refs[start], 0);

	if (__is_large_section(sbi)) {
		sit_i->sec_entries =
			f2fs_kvzalloc(sbi, array_size(sizeof(struct sec_entry),
						      MAIN_SECS(sbi)),
				      GFP_KERNEL);
		if (!sit_i->sec_entries)
			return -ENOMEM;
	}

	/* get information related with SIT */
	sit_segs = le32_to_cpu(raw_super->segment_count_sit) >> 1;

	/* setup SIT bitmap from ckeckpoint pack */
	sit_bitmap_size = __bitmap_size(sbi, SIT_BITMAP);
	src_bitmap = __bitmap_ptr(sbi, SIT_BITMAP);

	sit_i->sit_bitmap = kmemdup(src_bitmap, sit_bitmap_size, GFP_KERNEL);
	if (!sit_i->sit_bitmap)
		return -ENOMEM;

#ifdef CONFIG_F2FS_CHECK_FS
	sit_i->invalid_segmap = f2fs_kvzalloc(sbi,
					main_bitmap_size, GFP_KERNEL);
	if (!sit_i->invalid_segmap)
		return -ENOMEM;
#endif

	sit_i->sit_base_addr = le32_to_cpu(raw_super->sit_blkaddr);
	sit_i->sit_blocks = SEGS_TO_BLKS(sbi, sit_segs);
	atomic64_set(&sit_i->written_valid_blocks, 0);	/* DeLFS: atomic64 */
	sit_i->bitmap_size = sit_bitmap_size;
	atomic_set(&sit_i->dirty_sentries, 0);
	sit_i->sents_per_block = SIT_ENTRY_PER_BLOCK;
	sit_i->elapsed_time = le64_to_cpu(sbi->ckpt->elapsed_time);
	sit_i->mounted_time = ktime_get_boottime_seconds();
        sit_i->percore_sentry_lock = alloc_percpu(struct rw_semaphore);
        if (!sit_i->percore_sentry_lock)
                return -ENOMEM;

        for_each_online_cpu(sentry_cpu) {
                lock = per_cpu_ptr(sit_i->percore_sentry_lock, sentry_cpu);
                init_rwsem(lock);
        }
	return 0;
}

static int build_free_segmap(struct f2fs_sb_info *sbi)
{
	struct free_segmap_info *free_i;
	unsigned int bitmap_size, sec_bitmap_size;

	/* allocate memory for free segmap information */
	free_i = f2fs_kzalloc(sbi, sizeof(struct free_segmap_info), GFP_KERNEL);
	if (!free_i)
		return -ENOMEM;

	SM_I(sbi)->free_info = free_i;

	bitmap_size = f2fs_bitmap_size(MAIN_SEGS(sbi));
	free_i->free_segmap = f2fs_kvmalloc(sbi, bitmap_size, GFP_KERNEL);
	if (!free_i->free_segmap)
		return -ENOMEM;

	sec_bitmap_size = f2fs_bitmap_size(MAIN_SECS(sbi));
	free_i->free_secmap = f2fs_kvmalloc(sbi, sec_bitmap_size, GFP_KERNEL);
	if (!free_i->free_secmap)
		return -ENOMEM;

	/* set all segments as dirty temporarily */
	memset(free_i->free_segmap, 0xff, bitmap_size);
	memset(free_i->free_secmap, 0xff, sec_bitmap_size);

	/* init free segmap information */
	free_i->start_segno = GET_SEGNO_FROM_SEG0(sbi, MAIN_BLKADDR(sbi));
	free_i->free_segments = 0;
	free_i->free_sections = 0;
	spin_lock_init(&free_i->segmap_lock);
	return 0;
}

/*
 * DeLFS: every online core owns NR_CURSEG_TYPE cursegs. The global
 * SM_I(sbi)->curseg_array is no longer allocated (DeLFS doesn't either);
 * on failure, partially allocated arrays are freed by destroy_curseg().
 */
static int build_curseg(struct f2fs_sb_info *sbi)
{
	struct curseg_info_array __percpu *wrap_array;
	int i, cpu;

	if (num_online_cpus() > MAX_CORE_COUNT) {
		f2fs_err(sbi, "too many online cpus: %u > %d",
			 num_online_cpus(), MAX_CORE_COUNT);
		return -EINVAL;
	}

	wrap_array = alloc_percpu(struct curseg_info_array);
	if (!wrap_array)
		return -ENOMEM;

	SM_I(sbi)->wrap_curseg_array = wrap_array;

	for_each_online_cpu(cpu) {
		struct curseg_info_array *cpu_array = WRAP_CURSEG_I(sbi, cpu);
		struct curseg_info *array;

		array = f2fs_kzalloc(sbi, array_size(NR_CURSEG_TYPE,
					sizeof(*array)), GFP_KERNEL);
		if (!array)
			return -ENOMEM;
		cpu_array->array = array;

		for (i = 0; i < NO_CHECK_TYPE; i++) {
			mutex_init(&array[i].curseg_mutex);
			array[i].sum_blk = f2fs_kzalloc(sbi, sbi->sum_blocksize,
					GFP_KERNEL);
			if (!array[i].sum_blk)
				return -ENOMEM;
			init_rwsem(&array[i].journal_rwsem);
			array[i].journal = f2fs_kzalloc(sbi,
					sbi->sum_journal_size, GFP_KERNEL);
			if (!array[i].journal)
				return -ENOMEM;
			array[i].seg_type = log_type_to_seg_type(i);
			reset_curseg_fields(&array[i]);
			array[i].next_blkoff = 0;
		}
	}
	return restore_curseg_summaries(sbi);
}

static int build_sit_entries(struct f2fs_sb_info *sbi)
{
	struct sit_info *sit_i = SIT_I(sbi);
	struct seg_entry *se;
	struct f2fs_sit_entry sit;
	int sit_blk_cnt = SIT_BLK_CNT(sbi);
	unsigned int i, start, end, cpu;
	unsigned int readed, start_blk = 0;
	int err = 0;
	block_t sit_valid_blocks[2] = {0, 0};

	struct curseg_info_array *cpu_array;

	do {
		readed = f2fs_ra_meta_pages(sbi, start_blk, BIO_MAX_VECS,
							META_SIT, true);

		start = start_blk * sit_i->sents_per_block;
		end = (start_blk + readed) * sit_i->sents_per_block; /* (0 + 19) * 55*/

		for (; start < end && start < MAIN_SEGS(sbi); start++) {
			struct f2fs_sit_block *sit_blk;
			struct folio *folio;

			se = &sit_i->sentries[start];
			folio = get_current_sit_folio(sbi, start);
			if (IS_ERR(folio))
				return PTR_ERR(folio);
			sit_blk = folio_address(folio);
			sit = sit_blk->entries[SIT_ENTRY_OFFSET(sit_i, start)];
			f2fs_folio_put(folio, true);

			err = my_check_block_count(sbi, start, &sit);
			if (err)
				return err;
			seg_info_from_raw_sit(se, &sit);

			if (se->type >= NR_PERSISTENT_LOG) {
				f2fs_err(sbi, "Invalid segment type: %u, segno: %u",
							se->type, start);
				f2fs_handle_error(sbi,
						ERROR_INCONSISTENT_SUM_TYPE);
				return -EFSCORRUPTED;
			}

			sit_valid_blocks[SE_PAGETYPE(se)] += se->valid_blocks;

			if (!f2fs_block_unit_discard(sbi))
				goto init_discard_map_done;

			/* build discard map only one time */
			if (is_set_ckpt_flags(sbi, CP_TRIMMED_FLAG)) {
				memset(se->discard_map, 0xff,
						SIT_VBLOCK_MAP_SIZE);
				goto init_discard_map_done;
			}
			memcpy(se->discard_map, se->cur_valid_map,
						SIT_VBLOCK_MAP_SIZE);
			/* DeLFS: discard_blks is atomic_t */
			atomic_add(BLKS_PER_SEG(sbi) - se->valid_blocks,
						&sbi->discard_blks);
init_discard_map_done:
			if (__is_large_section(sbi))
				get_sec_entry(sbi, start)->valid_blocks +=
							se->valid_blocks;
		}
		start_blk += readed;
	} while (start_blk < sit_blk_cnt);

	for_each_online_cpu(cpu) {
		cpu_array = WRAP_CURSEG_I(sbi, cpu);

		struct curseg_info *curseg = cpu_array->array + CURSEG_COLD_DATA;
		struct f2fs_journal *journal = curseg->journal;

		down_read(&curseg->journal_rwsem);
		for (i = 0; i < sits_in_cursum(journal); i++) {
			unsigned int old_valid_blocks;

			start = le32_to_cpu(segno_in_journal(journal, i));
			if (start >= MAIN_SEGS(sbi)) {
				f2fs_err(sbi, "Wrong journal entry on segno %u",
					 start);
				err = -EFSCORRUPTED;
				f2fs_handle_error(sbi, ERROR_CORRUPTED_JOURNAL);
				break;
			}

			se = &sit_i->sentries[start];
			sit = sit_in_journal(journal, i);

			old_valid_blocks = se->valid_blocks;

			sit_valid_blocks[SE_PAGETYPE(se)] -= old_valid_blocks;

			err = my_check_block_count(sbi, start, &sit);
			if (err)
				break;
			seg_info_from_raw_sit(se, &sit);

			if (se->type >= NR_PERSISTENT_LOG) {
				f2fs_err(sbi, "Invalid segment type: %u, segno: %u",
								se->type, start);
				err = -EFSCORRUPTED;
				f2fs_handle_error(sbi, ERROR_INCONSISTENT_SUM_TYPE);
				break;
			}

			sit_valid_blocks[SE_PAGETYPE(se)] += se->valid_blocks;

			if (f2fs_block_unit_discard(sbi)) {
				if (is_set_ckpt_flags(sbi, CP_TRIMMED_FLAG)) {
					memset(se->discard_map, 0xff, SIT_VBLOCK_MAP_SIZE);
				} else {
					memcpy(se->discard_map, se->cur_valid_map,
								SIT_VBLOCK_MAP_SIZE);
					atomic_add(old_valid_blocks, &sbi->discard_blks);
					atomic_sub(se->valid_blocks, &sbi->discard_blks);
				}
			}

			if (__is_large_section(sbi)) {
				get_sec_entry(sbi, start)->valid_blocks +=
								se->valid_blocks;
				get_sec_entry(sbi, start)->valid_blocks -=
								old_valid_blocks;
			}
		}
		up_read(&curseg->journal_rwsem);

		if (err)
			return err;
	}

	/* update ckpt_valid_block */
	if (__is_large_section(sbi)) {
		unsigned int segno;

		for (segno = 0; segno < MAIN_SEGS(sbi); segno += SEGS_PER_SEC(sbi))
			set_ckpt_valid_blocks(sbi, segno);
	}

	if (sit_valid_blocks[NODE] != valid_node_count(sbi)) {
		f2fs_err(sbi, "SIT is corrupted node# %u vs %u",
			 sit_valid_blocks[NODE], valid_node_count(sbi));
		f2fs_handle_error(sbi, ERROR_INCONSISTENT_NODE_COUNT);
		return -EFSCORRUPTED;
	}

	if (sit_valid_blocks[DATA] + sit_valid_blocks[NODE] >
				valid_user_blocks(sbi)) {
		f2fs_err(sbi, "SIT is corrupted data# %u %u vs %u",
			 sit_valid_blocks[DATA], sit_valid_blocks[NODE],
			 valid_user_blocks(sbi));
		f2fs_handle_error(sbi, ERROR_INCONSISTENT_BLOCK_COUNT);
		return -EFSCORRUPTED;
	}


	return 0;
}

/*
 * DeLFS: recompute curseg_refs/cursec_refs from the cursegs of every core,
 * after build_sit_entries() has (re)loaded the SIT.
 */
static void rebuild_curseg_membership(struct f2fs_sb_info *sbi)
{
	struct sit_info *sit_i = SIT_I(sbi);
	unsigned int segno, secno;
	int cpu, type;

	for (segno = 0; segno < MAIN_SEGS(sbi); segno++) {
		atomic_set(&sit_i->sentries[segno].curseg_refs, 0);
		atomic_set(&sit_i->sentries[segno].curseg_cpu, -1); /* debug only */
	}

	for (secno = 0; secno < MAIN_SECS(sbi); secno++)
		atomic_set(&sit_i->cursec_refs[secno], 0);

	for_each_online_cpu(cpu) {
		struct curseg_info_array *cpu_array = WRAP_CURSEG_I(sbi, cpu);

		for (type = CURSEG_HOT_DATA; type <= CURSEG_COLD_NODE; type++) {
			struct curseg_info *curseg = cpu_array->array + type;
			unsigned int csegno = READ_ONCE(curseg->segno);

			if (!curseg->inited || csegno == NULL_SEGNO)
				continue;

			f2fs_bug_on(sbi, csegno >= MAIN_SEGS(sbi));

			f2fs_curseg_get(sbi, csegno);
			atomic_set(&get_seg_entry(sbi, csegno)->curseg_cpu, cpu);
		}
	}
}

static void init_free_segmap(struct f2fs_sb_info *sbi)
{
	unsigned int start;
	int type, cpu;
	struct seg_entry *sentry;

	for (start = 0; start < MAIN_SEGS(sbi); start++) {
		if (f2fs_usable_blks_in_seg(sbi, start) == 0)
			continue;
		sentry = get_seg_entry(sbi, start);
		if (!sentry->valid_blocks)
			__set_free(sbi, start);
		else	/* DeLFS (B): written_valid_blocks is atomic64_t */
			atomic64_add(sentry->valid_blocks,
				     &SIT_I(sbi)->written_valid_blocks);
	}

	/* DeLFS: set use the current segments of every core */
	for_each_online_cpu(cpu) {
		struct curseg_info_array *cpu_array = WRAP_CURSEG_I(sbi, cpu);

		for (type = CURSEG_HOT_DATA; type <= CURSEG_COLD_NODE; type++) {
			struct curseg_info *curseg_t = cpu_array->array + type;

			if (curseg_t->segno == NULL_SEGNO)
				continue;

			f2fs_bug_on(sbi, curseg_t->segno >= MAIN_SEGS(sbi));
			__set_test_and_inuse(sbi, curseg_t->segno);
		}
	}
}

static void init_dirty_segmap(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	struct free_segmap_info *free_i = FREE_I(sbi);
	unsigned int segno = 0, offset = 0;
	block_t valid_blocks, usable_blks_in_seg;

	while (1) {
		/* find dirty segment based on free segmap */
		segno = find_next_inuse(free_i, MAIN_SEGS(sbi), offset);
		if (segno >= MAIN_SEGS(sbi))
			break;
		offset = segno + 1;
		valid_blocks = get_valid_blocks(sbi, segno, false);
		usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
		if (valid_blocks == usable_blks_in_seg || !valid_blocks)
			continue;
		if (valid_blocks > usable_blks_in_seg) {
			f2fs_bug_on(sbi, 1);
			continue;
		}

		int seglist_cpu = segno_to_sentry_cpu(sbi, segno);
		struct dirty_seg_percpu *p = per_cpu_ptr(dirty_i->pcpu, seglist_cpu);
		mutex_lock(&p->p_seglist_lock);
		__locate_dirty_segment(sbi, segno, DIRTY);
		mutex_unlock(&p->p_seglist_lock);
	}

	/*
	 * DeLFS: no dirty_secmap and no global seglist_lock (one segment per
	 * section, dirty lists per core), so the 7.2.8 large-section loop
	 * that filled dirty_secmap is dropped, as in DeLFS 6.6.8.
	 */
	if (!__is_large_section(sbi))
		return;
}

static int init_victim_secmap(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	unsigned int bitmap_size = f2fs_bitmap_size(MAIN_SECS(sbi));

	dirty_i->victim_secmap = f2fs_kvzalloc(sbi, bitmap_size, GFP_KERNEL);
	if (!dirty_i->victim_secmap)
		return -ENOMEM;

	dirty_i->pinned_secmap = f2fs_kvzalloc(sbi, bitmap_size, GFP_KERNEL);
	if (!dirty_i->pinned_secmap)
		return -ENOMEM;

	dirty_i->pinned_secmap_cnt = 0;
	dirty_i->enable_pin_section = true;
	return 0;
}

static int build_dirty_segmap(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i;
	unsigned int bitmap_size, i;

	/* allocate memory for dirty segments list information */
	dirty_i = f2fs_kzalloc(sbi, sizeof(struct dirty_seglist_info),
								GFP_KERNEL);
	if (!dirty_i)
		return -ENOMEM;

	SM_I(sbi)->dirty_info = dirty_i;

	bitmap_size = f2fs_bitmap_size(MAIN_SEGS(sbi));

	int cpu;

	dirty_i->pcpu = alloc_percpu(struct dirty_seg_percpu);
	if (!dirty_i->pcpu)
		return -ENOMEM;

	for_each_online_cpu(cpu) {
		struct dirty_seg_percpu *p = per_cpu_ptr(dirty_i->pcpu, cpu);

		mutex_init(&p->p_seglist_lock);

		for (i = 0; i < NR_DIRTY_TYPE; i++) {
			p->p_dirty_segmap[i] = f2fs_kvzalloc(sbi, bitmap_size, 
									GFP_KERNEL);
			if (!p->p_dirty_segmap[i])
				return -ENOMEM;
		}

		if (__is_large_section(sbi)) {
		}
	}


	init_dirty_segmap(sbi);
	return init_victim_secmap(sbi);
}

static int sanity_check_curseg(struct f2fs_sb_info *sbi)
{
	int i, cpu;

	/*
	 * In LFS/SSR curseg, .next_blkoff should point to an unused blkaddr;
	 * In LFS curseg, all blkaddr after .next_blkoff should be unused.
	 * DeLFS: check the cursegs of every core.
	 */
	for_each_online_cpu(cpu) {
	struct curseg_info_array *cpu_array = WRAP_CURSEG_I(sbi, cpu);

	for (i = 0; i < NR_PERSISTENT_LOG; i++) {
		struct curseg_info *curseg = cpu_array->array + i;
		struct seg_entry *se = get_seg_entry(sbi, curseg->segno);
		unsigned int blkofs = curseg->next_blkoff;

		if (f2fs_sb_has_readonly(sbi) &&
			i != CURSEG_HOT_DATA && i != CURSEG_HOT_NODE)
			continue;

		sanity_check_seg_type(sbi, curseg->seg_type);

		if (curseg->alloc_type != LFS && curseg->alloc_type != SSR) {
			f2fs_err(sbi,
				 "Current segment has invalid alloc_type:%d",
				 curseg->alloc_type);
			f2fs_handle_error(sbi, ERROR_INVALID_CURSEG);
			return -EFSCORRUPTED;
		}

		if (f2fs_test_bit(blkofs, se->cur_valid_map))
			goto out;

		if (curseg->alloc_type == SSR)
			continue;

		for (blkofs += 1; blkofs < BLKS_PER_SEG(sbi); blkofs++) {
			if (!f2fs_test_bit(blkofs, se->cur_valid_map))
				continue;
out:
			f2fs_err(sbi,
				 "Current segment's next free block offset is inconsistent with bitmap, cpu:%d, logtype:%u, segno:%u, type:%u, next_blkoff:%u, blkofs:%u",
				 cpu, i, curseg->segno, curseg->alloc_type,
				 curseg->next_blkoff, blkofs);
			f2fs_handle_error(sbi, ERROR_INVALID_CURSEG);
			return -EFSCORRUPTED;
		}
	}
	}
	return 0;
}

#ifdef CONFIG_BLK_DEV_ZONED
static int check_zone_write_pointer(struct f2fs_sb_info *sbi,
				    struct f2fs_dev_info *fdev,
				    struct blk_zone *zone)
{
	unsigned int zone_segno;
	block_t zone_block, valid_block_cnt;
	unsigned int log_sectors_per_block = sbi->log_blocksize - SECTOR_SHIFT;
	int ret;
	unsigned int nofs_flags;

	if (zone->type != BLK_ZONE_TYPE_SEQWRITE_REQ)
		return 0;

	zone_block = fdev->start_blk + (zone->start >> log_sectors_per_block);
	zone_segno = GET_SEGNO(sbi, zone_block);

	/*
	 * Skip check of zones cursegs point to, since
	 * fix_curseg_write_pointer() checks them.
	 */
	if (zone_segno >= MAIN_SEGS(sbi))
		return 0;

	/*
	 * Get # of valid block of the zone.
	 */
	valid_block_cnt = get_valid_blocks(sbi, zone_segno, true);
	if (is_cursec(sbi, GET_SEC_FROM_SEG(sbi, zone_segno))) {
		f2fs_notice(sbi, "Open zones: valid block[0x%x,0x%x] cond[%s]",
				zone_segno, valid_block_cnt,
				blk_zone_cond_str(zone->cond));
		return 0;
	}

	if ((!valid_block_cnt && zone->cond == BLK_ZONE_COND_EMPTY) ||
	    (valid_block_cnt && zone->cond == BLK_ZONE_COND_FULL))
		return 0;

	if (!valid_block_cnt) {
		f2fs_notice(sbi, "Zone without valid block has non-zero write "
			    "pointer. Reset the write pointer: cond[%s]",
			    blk_zone_cond_str(zone->cond));
		ret = __f2fs_issue_discard_zone(sbi, fdev->bdev, zone_block,
					zone->len >> log_sectors_per_block);
		if (ret)
			f2fs_err(sbi, "Discard zone failed: %s (errno=%d)",
				 fdev->path, ret);
		return ret;
	}

	/*
	 * If there are valid blocks and the write pointer doesn't match
	 * with them, we need to report the inconsistency and fill
	 * the zone till the end to close the zone. This inconsistency
	 * does not cause write error because the zone will not be
	 * selected for write operation until it get discarded.
	 */
	f2fs_notice(sbi, "Valid blocks are not aligned with write "
		    "pointer: valid block[0x%x,0x%x] cond[%s]",
		    zone_segno, valid_block_cnt, blk_zone_cond_str(zone->cond));

	nofs_flags = memalloc_nofs_save();
	ret = blkdev_zone_mgmt(fdev->bdev, REQ_OP_ZONE_FINISH,
				zone->start, zone->len);
	memalloc_nofs_restore(nofs_flags);
	if (ret == -EOPNOTSUPP) {
		ret = blkdev_issue_zeroout(fdev->bdev, zone->wp,
					zone->len - (zone->wp - zone->start),
					GFP_NOFS, 0);
		if (ret)
			f2fs_err(sbi, "Fill up zone failed: %s (errno=%d)",
					fdev->path, ret);
	} else if (ret) {
		f2fs_err(sbi, "Finishing zone failed: %s (errno=%d)",
				fdev->path, ret);
	}

	return ret;
}

static struct f2fs_dev_info *get_target_zoned_dev(struct f2fs_sb_info *sbi,
						  block_t zone_blkaddr)
{
	int i;

	for (i = 0; i < sbi->s_ndevs; i++) {
		if (!bdev_is_zoned(FDEV(i).bdev))
			continue;
		if (sbi->s_ndevs == 1 || (FDEV(i).start_blk <= zone_blkaddr &&
				zone_blkaddr <= FDEV(i).end_blk))
			return &FDEV(i);
	}

	return NULL;
}

static int report_one_zone_cb(struct blk_zone *zone, unsigned int idx,
			      void *data)
{
	memcpy(data, zone, sizeof(struct blk_zone));
	return 0;
}

/*
 * DeLFS: fix @cpu's curseg @type.
 * TODO(A): zoned devices are untested with per-core cursegs; DeLFS 6.6.8
 * only marked this path "need to check".
 */
static int do_fix_curseg_write_pointer(struct f2fs_sb_info *sbi, int type,
								int cpu)
{
	struct curseg_info *cs = WRAP_CURSEG_I(sbi, cpu)->array + type;
	struct f2fs_dev_info *zbd;
	struct blk_zone zone;
	unsigned int cs_section, wp_segno, wp_blkoff, wp_sector_off;
	block_t cs_zone_block, wp_block;
	unsigned int log_sectors_per_block = sbi->log_blocksize - SECTOR_SHIFT;
	sector_t zone_sector;
	int err;

	cs_section = GET_SEC_FROM_SEG(sbi, cs->segno);
	cs_zone_block = START_BLOCK(sbi, GET_SEG_FROM_SEC(sbi, cs_section));

	zbd = get_target_zoned_dev(sbi, cs_zone_block);
	if (!zbd)
		return 0;

	/* report zone for the sector the curseg points to */
	zone_sector = (sector_t)(cs_zone_block - zbd->start_blk)
		<< log_sectors_per_block;
	err = blkdev_report_zones(zbd->bdev, zone_sector, 1,
				  report_one_zone_cb, &zone);
	if (err != 1) {
		f2fs_err(sbi, "Report zone failed: %s errno=(%d)",
			 zbd->path, err);
		return err;
	}

	if (zone.type != BLK_ZONE_TYPE_SEQWRITE_REQ)
		return 0;

	/*
	 * When safely unmounted in the previous mount, we could use current
	 * segments. Otherwise, allocate new sections.
	 */
	if (is_set_ckpt_flags(sbi, CP_UMOUNT_FLAG)) {
		wp_block = zbd->start_blk + (zone.wp >> log_sectors_per_block);
		wp_segno = GET_SEGNO(sbi, wp_block);
		wp_blkoff = wp_block - START_BLOCK(sbi, wp_segno);
		wp_sector_off = zone.wp & GENMASK(log_sectors_per_block - 1, 0);

		if (cs->segno == wp_segno && cs->next_blkoff == wp_blkoff &&
				wp_sector_off == 0)
			return 0;

		f2fs_notice(sbi, "Unaligned curseg[%d] with write pointer: "
			    "curseg[0x%x,0x%x] wp[0x%x,0x%x]", type, cs->segno,
			    cs->next_blkoff, wp_segno, wp_blkoff);
	}

	/* Allocate a new section if it's not new. */
	if (cs->next_blkoff ||
	    cs->segno != GET_SEG_FROM_SEC(sbi, GET_ZONE_FROM_SEC(sbi, cs_section))) {
		unsigned int old_segno = cs->segno, old_blkoff = cs->next_blkoff;

		f2fs_down_read(&SM_I(sbi)->curseg_lock);
		/* DeLFS (B): no global sentry_lock; zoned only (rare path) */
		f2fs_down_write_all_sentry_locks(sbi);
		__allocate_new_segment_cpu(sbi, type, true, true, cpu);
		f2fs_up_write_all_sentry_locks(sbi);
		f2fs_up_read(&SM_I(sbi)->curseg_lock);
		f2fs_notice(sbi, "Assign new section to curseg[%d]: "
				"[0x%x,0x%x] -> [0x%x,0x%x]",
				type, old_segno, old_blkoff,
				cs->segno, cs->next_blkoff);
	}

	/* check consistency of the zone curseg pointed to */
	if (check_zone_write_pointer(sbi, zbd, &zone))
		return -EIO;

	/* check newly assigned zone */
	cs_section = GET_SEC_FROM_SEG(sbi, cs->segno);
	cs_zone_block = START_BLOCK(sbi, GET_SEG_FROM_SEC(sbi, cs_section));

	zbd = get_target_zoned_dev(sbi, cs_zone_block);
	if (!zbd)
		return 0;

	zone_sector = (sector_t)(cs_zone_block - zbd->start_blk)
		<< log_sectors_per_block;
	err = blkdev_report_zones(zbd->bdev, zone_sector, 1,
				  report_one_zone_cb, &zone);
	if (err != 1) {
		f2fs_err(sbi, "Report zone failed: %s errno=(%d)",
			 zbd->path, err);
		return err;
	}

	if (zone.type != BLK_ZONE_TYPE_SEQWRITE_REQ)
		return 0;

	if (zone.wp != zone.start) {
		f2fs_notice(sbi,
			    "New zone for curseg[%d] is not yet discarded. "
			    "Reset the zone: curseg[0x%x,0x%x]",
			    type, cs->segno, cs->next_blkoff);
		err = __f2fs_issue_discard_zone(sbi, zbd->bdev,	cs_zone_block,
					zone.len >> log_sectors_per_block);
		if (err) {
			f2fs_err(sbi, "Discard zone failed: %s (errno=%d)",
				 zbd->path, err);
			return err;
		}
	}

	return 0;
}

static int fix_curseg_write_pointer(struct f2fs_sb_info *sbi)
{
	int i, cpu, ret;

	for_each_online_cpu(cpu) {
		for (i = 0; i < NR_PERSISTENT_LOG; i++) {
			ret = do_fix_curseg_write_pointer(sbi, i, cpu);
			if (ret)
				return ret;
		}
	}

	return 0;
}

struct check_zone_write_pointer_args {
	struct f2fs_sb_info *sbi;
	struct f2fs_dev_info *fdev;
};

static int check_zone_write_pointer_cb(struct blk_zone *zone, unsigned int idx,
				      void *data)
{
	struct check_zone_write_pointer_args *args;

	args = (struct check_zone_write_pointer_args *)data;

	return check_zone_write_pointer(args->sbi, args->fdev, zone);
}

static int check_write_pointer(struct f2fs_sb_info *sbi)
{
	int i, ret;
	struct check_zone_write_pointer_args args;

	for (i = 0; i < sbi->s_ndevs; i++) {
		if (!bdev_is_zoned(FDEV(i).bdev))
			continue;

		args.sbi = sbi;
		args.fdev = &FDEV(i);
		ret = blkdev_report_zones(FDEV(i).bdev, 0, BLK_ALL_ZONES,
					  check_zone_write_pointer_cb, &args);
		if (ret < 0)
			return ret;
	}

	return 0;
}

int f2fs_check_and_fix_write_pointer(struct f2fs_sb_info *sbi)
{
	int ret;

	if (!f2fs_sb_has_blkzoned(sbi) || f2fs_readonly(sbi->sb) ||
	    f2fs_hw_is_readonly(sbi))
		return 0;

	f2fs_notice(sbi, "Checking entire write pointers");
	ret = fix_curseg_write_pointer(sbi);
	if (!ret)
		ret = check_write_pointer(sbi);
	return ret;
}

/*
 * Return the number of usable blocks in a segment. The number of blocks
 * returned is always equal to the number of blocks in a segment for
 * segments fully contained within a sequential zone capacity or a
 * conventional zone. For segments partially contained in a sequential
 * zone capacity, the number of usable blocks up to the zone capacity
 * is returned. 0 is returned in all other cases.
 */
static inline unsigned int f2fs_usable_zone_blks_in_seg(
			struct f2fs_sb_info *sbi, unsigned int segno)
{
	block_t seg_start, sec_start_blkaddr, sec_cap_blkaddr;
	unsigned int secno;

	if (!sbi->unusable_blocks_per_sec)
		return BLKS_PER_SEG(sbi);

	secno = GET_SEC_FROM_SEG(sbi, segno);
	seg_start = START_BLOCK(sbi, segno);
	sec_start_blkaddr = START_BLOCK(sbi, GET_SEG_FROM_SEC(sbi, secno));
	sec_cap_blkaddr = sec_start_blkaddr + CAP_BLKS_PER_SEC(sbi);

	/*
	 * If segment starts before zone capacity and spans beyond
	 * zone capacity, then usable blocks are from seg start to
	 * zone capacity. If the segment starts after the zone capacity,
	 * then there are no usable blocks.
	 */
	if (seg_start >= sec_cap_blkaddr)
		return 0;
	if (seg_start + BLKS_PER_SEG(sbi) > sec_cap_blkaddr)
		return sec_cap_blkaddr - seg_start;

	return BLKS_PER_SEG(sbi);
}
#else
int f2fs_check_and_fix_write_pointer(struct f2fs_sb_info *sbi)
{
	return 0;
}

static inline unsigned int f2fs_usable_zone_blks_in_seg(struct f2fs_sb_info *sbi,
							unsigned int segno)
{
	return 0;
}

#endif
unsigned int f2fs_usable_blks_in_seg(struct f2fs_sb_info *sbi,
					unsigned int segno)
{
	if (f2fs_sb_has_blkzoned(sbi))
		return f2fs_usable_zone_blks_in_seg(sbi, segno);

	return BLKS_PER_SEG(sbi);
}

unsigned int f2fs_usable_segs_in_sec(struct f2fs_sb_info *sbi)
{
	if (f2fs_sb_has_blkzoned(sbi))
		return CAP_SEGS_PER_SEC(sbi);

	return SEGS_PER_SEC(sbi);
}

unsigned long long f2fs_get_section_mtime(struct f2fs_sb_info *sbi,
	unsigned int segno)
{
	unsigned int usable_segs_per_sec = f2fs_usable_segs_in_sec(sbi);
	unsigned int secno = 0, start = 0;
	unsigned int total_valid_blocks = 0;
	unsigned long long mtime = 0;
	unsigned int i = 0;

	secno = GET_SEC_FROM_SEG(sbi, segno);
	start = GET_SEG_FROM_SEC(sbi, secno);

	if (!__is_large_section(sbi)) {
		mtime = get_seg_entry(sbi, start + i)->mtime;
		goto out;
	}

	for (i = 0; i < usable_segs_per_sec; i++) {
		/* for large section, only check the mtime of valid segments */
		struct seg_entry *se = get_seg_entry(sbi, start+i);

		mtime += se->mtime * se->valid_blocks;
		total_valid_blocks += se->valid_blocks;
	}

	if (total_valid_blocks == 0)
		return INVALID_MTIME;

	mtime = div_u64(mtime, total_valid_blocks);
out:
	if (unlikely(mtime == INVALID_MTIME))
		mtime -= 1;
	return mtime;
}

/*
 * Update min, max modified time for cost-benefit GC algorithm
 */
static void init_min_max_mtime(struct f2fs_sb_info *sbi)
{
	struct sit_info *sit_i = SIT_I(sbi);
	unsigned int segno;
	int sentry_cpu, max_cpu = num_online_cpus();
	for_each_online_cpu(sentry_cpu) {
		struct rw_semaphore *se_lock = per_cpu_ptr(sit_i->percore_sentry_lock, sentry_cpu);
		down_write(se_lock);
	}

	sit_i->min_mtime = ULLONG_MAX;

	for (segno = 0; segno < MAIN_SEGS(sbi); segno += SEGS_PER_SEC(sbi)) {
		unsigned long long mtime = 0;

		mtime = f2fs_get_section_mtime(sbi, segno);

		if (sit_i->min_mtime > mtime)
			sit_i->min_mtime = mtime;
	}
	sit_i->max_mtime = get_mtime(sbi, false);
	sit_i->dirty_max_mtime = 0;
	for_each_online_cpu(sentry_cpu) {
		struct rw_semaphore *se_lock = per_cpu_ptr(sit_i->percore_sentry_lock, max_cpu-1-sentry_cpu);
		up_write(se_lock);
	}

}

int f2fs_build_segment_manager(struct f2fs_sb_info *sbi)
{
	struct f2fs_super_block *raw_super = F2FS_RAW_SUPER(sbi);
	struct f2fs_checkpoint *ckpt = F2FS_CKPT(sbi);
	struct f2fs_sm_info *sm_info;
	int err;

	sm_info = f2fs_kzalloc(sbi, sizeof(struct f2fs_sm_info), GFP_KERNEL);
	if (!sm_info)
		return -ENOMEM;

	/* init sm info */
	sbi->sm_info = sm_info;
	sm_info->seg0_blkaddr = le32_to_cpu(raw_super->segment0_blkaddr);
	sm_info->main_blkaddr = le32_to_cpu(raw_super->main_blkaddr);
	sm_info->segment_count = le32_to_cpu(raw_super->segment_count);
	sm_info->reserved_segments = le32_to_cpu(ckpt->rsvd_segment_count);
	sm_info->ovp_segments = le32_to_cpu(ckpt->overprov_segment_count);
	sm_info->main_segments = le32_to_cpu(raw_super->segment_count_main);
	sm_info->ssa_blkaddr = le32_to_cpu(raw_super->ssa_blkaddr);
	sm_info->rec_prefree_segments = sm_info->main_segments *
					DEF_RECLAIM_PREFREE_SEGMENTS / 100;
	if (sm_info->rec_prefree_segments > DEF_MAX_RECLAIM_PREFREE_SEGMENTS)
		sm_info->rec_prefree_segments = DEF_MAX_RECLAIM_PREFREE_SEGMENTS;

	if (!f2fs_lfs_mode(sbi))
		sm_info->ipu_policy = BIT(F2FS_IPU_FSYNC);
	sm_info->min_ipu_util = DEF_MIN_IPU_UTIL;
	sm_info->min_fsync_blocks = DEF_MIN_FSYNC_BLOCKS;
	sm_info->min_seq_blocks = BLKS_PER_SEG(sbi);
	sm_info->min_hot_blocks = DEF_MIN_HOT_BLOCKS;
	sm_info->min_ssr_sections = reserved_sections(sbi);

	INIT_LIST_HEAD(&sm_info->sit_entry_set);

	init_f2fs_rwsem(&sm_info->curseg_lock);

	err = f2fs_create_flush_cmd_control(sbi);
	if (err)
		return err;

	err = create_discard_cmd_control(sbi);
	if (err)
		return err;

	err = build_sit_info(sbi);
	if (err)
		return err;
	err = build_free_segmap(sbi);
	if (err)
		return err;
	err = build_curseg(sbi);
	if (err)
		return err;

	/* reinit free segmap based on SIT */
	err = build_sit_entries(sbi);
	if (err)
		return err;

	/* DeLFS: rebuild curseg/cursec refs of every core's cursegs */
	rebuild_curseg_membership(sbi);

	init_free_segmap(sbi);
	err = build_dirty_segmap(sbi);
	if (err)
		return err;

	err = sanity_check_curseg(sbi);
	if (err)
		return err;

	init_min_max_mtime(sbi);
	return 0;
}

static void discard_dirty_segmap(struct f2fs_sb_info *sbi,
		enum dirty_type dirty_type)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);

	int seglist_cpu;
	struct dirty_seg_percpu *p;

	for_each_online_cpu(seglist_cpu) {
		p = per_cpu_ptr(dirty_i->pcpu, seglist_cpu);
		mutex_lock(&p->p_seglist_lock);
		kvfree(p->p_dirty_segmap[dirty_type]);
		p->p_nr_dirty[dirty_type] = 0;
		mutex_unlock(&p->p_seglist_lock);
	}
	atomic_set(&dirty_i->nr_dirty[dirty_type], 0);
}

static void destroy_victim_secmap(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);

	kvfree(dirty_i->pinned_secmap);
	kvfree(dirty_i->victim_secmap);
}

static void destroy_dirty_segmap(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	int i;

	if (!dirty_i)
		return;

	/* discard pre-free/dirty segments list */
	for (i = 0; i < NR_DIRTY_TYPE; i++)
		discard_dirty_segmap(sbi, i);

	if (__is_large_section(sbi)) {
	}

	destroy_victim_secmap(sbi);
	SM_I(sbi)->dirty_info = NULL;
	kfree(dirty_i);
}

/*
 * DeLFS: free every core's curseg array. (DeLFS 6.6.8 only freed the unused
 * global curseg_array and leaked the per-core ones.) Iterate possible cpus
 * so that a partially built array is freed too.
 */
static void destroy_curseg(struct f2fs_sb_info *sbi)
{
	struct curseg_info_array __percpu *wrap_array =
					SM_I(sbi)->wrap_curseg_array;
	int i, cpu;

	if (!wrap_array)
		return;

	for_each_possible_cpu(cpu) {
		struct curseg_info_array *cpu_array = per_cpu_ptr(wrap_array, cpu);
		struct curseg_info *array = cpu_array->array;

		if (!array)
			continue;
		cpu_array->array = NULL;
		for (i = 0; i < NR_CURSEG_TYPE; i++) {
			kfree(array[i].sum_blk);
			kfree(array[i].journal);
		}
		kfree(array);
	}

	SM_I(sbi)->wrap_curseg_array = NULL;
	free_percpu(wrap_array);
}

static void destroy_free_segmap(struct f2fs_sb_info *sbi)
{
	struct free_segmap_info *free_i = SM_I(sbi)->free_info;

	if (!free_i)
		return;
	SM_I(sbi)->free_info = NULL;
	kvfree(free_i->free_segmap);
	kvfree(free_i->free_secmap);
	kfree(free_i);
}

static void destroy_sit_info(struct f2fs_sb_info *sbi)
{
        struct sit_info *sit_i = SIT_I(sbi);

        if (!sit_i)
                return;

        if (sit_i->sentries)
                kvfree(sit_i->bitmap);
        kfree(sit_i->tmp_map);

        kvfree(sit_i->cursec_refs);

        kvfree(sit_i->sentries);
        kvfree(sit_i->sec_entries);
        kvfree(sit_i->dirty_sentries_bitmap);

        if (sit_i->percore_sentry_lock)
                free_percpu(sit_i->percore_sentry_lock);

	SM_I(sbi)->sit_info = NULL;
	kfree(sit_i->sit_bitmap);	/* 7.2.8: kmemdup()ed, so kfree */
#ifdef CONFIG_F2FS_CHECK_FS
	kvfree(sit_i->invalid_segmap);	/* 7.2.8: no sit_bitmap_mir */
#endif
        kfree(sit_i);
}

void f2fs_destroy_segment_manager(struct f2fs_sb_info *sbi)
{
	struct f2fs_sm_info *sm_info = SM_I(sbi);

	if (!sm_info)
		return;
	f2fs_destroy_flush_cmd_control(sbi, true);
	destroy_discard_cmd_control(sbi);
	destroy_dirty_segmap(sbi);
	destroy_curseg(sbi);
	destroy_free_segmap(sbi);
	destroy_sit_info(sbi);
	sbi->sm_info = NULL;
	kfree(sm_info);
}

int __init f2fs_create_segment_manager_caches(void)
{
	discard_entry_slab = f2fs_kmem_cache_create("f2fs_discard_entry",
			sizeof(struct discard_entry));
	if (!discard_entry_slab)
		goto fail;

	discard_cmd_slab = f2fs_kmem_cache_create("f2fs_discard_cmd",
			sizeof(struct discard_cmd));
	if (!discard_cmd_slab)
		goto destroy_discard_entry;

	sit_entry_set_slab = f2fs_kmem_cache_create("f2fs_sit_entry_set",
			sizeof(struct sit_entry_set));
	if (!sit_entry_set_slab)
		goto destroy_discard_cmd;

	revoke_entry_slab = f2fs_kmem_cache_create("f2fs_revoke_entry",
			sizeof(struct revoke_entry));
	if (!revoke_entry_slab)
		goto destroy_sit_entry_set;
	return 0;

destroy_sit_entry_set:
	kmem_cache_destroy(sit_entry_set_slab);
destroy_discard_cmd:
	kmem_cache_destroy(discard_cmd_slab);
destroy_discard_entry:
	kmem_cache_destroy(discard_entry_slab);
fail:
	return -ENOMEM;
}

void f2fs_destroy_segment_manager_caches(void)
{
	kmem_cache_destroy(sit_entry_set_slab);
	kmem_cache_destroy(discard_cmd_slab);
	kmem_cache_destroy(discard_entry_slab);
	kmem_cache_destroy(revoke_entry_slab);
}
