// SPDX-License-Identifier: GPL-2.0
/*
 * fs/f2fs/segment.c
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 */
#include <linux/nvme.h>
#include <linux/fs.h>
#include <linux/f2fs_fs.h>
#include <linux/bio.h>
#include <linux/prefetch.h>
#include <linux/kthread.h>
#include <linux/swap.h>
#include <linux/timer.h>
#include <linux/freezer.h>
#include <linux/sched/signal.h>

#include "f2fs.h"
#include "segment.h"
#include "node.h"
#include "gc.h"
#include "trace.h"
#include <trace/events/f2fs.h>

#define __reverse_ffz(x) __reverse_ffs(~(x))

static struct kmem_cache *discard_entry_slab;
#ifdef IPLFS_CALLBACK_IO
static struct kmem_cache *mg_entry_slab;
static struct kmem_cache *slot_entry_slab;
static struct kmem_cache *discard_cnt_entry_slab;
#endif
static struct kmem_cache *discard_cmd_slab;
static struct kmem_cache *sit_entry_set_slab;
static struct kmem_cache *inmem_entry_slab;
static struct kmem_cache *discard_map_slab;
static struct kmem_cache *discard_range_slab;
DEFINE_HASHTABLE(ht, 7);
DEFINE_HASHTABLE(slot_ht, 15);
DEFINE_HASHTABLE(discard_ht, 10);

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

unsigned long long OS_TimeGetNS( void )
{
   	struct timespec64 lTime;
   	ktime_get_coarse_real_ts64(&lTime);
   	return (lTime.tv_sec * 1000000000 + lTime.tv_nsec );
}

unsigned long long OS_TimeGetUS( void )
{
   	struct timespec64 lTime;
   	ktime_get_coarse_real_ts64(&lTime);
   	return (lTime.tv_sec * 1000000 + div_u64(lTime.tv_nsec, 1000) );

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
	//int node_secs = get_blocktype_secs(sbi, F2FS_DIRTY_NODES);
	//int dent_secs = get_blocktype_secs(sbi, F2FS_DIRTY_DENTS);
	//int imeta_secs = get_blocktype_secs(sbi, F2FS_DIRTY_IMETA);

	if (f2fs_lfs_mode(sbi))
		return false;
	panic("f2fs_need_SSR(): not expected!! must be lfs mode!!");
	if (sbi->gc_mode == GC_URGENT_HIGH)
		return true;
	if (unlikely(is_sbi_flag_set(sbi, SBI_CP_DISABLED)))
		return true;
	return false;
	//return free_sections(sbi) <= (node_secs + 2 * dent_secs + imeta_secs +
	//		SM_I(sbi)->min_ssr_sections + reserved_sections(sbi));
}

void f2fs_register_inmem_page(struct inode *inode, struct page *page)
{
	struct inmem_pages *new;

	f2fs_trace_pid(page);

	f2fs_set_page_private(page, ATOMIC_WRITTEN_PAGE);

	new = f2fs_kmem_cache_alloc(inmem_entry_slab, GFP_NOFS);

	/* add atomic page indices to the list */
	new->page = page;
	INIT_LIST_HEAD(&new->list);

	/* increase reference count with clean state */
	get_page(page);
	mutex_lock(&F2FS_I(inode)->inmem_lock);
	list_add_tail(&new->list, &F2FS_I(inode)->inmem_pages);
	inc_page_count(F2FS_I_SB(inode), F2FS_INMEM_PAGES);
	mutex_unlock(&F2FS_I(inode)->inmem_lock);

	trace_f2fs_register_inmem_page(page, INMEM);
}

static int __revoke_inmem_pages(struct inode *inode,
				struct list_head *head, bool drop, bool recover,
				bool trylock)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct inmem_pages *cur, *tmp;
	int err = 0;

	list_for_each_entry_safe(cur, tmp, head, list) {
		struct page *page = cur->page;

		if (drop)
			trace_f2fs_commit_inmem_page(page, INMEM_DROP);

		if (trylock) {
			/*
			 * to avoid deadlock in between page lock and
			 * inmem_lock.
			 */
			if (!trylock_page(page))
				continue;
		} else {
			lock_page(page);
		}

		f2fs_wait_on_page_writeback(page, DATA, true, true);

		if (recover) {
			struct dnode_of_data dn;
			struct node_info ni;

			trace_f2fs_commit_inmem_page(page, INMEM_REVOKE);
retry:
			set_new_dnode(&dn, inode, NULL, NULL, 0);
			err = f2fs_get_dnode_of_data(&dn, page->index,
								LOOKUP_NODE);
			if (err) {
				if (err == -ENOMEM) {
					congestion_wait(BLK_RW_ASYNC,
							DEFAULT_IO_TIMEOUT);
					cond_resched();
					goto retry;
				}
				err = -EAGAIN;
				goto next;
			}

			err = f2fs_get_node_info(sbi, dn.nid, &ni);
			if (err) {
				f2fs_put_dnode(&dn);
				return err;
			}

			if (cur->old_addr == NEW_ADDR) {
				f2fs_invalidate_blocks(sbi, dn.data_blkaddr);
				f2fs_update_data_blkaddr(&dn, NEW_ADDR);
			} else
				f2fs_replace_block(sbi, &dn, dn.data_blkaddr,
					cur->old_addr, ni.version, true, true);
			f2fs_put_dnode(&dn);
		}
next:
		/* we don't need to invalidate this in the sccessful status */
		if (drop || recover) {
			ClearPageUptodate(page);
			clear_cold_data(page);
		}
		f2fs_clear_page_private(page);
		f2fs_put_page(page, 1);

		list_del(&cur->list);
		kmem_cache_free(inmem_entry_slab, cur);
		dec_page_count(F2FS_I_SB(inode), F2FS_INMEM_PAGES);
	}
	return err;
}

void f2fs_drop_inmem_pages_all(struct f2fs_sb_info *sbi, bool gc_failure)
{
	struct list_head *head = &sbi->inode_list[ATOMIC_FILE];
	struct inode *inode;
	struct f2fs_inode_info *fi;
	unsigned int count = sbi->atomic_files;
	unsigned int looped = 0;
next:
	spin_lock(&sbi->inode_lock[ATOMIC_FILE]);
	if (list_empty(head)) {
		spin_unlock(&sbi->inode_lock[ATOMIC_FILE]);
		return;
	}
	fi = list_first_entry(head, struct f2fs_inode_info, inmem_ilist);
	inode = igrab(&fi->vfs_inode);
	if (inode)
		list_move_tail(&fi->inmem_ilist, head);
	spin_unlock(&sbi->inode_lock[ATOMIC_FILE]);

	if (inode) {
		if (gc_failure) {
			if (!fi->i_gc_failures[GC_FAILURE_ATOMIC])
				goto skip;
		}
		set_inode_flag(inode, FI_ATOMIC_REVOKE_REQUEST);
		f2fs_drop_inmem_pages(inode);
skip:
		iput(inode);
	}
	congestion_wait(BLK_RW_ASYNC, DEFAULT_IO_TIMEOUT);
	cond_resched();
	if (gc_failure) {
		if (++looped >= count)
			return;
	}
	goto next;
}

void f2fs_drop_inmem_pages(struct inode *inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct f2fs_inode_info *fi = F2FS_I(inode);

	while (!list_empty(&fi->inmem_pages)) {
		mutex_lock(&fi->inmem_lock);
		__revoke_inmem_pages(inode, &fi->inmem_pages,
						true, false, true);
		mutex_unlock(&fi->inmem_lock);
	}

	fi->i_gc_failures[GC_FAILURE_ATOMIC] = 0;

	spin_lock(&sbi->inode_lock[ATOMIC_FILE]);
	if (!list_empty(&fi->inmem_ilist))
		list_del_init(&fi->inmem_ilist);
	if (f2fs_is_atomic_file(inode)) {
		clear_inode_flag(inode, FI_ATOMIC_FILE);
		sbi->atomic_files--;
	}
	spin_unlock(&sbi->inode_lock[ATOMIC_FILE]);
}

void f2fs_drop_inmem_page(struct inode *inode, struct page *page)
{
	struct f2fs_inode_info *fi = F2FS_I(inode);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct list_head *head = &fi->inmem_pages;
	struct inmem_pages *cur = NULL;

	f2fs_bug_on(sbi, !IS_ATOMIC_WRITTEN_PAGE(page));

	mutex_lock(&fi->inmem_lock);
	list_for_each_entry(cur, head, list) {
		if (cur->page == page)
			break;
	}

	f2fs_bug_on(sbi, list_empty(head) || cur->page != page);
	list_del(&cur->list);
	mutex_unlock(&fi->inmem_lock);

	dec_page_count(sbi, F2FS_INMEM_PAGES);
	kmem_cache_free(inmem_entry_slab, cur);

	ClearPageUptodate(page);
	f2fs_clear_page_private(page);
	f2fs_put_page(page, 0);

	trace_f2fs_commit_inmem_page(page, INMEM_INVALIDATE);
}

static int __f2fs_commit_inmem_pages(struct inode *inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct f2fs_inode_info *fi = F2FS_I(inode);
	struct inmem_pages *cur, *tmp;
	struct f2fs_io_info fio = {
		.sbi = sbi,
		.ino = inode->i_ino,
		.type = DATA,
		.op = REQ_OP_WRITE,
		.op_flags = REQ_SYNC | REQ_PRIO,
		.io_type = FS_DATA_IO,
	};
	struct list_head revoke_list;
	bool submit_bio = false;
	int err = 0;

	INIT_LIST_HEAD(&revoke_list);

	list_for_each_entry_safe(cur, tmp, &fi->inmem_pages, list) {
		struct page *page = cur->page;

		lock_page(page);
		if (page->mapping == inode->i_mapping) {
			trace_f2fs_commit_inmem_page(page, INMEM);

			f2fs_wait_on_page_writeback(page, DATA, true, true);

			set_page_dirty(page);
			if (clear_page_dirty_for_io(page)) {
				inode_dec_dirty_pages(inode);
				f2fs_remove_dirty_inode(inode);
			}
retry:
			fio.page = page;
			fio.old_blkaddr = NULL_ADDR;
			fio.encrypted_page = NULL;
			fio.need_lock = LOCK_DONE;
			err = f2fs_do_write_data_page(&fio);
			if (err) {
				if (err == -ENOMEM) {
					congestion_wait(BLK_RW_ASYNC,
							DEFAULT_IO_TIMEOUT);
					cond_resched();
					goto retry;
				}
				unlock_page(page);
				break;
			}
			/* record old blkaddr for revoking */
			cur->old_addr = fio.old_blkaddr;
			submit_bio = true;
		}
		unlock_page(page);
		list_move_tail(&cur->list, &revoke_list);
	}

	if (submit_bio)
		f2fs_submit_merged_write_cond(sbi, inode, NULL, 0, DATA);

	if (err) {
		/*
		 * try to revoke all committed pages, but still we could fail
		 * due to no memory or other reason, if that happened, EAGAIN
		 * will be returned, which means in such case, transaction is
		 * already not integrity, caller should use journal to do the
		 * recovery or rewrite & commit last transaction. For other
		 * error number, revoking was done by filesystem itself.
		 */
		err = __revoke_inmem_pages(inode, &revoke_list,
						false, true, false);

		/* drop all uncommitted pages */
		__revoke_inmem_pages(inode, &fi->inmem_pages,
						true, false, false);
	} else {
		__revoke_inmem_pages(inode, &revoke_list,
						false, false, false);
	}

	return err;
}

int f2fs_commit_inmem_pages(struct inode *inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct f2fs_inode_info *fi = F2FS_I(inode);
	int err;

	f2fs_balance_fs(sbi, true);

	down_write(&fi->i_gc_rwsem[WRITE]);

	f2fs_lock_op(sbi);
	set_inode_flag(inode, FI_ATOMIC_COMMIT);

	mutex_lock(&fi->inmem_lock);
	err = __f2fs_commit_inmem_pages(inode);
	mutex_unlock(&fi->inmem_lock);

	clear_inode_flag(inode, FI_ATOMIC_COMMIT);

	f2fs_unlock_op(sbi);
	up_write(&fi->i_gc_rwsem[WRITE]);

	return err;
}

#ifdef VOLFS_WRITE_STALL 

static inline bool has_not_enough_prefree_meta_slot(struct f2fs_sb_info *sbi) 
{                                                                          
    struct slot_info *slot_i = SLT_I(sbi);                                 
       bool ret =  atomic_read(&slot_i->prefree_slot_cnt) <=                  
               MAIN_SEG_SLOTS(sbi) * META_STALL_RATIO/2;                     
       //if (ret)                                                            
       //        printk("%s: not enough free meta slot!!!", __func__);       
       return ret;                                                         
}                                                                          

static inline bool has_not_enough_free_meta_slot(struct f2fs_sb_info *sbi) 
{                                                                          
    struct slot_info *slot_i = SLT_I(sbi);                                 
       bool ret =  atomic_read(&slot_i->free_slot_cnt) <=                  
               MAIN_SEG_SLOTS(sbi) * META_STALL_RATIO;                     
       //if (ret)                                                            
       //        printk("%s: not enough free meta slot!!!", __func__);       
       return ret;                                                         
}                                                                          
#endif

/*
 * This function balances dirty node and dentry pages.
 * In addition, it controls garbage collection.
 */
void f2fs_balance_fs(struct f2fs_sb_info *sbi, bool need)
{
	if (time_to_inject(sbi, FAULT_CHECKPOINT)) {
		f2fs_show_injection_info(sbi, FAULT_CHECKPOINT);
		f2fs_stop_checkpoint(sbi, false);
	}

	/* balance_fs_bg is able to be pending */
#ifdef LM
	if (need && (excess_cached_nats(sbi) || excess_prefree_segs(sbi)))
#else
	if (need && excess_cached_nats(sbi))
#endif
		f2fs_balance_fs_bg(sbi, false);

	if (!f2fs_is_checkpoint_ready(sbi))
		return;

	/*
	 * We should do GC or end up with checkpoint, if there are so many dirty
	 * dir/node pages without enough free segments.
	 */
	//if (has_not_enough_free_secs(sbi, 0, 0)) {

#ifdef VOLFS_WRITE_STALL
	//static int pcnt = 0;
	if (has_not_enough_free_meta_slot(sbi) || has_not_enough_free_physical_secs(sbi, 0, 0)
			|| has_not_enough_free_secs_in_regular_region(sbi)) { 
		struct cp_control cpc;
		//if (pcnt % 500 == 0)
		//	printk("%s cp triggered by stall!! pcnt: %d", __func__, pcnt);
		//pcnt ++;
		down_write(&sbi->gc_lock);
		cpc.reason = __get_cp_reason(sbi);
		struct slot_info *slot_i = SLT_I(sbi);                                 
		//if (atomic_read(&slot_i->prefree_slot_cnt))
			f2fs_write_checkpoint(sbi, &cpc);
		/*
		if (has_not_enough_free_secs_in_regular_region(sbi)) {
			while (has_not_enough_free_secs_in_regular_region(sbi)) {

			}
			printk("%s real stall!!", __func__);
		}
		*/
		//else {
		//	while (atomic_read(&slot_i->prefree_slot_cnt) <= 0) {

		//	}
		//	f2fs_write_checkpoint(sbi, &cpc);
		//	printk("%s real stall!!", __func__);
		//}
		//while (has_not_enough_prefree_meta_slot(sbi)) {

		//}
		//f2fs_write_checkpoint(sbi, &cpc);
		//while (!has_not_enough_free_meta_slot(sbi)) {
		//}
		up_write(&sbi->gc_lock);
		//printk("%s stall!!", __func__);
	}
#else
	if (has_not_enough_free_physical_secs(sbi, 0, 0)) {
		panic("f2fs_balance_fs: gc not expected!");
		down_write(&sbi->gc_lock);
		f2fs_gc(sbi, false, false, NULL_SEGNO);
	}
#endif
}

void f2fs_balance_fs_bg(struct f2fs_sb_info *sbi, bool from_bg)
{
	bool cp_by_prefree = false;
	bool cp_by_node = false;

	if (unlikely(is_sbi_flag_set(sbi, SBI_POR_DOING)))
		return;

	/* try to shrink extent cache when there is no enough memory */
	if (!f2fs_available_free_memory(sbi, EXTENT_CACHE))
		f2fs_shrink_extent_tree(sbi, EXTENT_CACHE_SHRINK_NUMBER);

	/* check the # of cached NAT entries */
	if (!f2fs_available_free_memory(sbi, NAT_ENTRIES))
		f2fs_try_to_free_nats(sbi, NAT_ENTRY_PER_BLOCK);

	if (!f2fs_available_free_memory(sbi, FREE_NIDS))
		f2fs_try_to_free_nids(sbi, MAX_FREE_NIDS);
	else
		f2fs_build_free_nids(sbi, false, false);

	//if (excess_dirty_nats(sbi) || excess_dirty_nodes(sbi))
	//	goto do_sync;
	if (excess_dirty_nats(sbi) || excess_dirty_nodes(sbi)) {
		//printk("excess dirty nats: %d dirty nodes: %d", 
		//		excess_dirty_nats(sbi),
		//	   	excess_dirty_nodes(sbi));
		cp_by_node = true;
		goto do_sync;
	}
	
	if (excess_prefree_segs(sbi)) {
		cp_by_prefree = true;
		goto do_sync;
	}

	/* there is background inflight IO or foreground operation recently */
	if (is_inflight_io(sbi, REQ_TIME) ||
		(!f2fs_time_over(sbi, REQ_TIME) && rwsem_is_locked(&sbi->cp_rwsem)))
		return;

	/* exceed periodical checkpoint timeout threshold */
	if (f2fs_time_over(sbi, CP_TIME))
		goto do_sync;

	/* checkpoint is the only way to shrink partial cached entries */
	if (f2fs_available_free_memory(sbi, NAT_ENTRIES) ||
		f2fs_available_free_memory(sbi, INO_ENTRIES))
		return;

do_sync:
	if (test_opt(sbi, DATA_FLUSH) && from_bg) {
		struct blk_plug plug;

		mutex_lock(&sbi->flush_lock);

		blk_start_plug(&plug);
		f2fs_sync_dirty_inodes(sbi, FILE_INODE);
		blk_finish_plug(&plug);

		mutex_unlock(&sbi->flush_lock);
	}
	f2fs_sync_fs(sbi->sb, true, cp_by_prefree, cp_by_node);
	stat_inc_bg_cp_count(sbi->stat_info);
}

static int __submit_flush_wait(struct f2fs_sb_info *sbi,
				struct block_device *bdev)
{
	struct bio *bio;
	int ret;

	bio = f2fs_bio_alloc(sbi, 0, false);
	if (!bio)
		return -ENOMEM;

	bio->bi_opf = REQ_OP_WRITE | REQ_SYNC | REQ_PREFLUSH;
	bio_set_dev(bio, bdev);
	ret = submit_bio_wait(bio);
	bio_put(bio);

	trace_f2fs_issue_flush(bdev, test_opt(sbi, NOBARRIER),
				test_opt(sbi, FLUSH_MERGE), ret);
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

	sb_start_intwrite(sbi->sb);

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

	sb_end_intwrite(sbi->sb);

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

	/* update issue_list before we wake up issue_flush thread */
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
	int err = 0;

	if (SM_I(sbi)->fcc_info) {
		fcc = SM_I(sbi)->fcc_info;
		if (fcc->f2fs_issue_flush)
			return err;
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
		return err;

init_thread:
	fcc->f2fs_issue_flush = kthread_run(issue_flush_thread, sbi,
				"f2fs_flush-%u:%u", MAJOR(dev), MINOR(dev));
	if (IS_ERR(fcc->f2fs_issue_flush)) {
		err = PTR_ERR(fcc->f2fs_issue_flush);
		kfree(fcc);
		SM_I(sbi)->fcc_info = NULL;
		return err;
	}

	return err;
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
		if (!f2fs_test_bit(i, (char *)&sbi->dirty_device))
			continue;
		ret = __submit_flush_wait(sbi, FDEV(i).bdev);
		if (ret)
			break;

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

	/* need not be added */
	if (IS_CURSEG(sbi, segno))
		return;

#ifdef SINGLE_INTERVAL
	if (segno >= sbi->START_SEGNO_INTERVAL_NODE) {
		segno -= sbi->START_SEGNO_INTERVAL_NODE;
		if (!test_and_set_bit(segno, dirty_i->dirty_segmap[dirty_type + NR_DIRTY_DATA_TYPE]))
			dirty_i->nr_dirty[dirty_type + NR_DIRTY_DATA_TYPE]++;
	} else {
		segno -= sbi->START_SEGNO_INTERVAL;
		if (!test_and_set_bit(segno, dirty_i->dirty_segmap[dirty_type]))
			dirty_i->nr_dirty[dirty_type]++;
	}
#else
	if (!test_and_set_bit(segno, dirty_i->dirty_segmap[dirty_type]))
		dirty_i->nr_dirty[dirty_type]++;

#endif
	

	//if (dirty_type == DIRTY) {
	//	//struct seg_entry *sentry = get_seg_entry(sbi, segno);
	//	//enum dirty_type t = sentry->type;

	//	//if (unlikely(t >= DIRTY)) {
	//	//	f2fs_bug_on(sbi, 1);
	//	//	return;
	//	//}
	//	//if (!test_and_set_bit(segno, dirty_i->dirty_segmap[t]))
	//	//	dirty_i->nr_dirty[t]++;

	//	if (__is_large_section(sbi)) {
	//		unsigned int secno = GET_SEC_FROM_SEG(sbi, segno);
	//		block_t valid_blocks =
	//			get_valid_blocks(sbi, segno, true);

	//		f2fs_bug_on(sbi, unlikely(!valid_blocks ||
	//				valid_blocks == BLKS_PER_SEC(sbi)));

	//		if (!IS_CURSEC(sbi, secno))
	//			set_bit(secno, dirty_i->dirty_secmap);
	//	}
	//}
}

static void __remove_dirty_segment(struct f2fs_sb_info *sbi, unsigned int segno,
		enum dirty_type dirty_type)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	block_t valid_blocks;

#ifdef SINGLE_INTERVAL
	if (segno >= sbi->START_SEGNO_INTERVAL_NODE) {
		segno -= sbi->START_SEGNO_INTERVAL_NODE;
		if (test_and_clear_bit(segno, dirty_i->dirty_segmap[dirty_type + NR_DIRTY_DATA_TYPE]))
			dirty_i->nr_dirty[dirty_type + NR_DIRTY_DATA_TYPE]--;
	} else {
		segno -= sbi->START_SEGNO_INTERVAL;
		if (test_and_clear_bit(segno, dirty_i->dirty_segmap[dirty_type]))
			dirty_i->nr_dirty[dirty_type]--;
	}

#else
	if (test_and_clear_bit(segno, dirty_i->dirty_segmap[dirty_type]))
		dirty_i->nr_dirty[dirty_type]--;
#endif

	/*
	if (dirty_type == DIRTY) {
		//struct seg_entry *sentry = get_seg_entry(sbi, segno);
		//enum dirty_type t = sentry->type;

		//if (test_and_clear_bit(segno, dirty_i->dirty_segmap[t]))
		//	dirty_i->nr_dirty[t]--;

		//valid_blocks = get_valid_blocks(sbi, segno, true);
		//if (valid_blocks == 0) {
		//	clear_bit(GET_SEC_FROM_SEG(sbi, segno),
		//				dirty_i->victim_secmap);
#ifdef CONFIG_F2FS_CHECK_FS
			clear_bit(segno, SIT_I(sbi)->invalid_segmap);
#endif
		}
		if (__is_large_section(sbi)) {
			unsigned int secno = GET_SEC_FROM_SEG(sbi, segno);

			if (!valid_blocks ||
					valid_blocks == BLKS_PER_SEC(sbi)) {
				clear_bit(secno, dirty_i->dirty_secmap);
				return;
			}

			if (!IS_CURSEC(sbi, secno))
				set_bit(secno, dirty_i->dirty_secmap);
		}
	}
	*/
}

/*
 * Should not occur error such as -ENOMEM.
 * Adding dirty entry into seglist is not critical operation.
 * If a given segment is one of current working segments, it won't be added.
 */
//static void locate_dirty_segment(struct f2fs_sb_info *sbi, unsigned int segno)
//{
//	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
//	unsigned short valid_blocks, ckpt_valid_blocks;
//	unsigned int usable_blocks;
//
//	if (segno == NULL_SEGNO || IS_CURSEG(sbi, segno))
//		return;
//	
//	usable_blocks = f2fs_usable_blks_in_seg(sbi, segno);
//	mutex_lock(&dirty_i->seglist_lock);
//
//	valid_blocks = get_valid_blocks(sbi, segno, false);
//	//ckpt_valid_blocks = get_ckpt_valid_blocks(sbi, segno);
//
//	if (valid_blocks == 0 && (!is_sbi_flag_set(sbi, SBI_CP_DISABLED) ||
//		ckpt_valid_blocks == usable_blocks)) {
//		//__locate_dirty_segment(sbi, segno, PRE);
//		//__remove_dirty_segment(sbi, segno, DIRTY);
//	} else if (valid_blocks < usable_blocks) {
//		//__locate_dirty_segment(sbi, segno, DIRTY);
//	} else {
//		// Recovery routine with SSR needs this 
//		//__remove_dirty_segment(sbi, segno, DIRTY);
//	}
//	mutex_unlock(&dirty_i->seglist_lock);
//}

static void locate_dirty_segment(struct f2fs_sb_info *sbi, unsigned int segno, 
		unsigned int slot_idx)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	unsigned short valid_blocks;
	unsigned int usable_blocks;

	if (segno == NULL_SEGNO || IS_CURSEG(sbi, segno) || IS_MIGRATION_SEGNO(sbi, segno))
		return;
	
	usable_blocks = f2fs_usable_blks_in_seg(sbi, segno);
	mutex_lock(&dirty_i->seglist_lock);
	if (slot_idx == NULL_SLOTNO) 
		printk("%s: segno: %lu slot_idx: 0x%lx", __func__, segno, slot_idx);

	valid_blocks = get_valid_blocks(sbi, slot_idx, false);

	if (valid_blocks == 0 && !is_sbi_flag_set(sbi, SBI_CP_DISABLED)) {
		__locate_dirty_segment(sbi, segno, PRE);
		__remove_dirty_segment(sbi, segno, DIRTY);
	} else if (valid_blocks < usable_blocks) {
		__locate_dirty_segment(sbi, segno, DIRTY);
	} else {
		// Recovery routine with SSR needs this 
		//printk("%s: unexpected. vblks: %u", __func__, valid_blocks);
		//f2fs_bug_on(sbi, 1);
		__remove_dirty_segment(sbi, segno, DIRTY);
	}
	mutex_unlock(&dirty_i->seglist_lock);
}

/* This moves currently empty dirty blocks to prefree. Must hold seglist_lock */
//void f2fs_dirty_to_prefree(struct f2fs_sb_info *sbi)
//{
//	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
//	unsigned int segno;
//
//	mutex_lock(&dirty_i->seglist_lock);
//	for_each_set_bit(segno, dirty_i->dirty_segmap[DIRTY], MAIN_SEGS(sbi)) {
//		if (get_valid_blocks(sbi, segno, false))
//			continue;
//		if (IS_CURSEG(sbi, segno))
//			continue;
//		//__locate_dirty_segment(sbi, segno, PRE);
//		//__remove_dirty_segment(sbi, segno, DIRTY);
//	}
//	mutex_unlock(&dirty_i->seglist_lock);
//}

static inline struct slot_entry *lookup_slot_hash(uint64_t key);
	
void f2fs_dirty_to_prefree(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	struct slot_info *slot_i = SLT_I(sbi);
	unsigned int segno, real_segno, slot_idx;
	struct slot_entry *slte;

	unsigned int start_segno_interval = sbi->START_SEGNO_INTERVAL;
	unsigned int type_off = 0;

	printk("%s: start ", __func__);
AGAIN:
	mutex_lock(&dirty_i->seglist_lock);
	for_each_set_bit(segno, dirty_i->dirty_segmap[DIRTY+type_off], MAIN_SEGS_INTERVAL(sbi)) {

#ifdef SINGLE_INTERVAL
		real_segno = segno + start_segno_interval;
		mutex_lock(&slot_i->lock);
		
		/* translate segno into slot index */
		/* get slot index of segno from hash table */
	   	slte = lookup_slot_hash(real_segno);
		if (slte == NULL) {
		    printk("%s: blkaddr: 0x%lx segno: %u", __func__,
		 		   START_BLOCK(sbi, real_segno), real_segno);
		    unsigned int tmp_ii;
		
		    for (tmp_ii = 0; tmp_ii < MAIN_SEG_SLOTS(sbi); tmp_ii ++) {
		 	   slte = get_slot_entry(sbi, tmp_ii);
		 	   printk("%s: slot idx: %u slte slot_idx %lu segno: %lu", __func__, tmp_ii, 
		 		slte->slot_idx, slte->segno);
		    }
		    f2fs_bug_on(sbi, 1);
		}
		slot_idx = slte->slot_idx;
		mutex_unlock(&slot_i->lock);
		if (get_valid_blocks(sbi, slot_idx, false))
			continue;
		if (IS_CURSEG(sbi, real_segno))
			continue;
		__locate_dirty_segment(sbi, real_segno, PRE);
		__remove_dirty_segment(sbi, real_segno, DIRTY);
#endif
	}
	mutex_unlock(&dirty_i->seglist_lock);
	if (start_segno_interval == sbi->START_SEGNO_INTERVAL) {
		start_segno_interval = sbi->START_SEGNO_INTERVAL_NODE;
		type_off = NR_DIRTY_DATA_TYPE;
		goto AGAIN;
	}
	printk("%s: end ", __func__);
}

block_t f2fs_get_unusable_blocks(struct f2fs_sb_info *sbi)
{
	printk("f2fs_get_unusable_blocks: did not expect");
	f2fs_bug_on(sbi, 1);
	return 0;
	/*
	int ovp_hole_segs =
		(overprovision_segments(sbi) - reserved_segments(sbi));
	block_t ovp_holes = ovp_hole_segs << sbi->log_blocks_per_seg;
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	block_t holes[2] = {0, 0};	// DATA and NODE 
	block_t unusable;
	struct seg_entry *se;
	unsigned int segno;
	mutex_lock(&dirty_i->seglist_lock);
	for_each_set_bit(segno, dirty_i->dirty_segmap[DIRTY], MAIN_SEGS(sbi)) {
		se = get_seg_entry(sbi, segno);
		if (IS_NODESEG(se->type))
			holes[NODE] += f2fs_usable_blks_in_seg(sbi, segno) -
							se->valid_blocks;
		else
			holes[DATA] += f2fs_usable_blks_in_seg(sbi, segno) -
							se->valid_blocks;
	}
	mutex_unlock(&dirty_i->seglist_lock);
	unusable = holes[DATA] > holes[NODE] ? holes[DATA] : holes[NODE];
	if (unusable > ovp_holes)
		return unusable - ovp_holes;
	return 0;
	*/
}

int f2fs_disable_cp_again(struct f2fs_sb_info *sbi, block_t unusable)
{
	//int ovp_hole_segs =
	//	(overprovision_segments(sbi) - reserved_segments(sbi));
	if (unusable > F2FS_OPTION(sbi).unusable_cap)
		return -EAGAIN;
	if (is_sbi_flag_set(sbi, SBI_CP_DISABLED_QUICK))
		panic("f2fs_disable_cp_again(): SBI_CP_DISABLED_QUICK not expected!!");
	/*if (is_sbi_flag_set(sbi, SBI_CP_DISABLED_QUICK) &&
		dirty_segments(sbi) > ovp_hole_segs)
		return -EAGAIN;
	*/
	return 0;
}

/* This is only used by SBI_CP_DISABLED */
/*static unsigned int get_free_segment(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	unsigned int segno = 0;

	mutex_lock(&dirty_i->seglist_lock);
	for_each_set_bit(segno, dirty_i->dirty_segmap[DIRTY], MAIN_SEGS(sbi)) {
		if (get_valid_blocks(sbi, segno, false))
			continue;
		if (get_ckpt_valid_blocks(sbi, segno))
			continue;
		mutex_unlock(&dirty_i->seglist_lock);
		return segno;
	}
	mutex_unlock(&dirty_i->seglist_lock);
	return NULL_SEGNO;
}*/

static struct discard_cmd *__create_discard_cmd(struct f2fs_sb_info *sbi,
		struct block_device *bdev, block_t lstart,
		block_t start, block_t len)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct list_head *pend_list;
	struct discard_cmd *dc;

	f2fs_bug_on(sbi, !len);

	pend_list = &dcc->pend_list[plist_idx(len)];

	dc = f2fs_kmem_cache_alloc(discard_cmd_slab, GFP_NOFS);
	INIT_LIST_HEAD(&dc->list);
	dc->bdev = bdev;
	dc->lstart = lstart;
	dc->start = start;
	dc->len = len;
	dc->ref = 0;
	dc->state = D_PREP;
	dc->queued = 0;
	dc->error = 0;
	init_completion(&dc->wait);
	list_add_tail(&dc->list, pend_list);
	spin_lock_init(&dc->lock);
	dc->bio_ref = 0;
	atomic_inc(&dcc->discard_cmd_cnt);
	dcc->undiscard_blks += len;

	return dc;
}

static struct discard_cmd *__attach_discard_cmd(struct f2fs_sb_info *sbi,
				struct block_device *bdev, block_t lstart,
				block_t start, block_t len,
				struct rb_node *parent, struct rb_node **p,
				bool leftmost)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct discard_cmd *dc;

	dc = __create_discard_cmd(sbi, bdev, lstart, start, len);

	rb_link_node(&dc->rb_node, parent, p);
	rb_insert_color_cached(&dc->rb_node, &dcc->root, leftmost);

	return dc;
}

static void __detach_discard_cmd(struct discard_cmd_control *dcc,
							struct discard_cmd *dc)
{
	if (dc->state == D_DONE)
		atomic_sub(dc->queued, &dcc->queued_discard);

	list_del(&dc->list);
	rb_erase_cached(&dc->rb_node, &dcc->root);
	dcc->undiscard_blks -= dc->len;

	kmem_cache_free(discard_cmd_slab, dc);

	atomic_dec(&dcc->discard_cmd_cnt);
}

static void __remove_discard_cmd(struct f2fs_sb_info *sbi,
							struct discard_cmd *dc)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	unsigned long flags;

	trace_f2fs_remove_discard(dc->bdev, dc->start, dc->len);

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
		printk_ratelimited(
			"%sF2FS-fs (%s): Issue discard(%u, %u, %u) failed, ret: %d",
			KERN_INFO, sbi->sb->s_id,
			dc->lstart, dc->start, dc->len, dc->error);
	__detach_discard_cmd(dcc, dc);
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

//static void __check_sit_bitmap(struct f2fs_sb_info *sbi,
//				block_t start, block_t end)
//{
//#ifdef CONFIG_F2FS_CHECK_FS
//	struct seg_entry *sentry;
//	unsigned int segno;
//	block_t blk = start;
//	unsigned long offset, size, max_blocks = sbi->blocks_per_seg;
//	unsigned long *map;
//
//	while (blk < end) {
//		segno = GET_SEGNO(sbi, blk);
//		sentry = get_seg_entry(sbi, segno);
//		offset = GET_BLKOFF_FROM_SEG0(sbi, blk);
//
//		if (end < START_BLOCK(sbi, segno + 1))
//			size = GET_BLKOFF_FROM_SEG0(sbi, end);
//		else
//			size = max_blocks;
//		map = (unsigned long *)(sentry->cur_valid_map);
//		offset = __find_rev_next_bit(map, size, offset);
//		f2fs_bug_on(sbi, offset != size);
//		blk = START_BLOCK(sbi, segno + 1);
//	}
//#endif
//}

static void __init_discard_policy(struct f2fs_sb_info *sbi,
				struct discard_policy *dpolicy,
				int discard_type, unsigned int granularity)
{
	/* common policy */
	dpolicy->type = discard_type;
	dpolicy->sync = true;
	dpolicy->ordered = false;
	dpolicy->granularity = granularity;

	dpolicy->max_requests = DEF_MAX_DISCARD_REQUEST;
	//dpolicy->max_requests = 5000000;//DEF_MAX_DISCARD_REQUEST;
	dpolicy->io_aware_gran = MAX_PLIST_NUM;
	dpolicy->timeout = false;

	if (discard_type == DPOLICY_BG) {
		dpolicy->min_interval = DEF_MIN_DISCARD_ISSUE_TIME;
		dpolicy->mid_interval = DEF_MID_DISCARD_ISSUE_TIME;
		dpolicy->max_interval = DEF_MAX_DISCARD_ISSUE_TIME;
		dpolicy->io_aware = true;
		dpolicy->sync = false;
		dpolicy->ordered = true;
		if (utilization(sbi) > DEF_DISCARD_URGENT_UTIL) {
			dpolicy->granularity = 1;
			dpolicy->max_interval = DEF_MIN_DISCARD_ISSUE_TIME;
		}
	} else if (discard_type == DPOLICY_FORCE) {
		dpolicy->min_interval = DEF_MIN_DISCARD_ISSUE_TIME;
		dpolicy->mid_interval = DEF_MID_DISCARD_ISSUE_TIME;
		dpolicy->max_interval = DEF_MAX_DISCARD_ISSUE_TIME;
		dpolicy->io_aware = false;
	} else if (discard_type == DPOLICY_FSTRIM) {
		dpolicy->io_aware = false;
	} else if (discard_type == DPOLICY_UMOUNT) {
		dpolicy->io_aware = false;
		/* we need to issue all to keep CP_TRIMMED_FLAG */
		dpolicy->granularity = 1;
		dpolicy->ordered = true;
		dpolicy->max_requests = 500000000;//DEF_MAX_DISCARD_REQUEST;
		//dpolicy->timeout = true;
	}
}

static void __update_discard_tree_range(struct f2fs_sb_info *sbi,
				struct block_device *bdev, block_t lstart,
				block_t start, block_t len);
/* this function is copied from blkdev_issue_discard from block/blk-lib.c */
static int __submit_discard_cmd(struct f2fs_sb_info *sbi,
						struct discard_policy *dpolicy,
						struct discard_cmd *dc,
						unsigned int *issued)
{
	struct block_device *bdev = dc->bdev;
	struct request_queue *q = bdev_get_queue(bdev);
	unsigned int max_discard_blocks =
			SECTOR_TO_BLOCK(q->limits.max_discard_sectors);
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct list_head *wait_list = (dpolicy->type == DPOLICY_FSTRIM) ?
					&(dcc->fstrim_list) : &(dcc->wait_list);
	int flag = dpolicy->sync ? REQ_SYNC : 0;
	block_t lstart, start, len, total_len;
	int err = 0;


	if (dc->state != D_PREP)
		return 0;

	if (is_sbi_flag_set(sbi, SBI_NEED_FSCK))
		return 0;

	trace_f2fs_issue_discard(bdev, dc->start, dc->len);

	lstart = dc->lstart;
	start = dc->start;
	len = dc->len;
	total_len = len;

	dc->len = 0;

	while (total_len && *issued < dpolicy->max_requests && !err) {
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

		dc->len += len;

		if (time_to_inject(sbi, FAULT_DISCARD)) {
			f2fs_show_injection_info(sbi, FAULT_DISCARD);
			err = -EIO;
			goto submit;
		}
		err = __blkdev_issue_discard(bdev,
					SECTOR_FROM_BLOCK(start),
					SECTOR_FROM_BLOCK(len),
					GFP_NOFS, 0, &bio);
submit:
		if (err) {
			spin_lock_irqsave(&dc->lock, flags);
			if (dc->state == D_PARTIAL)
				dc->state = D_SUBMIT;
			spin_unlock_irqrestore(&dc->lock, flags);

			break;
		}

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
		list_move_tail(&dc->list, wait_list);

		/* sanity check on discard range */
		//__check_sit_bitmap(sbi, lstart, lstart + len);

		bio->bi_private = dc;
		bio->bi_end_io = f2fs_submit_discard_endio;
		bio->bi_opf |= flag;
		submit_bio(bio);

		atomic_inc(&dcc->issued_discard);

		f2fs_update_iostat(sbi, FS_DISCARD, 1);

		lstart += len;
		start += len;
		total_len -= len;
		len = total_len;
	}

	if (!err && len) {
		dcc->undiscard_blks -= len;
		__update_discard_tree_range(sbi, bdev, lstart, start, len);
	}
	return err;
}

static void __insert_discard_tree(struct f2fs_sb_info *sbi,
				struct block_device *bdev, block_t lstart,
				block_t start, block_t len,
				struct rb_node **insert_p,
				struct rb_node *insert_parent)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct rb_node **p;
	struct rb_node *parent = NULL;
	bool leftmost = true;

	if (insert_p && insert_parent) {
		parent = insert_parent;
		p = insert_p;
		goto do_insert;
	}

	p = f2fs_lookup_rb_tree_for_insert(sbi, &dcc->root, &parent,
							lstart, &leftmost);
do_insert:
	__attach_discard_cmd(sbi, bdev, lstart, start, len, parent,
								p, leftmost);
}

static void __relocate_discard_cmd(struct discard_cmd_control *dcc,
						struct discard_cmd *dc)
{
	list_move_tail(&dc->list, &dcc->pend_list[plist_idx(dc->len)]);
}

static void __punch_discard_cmd(struct f2fs_sb_info *sbi,
				struct discard_cmd *dc, block_t blkaddr)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct discard_info di = dc->di;
	bool modified = false;

	if (dc->state == D_DONE || dc->len == 1) {
		__remove_discard_cmd(sbi, dc);
		return;
	}

	dcc->undiscard_blks -= di.len;

	if (blkaddr > di.lstart) {
		dc->len = blkaddr - dc->lstart;
		dcc->undiscard_blks += dc->len;
		__relocate_discard_cmd(dcc, dc);
		modified = true;
	}

	if (blkaddr < di.lstart + di.len - 1) {
		if (modified) {
			__insert_discard_tree(sbi, dc->bdev, blkaddr + 1,
					di.start + blkaddr + 1 - di.lstart,
					di.lstart + di.len - 1 - blkaddr,
					NULL, NULL);
		} else {
			dc->lstart++;
			dc->len--;
			dc->start++;
			dcc->undiscard_blks += dc->len;
			__relocate_discard_cmd(dcc, dc);
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
	struct request_queue *q = bdev_get_queue(bdev);
	unsigned int max_discard_blocks =
			SECTOR_TO_BLOCK(q->limits.max_discard_sectors);
	block_t end = lstart + len;

	dc = (struct discard_cmd *)f2fs_lookup_rb_tree_ret(&dcc->root,
					NULL, lstart,
					(struct rb_entry **)&prev_dc,
					(struct rb_entry **)&next_dc,
					&insert_p, &insert_parent, true, NULL);
	if (dc)
		prev_dc = dc;

	if (!prev_dc) {
		di.lstart = lstart;
		di.len = next_dc ? next_dc->lstart - lstart : len;
		di.len = min(di.len, len);
		di.start = start;
	}

	while (1) {
		struct rb_node *node;
		bool merged = false;
		struct discard_cmd *tdc = NULL;

		if (prev_dc) {
			di.lstart = prev_dc->lstart + prev_dc->len;
			if (di.lstart < lstart)
				di.lstart = lstart;
			if (di.lstart >= end)
				break;

			if (!next_dc || next_dc->lstart > end)
				di.len = end - di.lstart;
			else
				di.len = next_dc->lstart - di.lstart;
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
			__relocate_discard_cmd(dcc, prev_dc);
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
			__relocate_discard_cmd(dcc, next_dc);
			if (tdc)
				__remove_discard_cmd(sbi, tdc);
			merged = true;
		}

		if (!merged) {
			__insert_discard_tree(sbi, bdev, di.lstart, di.start,
							di.len, NULL, NULL);
		}
 next:
		prev_dc = next_dc;
		if (!prev_dc)
			break;

		node = rb_next(&prev_dc->rb_node);
		next_dc = rb_entry_safe(node, struct discard_cmd, rb_node);
	}
}

static int __queue_discard_cmd(struct f2fs_sb_info *sbi,
		struct block_device *bdev, block_t blkstart, block_t blklen)
{
	block_t lblkstart = blkstart;

	if (!f2fs_bdev_support_discard(bdev))
		return 0;

	trace_f2fs_queue_discard(bdev, blkstart, blklen);

	if (f2fs_is_multi_device(sbi)) {
		int devi = f2fs_target_device_index(sbi, blkstart);

		blkstart -= FDEV(devi).start_blk;
	}
	mutex_lock(&SM_I(sbi)->dcc_info->cmd_lock);
	__update_discard_tree_range(sbi, bdev, lblkstart, blkstart, blklen);
	mutex_unlock(&SM_I(sbi)->dcc_info->cmd_lock);
	return 0;
}

static unsigned int __issue_discard_cmd_orderly(struct f2fs_sb_info *sbi,
					struct discard_policy *dpolicy)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct discard_cmd *prev_dc = NULL, *next_dc = NULL;
	struct rb_node **insert_p = NULL, *insert_parent = NULL;
	struct discard_cmd *dc;
	struct blk_plug plug;
	unsigned int pos = dcc->next_pos;
	unsigned int issued = 0;
	bool io_interrupted = false;

	mutex_lock(&dcc->cmd_lock);
	dc = (struct discard_cmd *)f2fs_lookup_rb_tree_ret(&dcc->root,
					NULL, pos,
					(struct rb_entry **)&prev_dc,
					(struct rb_entry **)&next_dc,
					&insert_p, &insert_parent, true, NULL);
	if (!dc)
		dc = next_dc;

	blk_start_plug(&plug);

	while (dc) {
		struct rb_node *node;
		int err = 0;

		if (dc->state != D_PREP)
			goto next;

		/*if (dpolicy->io_aware && !is_idle(sbi, DISCARD_TIME)) {
			io_interrupted = true;
			break;
		}*/

		dcc->next_pos = dc->lstart + dc->len;
		err = __submit_discard_cmd(sbi, dpolicy, dc, &issued);

		if (issued >= dpolicy->max_requests)
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

	mutex_unlock(&dcc->cmd_lock);

	if (!issued && io_interrupted)
		issued = -1;

	return issued;
}
static unsigned int __wait_all_discard_cmd(struct f2fs_sb_info *sbi,
					struct discard_policy *dpolicy);

static int __issue_discard_cmd(struct f2fs_sb_info *sbi,
					struct discard_policy *dpolicy)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct list_head *pend_list;
	struct discard_cmd *dc, *tmp;
	struct blk_plug plug;
	int i, issued;
	bool io_interrupted = false;
	//static unsigned int issue_cnt = 0;
	//static unsigned int order_cnt = 0;
	//static unsigned int pend_cnt = 0;
	//int rtr = 0;
	//issue_cnt += 1;
	//printk("[JW DBG] %s: discard issue cnt: %u \n", __func__, issue_cnt);


	if (dpolicy->timeout)
		f2fs_update_time(sbi, UMOUNT_DISCARD_TIMEOUT);

retry:
	issued = 0;
	for (i = MAX_PLIST_NUM - 1; i >= 0; i--) {
	//	if (dpolicy->timeout &&
	//			f2fs_time_over(sbi, UMOUNT_DISCARD_TIMEOUT))
	//		break;

	//	if (i + 1 < dpolicy->granularity)
	//		break;

		if (i < DEFAULT_DISCARD_GRANULARITY && dpolicy->ordered)
			return  __issue_discard_cmd_orderly(sbi, dpolicy);
			//pend_cnt += issued;
			//printk("[JW DBG] %s: discard pend cnt: %u \n", __func__, pend_cnt);
			//order_cnt += rtr;
			//printk("[JW DBG] %s: discard order cnt: %u \n", __func__, order_cnt);
			//return rtr;
		if (i < DEFAULT_DISCARD_GRANULARITY && !(dpolicy->ordered))
			printk("%s: suspicious", __func__);
		pend_list = &dcc->pend_list[i];

		mutex_lock(&dcc->cmd_lock);
		if (list_empty(pend_list))
			goto next;
		if (unlikely(dcc->rbtree_check))
			f2fs_bug_on(sbi, !f2fs_check_rb_tree_consistence(sbi,
							&dcc->root, false));
		blk_start_plug(&plug);
		list_for_each_entry_safe(dc, tmp, pend_list, list) {
			f2fs_bug_on(sbi, dc->state != D_PREP);

			if (dpolicy->timeout &&
				f2fs_time_over(sbi, UMOUNT_DISCARD_TIMEOUT))
				break;

			/*if (dpolicy->io_aware && i < dpolicy->io_aware_gran &&
						!is_idle(sbi, DISCARD_TIME)) {
				io_interrupted = true;
				break;
			}*/

			//printk("[JW DBG] %s: submit discard lstart: %zu len: %zu devstart: %zu", __func__, dc->lstart, dc->len, dc->start);
			__submit_discard_cmd(sbi, dpolicy, dc, &issued);

			if (issued >= dpolicy->max_requests && dpolicy->type != DPOLICY_UMOUNT)
				break;
		}
		blk_finish_plug(&plug);
next:
		mutex_unlock(&dcc->cmd_lock);

		if (issued >= dpolicy->max_requests && dpolicy->type != DPOLICY_UMOUNT)// || io_interrupted)
			break;
	}

	if (dpolicy->type == DPOLICY_UMOUNT && issued) {
		__wait_all_discard_cmd(sbi, dpolicy);
		goto retry;
	}

	if (!issued && io_interrupted)
		issued = -1;

	//pend_cnt += issued;
	//printk("[JW DBG] %s: discard pend cnt: %u \n", __func__, pend_cnt);
	//printk("[JW DBG] %s: discard order cnt: %u \n", __func__, order_cnt);
	return issued;
}

static bool __drop_discard_cmd(struct f2fs_sb_info *sbi)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct list_head *pend_list;
	struct discard_cmd *dc, *tmp;
	int i;
	bool dropped = false;

	mutex_lock(&dcc->cmd_lock);
	for (i = MAX_PLIST_NUM - 1; i >= 0; i--) {
		pend_list = &dcc->pend_list[i];
		list_for_each_entry_safe(dc, tmp, pend_list, list) {
			f2fs_bug_on(sbi, dc->state != D_PREP);
			__remove_discard_cmd(sbi, dc);
			dropped = true;
		}
	}
	mutex_unlock(&dcc->cmd_lock);

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

	wait_for_completion_io(&dc->wait);
	mutex_lock(&dcc->cmd_lock);
	f2fs_bug_on(sbi, dc->state != D_DONE);
	dc->ref--;
	if (!dc->ref) {
		if (!dc->error)
			len = dc->len;
		__remove_discard_cmd(sbi, dc);
	}
	mutex_unlock(&dcc->cmd_lock);

	return len;
}

static void reflect_discard_cnt(struct f2fs_sb_info *sbi, block_t lstart, block_t len);

static unsigned int __wait_discard_cmd_range(struct f2fs_sb_info *sbi,
						struct discard_policy *dpolicy,
						block_t start, block_t end)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct list_head *wait_list = (dpolicy->type == DPOLICY_FSTRIM) ?
					&(dcc->fstrim_list) : &(dcc->wait_list);
	struct discard_cmd *dc, *tmp;
	bool need_wait;
	unsigned int trimmed = 0;
	block_t lstart_tmp, len_tmp;

next:
	need_wait = false;

	mutex_lock(&dcc->cmd_lock);
	list_for_each_entry_safe(dc, tmp, wait_list, list) {
		if (dc->lstart + dc->len <= start || end <= dc->lstart)
			continue;
		if (dc->len < dpolicy->granularity)
			continue;
		if (dc->state == D_DONE && !dc->ref) {
			wait_for_completion_io(&dc->wait);
			if (!dc->error)
				trimmed += dc->len;
			
#ifdef ASYNC_SECTION_FREE
			reflect_discard_cnt(sbi, dc->lstart, dc->len);
#endif			
			__remove_discard_cmd(sbi, dc);
		} else {
			dc->ref++;
			need_wait = true;
			break;
		}
	}
	mutex_unlock(&dcc->cmd_lock);

	if (need_wait) {
		lstart_tmp = dc->lstart;
		len_tmp = dc->len;
		
		trimmed += __wait_one_discard_bio(sbi, dc);
		
#ifdef ASYNC_SECTION_FREE
		reflect_discard_cnt(sbi, lstart_tmp, len_tmp);
#endif
		goto next;
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
	__init_discard_policy(sbi, &dp, DPOLICY_FSTRIM, 1);
	discard_blks = __wait_discard_cmd_range(sbi, &dp, 0, UINT_MAX);
	__init_discard_policy(sbi, &dp, DPOLICY_UMOUNT, 1);
	discard_blks += __wait_discard_cmd_range(sbi, &dp, 0, UINT_MAX);

	return discard_blks;
}

/* This should be covered by global mutex, &sit_i->sentry_lock */
static void f2fs_wait_discard_bio(struct f2fs_sb_info *sbi, block_t blkaddr)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct discard_cmd *dc;
	bool need_wait = false;

	mutex_lock(&dcc->cmd_lock);
	dc = (struct discard_cmd *)f2fs_lookup_rb_tree(&dcc->root,
							NULL, blkaddr);
	if (dc) {
		if (dc->state == D_PREP) {
			__punch_discard_cmd(sbi, dc, blkaddr);
		} else {
			dc->ref++;
			need_wait = true;
		}
	}
	mutex_unlock(&dcc->cmd_lock);

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

/* This comes from f2fs_put_super */
bool f2fs_issue_discard_timeout(struct f2fs_sb_info *sbi)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct discard_policy dpolicy;
	bool dropped;

	__init_discard_policy(sbi, &dpolicy, DPOLICY_UMOUNT,
					dcc->discard_granularity);
	__issue_discard_cmd(sbi, &dpolicy);
	dropped = __drop_discard_cmd(sbi);

	/* just to make sure there is no pending discard commands */
	__wait_all_discard_cmd(sbi, NULL);

	f2fs_bug_on(sbi, atomic_read(&dcc->discard_cmd_cnt));
	return dropped;
}

static int issue_discard_thread(void *data)
{
	struct f2fs_sb_info *sbi = data;
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	wait_queue_head_t *q = &dcc->discard_wait_queue;
	struct discard_policy dpolicy;
	unsigned int wait_ms = DEF_MIN_DISCARD_ISSUE_TIME;
	int issued;

	//set_freezable();

	do {
		__init_discard_policy(sbi, &dpolicy, DPOLICY_BG,
					dcc->discard_granularity);

		//wait_event_interruptible_timeout(*q,
		//		kthread_should_stop() || freezing(current) ||
		//		dcc->discard_wake,
		//		msecs_to_jiffies(wait_ms));
		
		wait_event_interruptible_timeout(*q,
				kthread_should_stop() ||
				dcc->discard_wake,
				msecs_to_jiffies(wait_ms));

		if (dcc->discard_wake)
			dcc->discard_wake = 0;

		/* clean up pending candidates before going to sleep */
		if (atomic_read(&dcc->queued_discard))
			__wait_all_discard_cmd(sbi, NULL);

		//if (try_to_freeze())
		//	continue;
		if (f2fs_readonly(sbi->sb)){
			printk("%s: unexp 1 !!", __func__);
			continue;
		}
		if (kthread_should_stop()) {
			printk("%s: unexp 2 !!", __func__);
			return 0;
		}
		if (is_sbi_flag_set(sbi, SBI_NEED_FSCK)) {
			wait_ms = dpolicy.max_interval;
			printk("%s: unexp 3 !!", __func__);
			continue;
		}

		if (sbi->gc_mode == GC_URGENT_HIGH)
			__init_discard_policy(sbi, &dpolicy, DPOLICY_FORCE, 1);

		sb_start_intwrite(sbi->sb);

		issued = __issue_discard_cmd(sbi, &dpolicy);
		if (issued > 0) {
			__wait_all_discard_cmd(sbi, &dpolicy);
			wait_ms = dpolicy.min_interval;
			//wait_ms = dpolicy.max_interval;
		} else if (issued == -1){
			wait_ms = f2fs_time_to_wait(sbi, DISCARD_TIME);
			if (!wait_ms)
				wait_ms = dpolicy.mid_interval;
		} else {
			wait_ms = dpolicy.max_interval;
		}

		sb_end_intwrite(sbi->sb);

#ifdef IPLFS_CALLBACK_IO
		if (atomic_read(&dcc->queued_discard) == 0 
				&& atomic_read(&dcc->discard_cmd_cnt) == 0) {
			//printk("%s: complete mg cmds", __func__);
			complete_migration_cmds(sbi);
		}
#endif

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

		if (sector & (bdev_zone_sectors(bdev) - 1) ||
				nr_sects != bdev_zone_sectors(bdev)) {
			f2fs_err(sbi, "(%d) %s: Unaligned zone reset attempted (block %x + %x)",
				 devi, sbi->s_ndevs ? FDEV(devi).path : "",
				 blkstart, blklen);
			return -EIO;
		}
		trace_f2fs_issue_reset_zone(bdev, blkstart);
		return blkdev_zone_mgmt(bdev, REQ_OP_ZONE_RESET,
					sector, nr_sects, GFP_NOFS);
	}

	/* For conventional zones, use regular discard if supported */
	return __queue_discard_cmd(sbi, bdev, lblkstart, blklen);
}
#endif

static int __issue_discard_async(struct f2fs_sb_info *sbi,
		struct block_device *bdev, block_t blkstart, block_t blklen)
{
#ifdef CONFIG_BLK_DEV_ZONED
	if (f2fs_sb_has_blkzoned(sbi) && bdev_is_zoned(bdev))
		return __f2fs_issue_discard_zone(sbi, bdev, blkstart, blklen);
#endif
	return __queue_discard_cmd(sbi, bdev, blkstart, blklen);
}

static int f2fs_issue_discard(struct f2fs_sb_info *sbi,
				block_t blkstart, block_t blklen)
{
	sector_t start = blkstart, len = 0;
	struct block_device *bdev;
	//struct seg_entry *se;
	//unsigned int offset;
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

		//se = get_seg_entry(sbi, GET_SEGNO(sbi, i));
		//offset = GET_BLKOFF_FROM_SEG0(sbi, i);

		//if (!f2fs_test_and_set_bit(offset, se->discard_map))
		//	sbi->discard_blks--;
	}

	if (len)
		err = __issue_discard_async(sbi, bdev, start, len);
	return err;
}

/*
static struct dynamic_discard_map* get_dynamic_discard_map(struct f2fs_sb_info *sbi,
	       						unsigned long long segno, int* height)
{
	struct dynamic_discard_map_control *ddmc = SM_I(sbi)->ddmc_info;
	struct rb_node **p, *parent = NULL;
	struct rb_entry *re;
	bool left_most;
	struct dynamic_discard_map* ddm;

	p = f2fs_lookup_pos_rb_tree_ext(sbi, &ddmc->root, &parent, segno, &left_most, height);
	
	re = rb_entry_safe(*p, struct rb_entry, rb_node);
	ddm = dynamic_discard_map(re, struct dynamic_discard_map, rbe);
	return ddm;
}
*/



static void __remove_dynamic_discard_map(struct f2fs_sb_info *sbi, struct dynamic_discard_map *ddm)
{
	struct dynamic_discard_map_control *ddmc = SM_I(sbi)->ddmc_info;
	atomic_dec(&ddmc->node_cnt);
	//printk("[JW DBG] %s: 1", __func__);
	//list_del(&ddm->list);
	//printk("[JW DBG] %s: 2", __func__);
	list_del(&ddm->history_list);
	list_del(&ddm->drange_journal_list);
	list_del(&ddm->dmap_journal_list);
	//printk("[JW DBG] %s: 3", __func__);
	//rb_erase_cached(&ddm->rbe.rb_node, &ddmc->root);
	hash_del(&ddm->hnode);
	kvfree(ddm->dc_map);
	kmem_cache_free(discard_map_slab, ddm);
}

static void remove_issued_discard_journals(struct f2fs_sb_info *sbi);
static void issue_all_discard_journals(struct f2fs_sb_info *sbi);

void issue_and_clean_all_ddm(struct f2fs_sb_info *sbi)
{
	struct dynamic_discard_map_control *ddmc = SM_I(sbi)->ddmc_info;
	struct dynamic_discard_map *ddm, *tmpddm;
	struct list_head *history_head_ddm = &ddmc->history_head;

	issue_all_discard_journals(sbi);
	list_for_each_entry_safe(ddm, tmpddm, history_head_ddm, history_list) {
        	__remove_dynamic_discard_map(sbi, ddm);
	}
	remove_issued_discard_journals(sbi);
}

//static bool add_discard_addrs(struct f2fs_sb_info *sbi, struct cp_control *cpc,
//							bool check_only)
//{
//	int entries = SIT_VBLOCK_MAP_SIZE / sizeof(unsigned long);
//	int max_blocks = sbi->blocks_per_seg;
//	struct seg_entry *se = get_seg_entry(sbi, cpc->trim_start);
//	unsigned long *cur_map = (unsigned long *)se->cur_valid_map;
//	unsigned long *ckpt_map = (unsigned long *)se->ckpt_valid_map;
//	unsigned long *discard_map = (unsigned long *)se->discard_map;
//	unsigned long *dmap = SIT_I(sbi)->tmp_map;
//	//unsigned long *ddmap;
//	unsigned int start = 0, end = -1;
//	bool force = (cpc->reason & CP_DISCARD);
//	struct discard_entry *de = NULL;
//	struct list_head *head = &SM_I(sbi)->dcc_info->entry_list;
//	int i;
//	//struct dynamic_discard_map *ddm;
//	//bool ddm_blk_exst = true;
//	//bool ori_blk_exst = true;
//	//unsigned int start_ddm = 0, end_ddm = -1;
//	//int height = 0;
//
//	if (force)
//		panic("add_discard_addrs: cpc_discard, FITRIM occurs!!!\n");
//
//	//ddm = get_dynamic_discard_map(sbi, (unsigned long long) cpc->trim_start, &height);
//	/*if (!ddm)
//		ddm_blk_exst = false;
//	else{
//		printk("add_discard_addrs: DDM Height is %d for segno %d\n", height, cpc->trim_start);
//		ddmap = (unsigned long *)ddm->dc_map;
//		start = __find_rev_next_bit(ddmap, max_blocks, end + 1);
//		if (start >= max_blocks){
//			ddm_blk_exst = false;
//			__remove_dynamic_discard_map(sbi, ddm);
//		}
//	}*/
//
//	if (se->valid_blocks == max_blocks || !f2fs_hw_support_discard(sbi)){
//		/*if (ddm_blk_exst){
//			__remove_dynamic_discard_map(sbi, ddm);
//			
//		}*/
//		
//		return false;
//	} 
//	if (!force) {
//		if (!f2fs_realtime_discard_enable(sbi) || !se->valid_blocks ||
//			SM_I(sbi)->dcc_info->nr_discards >=
//				SM_I(sbi)->dcc_info->max_discards){
//			//The condition !se->valid_blocks must be commented later. 
//			//My code should handle case empty segments. 
//			//Cuz I'll erase prefree segments issuing in clear_prefree_segments function. 
//			/*if (ddm_blk_exst){
//				__remove_dynamic_discard_map(sbi, ddm);
//			}*/
//			
//			return false;
//		}
//	}
//
//	
//	/* SIT_VBLOCK_MAP_SIZE should be multiple of sizeof(unsigned long) */
//	for (i = 0; i < entries; i++)
//		dmap[i] = force ? ~ckpt_map[i] & ~discard_map[i] :
//				(cur_map[i] ^ ckpt_map[i]) & ckpt_map[i];
//	
//	/* check existence of discarded block in original version dmap*/
//	//start = __find_rev_next_bit(dmap, max_blocks, end + 1);
//	
//	//if (start >= max_blocks)
//	//	ori_blk_exst = false;
//	//ori_blk_exst = !(start >= max_blocks);
//	//if (ddm_blk_exst != ori_blk_exst)
//	//	panic("add discard addrs: exst not match\n");
//		//printk("add discard addrs: exst not match\n");
//	//f2fs_bug_on(sbi, ddm_blk_exst != ori_blk_exst);
//
//	//if (!(ddm_blk_exst | ori_blk_exst))
//	/*if (!ddm_blk_exst)
//		return false;
//	*/
//	while (force || SM_I(sbi)->dcc_info->nr_discards <=
//				SM_I(sbi)->dcc_info->max_discards) {
//		start = __find_rev_next_bit(dmap, max_blocks, end + 1);
//		if (start >= max_blocks)
//			break;
//		//start = __find_rev_next_bit(ddmap, max_blocks, end_ddm + 1);
//
//		end = __find_rev_next_zero_bit(dmap, max_blocks, start + 1);
//		//end = __find_rev_next_zero_bit(ddmap, max_blocks, start_ddm +1);
//
//		/*if (!force){
//			if (start != start_ddm || end != end_ddm)
//				panic("start end not match in add_discard_addrs");
//				//printk("start end not match in add_discard_addrs");
//			//f2fs_bug_on(sbi, start != start_ddm || end != end_ddm);
//		}*/
//		if (force && start && end != max_blocks
//					&& (end - start) < cpc->trim_minlen)
//			continue;
//
//		if (check_only){
//			//__remove_dynamic_discard_map(sbi, ddm);
//			return true;
//		}
//		if (!de) {
//			de = f2fs_kmem_cache_alloc(discard_entry_slab,
//								GFP_F2FS_ZERO);
//			de->start_blkaddr = START_BLOCK(sbi, cpc->trim_start);
//			list_add_tail(&de->list, head);
//		}
//
//		for (i = start; i < end; i++)
//			__set_bit_le(i, (void *)de->discard_map);
//
//		SM_I(sbi)->dcc_info->nr_discards += end - start;
//	}
//	//__remove_dynamic_discard_map(sbi, ddm);
//	return false;
//}

static void release_discard_addr(struct discard_entry *entry)
{
	list_del(&entry->list);
	list_del(&entry->ddm_list);
	kmem_cache_free(discard_entry_slab, entry);
}


void f2fs_release_discard_addrs(struct f2fs_sb_info *sbi)
{
	struct list_head *head = &(SM_I(sbi)->dcc_info->entry_list);
	struct discard_entry *entry, *this;

	/* drop caches */
	list_for_each_entry_safe(entry, this, head, list)
		release_discard_addr(entry);
}

/*
 * Should call f2fs_clear_prefree_segments after checkpoint is done.
 */

static void set_prefree_as_free_segments(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	unsigned int segno;
	struct discard_cnt_info *dc_info = SM_I(sbi)->dcnt_info;
	unsigned int total_dce_cnt;

//	mutex_lock(&dirty_i->seglist_lock);
//	for_each_set_bit(segno, dirty_i->dirty_segmap[PRE], MAIN_SEGS_INTERVAL(sbi)) {
//		__set_test_and_free(sbi, segno, false);
//		if (test_and_clear_bit(segno, (dirty_i->dirty_segmap[PRE])))
//			dirty_i->nr_dirty[PRE]--;
//	}
//	mutex_unlock(&dirty_i->seglist_lock);
	
	mutex_lock(&dirty_i->seglist_lock);
	
#ifdef PRINT_DIRTY_TYPE_SEG
//	printk("%s: data seg: pre: %u ready: %u node seg: pre: %u ready: %u", __func__, 
//			dirty_i->nr_dirty[PRE], 
//			dirty_i->nr_dirty[READY], 
//			dirty_i->nr_dirty[PRE_NODE], 
//			dirty_i->nr_dirty[READY_NODE]
//			);
#endif

	spin_lock(&dc_info->lock);
	total_dce_cnt = dc_info->last_total_dce_cnt;
	spin_unlock(&dc_info->lock);
	if (total_dce_cnt == 0) {
		/* free segment by migration handler */
		for_each_set_bit(segno, dirty_i->dirty_segmap[READY], MAIN_SEGS_INTERVAL(sbi)) {
			__set_test_and_free(sbi, segno, false);
			if (test_and_clear_bit(segno, (dirty_i->dirty_segmap[READY])))
				dirty_i->nr_dirty[READY]--;
		}
		
		for_each_set_bit(segno, dirty_i->dirty_segmap[READY_NODE], MAIN_SEGS_INTERVAL(sbi)) {
			__set_test_and_free_node(sbi, segno, false);
			if (test_and_clear_bit(segno, (dirty_i->dirty_segmap[READY_NODE])))
				dirty_i->nr_dirty[READY_NODE]--;
		}
		//printk("%s: dce zero.", __func__);
	} else {
		//printk("%s: not exp... total dce cnt: %u", __func__, total_dce_cnt);
	}

	for_each_set_bit(segno, dirty_i->dirty_segmap[PRE], MAIN_SEGS_INTERVAL(sbi)) {
		if (test_and_clear_bit(segno, (dirty_i->dirty_segmap[PRE])))
			dirty_i->nr_dirty[PRE]--;
		if (test_and_set_bit(segno, (dirty_i->dirty_segmap[READY]))) {
			printk("%s: something weird. ", __func__);
		} else {
			dirty_i->nr_dirty[READY]++;
			//printk("%s: type: %u segno: %u READY", __func__, READY, segno);
		}
	}
	
	for_each_set_bit(segno, dirty_i->dirty_segmap[PRE_NODE], MAIN_SEGS_INTERVAL(sbi)) {
		if (test_and_clear_bit(segno, (dirty_i->dirty_segmap[PRE_NODE])))
			dirty_i->nr_dirty[PRE_NODE]--;
		if (test_and_set_bit(segno, (dirty_i->dirty_segmap[READY_NODE]))) {
			printk("%s: something weird. ", __func__);
		} else {
			dirty_i->nr_dirty[READY_NODE]++;
			//printk("%s: type: %u segno: %u READY_NODE", __func__, READY_NODE, segno);
		}
	}

	mutex_unlock(&dirty_i->seglist_lock);
}

//static void set_prefree_as_free_segments(struct f2fs_sb_info *sbi)
//{
//	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
//	unsigned int segno;
//
//	mutex_lock(&dirty_i->seglist_lock);
//	for_each_set_bit(segno, dirty_i->dirty_segmap[PRE], MAIN_SEGS(sbi))
//		__set_test_and_free(sbi, segno, false);
//	mutex_unlock(&dirty_i->seglist_lock);
//}

void f2fs_clear_prefree_segments(struct f2fs_sb_info *sbi,
						struct cp_control *cpc)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct dynamic_discard_map_control *ddmc = SM_I(sbi)->ddmc_info;
	//struct list_head *head = &dcc->entry_list;
	//struct discard_entry *entry, *this;
	////struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	//unsigned long *prefree_map = dirty_i->dirty_segmap[PRE];
	//unsigned int start = 0, end = -1;
	//unsigned int secno, start_segno;
	//bool force = (cpc->reason & CP_DISCARD);

	int tmp;
	tmp = (int) atomic_read(&dcc->discard_cmd_cnt );
	
#ifdef LM
	atomic_set(&ddmc->cur_inv_blk_cnt, 0);
#endif
	//printk("[JW DBG] %s: aft discard cmd count: %d \n", __func__, tmp);
	//printk("[JW DBG] %s: prev discard cmd count: %d \n", __func__, tmp);

	//bool need_align = f2fs_lfs_mode(sbi) && __is_large_section(sbi);

	/*
	mutex_lock(&dirty_i->seglist_lock);

	while (1) {
		int i;

		if (need_align && end != -1)
			end--;
		start = find_next_bit(prefree_map, MAIN_SEGS(sbi), end + 1);
		if (start >= MAIN_SEGS(sbi))
			break;
		end = find_next_zero_bit(prefree_map, MAIN_SEGS(sbi),
								start + 1);

		if (need_align) {
			start = rounddown(start, sbi->segs_per_sec);
			end = roundup(end, sbi->segs_per_sec);
		}

		for (i = start; i < end; i++) {
			if (test_and_clear_bit(i, prefree_map))
				dirty_i->nr_dirty[PRE]--;
		}

		if (!f2fs_realtime_discard_enable(sbi))
			continue;

		if (force && start >= cpc->trim_start &&
					(end - 1) <= cpc->trim_end)
				continue;

		if (!f2fs_lfs_mode(sbi) || !__is_large_section(sbi)) {
			f2fs_issue_discard(sbi, START_BLOCK(sbi, start),
				(end - start) << sbi->log_blocks_per_seg);
			continue;
		}
next:
		secno = GET_SEC_FROM_SEG(sbi, start);
		start_segno = GET_SEG_FROM_SEC(sbi, secno);
		if (!IS_CURSEC(sbi, secno) &&
			!get_valid_blocks(sbi, start, true))
			f2fs_issue_discard(sbi, START_BLOCK(sbi, start_segno),
				sbi->segs_per_sec << sbi->log_blocks_per_seg);

		start = start_segno + sbi->segs_per_sec;
		if (start < end)
			goto next;
		else
			end = start - 1;
	}
	mutex_unlock(&dirty_i->seglist_lock);
	*/
	/* send small discards */
	/*list_for_each_entry_safe(entry, this, head, list) {
		unsigned int cur_pos = 0, next_pos, len, total_len = 0;
		bool is_valid = test_bit_le(0, entry->discard_map);

find_next:
		if (is_valid) {
			next_pos = find_next_zero_bit_le(entry->discard_map,
					sbi->blocks_per_seg, cur_pos);
			len = next_pos - cur_pos;

			if (f2fs_sb_has_blkzoned(sbi) ||
			    (force && len < cpc->trim_minlen))
				goto skip;

			f2fs_issue_discard(sbi, entry->start_blkaddr + cur_pos,
									len);
			total_len += len;
		} else {
			next_pos = find_next_bit_le(entry->discard_map,
					sbi->blocks_per_seg, cur_pos);
		}
skip:
		cur_pos = next_pos;
		is_valid = !is_valid;

		if (cur_pos < sbi->blocks_per_seg)
			goto find_next;

		release_discard_addr(entry);
		dcc->nr_discards -= total_len;
	}*/
	//tmp = (int) atomic_read(&dcc->discard_cmd_cnt );
	//printk("[JW DBG] %s: aft discard cmd count: %d \n", __func__, tmp);

	//wake_up_discard_thread(sbi, false);
	//printk("%s: issue_all_discard_journals done. qd %d cd %d", __func__, 
	//		atomic_read(&dcc->queued_discard), 
	//		atomic_read(&dcc->discard_cmd_cnt));
#ifdef LM
	issue_all_discard_journals(sbi);
#endif
	//printk("%s: issue_all_discard_journals done. qd %d cd %d", __func__, 
	//		atomic_read(&dcc->queued_discard), 
	//		atomic_read(&dcc->discard_cmd_cnt));
	wake_up_discard_thread(sbi, true);
}

#ifdef IPLFS_CALLBACK_IO

static inline void init_slot_entry(struct slot_info *slot_i, struct slot_entry *slte, 
		uint64_t segno,  uint64_t slot_idx, unsigned int vblks)
{
	slte->segno = segno;
	slte->slot_idx = slot_idx;

	slte->written_blks = vblks;
	
	INIT_HLIST_NODE(&slte->hnode);
	INIT_LIST_HEAD(&slte->list);
	
	slot_i->total_slot_cnt ++;
}

static inline void set_slot_inuse(struct slot_info *slot_i, struct slot_entry *slte)
{
	//if (!list_empty(&slte->list))
	//	list_del(&slte->list);
	//	list_del(&slte->list);
//#ifdef SHIVAL_SUM
//	static int cnt = 0;
//	static uint64_t segno_stack[10];
//	static uint64_t slot_stack[10];
//	segno_stack[cnt] = slte->segno;
//	slot_stack[cnt] = slte->slot_idx;
//	cnt ++;
//	if (cnt == 10) {
//		printk("%s: segno: %lu slot_idx: %lu, \n \
//				segno: %lu slot_idx: %lu, \n \
//				segno: %lu slot_idx: %lu, \n \
//				segno: %lu slot_idx: %lu, \n \
//				segno: %lu slot_idx: %lu, \n \
//				segno: %lu slot_idx: %lu, \n \
//				segno: %lu slot_idx: %lu, \n \
//				segno: %lu slot_idx: %lu, \n \
//				segno: %lu slot_idx: %lu, \n \
//				segno: %lu slot_idx: %lu, \
//				", __func__,
//				segno_stack[0], slot_stack[0], 
//				segno_stack[1], slot_stack[1],
//				segno_stack[2], slot_stack[2],
//				segno_stack[3], slot_stack[3],
//				segno_stack[4], slot_stack[4],
//				segno_stack[5], slot_stack[5],
//				segno_stack[6], slot_stack[6],
//				segno_stack[7], slot_stack[7],
//				segno_stack[8], slot_stack[8],
//				segno_stack[9], slot_stack[9]
//				);
//		cnt = 0;
//	}
//#endif
	atomic_inc(&slot_i->inuse_slot_cnt);
	hash_add(slot_ht, &slte->hnode, slte->segno);
#ifdef SHIVAL
	//printk("%s: slot_idx: %u segno: %u", __func__, slte->slot_idx, slte->segno);
#endif
}

static inline void unset_slot_inuse(struct slot_info *slot_i, struct slot_entry *slte)
{
	//if (!list_empty(&slte->list))
	//	list_del(&slte->list);
//#ifdef SHIVAL_SUM
//	static int cnt = 0;
//	static uint64_t segno_stack[10];
//	static uint64_t slot_stack[10];
//	segno_stack[cnt] = slte->segno;
//	slot_stack[cnt] = slte->slot_idx;
//	cnt ++;
//	if (cnt == 10) {
//		printk("%s: segno: %lu slot_idx: %lu, \n \
//				segno: %lu slot_idx: %lu, \n \
//				segno: %lu slot_idx: %lu, \n \
//				segno: %lu slot_idx: %lu, \n \
//				segno: %lu slot_idx: %lu, \n \
//				segno: %lu slot_idx: %lu, \n \
//				segno: %lu slot_idx: %lu, \n \
//				segno: %lu slot_idx: %lu, \n \
//				segno: %lu slot_idx: %lu, \n \
//				segno: %lu slot_idx: %lu", __func__,
//				segno_stack[0], slot_stack[0], 
//				segno_stack[1], slot_stack[1],
//				segno_stack[2], slot_stack[2],
//				segno_stack[3], slot_stack[3],
//				segno_stack[4], slot_stack[4],
//				segno_stack[5], slot_stack[5],
//				segno_stack[6], slot_stack[6],
//				segno_stack[7], slot_stack[7],
//				segno_stack[8], slot_stack[8],
//				segno_stack[9], slot_stack[9]
//				);
//		cnt = 0;
//	}
//#endif
	atomic_dec(&slot_i->inuse_slot_cnt);
	hash_del(&slte->hnode);
	slte->written_blks = 0;
#ifdef SHIVAL
//	printk("%s: slot_idx: %u segno: %u", __func__, slte->slot_idx, slte->segno);
#endif
	slte->segno = NULL_SEGNO;
}

static inline void set_slot_free(struct slot_info *slot_i, struct slot_entry *slte)
{
#ifdef SHIVAL
//	static int cnt = 0;
//	static uint64_t segno_stack[10];
//	static uint64_t slot_stack[10];
//	segno_stack[cnt] = slte->segno;
//	slot_stack[cnt] = slte->slot_idx;
//	cnt ++;
//	if (cnt == 10) {
//		printk("%s: segno: %lu slot_idx: %lu, \
//				segno: %lu slot_idx: %lu, \
//				segno: %lu slot_idx: %lu, \
//				segno: %lu slot_idx: %lu, \
//				segno: %lu slot_idx: %lu, \
//				segno: %lu slot_idx: %lu, \
//				segno: %lu slot_idx: %lu, \
//				segno: %lu slot_idx: %lu, \
//				segno: %lu slot_idx: %lu, \
//				segno: %lu slot_idx: %lu, \
//				", __func__,
//				segno_stack[0], slot_stack[0], 
//				segno_stack[1], slot_stack[1],
//				segno_stack[2], slot_stack[2],
//				segno_stack[3], slot_stack[3],
//				segno_stack[4], slot_stack[4],
//				segno_stack[5], slot_stack[5],
//				segno_stack[6], slot_stack[6],
//				segno_stack[7], slot_stack[7],
//				segno_stack[8], slot_stack[8],
//				segno_stack[9], slot_stack[9]
//				);
//		cnt = 0;
//	}
#endif
	atomic_inc(&slot_i->free_slot_cnt);
	/* adding free slot into the head of list is important in terms of discarding by inplace update */
	list_add(&slte->list, &slot_i->free_list);
}

static inline void unset_slot_free(struct slot_info *slot_i, struct slot_entry *slte)
{
	atomic_dec(&slot_i->free_slot_cnt);
	list_del(&slte->list);
	INIT_LIST_HEAD(&slte->list);
}

static inline void set_slot_prefree(struct slot_info *slot_i, struct slot_entry *slte)
{
	/* depart if it is in prefree candidate list */
	if (!list_empty(&slte->list))
		list_del(&slte->list);

	atomic_inc(&slot_i->prefree_slot_cnt);
	list_add_tail(&slte->list, &slot_i->prefree_list);
}

static inline void unset_slot_prefree(struct slot_info *slot_i, struct slot_entry *slte)
{
	atomic_dec(&slot_i->prefree_slot_cnt);
	list_del(&slte->list);
}

static inline uint32_t get_new_slot(struct f2fs_sb_info *sbi, uint64_t segno)
{
    struct slot_info *slot_i = SLT_I(sbi);
	struct slot_entry *slte;

	if (atomic_read(&slot_i->free_slot_cnt) == 0) {
		int i;
		struct seg_entry *se;
		struct slot_entry *slte;
		printk("%s: free slot empty!!!!!!!!!!!!11", __func__);
		for (i = 0; i < MAIN_SEG_SLOTS(sbi); i ++) {
			slte = get_slot_entry(sbi, i);
			se = get_seg_entry(sbi, i);
			printk("%s: %dth slte_segno: %u slotno: %u written_blks: %u se_segno: %u vblks: %u type: %u", 
					__func__,
					i, slte->segno, slte->slot_idx, slte->written_blks, 
					se->segno, se->valid_blocks, se->type);
			//printk("%s: %dth se slotno: %u vblks: %u type: %u", __func__,
			//		i, se->segno, se->valid_blocks, se->type);
		}

	}

	f2fs_bug_on(sbi, atomic_read(&slot_i->free_slot_cnt) == 0);
	f2fs_bug_on(sbi, list_empty(&slot_i->free_list));

	slte = list_first_entry(&slot_i->free_list, struct slot_entry, list);
	unset_slot_free(slot_i, slte);

	slte->segno = segno;
	set_slot_inuse(slot_i, slte);
	return slte->slot_idx;
}

static inline struct slot_entry *lookup_slot_hash(uint64_t key)//, unsigned int *height)
{
	struct hlist_head *head = &slot_ht[hash_min(key, HASH_BITS(slot_ht))];
	struct slot_entry *slte;
	//*height = 0;

	hlist_for_each_entry(slte, head, hnode){
		//*height += 1;
		if (slte->segno == key)
			return slte;
	}
	return NULL;
}

//static inline uint32_t select_new_slot(struct f2fs_sb_info *sbi, uint64_t segno, uint64_t slot_idx)
//{
//    struct slot_info *slot_i = SLT_I(sbi);
//	struct slot_entry *slte;
//	 	
//	f2fs_bug_on(sbi, lookup_slot_hash(segno)!= NULL);
//	f2fs_bug_on(sbi, atomic_read(&slot_i->free_slot_cnt) == 0);
//	f2fs_bug_on(sbi, list_empty(&slot_i->free_list));
//	
//	slte = &slot_i->slot_entries[slot_idx];
//
//	unset_slot_free(slot_i, slte);
//	slte->segno = segno;
//	set_slot_inuse(slot_i, slte);
//
//	return slte->slot_idx;
//}

void clear_prefree_slots(struct f2fs_sb_info *sbi)
{
	struct slot_info *slot_i = SLT_I(sbi);
	struct slot_entry *slte, *tmp;
	static unsigned long long last_t = 0;
	unsigned long long cur_t;
	mutex_lock(&slot_i->lock);
	
	//printk("%s: bef flush prefree list", __func__);
	/* prefree to free */
	list_for_each_entry_safe(slte, tmp, &slot_i->prefree_list, list) {
		unset_slot_prefree(slot_i, slte);
		set_slot_free(slot_i, slte);
	}
	
	f2fs_bug_on(sbi, !list_empty(&slot_i->prefree_list));
	
	//printk("%s: aft flush prefree list", __func__);
	
	/* prefree candidate to free */
	list_for_each_entry_safe(slte, tmp, &slot_i->prefree_candidate_list, list) {
		//printk("%s: inloop. 1", __func__);
		list_del(&slte->list);
		INIT_LIST_HEAD(&slte->list);
		
		//printk("%s: inloop. 2", __func__);

		if (CURSEG_I(sbi, CURSEG_MIGRATION_DATA) == NULL || CURSEG_I(sbi, CURSEG_MIGRATION_NODE) == NULL)
			printk("%s: ERROR!!!!!!!!!!!!", __func__);
		
		if (slte->segno != CURSEG_I(sbi, CURSEG_MIGRATION_DATA)->segno 
				&& slte->segno != CURSEG_I(sbi, CURSEG_MIGRATION_NODE)->segno) {
			//printk("%s: inloop. 3-1", __func__);
			if (!hash_hashed(&slte->hnode))
				printk("%s: ERRR!!!!! slte->segno %u , slte->slot_idx %u , slte->written_blks %u",
						__func__, slte->segno, slte->slot_idx, slte->written_blks);
			unset_slot_inuse(slot_i, slte);
			//printk("%s: inloop. 3-2", __func__);
			set_slot_free(slot_i, slte);
			//printk("%s: inloop. 3-3", __func__);
		}
	}
	
	f2fs_bug_on(sbi, !list_empty(&slot_i->prefree_candidate_list));
#ifdef PRINT_META_SLOT	
	cur_t = OS_TimeGetUS();
	if (cur_t - last_t > 5000000) {
		printk("%s: slot util: %lu per. free %d / %u inuse %d / %u prefree: %d", __func__,
				100 * atomic_read(&slot_i->inuse_slot_cnt) / slot_i->total_slot_cnt,
				atomic_read(&slot_i->free_slot_cnt), slot_i->total_slot_cnt,
				atomic_read(&slot_i->inuse_slot_cnt), slot_i->total_slot_cnt,
				atomic_read(&slot_i->prefree_slot_cnt)
				);
		last_t = cur_t;
	}
#endif
	mutex_unlock(&slot_i->lock);
}

void clear_precompleted_mg_cmds(struct f2fs_sb_info *sbi)
{
	struct migration_control *mgc = SM_I(sbi)->mgc_info;
	struct mg_entry *mge, *tmp;
	
	spin_lock(&mgc->precompletion_lock);	
	list_for_each_entry_safe(mge, tmp, &mgc->precompletion_list, list) {
		/* race condition between discard thread */
		list_del(&mge->list);
		
		spin_lock(&mgc->completion_lock);	
		list_add_tail(&mge->list, &mgc->completion_list);
		spin_unlock(&mgc->completion_lock);	

	}
	spin_unlock(&mgc->precompletion_lock);	
}

void complete_migration_cmds(struct f2fs_sb_info *sbi)
{
	struct migration_control *mgc = SM_I(sbi)->mgc_info;
	struct mg_entry *mge, *tmp;

	//printk("%s!", __func__);
	spin_lock(&mgc->completion_lock);	
	
	list_for_each_entry_safe(mge, tmp, &mgc->completion_list, list) {
		list_del(&mge->list);
		//printk("%s: bef mge cid: %u", __func__,
		//		mge->command_id);
		complete_migration_cmd(sbi->q, mge->command_id, mge->nsid);
		//printk("%s: aft mge cid: %u", __func__,
		//		mge->command_id);
		kmem_cache_free(mg_entry_slab, mge);
		atomic_dec(&mgc->mg_entry_cnt_pre_comp);
	}

	spin_unlock(&mgc->completion_lock);	
}
#endif


static bool add_discard_journal(struct f2fs_sb_info *sbi, struct discard_journal_bitmap *dj_map)
{
        
        struct discard_entry *de = NULL;
        struct list_head *head = &SM_I(sbi)->dcc_info->entry_list;

        if (!f2fs_hw_support_discard(sbi)){
		panic("Why HW not support discard!!");
                return false;
        }
        if (!f2fs_realtime_discard_enable(sbi)){// || 
                //SM_I(sbi)->dcc_info->nr_discards >=
                //        SM_I(sbi)->dcc_info->max_discards){
                panic("Why discard not accepted?");
                return false;
        }


        de = f2fs_kmem_cache_alloc(discard_entry_slab,
        	                               GFP_F2FS_ZERO);

        
	de->start_blkaddr = le32_to_cpu(dj_map->start_blkaddr);
        list_add_tail(&de->list, head);
	memcpy(de->discard_map, dj_map->discard_map, DISCARD_BLOCK_MAP_SIZE);

	return true;		

}

int f2fs_recover_discard_journals(struct f2fs_sb_info *sbi)
{

	block_t start_blk, discard_journal_blocks, i, j;
	int err = 0;
	struct discard_entry *entry, *this;
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct list_head *head = &dcc->entry_list;
	start_blk = __start_cp_addr(sbi) +
		le32_to_cpu(F2FS_CKPT(sbi)->cp_pack_total_block_count);

	discard_journal_blocks = le32_to_cpu(F2FS_CKPT(sbi)->discard_journal_block_count);
	//printk("[JW DBG] %s: dj blocks %u when filling super \n", __func__, discard_journal_blocks);
	if (discard_journal_blocks == 0)
		return 1;

	f2fs_ra_meta_pages(sbi, start_blk, discard_journal_blocks, META_CP, true);

	for (i = 0; i < discard_journal_blocks; i++) {
		struct page *page;
		struct discard_journal_block *dj_blk;
		struct discard_journal_block_info *dj_blk_info;

		page = f2fs_get_meta_page(sbi, start_blk + i);
		if (IS_ERR(page)) {
			err = PTR_ERR(page);
			goto fail;
		}

		dj_blk = (struct discard_journal_block *)page_address(page);
		dj_blk_info = (struct discard_journal_block_info *)&dj_blk->dj_block_info;
		f2fs_bug_on(sbi, dj_blk_info->type != DJ_BLOCK_BITMAP);
	
		for (j = 0; j < le32_to_cpu(dj_blk_info->entry_cnt); j++) {
			struct discard_journal_bitmap *dj_map;

			dj_map = &dj_blk->bitmap_entries[j];
			if (!add_discard_journal(sbi, dj_map)){
				f2fs_put_page(page, 1);
				goto fail;
			}
		}
		f2fs_put_page(page, 1);
	}

	/*This part is modification of f2fs_clear_prefree_segments*/
	
	list_for_each_entry_safe(entry, this, head, list) {
		unsigned int cur_pos = 0, next_pos, len, total_len = 0;
		bool is_valid = test_bit_le(0, entry->discard_map);

find_next:
		if (is_valid) {
			next_pos = find_next_zero_bit_le(entry->discard_map,
					sbi->blocks_per_seg, cur_pos);
			len = next_pos - cur_pos;

			f2fs_issue_discard(sbi, entry->start_blkaddr + cur_pos,
									len);
			total_len += len;
		} else {
			next_pos = find_next_bit_le(entry->discard_map,
					sbi->blocks_per_seg, cur_pos);
		}

		cur_pos = next_pos;
		is_valid = !is_valid;

		if (cur_pos < sbi->blocks_per_seg)
			goto find_next;

		release_discard_addr(entry);
	}

	wake_up_discard_thread(sbi, true);

	return 1;
fail:
	panic("[JW DBG] %s: discard journal flushing error! \n", __func__);
}


static int create_dynamic_discard_map_control(struct f2fs_sb_info *sbi)
{
	struct dynamic_discard_map_control *ddmc;

	if (SM_I(sbi)->ddmc_info) {
		ddmc = SM_I(sbi)->ddmc_info;
		return 0;
	}

	ddmc = f2fs_kzalloc(sbi, sizeof(struct dynamic_discard_map_control), GFP_KERNEL);
        if (!ddmc)
                return -ENOMEM;

        mutex_init(&ddmc->ddm_lock);
        ddmc->root = RB_ROOT_CACHED;

	/*variable*/
	hash_init(ht);
	ddmc->ht = ht;
	ddmc->hbits = 7;
	//ddmc->ht_lkc_list = f2fs_kzalloc(sbi, sizeof(struct mutex)*pow(2, 7));
	//ddmc->segs_per_node = 300;  
	ddmc->segs_per_node = 512;  
	//ddmc->segs_per_node = 256;  
	//ddmc->segs_per_node = 1024;  
	
	ddmc->long_threshold = 512;	
	atomic_set(&ddmc->node_cnt, 0);
	atomic_set(&ddmc->cur_inv_blk_cnt, 0);
	atomic_set(&ddmc->dj_seg_cnt, 0);
	atomic_set(&ddmc->dj_range_cnt, 0);
	atomic_set(&ddmc->history_seg_cnt, 0);
	atomic_set(&ddmc->total_inv_blk_cnt, 0);
	atomic_set(&ddmc->total_val_blk_cnt, 0);
	INIT_LIST_HEAD(&ddmc->dirty_head);
	INIT_LIST_HEAD(&ddmc->history_head);

	atomic_set(&ddmc->drange_entry_cnt, 0);
	INIT_LIST_HEAD(&ddmc->discard_range_head);
	INIT_LIST_HEAD(&ddmc->discard_map_head);
	INIT_LIST_HEAD(&ddmc->issued_discard_head);
	

	SM_I(sbi)->ddmc_info = ddmc;
	return 0;

}

static int create_discard_cmd_control(struct f2fs_sb_info *sbi)
{
	dev_t dev = sbi->sb->s_bdev->bd_dev;
	struct discard_cmd_control *dcc;
	int err = 0, i;

	if (SM_I(sbi)->dcc_info) {
		dcc = SM_I(sbi)->dcc_info;
		goto init_thread;
	}

	dcc = f2fs_kzalloc(sbi, sizeof(struct discard_cmd_control), GFP_KERNEL);
	if (!dcc)
		return -ENOMEM;

	dcc->discard_granularity = DEFAULT_DISCARD_GRANULARITY;
	INIT_LIST_HEAD(&dcc->entry_list);
	for (i = 0; i < MAX_PLIST_NUM; i++)
		INIT_LIST_HEAD(&dcc->pend_list[i]);
	INIT_LIST_HEAD(&dcc->wait_list);
	INIT_LIST_HEAD(&dcc->fstrim_list);
	mutex_init(&dcc->cmd_lock);
	atomic_set(&dcc->issued_discard, 0);
	atomic_set(&dcc->queued_discard, 0);
	atomic_set(&dcc->discard_cmd_cnt, 0);
	dcc->nr_discards = 0;
	dcc->max_discards = MAIN_SEGS(sbi) << sbi->log_blocks_per_seg;
	dcc->undiscard_blks = 0;
	dcc->next_pos = 0;
	dcc->root = RB_ROOT_CACHED;
	dcc->rbtree_check = false;

	init_waitqueue_head(&dcc->discard_wait_queue);
	SM_I(sbi)->dcc_info = dcc;
init_thread:
	dcc->f2fs_issue_discard = kthread_run(issue_discard_thread, sbi,
				"f2fs_discard-%u:%u", MAJOR(dev), MINOR(dev));
	if (IS_ERR(dcc->f2fs_issue_discard)) {
		err = PTR_ERR(dcc->f2fs_issue_discard);
		kfree(dcc);
		SM_I(sbi)->dcc_info = NULL;
		return err;
	}

	return err;
}


static void destroy_dynamic_discard_map_control(struct f2fs_sb_info *sbi)
{
	struct dynamic_discard_map_control *ddmc = SM_I(sbi)->ddmc_info;

	if (!ddmc)
		return;

	//f2fs_stop_discard_thread(sbi);

	/*
	 * Recovery can cache discard commands, so in error path of
	 * fill_super(), it needs to give a chance to handle them.
	 */
	/*if (unlikely(atomic_read(&ddmcc->discard_cmd_cnt)))
		f2fs_issue_discard_timeout(sbi);
	*/
	kfree(ddmc);
	SM_I(sbi)->ddmc_info = NULL;
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
	if (unlikely(atomic_read(&dcc->discard_cmd_cnt)))
		f2fs_issue_discard_timeout(sbi);

	kfree(dcc);
	SM_I(sbi)->dcc_info = NULL;
}

static bool __mark_sit_entry_dirty(struct f2fs_sb_info *sbi, unsigned int segno)
{
	struct sit_info *sit_i = SIT_I(sbi);
	f2fs_bug_on(sbi, segno > MAIN_SEG_SLOTS(sbi));

	if (!__test_and_set_bit(segno, sit_i->dirty_sentries_bitmap)) {
		sit_i->dirty_sentries++;
		//printk("%s: segno: %u dirty_sentries: %u", __func__, segno, 
		//		sit_i->dirty_sentries);
		return false;
	}

	return true;
}

static void __set_sit_entry_type(struct f2fs_sb_info *sbi, int type,
					unsigned int segno, int modified)
{
	struct seg_entry *se = get_seg_entry(sbi, segno);
	se->type = type;
	//f2fs_bug_on(sbi, segno > MAIN_SEG_SLOTS(sbi));
	if (modified)
		__mark_sit_entry_dirty(sbi, segno);
}

//static inline unsigned long long get_segment_mtime(struct f2fs_sb_info *sbi,
//								block_t blkaddr)
//{
//	unsigned int segno = GET_SEGNO(sbi, blkaddr);
//
//	if (segno == NULL_SEGNO)
//		return 0;
//	return get_seg_entry(sbi, segno)->mtime;
//}

//static void update_segment_mtime(struct f2fs_sb_info *sbi, block_t blkaddr,
//						unsigned long long old_mtime)
//{
//	struct seg_entry *se;
//	unsigned int segno = GET_SEGNO(sbi, blkaddr);
//	unsigned long long ctime = get_mtime(sbi, false);
//	unsigned long long mtime = old_mtime ? old_mtime : ctime;
//	panic("update_segment_mtime(): this must not be executed");
//
//	if (segno == NULL_SEGNO)
//		return;
//
//	se = get_seg_entry(sbi, segno);
//
//	if (!se->mtime)
//		se->mtime = mtime;
//	else
//		se->mtime = div_u64(se->mtime * se->valid_blocks + mtime,
//						se->valid_blocks + 1);
//
//	if (ctime > SIT_I(sbi)->max_mtime)
//		SIT_I(sbi)->max_mtime = ctime;
//}


static struct dynamic_discard_map *__create_discard_map(struct f2fs_sb_info *sbi)
{
	struct dynamic_discard_map *ddm;
	//unsigned int count_down = SM_I(sbi)->ddmc_info->removal_count;
	unsigned int segs_per_ddm = SM_I(sbi)->ddmc_info->segs_per_node;

	ddm = f2fs_kmem_cache_alloc(discard_map_slab, GFP_NOFS);
	ddm->dc_map = f2fs_kvzalloc(sbi, SIT_VBLOCK_MAP_SIZE * segs_per_ddm, GFP_KERNEL);
	INIT_LIST_HEAD(&ddm->dirty_list);
	INIT_LIST_HEAD(&ddm->history_list);

	INIT_LIST_HEAD(&ddm->drange_journal_list);
	INIT_LIST_HEAD(&ddm->dmap_journal_list);
	
	atomic_set(&ddm->is_dirty, 0);
	//atomic_set(&ddm->remove_cnt_down, count_down);	
	//hash
	INIT_HLIST_NODE(&ddm->hnode);
	return ddm;
	
}

static struct dynamic_discard_map *f2fs_lookup_hash(struct f2fs_sb_info *sbi,  
					unsigned long long key, unsigned int *height)
{
	struct hlist_head *head = &ht[hash_min(key, HASH_BITS(ht))];
	struct dynamic_discard_map *ddm;
	*height = 0;

	hlist_for_each_entry(ddm, head, hnode){
		*height += 1;
		
		if (ddm->key == key)
			return ddm;
	}
	return NULL;
}

static void get_ddm_info(struct f2fs_sb_info *sbi, unsigned int segno, unsigned int offset, 
			unsigned long long *p_ddmkey, unsigned int *p_offset)
{
	unsigned int segs_per_ddm = SM_I(sbi)->ddmc_info->segs_per_node;
	unsigned int blocks_per_seg = sbi->blocks_per_seg;
	unsigned int start_segno;
	unsigned int delta_segno;	
	*p_ddmkey = (unsigned long long) segno/segs_per_ddm;
	start_segno = (*p_ddmkey) * segs_per_ddm;
	delta_segno = segno - start_segno;
	*p_offset = offset + (delta_segno) * blocks_per_seg;
	if (segno/segs_per_ddm - (int)(segno/segs_per_ddm))
		panic("%s: float in key!!", __func__);
}

static void update_dynamic_discard_map(struct f2fs_sb_info *sbi, unsigned int segno,
	       					unsigned int offset, int del)
{
	struct dynamic_discard_map_control *ddmc = SM_I(sbi)->ddmc_info;
	//struct hlist_head *ht = ddmc->ht;
	struct dynamic_discard_map *ddm;
	unsigned long long ddmkey;
	unsigned int offset_in_ddm;
	unsigned int height;
	struct list_head *dirty_head = &ddmc->dirty_head;
	struct list_head *history_head = &ddmc->history_head;

	get_ddm_info(sbi, segno, offset, &ddmkey, &offset_in_ddm);
	
	ddm = f2fs_lookup_hash(sbi, ddmkey, &height);


	//printk("update_ddm_hash: height is %d\n", height);
	if (del < 0) {
		//if (segno == GET_SEGNO(sbi, 37120) && offset == (GET_BLKOFF_FROM_SEG0(sbi, 37120)))
		//	printk("[JW DBG] %s: 37120 is added to ddm!!\n", __func__);
		if (!ddm){
			/*not exist, so create it*/
			ddm = __create_discard_map(sbi);
			if (ddm == 0)
				panic("__create_discard_map failed");
			ddm->key = ddmkey;
			//if (ddmkey == 0){
				//printk("[JW DBG] %s: ddm node with ddmkey 0 created by segno: %u, offset: %u!!\n", __func__, segno, offset);
			//}
			//hash_add(ddmc->ht, &ddm->hnode, ddmkey);
			hash_add(ht, &ddm->hnode, ddmkey);
			//list_add_tail(&ddm->list, head);
			list_add_tail(&ddm->history_list, history_head);
			atomic_inc(&ddmc->node_cnt);
			//printk("[JW DBG] %s: ddm created with ddmkey %u!!\n", __func__, ddmkey);
		}

		if (atomic_read(&ddm->is_dirty) == 0){
			atomic_set(&ddm->is_dirty, 1);
			list_add_tail(&ddm->dirty_list, dirty_head);
			//printk("[JW DBG] %s: ddm added to dirty list %u!!\n", __func__, ddmkey);
		}

		f2fs_test_and_set_bit(offset_in_ddm, ddm->dc_map);
		atomic_inc(&ddmc->cur_inv_blk_cnt);
		atomic_inc(&ddmc->total_inv_blk_cnt);
			
		return;
	}

	if (del > 0) {
		atomic_inc(&ddmc->total_val_blk_cnt);
		//if (segno == GET_SEGNO(sbi, 37120) && offset == (GET_BLKOFF_FROM_SEG0(sbi, 37120)))
		//	printk("[JW DBG] %s: 37120 is deleted in ddm!!\n", __func__);
		if (!ddm){
			return;	
		}
		if (atomic_read(&ddm->is_dirty) == 0){
			atomic_set(&ddm->is_dirty, 1);
			list_add_tail(&ddm->dirty_list, dirty_head);
			//printk("[JW DBG] %s: ddm added to dirty list %u!!\n", __func__, ddmkey);
		}
		if (f2fs_test_and_clear_bit(offset_in_ddm, ddm->dc_map)) {
			printk("%s: unexpected!!!!!!!!!!!", __func__);
			atomic_dec(&ddmc->cur_inv_blk_cnt);
		}
	}

}

/* sentry_lock and slot_info lock should be held before calling this. */
void update_slot_entry(struct f2fs_sb_info *sbi, uint64_t slot_idx, int del, unsigned int segno)
{
	struct seg_entry *se;
	long int new_vblocks;
	struct slot_entry *slte;
	struct slot_info *slot_i = SLT_I(sbi);

	se = get_seg_entry(sbi, slot_idx);
	new_vblocks = se->valid_blocks + del;
	
	if (segno != se->segno) {
		printk("%s: segentry corrupted. segno: %u se->segno: %u", __func__,
				segno, se->segno);
		f2fs_bug_on(sbi, 1);
	}

#ifdef SHIVAL_SUM
	if (new_vblocks < 0 ||
			(new_vblocks > f2fs_usable_blks_in_seg(sbi, segno))) {
		printk("%s: new_vblocks: %ld segno: %u segstartaddr: 0x%lx ori vblks: %u del: %d slot_idx: %lu, se segno: %u",
			 __func__, new_vblocks, segno, START_BLOCK(sbi, segno), se->valid_blocks, del, slot_idx,
			se->segno );
		int i_;
		struct seg_entry *se_;
		struct slot_entry *slte_;
		for (i_ = 0; i_ < MAIN_SEG_SLOTS(sbi); i_ ++) {
			slte_ = get_slot_entry(sbi, i_);
			se_ = get_seg_entry(sbi, i_);
			printk("%s: %dth slte_segno: %u slotno: %u written_blks: %u se_segno: %u vblks: %u type: %u", 
					__func__,
					i_, slte_->segno, slte_->slot_idx, slte_->written_blks, 
					se_->segno, se_->valid_blocks, se_->type);
			//printk("%s: %dth se slotno: %u vblks: %u type: %u", __func__,
			//		i, se->segno, se->valid_blocks, se->type);
		}

	}
#endif

	f2fs_bug_on(sbi, (new_vblocks < 0 ||
			(new_vblocks > f2fs_usable_blks_in_seg(sbi, segno))));
	
	if (new_vblocks < 0) {
		printk("%s: segno: %u slot_idx: %u se->segno: %u se_vblks: %u del: %d start_addr: 0x%lx", 
				__func__, segno, slot_idx, se->segno, se->valid_blocks, del, 
				START_BLOCK(sbi, segno));
	}
		   
	if (new_vblocks > f2fs_usable_blks_in_seg(sbi, segno)) {
		printk("%s: segno: %u slot_idx: %u se->segno: %u se_vblks: %u del: %d start_addr: 0x%lx usable_blks: %u", 
				__func__, segno, slot_idx, se->segno, se->valid_blocks, del, 
				START_BLOCK(sbi, segno), f2fs_usable_blks_in_seg(sbi, segno));
	}
	
	se->valid_blocks = new_vblocks;

	slte = get_slot_entry(sbi, slot_idx);
	
	if (del > 0) {
		slte->written_blks += del;
	}

	/* if valid blocks is zero, prefree slot index */
	if (se->valid_blocks == 0 && slte->written_blks == f2fs_usable_blks_in_seg(sbi, segno)) {
		//slte = get_slot_entry(sbi, slot_idx);
		f2fs_bug_on(sbi, slte == NULL);
		unset_slot_inuse(slot_i, slte);
		set_slot_prefree(slot_i, slte);
	
		se->segno = NULL_SEGNO;
	} 
	else if (se->valid_blocks == 0 && IS_MIGRATION_SEGNO(sbi, segno)) {
		f2fs_bug_on(sbi, !list_empty(&slte->list));	
		//0313 modify
		list_add_tail(&slte->list, &slot_i->prefree_candidate_list);
		//unset_slot_inuse(slot_i, slte);
		//set_slot_prefree(slot_i, slte);
	
		//se->segno = NULL_SEGNO;
	}

	/* prefree candidate case! */
	if (del > 0 && !list_empty(&slte->list)) {
		f2fs_bug_on(sbi, !IS_MIGRATION_SEGNO(sbi, segno));	
		f2fs_bug_on(sbi, se->valid_blocks == 0);
		/* remove from prefree candidate list */
		list_del(&slte->list);
		INIT_LIST_HEAD(&slte->list);
	}
}

static void update_sit_entry(struct f2fs_sb_info *sbi, block_t blkaddr, int del, 
		uint64_t *slot_idx)
{
	struct seg_entry *se;
	unsigned int segno, offset;
	//long int new_vblocks;
	//bool exist;
	//bool ddmhash = false;
#ifdef CONFIG_F2FS_CHECK_FS
	bool mir_exist;
#endif
#ifdef IPLFS_CALLBACK_IO
	struct slot_info *slot_i= SLT_I(sbi);
	struct slot_entry *slte = NULL;
#endif	

	segno = GET_SEGNO(sbi, blkaddr);
	offset = GET_BLKOFF_FROM_SEG0(sbi, blkaddr);

	/* TODO: don't try this if del > 0 */	
	mutex_lock(&SM_I(sbi)->ddmc_info->ddm_lock);
	update_dynamic_discard_map(sbi, segno, offset, del);
	mutex_unlock(&SM_I(sbi)->ddmc_info->ddm_lock);
	
#ifdef IPLFS_CALLBACK_IO
	mutex_lock(&slot_i->lock);
	
	/* translate segno into slot index */
	if (*slot_idx == NULL_SLOTNO) {
		/* get slot index of segno from hash table */
		slte = lookup_slot_hash(segno);
		if (slte == NULL) {
			printk("%s: blkaddr: 0x%lx segno: %u del: %d", __func__,
					blkaddr, segno, del);
			unsigned int tmp_ii;
			for (tmp_ii = 0; tmp_ii < MAIN_SEG_SLOTS(sbi); tmp_ii ++) {
				slte = get_slot_entry(sbi, tmp_ii);
				printk("%s: slot idx: %u slte slot_idx %lu segno: %lu", __func__, tmp_ii, 
						slte->slot_idx, slte->segno);
			}
			f2fs_bug_on(sbi, 1);
		}
		*slot_idx = slte->slot_idx;
	}

	update_slot_entry(sbi, *slot_idx, del, segno);
	
	mutex_unlock(&slot_i->lock);

#endif
	/* make seg_entry dirty for durability after next checkpoint */
	__mark_sit_entry_dirty(sbi, *slot_idx);

#ifdef SHIVAL
	static int cnt = 0;
	static uint64_t segno_stack[10];
	static uint64_t slot_stack[10];
	static uint64_t addr_stack[10];
	static int del_stack[10];
	if ( (blkaddr & 0xe0000000) == 0xe0000000) {
			printk("%s: segno: %lu addr: 0x%lx slot_idx: %lu del: %d ", __func__, 
					GET_SEGNO(sbi, blkaddr), blkaddr, *slot_idx, del);
	}
	
	//if ( (blkaddr & 0xe0000000) == 0xe0000000) {
	//	segno_stack[cnt] = GET_SEGNO(sbi, blkaddr);
	//	addr_stack[cnt] = blkaddr;
	//	slot_stack[cnt] = slot_idx;
	//	del_stack[cnt] = del;
	//	cnt ++;
	//	if (cnt == 10) {
	//		printk("%s: \n \
	//		segno: %lu addr: 0x%lx slot_idx: %lu del: %d \n \
	//		segno: %lu addr: 0x%lx slot_idx: %lu del: %d \n \
	//		segno: %lu addr: 0x%lx slot_idx: %lu del: %d \n \
	//		segno: %lu addr: 0x%lx slot_idx: %lu del: %d \n \
	//		segno: %lu addr: 0x%lx slot_idx: %lu del: %d \n \
	//		segno: %lu addr: 0x%lx slot_idx: %lu del: %d \n \
	//		segno: %lu addr: 0x%lx slot_idx: %lu del: %d \n \
	//		segno: %lu addr: 0x%lx slot_idx: %lu del: %d \n \
	//		segno: %lu addr: 0x%lx slot_idx: %lu del: %d \n \
	//		segno: %lu addr: 0x%lx slot_idx: %lu del: %d \
	//		", __func__,
	//		segno_stack[0], addr_stack[0], slot_stack[0], del_stack[0], 
	//		segno_stack[1], addr_stack[1], slot_stack[1], del_stack[1],
	//		segno_stack[2], addr_stack[2], slot_stack[2], del_stack[2],
	//		segno_stack[3], addr_stack[3], slot_stack[3], del_stack[3],
	//		segno_stack[4], addr_stack[4], slot_stack[4], del_stack[4],
	//		segno_stack[5], addr_stack[5], slot_stack[5], del_stack[5],
	//		segno_stack[6], addr_stack[6], slot_stack[6], del_stack[6],
	//		segno_stack[7], addr_stack[7], slot_stack[7], del_stack[7],
	//		segno_stack[8], addr_stack[8], slot_stack[8], del_stack[8],
	//		segno_stack[9], addr_stack[9], slot_stack[9], del_stack[9] 
	//		);
	//		cnt = 0;
	//	}
	//}
#endif
	
	/* Update valid block bitmap */
	/*if (del > 0) {
		exist = f2fs_test_and_set_bit(offset, se->cur_valid_map);
#ifdef CONFIG_F2FS_CHECK_FS
		mir_exist = f2fs_test_and_set_bit(offset,
						se->cur_valid_map_mir);
		if (unlikely(exist != mir_exist)) {
			f2fs_err(sbi, "Inconsistent error when setting bitmap, blk:%u, old bit:%d",
				 blkaddr, exist);
			f2fs_bug_on(sbi, 1);
		}
#endif
		if (unlikely(exist)) {
			f2fs_err(sbi, "Bitmap was wrongly set, blk:%u",
				 blkaddr);
			f2fs_bug_on(sbi, 1);
			se->valid_blocks--;
			del = 0;
		}

		if (!f2fs_test_and_set_bit(offset, se->discard_map))
			sbi->discard_blks--;

		//
		// SSR should never reuse block which is checkpointed
		// or newly invalidated.
		//
		if (!is_sbi_flag_set(sbi, SBI_CP_DISABLED)) {
			if (!f2fs_test_and_set_bit(offset, se->ckpt_valid_map))
				se->ckpt_valid_blocks++;
		}
	} else {
		exist = f2fs_test_and_clear_bit(offset, se->cur_valid_map);
#ifdef CONFIG_F2FS_CHECK_FS
		mir_exist = f2fs_test_and_clear_bit(offset,
						se->cur_valid_map_mir);
		if (unlikely(exist != mir_exist)) {
			f2fs_err(sbi, "Inconsistent error when clearing bitmap, blk:%u, old bit:%d",
				 blkaddr, exist);
			f2fs_bug_on(sbi, 1);
		}
#endif
		if (unlikely(!exist)) {
			f2fs_err(sbi, "Bitmap was wrongly cleared, blk:%u",
				 blkaddr);
			f2fs_bug_on(sbi, 1);
			se->valid_blocks++;
			del = 0;
		} else if (unlikely(is_sbi_flag_set(sbi, SBI_CP_DISABLED))) {
			//
			// If checkpoints are off, we must not reuse data that
			// was used in the previous checkpoint. If it was used
			// before, we must track that to know how much space we
			// really have.
			//
			if (f2fs_test_bit(offset, se->ckpt_valid_map)) {
				spin_lock(&sbi->stat_lock);
				sbi->unusable_block_count++;
				spin_unlock(&sbi->stat_lock);
			}
		}

		if (f2fs_test_and_clear_bit(offset, se->discard_map))
			sbi->discard_blks++;
	}
	if (!f2fs_test_bit(offset, se->ckpt_valid_map))
		se->ckpt_valid_blocks += del;

	*/
	/* update total number of valid blocks to be written in ckpt area */
	SIT_I(sbi)->written_valid_blocks += del;

	/*if (__is_large_section(sbi))
		get_sec_entry(sbi, segno)->valid_blocks += del;
	*/
}

void f2fs_invalidate_blocks(struct f2fs_sb_info *sbi, block_t addr)
{
	unsigned int segno = GET_SEGNO(sbi, addr);
	struct sit_info *sit_i = SIT_I(sbi);
	uint64_t slot_idx;

	f2fs_bug_on(sbi, addr == NULL_ADDR);
	if (addr == NEW_ADDR || addr == COMPRESS_ADDR)
		return;

	invalidate_mapping_pages(META_MAPPING(sbi), addr, addr);

	/* add it into sit main buffer */
	down_write(&sit_i->sentry_lock);

	//update_segment_mtime(sbi, addr, 0);
	slot_idx = NULL_SLOTNO;
	update_sit_entry(sbi, addr, -1, &slot_idx);

	/* add it into dirty seglist */
	locate_dirty_segment(sbi, segno, slot_idx);

	up_write(&sit_i->sentry_lock);
}

bool f2fs_is_checkpointed_data(struct f2fs_sb_info *sbi, block_t blkaddr)
{
	struct sit_info *sit_i = SIT_I(sbi);
	unsigned int segno, offset;
	struct seg_entry *se;
	bool is_cp = false;

	if (!__is_valid_data_blkaddr(blkaddr))
		return true;

	down_read(&sit_i->sentry_lock);

	segno = GET_SEGNO(sbi, blkaddr);
	se = get_seg_entry(sbi, segno);
	offset = GET_BLKOFF_FROM_SEG0(sbi, blkaddr);

	/* TODO: need to change by using last checkpoint write pointer */
	//if (f2fs_test_bit(offset, se->ckpt_valid_map))
	//	is_cp = true;

	up_read(&sit_i->sentry_lock);

	return is_cp;
}

/*
 * Calculate the number of current summary pages for writing
 */
int f2fs_npages_for_summary_flush(struct f2fs_sb_info *sbi, bool for_ra)
{
	int valid_sum_count = 0;
	int i, sum_in_page;

	for (i = CURSEG_HOT_DATA; i <= CURSEG_COLD_DATA; i++) {
		if (sbi->ckpt->alloc_type[i] == SSR)
			valid_sum_count += sbi->blocks_per_seg;
		else {
			if (for_ra)
				valid_sum_count += le16_to_cpu(
					F2FS_CKPT(sbi)->cur_data_blkoff[i]);
			else
				valid_sum_count += curseg_blkoff(sbi, i);
		}
	}

	sum_in_page = (PAGE_SIZE - 2 * SUM_JOURNAL_SIZE -
			SUM_FOOTER_SIZE) / SUMMARY_SIZE;
	if (valid_sum_count <= sum_in_page)
		return 1;
	else if ((valid_sum_count - sum_in_page) <=
		(PAGE_SIZE - SUM_FOOTER_SIZE) / SUMMARY_SIZE)
		return 2;
	return 3;
}

/*
 * Caller should put this summary page
 */
struct page *f2fs_get_sum_page(struct f2fs_sb_info *sbi, unsigned int segno)
{
	if (unlikely(f2fs_cp_error(sbi)))
		return ERR_PTR(-EIO);
	return f2fs_get_meta_page_retry(sbi, GET_SUM_BLOCK(sbi, segno));
}

void f2fs_update_meta_page(struct f2fs_sb_info *sbi,
					void *src, block_t blk_addr)
{
	struct page *page = f2fs_grab_meta_page(sbi, blk_addr);

	memcpy(page_address(page), src, PAGE_SIZE);
	set_page_dirty(page);
	f2fs_put_page(page, 1);
}

static void write_sum_page(struct f2fs_sb_info *sbi,
			struct f2fs_summary_block *sum_blk, block_t blk_addr)
{
	f2fs_update_meta_page(sbi, (void *)sum_blk, blk_addr);
}

static void write_current_sum_page(struct f2fs_sb_info *sbi,
						int type, block_t blk_addr)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	struct page *page = f2fs_grab_meta_page(sbi, blk_addr);
	struct f2fs_summary_block *src = curseg->sum_blk;
	struct f2fs_summary_block *dst;

	dst = (struct f2fs_summary_block *)page_address(page);
	memset(dst, 0, PAGE_SIZE);

	mutex_lock(&curseg->curseg_mutex);

	down_read(&curseg->journal_rwsem);
	memcpy(&dst->journal, curseg->journal, SUM_JOURNAL_SIZE);
	up_read(&curseg->journal_rwsem);

	memcpy(dst->entries, src->entries, SUM_ENTRY_SIZE);
	memcpy(&dst->footer, &src->footer, SUM_FOOTER_SIZE);

	mutex_unlock(&curseg->curseg_mutex);

	set_page_dirty(page);
	f2fs_put_page(page, 1);
}

//static int is_next_segment_free(struct f2fs_sb_info *sbi,
//				struct curseg_info *curseg, int type)
//{
//	unsigned int segno = curseg->segno + 1;
//	struct free_segmap_info *free_i = FREE_I(sbi);
//
//	if (segno < MAIN_SEGS(sbi) && segno % sbi->segs_per_sec)
//		return !test_bit(segno, free_i->free_segmap);
//	return 0;
//}

//for IF LBA. calculate rightmost zoneno and return zoneno + 1
//static unsigned int get_free_zone(struct f2fs_sb_info *sbi)
//{
//	int i;
//	unsigned int zone = 0;
//	unsigned int total_zones = MAIN_SECS(sbi) / sbi->secs_per_zone;
//
//	down_read(&SM_I(sbi)->curseg_zone_lock);
//	for (i = 0; i < NR_CURSEG_TYPE; i++)
//		zone = max(zone, CURSEG_I(sbi, i)->zone);
//	up_read(&SM_I(sbi)->curseg_zone_lock);
//	//if (zone + 1 > total_zones)
//	//	printk("get_free_zone: new zone %d is out of total zone %d",zone + 1, total_zones );
//	return zone + 1;
//}

static unsigned int get_free_zone_in_superzone(struct f2fs_sb_info *sbi, int type)
{
	unsigned int zone, new_zone, szone;
	//unsigned int total_zones = MAIN_SECS(sbi) / sbi->secs_per_zone;

	down_read(&SM_I(sbi)->curseg_zone_lock);
	zone = CURSEG_I(sbi, type)->zone;
	szone = GET_SUPERZONE_FROM_ZONE(sbi, zone);

	up_read(&SM_I(sbi)->curseg_zone_lock);
	if (type >= NR_PERSISTENT_LOG){
		printk("%s: ?????????", __func__);
		f2fs_bug_on(sbi, 1);
	}
	new_zone = zone + 1;
	return new_zone;
}

static void get_new_segment_IFLBA(struct f2fs_sb_info *sbi,
			unsigned int *newseg, bool new_sec, int type)
{
	unsigned int segno, secno, zoneno;
	unsigned int old_secno = GET_SEC_FROM_SEG(sbi, *newseg);
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	//if (curseg->segno == NULL_SEGNO)
	//	panic("%s: unexpected\n", __func__);
	//find next free segment in section
	if (!new_sec && ((*newseg + 1) % sbi->segs_per_sec)) {
		segno = *newseg + 1;
		if (segno < GET_SEG_FROM_SEC(sbi, old_secno + 1))
			panic("get_new_segment_IFLBA: must not be here\n");
		goto got_it;
	}
	//find to next section in zone. 
	if((old_secno + 1) % sbi->secs_per_zone ) {
		secno = old_secno + 1;
		segno = GET_SEG_FROM_SEC(sbi, secno);
		goto got_it;
	}
	//find next free zone
#if (SUPERZONE == 1)
	zoneno = get_free_zone_in_superzone(sbi, type);
#else
	f2fs_bug_on(sbi, 1);
	//zoneno = get_free_zone(sbi);
#endif
	//printk("%s: type: %d new zoneno: %d", __func__, type, zoneno );
	secno = zoneno * sbi->secs_per_zone;
	segno = secno * sbi->segs_per_sec;

got_it:
	/* set it as dirty segment in free segmap */
	//if (segno % 64 == 0){
	//	printk("%s: type: %d prevsegno: %d segno: %d zoneno: %d", __func__, type, curseg->segno, segno, GET_ZONE_FROM_SEG(sbi, segno));
	//}
	
	*newseg = segno;

	//printk("%s 0x%lx", __func__, START_BLOCK(sbi, segno));
	//if ((START_BLOCK(sbi, segno) >> 29) == CURSEG_COLD_DATA+1) {
	//	printk("%s: Why CURSEG_COLD_DATA?", __func__);
	//	dump_stack();
	//}

	//if (type == CURSEG_COLD_DATA) { 
	//	printk("%s: CURSEG_COLD_DATA!!", __func__);
	//}
	/*printk("%s: type %s new segno: %u blkaddr: 0x%x", __func__, 
			(type == CURSEG_HOT_DATA)? "HOT DATA" :
			(type == CURSEG_WARM_DATA)? "WARM DATA" :
			(type == CURSEG_COLD_DATA)? "COLD DATA" :
			(type == CURSEG_HOT_NODE)? "HOT NODE" :
			(type == CURSEG_WARM_NODE)? "WARM NODE" :
			(type == CURSEG_COLD_NODE)? "COLD NODE" :
			"Unknown Type",
			segno, START_BLOCK(sbi, segno));
	*/
}

//static void get_new_segment_interval(struct f2fs_sb_info *sbi,
//			unsigned int *newseg, int type)
//{
//	unsigned int segno, secno, zoneno;
//	unsigned int old_secno = GET_SEC_FROM_SEG(sbi, *newseg);
//	struct curseg_info *curseg = CURSEG_I(sbi, type);
//	//if (curseg->segno == NULL_SEGNO)
//	//	panic("%s: unexpected\n", __func__);
//	//find next free segment in section
//	if (!new_sec && ((*newseg + 1) % sbi->segs_per_sec)) {
//		segno = *newseg + 1;
//		if (segno < GET_SEG_FROM_SEC(sbi, old_secno + 1))
//			panic("get_new_segment_IFLBA: must not be here\n");
//		goto got_it;
//	}
//	//find to next section in zone. 
//	if((old_secno + 1) % sbi->secs_per_zone ) {
//		secno = old_secno + 1;
//		segno = GET_SEG_FROM_SEC(sbi, secno);
//		goto got_it;
//	}
//	//find next free zone
//#if (SUPERZONE == 1)
//	zoneno = get_free_zone_in_superzone(sbi, type);
//#else
//	f2fs_bug_on(sbi, 1);
//	//zoneno = get_free_zone(sbi);
//#endif
//	//printk("%s: type: %d new zoneno: %d", __func__, type, zoneno );
//	secno = zoneno * sbi->secs_per_zone;
//	segno = secno * sbi->segs_per_sec;
//
//got_it:
//	/* set it as dirty segment in free segmap */
//	
//	*newseg = segno;
//
//}

static void get_new_segment_interval_node(struct f2fs_sb_info *sbi,
			unsigned int *newseg)
{
	struct free_segmap_info *free_i = FREE_I(sbi);
	unsigned int segno, secno;
#ifdef SINGLE_INTERVAL
	unsigned int ori_segno = *newseg;
	*newseg = *newseg - sbi->START_SEGNO_INTERVAL_NODE;
#endif
	unsigned int hint = GET_SEC_FROM_SEG(sbi, *newseg);
	//static int print = 0;
	//static int first = 1, pcnt = 0;

	//if (pcnt < 50){
	//	printk("%s: hint: %u *newseg: %u ori_segno: %u", 
	//			__func__, hint, *newseg, ori_segno);
	//	pcnt ++;
	//}

	spin_lock(&free_i->segmap_lock);

	if ((*newseg + 1) % sbi->segs_per_sec) {
		segno = find_next_zero_bit(free_i->free_segmap_node,
			GET_SEG_FROM_SEC(sbi, hint + 1), *newseg + 1);
		if (segno < GET_SEG_FROM_SEC(sbi, hint + 1))
			goto got_it;
	}
find_other_zone:
	secno = find_next_zero_bit(free_i->free_secmap_node, MAIN_SECS_INTERVAL(sbi), hint);
	if (secno >= MAIN_SECS_INTERVAL(sbi)) {
		//printk("%s: PASS!!! *newseg: %u ori_segno: %u", __func__, *newseg, ori_segno);
		//print = 1;
		secno = find_next_zero_bit(free_i->free_secmap_node,
						MAIN_SECS_INTERVAL(sbi), 0);
		f2fs_bug_on(sbi, secno >= MAIN_SECS_INTERVAL(sbi));
	}
	
	segno = GET_SEG_FROM_SEC(sbi, secno);
	//if (print || first) {
	//	printk("%s: segno: %lu %lu print: %d first: %d", __func__, 
	//		segno, segno+1, print, first);
	//	//printk("%s: segno: %lu %lu last: %lu", __func__, 
	//	//	segno, segno+1, segno + sbi->segs_per_sec - 1);
	//	first = 0;
	//}

got_it:
	/* set it as dirty segment in free segmap */
	f2fs_bug_on(sbi, test_bit(segno, free_i->free_segmap_node));
	
	__set_inuse_node(sbi, segno);
#ifdef SINGLE_INTERVAL
	*newseg = segno + sbi->START_SEGNO_INTERVAL_NODE;
#else
	*newseg = segno;
#endif
	spin_unlock(&free_i->segmap_lock);
}

static void get_new_segment_interval(struct f2fs_sb_info *sbi,
			unsigned int *newseg)
{
	struct free_segmap_info *free_i = FREE_I(sbi);
	unsigned int segno, secno;
#ifdef SINGLE_INTERVAL
	unsigned int ori_segno = *newseg;
	*newseg = *newseg - sbi->START_SEGNO_INTERVAL;
#endif
	unsigned int hint = GET_SEC_FROM_SEG(sbi, *newseg);
	//static int print = 0;
	//static int first = 1, pcnt = 0;

	//if (pcnt < 50){
	//	printk("%s: hint: %u *newseg: %u ori_segno: %u", 
	//			__func__, hint, *newseg, ori_segno);
	//	pcnt ++;
	//}

	spin_lock(&free_i->segmap_lock);

	if ((*newseg + 1) % sbi->segs_per_sec) {
		segno = find_next_zero_bit(free_i->free_segmap,
			GET_SEG_FROM_SEC(sbi, hint + 1), *newseg + 1);
		if (segno < GET_SEG_FROM_SEC(sbi, hint + 1))
			goto got_it;
	}
find_other_zone:
	secno = find_next_zero_bit(free_i->free_secmap, MAIN_SECS_INTERVAL(sbi), hint);
	if (secno >= MAIN_SECS_INTERVAL(sbi)) {
		//printk("%s: PASS!!! *newseg: %u ori_segno: %u", __func__, *newseg, ori_segno);
		//print = 1;
		secno = find_next_zero_bit(free_i->free_secmap,
						MAIN_SECS_INTERVAL(sbi), 0);
		f2fs_bug_on(sbi, secno >= MAIN_SECS_INTERVAL(sbi));
	}
	
	segno = GET_SEG_FROM_SEC(sbi, secno);
	//if (print || first) {
	//	printk("%s: segno: %lu %lu print: %d first: %d", __func__, 
	//		segno, segno+1, print, first);
	//	//printk("%s: segno: %lu %lu last: %lu", __func__, 
	//	//	segno, segno+1, segno + sbi->segs_per_sec - 1);
	//	first = 0;
	//}

got_it:
	/* set it as dirty segment in free segmap */
	f2fs_bug_on(sbi, test_bit(segno, free_i->free_segmap));
	
	__set_inuse(sbi, segno);
#ifdef SINGLE_INTERVAL
	*newseg = segno + sbi->START_SEGNO_INTERVAL;
#else
	*newseg = segno;
#endif
	spin_unlock(&free_i->segmap_lock);
}


/*
 * Find a new segment from the free segments bitmap to right order
 * This function should be returned with success, otherwise BUG
 */
static void get_new_segment(struct f2fs_sb_info *sbi,
			unsigned int *newseg, bool new_sec, int dir)
{
	printk("%s: unexpected", __func__);
	f2fs_bug_on(sbi, 1);
	//struct free_segmap_info *free_i = FREE_I(sbi);
	//unsigned int segno, secno, zoneno;
	//unsigned int total_zones = MAIN_SECS(sbi) / sbi->secs_per_zone;
	//unsigned int hint = GET_SEC_FROM_SEG(sbi, *newseg);
	//unsigned int old_zoneno = GET_ZONE_FROM_SEG(sbi, *newseg);
	//unsigned int left_start = hint;
	//bool init = true;
	//int go_left = 0;
	//int i;

	//spin_lock(&free_i->segmap_lock);

	//if (!new_sec && ((*newseg + 1) % sbi->segs_per_sec)) {
	//	segno = find_next_zero_bit(free_i->free_segmap,
	//		GET_SEG_FROM_SEC(sbi, hint + 1), *newseg + 1);
	//	if (segno < GET_SEG_FROM_SEC(sbi, hint + 1))
	//		goto got_it;
	//}
//find_other_zone:
	//secno = find_next_zero_bit(free_i->free_secmap, MAIN_SECS(sbi), hint);
	//if (secno >= MAIN_SECS(sbi)) {
	//	if (dir == ALLOC_RIGHT) {
	//		secno = find_next_zero_bit(free_i->free_secmap,
	//						MAIN_SECS(sbi), 0);
	//		f2fs_bug_on(sbi, secno >= MAIN_SECS(sbi));
	//	} else {
	//		go_left = 1;
	//		left_start = hint - 1;
	//	}
	//}
	//if (go_left == 0)
	//	goto skip_left;

	//while (test_bit(left_start, free_i->free_secmap)) {
	//	if (left_start > 0) {
	//		left_start--;
	//		continue;
	//	}
	//	left_start = find_next_zero_bit(free_i->free_secmap,
	//						MAIN_SECS(sbi), 0);
	//	f2fs_bug_on(sbi, left_start >= MAIN_SECS(sbi));
	//	break;
	//}
	//secno = left_start;
//skip_left:
	//segno = GET_SEG_FROM_SEC(sbi, secno);
	//zoneno = GET_ZONE_FROM_SEC(sbi, secno);

	///* give up on finding another zone */
	//if (!init)
	//	goto got_it;
	//if (sbi->secs_per_zone == 1)
	//	goto got_it;
	//if (zoneno == old_zoneno)
	//	goto got_it;
	//if (dir == ALLOC_LEFT) {
	//	if (!go_left && zoneno + 1 >= total_zones)
	//		goto got_it;
	//	if (go_left && zoneno == 0)
	//		goto got_it;
	//}
	//for (i = 0; i < NR_CURSEG_TYPE; i++)
	//	if (CURSEG_I(sbi, i)->zone == zoneno)
	//		break;

	//if (i < NR_CURSEG_TYPE) {
	//	/* zone is in user, try another */
	//	if (go_left)
	//		hint = zoneno * sbi->secs_per_zone - 1;
	//	else if (zoneno + 1 >= total_zones)
	//		hint = 0;
	//	else
	//		hint = (zoneno + 1) * sbi->secs_per_zone;
	//	init = false;
	//	goto find_other_zone;
	//}
//got_it:
	///* set it as dirty segment in free segmap */
	//f2fs_bug_on(sbi, test_bit(segno, free_i->free_segmap));
	//__set_inuse(sbi, segno);
	//*newseg = segno;
	//spin_unlock(&free_i->segmap_lock);
}

static void reset_curseg(struct f2fs_sb_info *sbi, int type, int modified)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	struct summary_footer *sum_footer;
	unsigned short seg_type = curseg->seg_type;

	curseg->inited = true;
	curseg->segno = curseg->next_segno;

	down_write(&SM_I(sbi)->curseg_zone_lock);
	curseg->zone = GET_ZONE_FROM_SEG(sbi, curseg->segno);
	up_write(&SM_I(sbi)->curseg_zone_lock);

	curseg->next_blkoff = 0;
	curseg->next_segno = NULL_SEGNO;

	sum_footer = &(curseg->sum_blk->footer);
	memset(sum_footer, 0, sizeof(struct summary_footer));

	sanity_check_seg_type(sbi, seg_type);

	if (IS_DATASEG(seg_type))
		SET_SUM_TYPE(sum_footer, SUM_TYPE_DATA);
	if (IS_NODESEG(seg_type))
		SET_SUM_TYPE(sum_footer, SUM_TYPE_NODE);
	//if (IS_MIGRATION_SEG(seg_type))
	//	SET_SUM_TYPE(sum_footer, SUM_TYPE_MIGRATION);
	/* TODO: may need to consider for seg cnt */
	//__set_sit_entry_type(sbi, seg_type, curseg->segno, modified);
}

static unsigned int __get_next_segno(struct f2fs_sb_info *sbi, int type)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	unsigned short seg_type = curseg->seg_type;

	sanity_check_seg_type(sbi, seg_type);

	/* if segs_per_sec is large than 1, we need to keep original policy. */
	if (__is_large_section(sbi))
		return curseg->segno;

	/* inmem log may not locate on any segment after mount */
	if (!curseg->inited)
		return 0;

	if (unlikely(is_sbi_flag_set(sbi, SBI_CP_DISABLED)))
		return 0;

	if (test_opt(sbi, NOHEAP) &&
		(seg_type == CURSEG_HOT_DATA || IS_NODESEG(seg_type)))
		return 0;

	if (SIT_I(sbi)->last_victim[ALLOC_NEXT])
		return SIT_I(sbi)->last_victim[ALLOC_NEXT];

	/* find segments from 0 to reuse freed segments */
	if (F2FS_OPTION(sbi).alloc_mode == ALLOC_MODE_REUSE)
		return 0;

	return curseg->segno;
}

/*
 * Allocate a current working segment.
 * This function always allocates a free segment in LFS manner.
 */
static void new_curseg(struct f2fs_sb_info *sbi, int type, bool new_sec)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	unsigned short seg_type = curseg->seg_type;
	unsigned int segno = curseg->segno;
	int dir = ALLOC_LEFT;

	/*if (curseg->inited)
		write_sum_page(sbi, curseg->sum_blk,
				GET_SUM_BLOCK(sbi, segno));
	*/
	if (seg_type == CURSEG_WARM_DATA || seg_type == CURSEG_COLD_DATA)
		dir = ALLOC_RIGHT;

	if (test_opt(sbi, NOHEAP))
		dir = ALLOC_RIGHT;

	segno = __get_next_segno(sbi, type);
	get_new_segment(sbi, &segno, new_sec, dir);
	curseg->next_segno = segno;
	reset_curseg(sbi, type, 1);
	curseg->alloc_type = LFS;
}

static void new_curseg_IFLBA(struct f2fs_sb_info *sbi, int type, bool new_sec)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	//unsigned short seg_type = curseg->seg_type;
	unsigned int segno = curseg->segno;
#ifdef IPLFS_CALLBACK_IO	
	struct seg_entry *se;
//#ifdef SHIVAL
//	static int cnt = 0;
//	static uint64_t segno_stack[10];
//	static uint64_t slot_stack[10];
//#endif
	if (curseg->inited) {
		write_sum_page(sbi, curseg->sum_blk,
				GET_SUM_BLOCK(sbi, curseg->slot_idx));
//#ifdef SHIVAL
//		segno_stack[cnt] = curseg->segno;
//		slot_stack[cnt] = curseg->slot_idx;
//		cnt ++;
//		if (cnt == 10) {
//			printk("%s: segno: %lu slot_idx: %lu, \n \
//					segno: %lu slot_idx: %lu, \n \
//					segno: %lu slot_idx: %lu, \n \
//					segno: %lu slot_idx: %lu, \n \
//					segno: %lu slot_idx: %lu, \n \
//					segno: %lu slot_idx: %lu, \n \
//					segno: %lu slot_idx: %lu, \n \
//					segno: %lu slot_idx: %lu, \n \
//					segno: %lu slot_idx: %lu, \n \
//					segno: %lu slot_idx: %lu, \
//					", __func__,
//					segno_stack[0], slot_stack[0], 
//					segno_stack[1], slot_stack[1],
//					segno_stack[2], slot_stack[2],
//					segno_stack[3], slot_stack[3],
//					segno_stack[4], slot_stack[4],
//					segno_stack[5], slot_stack[5],
//					segno_stack[6], slot_stack[6],
//					segno_stack[7], slot_stack[7],
//					segno_stack[8], slot_stack[8],
//					segno_stack[9], slot_stack[9]
//					);
//			cnt = 0;
//		}
//#endif
	}
	else 
		printk("%s: why not write sum page!!! slot_idx: %u segno: %u", __func__,
				curseg->slot_idx, curseg->segno);
		//write_sum_page(sbi, curseg->sum_blk,
		//		GET_SUM_BLOCK(sbi, segno));
#endif	

	//segno = __get_next_segno(sbi, type);
	get_new_segment_IFLBA(sbi, &segno, new_sec, type);
	curseg->next_segno = segno;
	reset_curseg(sbi, type, 1);

#ifdef IPLFS_CALLBACK_IO	
	mutex_lock(&SLT_I(sbi)->lock);
	curseg->slot_idx = get_new_slot(sbi, curseg->segno);
	mutex_unlock(&SLT_I(sbi)->lock);

	se = get_seg_entry(sbi, curseg->slot_idx);
	se->segno = curseg->segno;

	/* need to consider for seg cnt */
	__set_sit_entry_type(sbi, curseg->seg_type, curseg->slot_idx, 1);
#endif

	curseg->alloc_type = LFS;
}

static void new_curseg_interval(struct f2fs_sb_info *sbi, int type)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	//unsigned short seg_type = curseg->seg_type;
	unsigned int segno = curseg->segno;
	if (segno == NULL_SEGNO){
		printk("%s: NULL SEGNO!!! type: %d", __func__, type);
		dump_stack();
	}
#ifdef IPLFS_CALLBACK_IO	
	struct seg_entry *se;
//#ifdef SHIVAL
//	static int cnt = 0;
//	static uint64_t segno_stack[10];
//	static uint64_t slot_stack[10];
//#endif
	if (curseg->inited) {
		write_sum_page(sbi, curseg->sum_blk,
				GET_SUM_BLOCK(sbi, curseg->slot_idx));
//#ifdef SHIVAL
//		segno_stack[cnt] = curseg->segno;
//		slot_stack[cnt] = curseg->slot_idx;
//		cnt ++;
//		if (cnt == 10) {
//			printk("%s: segno: %lu slot_idx: %lu, \n \
//					segno: %lu slot_idx: %lu, \n \
//					segno: %lu slot_idx: %lu, \n \
//					segno: %lu slot_idx: %lu, \n \
//					segno: %lu slot_idx: %lu, \n \
//					segno: %lu slot_idx: %lu, \n \
//					segno: %lu slot_idx: %lu, \n \
//					segno: %lu slot_idx: %lu, \n \
//					segno: %lu slot_idx: %lu, \n \
//					segno: %lu slot_idx: %lu, \
//					", __func__,
//					segno_stack[0], slot_stack[0], 
//					segno_stack[1], slot_stack[1],
//					segno_stack[2], slot_stack[2],
//					segno_stack[3], slot_stack[3],
//					segno_stack[4], slot_stack[4],
//					segno_stack[5], slot_stack[5],
//					segno_stack[6], slot_stack[6],
//					segno_stack[7], slot_stack[7],
//					segno_stack[8], slot_stack[8],
//					segno_stack[9], slot_stack[9]
//					);
//			cnt = 0;
//		}
//#endif
	}
	else 
		printk("%s: why not write sum page!!! slot_idx: %u segno: %u", __func__,
				curseg->slot_idx, curseg->segno);
		//write_sum_page(sbi, curseg->sum_blk,
		//		GET_SUM_BLOCK(sbi, segno));
#endif	

	//segno = __get_next_segno(sbi, type);
	if (IS_NODESEG(type))
		get_new_segment_interval_node(sbi, &segno);
	else if (IS_DATASEG(type))
		get_new_segment_interval(sbi, &segno);
	curseg->next_segno = segno;
	reset_curseg(sbi, type, 1);

	//printk("%s: slba: 0x%lx segno: %lu type: %u", __func__, START_BLOCK(sbi, curseg->segno), curseg->segno, type);

#ifdef IPLFS_CALLBACK_IO	
	mutex_lock(&SLT_I(sbi)->lock);
	curseg->slot_idx = get_new_slot(sbi, curseg->segno);
	mutex_unlock(&SLT_I(sbi)->lock);

	se = get_seg_entry(sbi, curseg->slot_idx);
	se->segno = curseg->segno;

	/* need to consider for seg cnt */
	__set_sit_entry_type(sbi, curseg->seg_type, curseg->slot_idx, 1);
#endif

	curseg->alloc_type = LFS;
}


//static void __next_free_blkoff(struct f2fs_sb_info *sbi,
//			struct curseg_info *seg, block_t start)
//{
//	struct seg_entry *se = get_seg_entry(sbi, seg->segno);
//	int entries = SIT_VBLOCK_MAP_SIZE / sizeof(unsigned long);
//	unsigned long *target_map = SIT_I(sbi)->tmp_map;
//	unsigned long *ckpt_map = (unsigned long *)se->ckpt_valid_map;
//	unsigned long *cur_map = (unsigned long *)se->cur_valid_map;
//	int i, pos;
//
//	for (i = 0; i < entries; i++)
//		target_map[i] = ckpt_map[i] | cur_map[i];
//
//	pos = __find_rev_next_zero_bit(target_map, sbi->blocks_per_seg, start);
//
//	seg->next_blkoff = pos;
//}

/*
 * If a segment is written by LFS manner, next block offset is just obtained
 * by increasing the current block offset. However, if a segment is written by
 * SSR manner, next block offset obtained by calling __next_free_blkoff
 */
static void __refresh_next_blkoff(struct f2fs_sb_info *sbi,
				struct curseg_info *seg)
{
	if (seg->alloc_type == SSR){
		printk("__refresh_next_blkoff(): why SSR? not expected\n");
		f2fs_bug_on(sbi, 1);
		//__next_free_blkoff(sbi, seg, seg->next_blkoff + 1);
	}
	else
		seg->next_blkoff++;
}

/*
 * This function always allocates a used segment(from dirty seglist) by SSR
 * manner, so it should recover the existing segment information of valid blocks
 */
static void change_curseg(struct f2fs_sb_info *sbi, int type, bool flush)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	unsigned int new_segno = curseg->next_segno;
	struct f2fs_summary_block *sum_node;
	struct page *sum_page;
	struct slot_entry *slte;
	struct sit_info *sit_i = SIT_I(sbi);
	struct slot_info *slot_i = SLT_I(sbi);
	uint64_t slot_idx;
#ifdef IPLFS_CALLBACK_IO	
	struct seg_entry *se;
#endif

	if (flush && curseg->segno != NULL_SEGNO) {
#ifdef SHIVAL
		if ( (START_BLOCK(sbi, curseg->segno) & 0xe0000000) == 0xe0000000) {
			printk("%s: write sum!! segno: %u slot_idx: %u saddr: 0x%llx", __func__, 
					curseg->segno, curseg->slot_idx, 
					START_BLOCK(sbi, curseg->segno) );
		}
#endif
		write_sum_page(sbi, curseg->sum_blk,
						GET_SUM_BLOCK(sbi, curseg->slot_idx));
	}
	//else if (IS_MIGRATION_SEG(type)) {
	//	printk("%s: dont know !!!! unexpected seg start addr: 0x%lx type: %d", __func__, 
	//			START_BLOCK(sbi, curseg->segno), type);
	//}
#ifdef SINGLE_INTERVAL	
	if (!IS_MIGRATION_SEGNO(sbi, new_segno)) {
		if (IS_NODESEG(type))
			__set_test_and_inuse_node(sbi, new_segno - sbi->START_SEGNO_INTERVAL_NODE);
		else if (IS_DATASEG(type))
			__set_test_and_inuse(sbi, new_segno - sbi->START_SEGNO_INTERVAL);
		else
			f2fs_bug_on(sbi, 1);

		mutex_lock(&dirty_i->seglist_lock);
		__remove_dirty_segment(sbi, new_segno, PRE);
		__remove_dirty_segment(sbi, new_segno, DIRTY);
		mutex_unlock(&dirty_i->seglist_lock);
	}
#endif
	/* TODO: May be used for seg count metadata */

	reset_curseg(sbi, type, 1);
	//curseg->alloc_type = SSR;
	//__next_free_blkoff(sbi, curseg, 0);

#ifdef IPLFS_CALLBACK_IO	
	down_write(&sit_i->sentry_lock);
	mutex_lock(&SLT_I(sbi)->lock);
	if ((slte = lookup_slot_hash(curseg->segno)) == NULL) { 
		slot_idx = get_new_slot(sbi, curseg->segno);
		slte = &slot_i->slot_entries[slot_idx];
		
		se = get_seg_entry(sbi, slot_idx);
		se->segno = curseg->segno;
	}
	
	__set_sit_entry_type(sbi, curseg->seg_type, slte->slot_idx, 1);
	
	curseg->slot_idx = slte->slot_idx;

	mutex_unlock(&SLT_I(sbi)->lock);
	up_write(&sit_i->sentry_lock);
#endif

	sum_page = f2fs_get_sum_page(sbi, curseg->slot_idx);
	if (IS_ERR(sum_page)) {
	  /* GC won't be able to use stale summary pages by cp_error */
		memset(curseg->sum_blk, 0, SUM_ENTRY_SIZE);
		printk("%s: unexpected!!!!!!!!!", __func__);
		return;
	}
	sum_node = (struct f2fs_summary_block *)page_address(sum_page);
	memcpy(curseg->sum_blk, sum_node, SUM_ENTRY_SIZE);
	f2fs_put_page(sum_page, 1);
	if (curseg->seg_type == CURSEG_MIGRATION_DATA) {
		//printk("%s: segno: %lu slot_idx: %lu seg_type: %d footer type: %d", __func__, 
		//			curseg->segno, curseg->slot_idx, 
		//			curseg->seg_type, curseg->sum_blk->footer.entry_type);
		if (curseg->sum_blk->footer.entry_type != SUM_TYPE_DATA) {
			printk("%s: something wrong!!!! segno: %lu seg_type: %d footer type: %d", __func__, 
					curseg->segno, 
					curseg->seg_type, curseg->sum_blk->footer.entry_type);
			f2fs_bug_on(sbi, 1);
		}
	}
	if (curseg->seg_type == CURSEG_MIGRATION_NODE) {
		//printk("%s: segno: %lu slot_idx: %lu seg_type: %d footer type: %d", __func__, 
		//			curseg->segno, curseg->slot_idx, 
		//			curseg->seg_type, curseg->sum_blk->footer.entry_type);
		if (curseg->sum_blk->footer.entry_type != SUM_TYPE_NODE) {
			printk("%s: something wrong!! segno: %lu seg_type: %d footer type: %d", __func__,  
					curseg->segno, 
					curseg->seg_type, curseg->sum_blk->footer.entry_type);
			f2fs_bug_on(sbi, 1);
		}
	}
	
}

static void reset_migration_curseg(struct f2fs_sb_info *sbi, int type, bool flush)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	unsigned int new_segno = curseg->next_segno;
	struct f2fs_summary_block *sum_node;
	struct page *sum_page;
	struct slot_entry *slte;
	struct sit_info *sit_i = SIT_I(sbi);
	struct slot_info *slot_i = SLT_I(sbi);
	uint64_t slot_idx;
#ifdef IPLFS_CALLBACK_IO	
	struct seg_entry *se;
#endif
	bool is_new_seg = false;

	if (flush && curseg->segno != NULL_SEGNO) {
#ifdef SHIVAL
		if ( (START_BLOCK(sbi, curseg->segno) & 0xe0000000) == 0xe0000000) {
			printk("%s: write sum!! segno: %u slot_idx: %u saddr: 0x%llx", __func__, 
					curseg->segno, curseg->slot_idx, 
					START_BLOCK(sbi, curseg->segno) );
		}
#endif
		write_sum_page(sbi, curseg->sum_blk,
						GET_SUM_BLOCK(sbi, curseg->slot_idx));
	}
	//else if (IS_MIGRATION_SEG(type)) {
	//	printk("%s: dont know !!!! unexpected seg start addr: 0x%lx type: %d", __func__, 
	//			START_BLOCK(sbi, curseg->segno), type);
	//}
#ifdef SINGLE_INTERVAL	
	if (!IS_MIGRATION_SEGNO(sbi, new_segno)) {
		if (IS_NODESEG(type))
			__set_test_and_inuse_node(sbi, new_segno - sbi->START_SEGNO_INTERVAL_NODE);
		else if (IS_DATASEG(type))
			__set_test_and_inuse(sbi, new_segno - sbi->START_SEGNO_INTERVAL);
		else
			f2fs_bug_on(sbi, 1);

		mutex_lock(&dirty_i->seglist_lock);
		__remove_dirty_segment(sbi, new_segno, PRE);
		__remove_dirty_segment(sbi, new_segno, DIRTY);
		mutex_unlock(&dirty_i->seglist_lock);
	}
#endif
	/* TODO: May be used for seg count metadata */

	reset_curseg(sbi, type, 1);
	//curseg->alloc_type = SSR;
	//__next_free_blkoff(sbi, curseg, 0);

#ifdef IPLFS_CALLBACK_IO	
	down_write(&sit_i->sentry_lock);
	mutex_lock(&SLT_I(sbi)->lock);
	if ((slte = lookup_slot_hash(curseg->segno)) == NULL) { 
		is_new_seg = true;
		slot_idx = get_new_slot(sbi, curseg->segno);
		slte = &slot_i->slot_entries[slot_idx];
		
		se = get_seg_entry(sbi, slot_idx);
		se->segno = curseg->segno;
	}
	
	__set_sit_entry_type(sbi, curseg->seg_type, slte->slot_idx, 1);
	
	curseg->slot_idx = slte->slot_idx;

	mutex_unlock(&SLT_I(sbi)->lock);
	up_write(&sit_i->sentry_lock);
#endif
	if (!is_new_seg) {
		//printk("%s: need to read sum page from disk segno: %lu saddr: 0x%llx", __func__, new_segno, 
		//		START_BLOCK(sbi, new_segno));
		sum_page = f2fs_get_sum_page(sbi, curseg->slot_idx);
		if (IS_ERR(sum_page)) {
		  /* GC won't be able to use stale summary pages by cp_error */
			memset(curseg->sum_blk, 0, SUM_ENTRY_SIZE);
			printk("%s: unexpected!!!!!!!!!", __func__);
			return;
		}
		sum_node = (struct f2fs_summary_block *)page_address(sum_page);
		memcpy(curseg->sum_blk, sum_node, SUM_ENTRY_SIZE);
		f2fs_put_page(sum_page, 1);
	} else {
		//printk("%s: fresh segno: %lu saddr: 0x%llx", __func__, new_segno, 
		//		START_BLOCK(sbi, new_segno));
	}
	
	if (curseg->seg_type == CURSEG_MIGRATION_DATA) {
		//printk("%s: segno: %lu slot_idx: %lu seg_type: %d footer type: %d", __func__, 
		//			curseg->segno, curseg->slot_idx, 
		//			curseg->seg_type, curseg->sum_blk->footer.entry_type);
		if (curseg->sum_blk->footer.entry_type != SUM_TYPE_DATA) {
			printk("%s: something wrong!!!! segno: %lu seg_type: %d footer type: %d", __func__, 
					curseg->segno, 
					curseg->seg_type, curseg->sum_blk->footer.entry_type);
			f2fs_bug_on(sbi, 1);
		}
	}
	if (curseg->seg_type == CURSEG_MIGRATION_NODE) {
		//printk("%s: segno: %lu slot_idx: %lu seg_type: %d footer type: %d", __func__, 
		//			curseg->segno, curseg->slot_idx, 
		//			curseg->seg_type, curseg->sum_blk->footer.entry_type);
		if (curseg->sum_blk->footer.entry_type != SUM_TYPE_NODE) {
			printk("%s: something wrong!! segno: %lu seg_type: %d footer type: %d", __func__,  
					curseg->segno, 
					curseg->seg_type, curseg->sum_blk->footer.entry_type);
			f2fs_bug_on(sbi, 1);
		}
	}
	
}
//#ifdef SHIVAL
//static void __change_curseg(struct f2fs_sb_info *sbi, int type, bool flush)
//{
//	//struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
//	struct curseg_info *curseg = CURSEG_I(sbi, type);
//	//unsigned int new_segno = curseg->next_segno;
//	struct f2fs_summary_block *sum_node;
//	struct page *sum_page;
//	struct slot_entry *slte;
//	struct sit_info *sit_i = SIT_I(sbi);
//	struct slot_info *slot_i = SLT_I(sbi);
//	uint64_t slot_idx;
//#ifdef IPLFS_CALLBACK_IO	
//	struct seg_entry *se;
//#endif
//
//	if (flush && curseg->segno != NULL_SEGNO)
//		write_sum_page(sbi, curseg->sum_blk,
//						GET_SUM_BLOCK(sbi, curseg->slot_idx));
//	
//			//__set_test_and_inuse(sbi, new_segno);
//	
//	/* TODO: May be used for seg count metadata */
//	//mutex_lock(&dirty_i->seglist_lock);
//	//__remove_dirty_segment(sbi, new_segno, PRE);
//	//__remove_dirty_segment(sbi, new_segno, DIRTY);
//	//mutex_unlock(&dirty_i->seglist_lock);
//
//	reset_curseg(sbi, type, 1);
//	//curseg->alloc_type = SSR;
//	//__next_free_blkoff(sbi, curseg, 0);
//
//#ifdef IPLFS_CALLBACK_IO	
//	down_write(&sit_i->sentry_lock);
//	mutex_lock(&SLT_I(sbi)->lock);
//#ifdef SHIVAL
//	bool is_new_slot = false;
//#endif
//	if ((slte = lookup_slot_hash(curseg->segno)) == NULL) { 
//		slot_idx = get_new_slot(sbi, curseg->segno);
//		slte = &slot_i->slot_entries[slot_idx];
//		
//		se = get_seg_entry(sbi, slot_idx);
//		se->segno = curseg->segno;
//#ifdef SHIVAL
//		is_new_slot = true;
//#endif
//	}
//	
//	__set_sit_entry_type(sbi, curseg->seg_type, slte->slot_idx, 1);
//	
//	curseg->slot_idx = slte->slot_idx;
//
//	mutex_unlock(&SLT_I(sbi)->lock);
//	up_write(&sit_i->sentry_lock);
//#endif
//
//	/* TODO: need to allocate new sum page if its new */
//	sum_page = f2fs_get_sum_page(sbi, curseg->slot_idx);
//	if (IS_ERR(sum_page)) {
//	  /* GC won't be able to use stale summary pages by cp_error */
//		memset(curseg->sum_blk, 0, SUM_ENTRY_SIZE);
//		printk("%s: unexpected!!!!!!!!!", __func__);
//		return;
//	}
//	sum_node = (struct f2fs_summary_block *)page_address(sum_page);
//#ifdef SHIVAL
//	if (is_new_slot) {
//		int tmp_ii;
//		struct f2fs_summary *entry_ = sum_node->entries;
//		for (tmp_ii = 0; tmp_ii < f2fs_usable_blks_in_seg(sbi, curseg->segno); tmp_ii ++, entry_ ++) {
//			entry_->nid = cpu_to_le32(0xdeadbeef);
//			entry_->ofs_in_node = cpu_to_le16(0xbeef);
//		}
//	}
//#endif
//	memcpy(curseg->sum_blk, sum_node, SUM_ENTRY_SIZE);
//	f2fs_put_page(sum_page, 1);
//	
//}
//#endif

static int get_ssr_segment(struct f2fs_sb_info *sbi, int type,
				int alloc_mode, unsigned long long age);

static void get_atssr_segment(struct f2fs_sb_info *sbi, int type,
					int target_type, int alloc_mode,
					unsigned long long age)
{
	printk("%s: not expected!!", __func__);
	f2fs_bug_on(sbi, 1);

	struct curseg_info *curseg = CURSEG_I(sbi, type);
	curseg->seg_type = target_type;

	if (get_ssr_segment(sbi, type, alloc_mode, age)) {
		struct seg_entry *se = get_seg_entry(sbi, curseg->next_segno);

		curseg->seg_type = se->type;
		change_curseg(sbi, type, true);
	} else {
		/* allocate cold segment by default */
		curseg->seg_type = CURSEG_COLD_DATA;
		new_curseg(sbi, type, true);
	}
	stat_inc_seg_type(sbi, curseg);
}

static void __f2fs_init_atgc_curseg(struct f2fs_sb_info *sbi)
{
	struct curseg_info *curseg = CURSEG_I(sbi, CURSEG_ALL_DATA_ATGC);

	if (!sbi->am.atgc_enabled)
		return;

	down_read(&SM_I(sbi)->curseg_lock);

	mutex_lock(&curseg->curseg_mutex);
	down_write(&SIT_I(sbi)->sentry_lock);

	get_atssr_segment(sbi, CURSEG_ALL_DATA_ATGC, CURSEG_COLD_DATA, SSR, 0);

	up_write(&SIT_I(sbi)->sentry_lock);
	mutex_unlock(&curseg->curseg_mutex);

	up_read(&SM_I(sbi)->curseg_lock);

}
void f2fs_init_inmem_curseg(struct f2fs_sb_info *sbi)
{
	__f2fs_init_atgc_curseg(sbi);
}

static void __f2fs_save_inmem_curseg(struct f2fs_sb_info *sbi, int type)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);

	mutex_lock(&curseg->curseg_mutex);
	if (!curseg->inited)
		goto out;

	//if (get_valid_blocks(sbi, curseg->segno, false)) {
		/*write_sum_page(sbi, curseg->sum_blk,
				GET_SUM_BLOCK(sbi, curseg->segno));*/
	//} else {
	/*
	if (!get_valid_blocks(sbi, curseg->segno, false)) {
		mutex_lock(&DIRTY_I(sbi)->seglist_lock);
		__set_test_and_free(sbi, curseg->segno, true);
		mutex_unlock(&DIRTY_I(sbi)->seglist_lock);
	}*/
out:
	mutex_unlock(&curseg->curseg_mutex);
}

void f2fs_save_inmem_curseg(struct f2fs_sb_info *sbi)
{
	__f2fs_save_inmem_curseg(sbi, CURSEG_COLD_DATA_PINNED);

	if (sbi->am.atgc_enabled)
		__f2fs_save_inmem_curseg(sbi, CURSEG_ALL_DATA_ATGC);
}

static void __f2fs_restore_inmem_curseg(struct f2fs_sb_info *sbi, int type)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);

	mutex_lock(&curseg->curseg_mutex);
	if (!curseg->inited)
		goto out;
	if (get_valid_blocks(sbi, curseg->segno, false))
		goto out;

	/*mutex_lock(&DIRTY_I(sbi)->seglist_lock);
	__set_test_and_inuse(sbi, curseg->segno);
	mutex_unlock(&DIRTY_I(sbi)->seglist_lock);
	*/
out:
	mutex_unlock(&curseg->curseg_mutex);
}

void f2fs_restore_inmem_curseg(struct f2fs_sb_info *sbi)
{
	__f2fs_restore_inmem_curseg(sbi, CURSEG_COLD_DATA_PINNED);

	if (sbi->am.atgc_enabled)
		__f2fs_restore_inmem_curseg(sbi, CURSEG_ALL_DATA_ATGC);
}

static int get_ssr_segment(struct f2fs_sb_info *sbi, int type,
				int alloc_mode, unsigned long long age)
{
//	struct curseg_info *curseg = CURSEG_I(sbi, type);
//	const struct victim_selection *v_ops = DIRTY_I(sbi)->v_ops;
//	unsigned segno = NULL_SEGNO;
//	unsigned short seg_type = curseg->seg_type;
//	int i, cnt;
//	bool reversed = false;
	printk("get_ssr_segment(): this should not be called except resize fs case.");
	f2fs_bug_on(sbi, 1);
//	sanity_check_seg_type(sbi, seg_type);
//
//	/* f2fs_need_SSR() already forces to do this */
//	if (!v_ops->get_victim(sbi, &segno, BG_GC, seg_type, alloc_mode, age)) {
//		curseg->next_segno = segno;
//		return 1;
//	}
//
//	/* For node segments, let's do SSR more intensively */
//	if (IS_NODESEG(seg_type)) {
//		if (seg_type >= CURSEG_WARM_NODE) {
//			reversed = true;
//			i = CURSEG_COLD_NODE;
//		} else {
//			i = CURSEG_HOT_NODE;
//		}
//		cnt = NR_CURSEG_NODE_TYPE;
//	} else {
//		if (seg_type >= CURSEG_WARM_DATA) {
//			reversed = true;
//			i = CURSEG_COLD_DATA;
//		} else {
//			i = CURSEG_HOT_DATA;
//		}
//		cnt = NR_CURSEG_DATA_TYPE;
//	}
//
//	for (; cnt-- > 0; reversed ? i-- : i++) {
//		if (i == seg_type)
//			continue;
//		if (!v_ops->get_victim(sbi, &segno, BG_GC, i, alloc_mode, age)) {
//			curseg->next_segno = segno;
//			return 1;
//		}
//	}
//
//	/* find valid_blocks=0 in dirty list */
//	if (unlikely(is_sbi_flag_set(sbi, SBI_CP_DISABLED))) {
//		segno = get_free_segment(sbi);
//		if (segno != NULL_SEGNO) {
//			curseg->next_segno = segno;
//			return 1;
//		}
//	}
	return 0;
}

/*
 * flush out current segment and replace it with new segment
 * This function should be returned with success, otherwise BUG
 */
/*
static void allocate_segment_by_default(struct f2fs_sb_info *sbi,
						int type, bool force)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);

	if (force)
		new_curseg(sbi, type, true);
	else if (!is_set_ckpt_flags(sbi, CP_CRC_RECOVERY_FLAG) &&
					curseg->seg_type == CURSEG_WARM_NODE)
		new_curseg(sbi, type, false);
	else if (curseg->alloc_type == LFS &&
			is_next_segment_free(sbi, curseg, type) &&
			likely(!is_sbi_flag_set(sbi, SBI_CP_DISABLED)))
		new_curseg(sbi, type, false);
	else if (f2fs_need_SSR(sbi) &&
			get_ssr_segment(sbi, type, SSR, 0))
		change_curseg(sbi, type, true);
	else
		new_curseg(sbi, type, false);

	stat_inc_seg_type(sbi, curseg);
}
*/

#ifdef INTERVAL_MANAGER
static void allocate_segment_in_interval(struct f2fs_sb_info *sbi,
						int type, bool force)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);

	new_curseg_interval(sbi, type);

	stat_inc_seg_type(sbi, curseg);
}
#endif

static void append_only_allocate_segment(struct f2fs_sb_info *sbi,
						int type, bool force)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);

	if (force)
		new_curseg_IFLBA(sbi, type, true);
	else
		new_curseg_IFLBA(sbi, type, false);

	stat_inc_seg_type(sbi, curseg);
}

//void f2fs_allocate_segment_for_resize(struct f2fs_sb_info *sbi, int type,
//					unsigned int start, unsigned int end)
//{
//	struct curseg_info *curseg = CURSEG_I(sbi, type);
//	unsigned int segno;
//
//	down_read(&SM_I(sbi)->curseg_lock);
//	mutex_lock(&curseg->curseg_mutex);
//	down_write(&SIT_I(sbi)->sentry_lock);
//
//	segno = CURSEG_I(sbi, type)->segno;
//	if (segno < start || segno > end)
//		goto unlock;
//
//	if (f2fs_need_SSR(sbi) && get_ssr_segment(sbi, type, SSR, 0))
//		change_curseg(sbi, type, true);
//	else
//		new_curseg(sbi, type, true);
//
//	stat_inc_seg_type(sbi, curseg);
//
//	//locate_dirty_segment(sbi, segno);
//unlock:
//	up_write(&SIT_I(sbi)->sentry_lock);
//
//	if (segno != curseg->segno)
//		f2fs_notice(sbi, "For resize: curseg of type %d: %u ==> %u",
//			    type, segno, curseg->segno);
//
//	mutex_unlock(&curseg->curseg_mutex);
//	up_read(&SM_I(sbi)->curseg_lock);
//}

static void __allocate_new_segment(struct f2fs_sb_info *sbi, int type)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	unsigned int old_segno;

	printk("%s: unexpected!!!", __func__);

	if (!curseg->inited)
		goto alloc;

	if (!curseg->next_blkoff)
		return;
	/*if (!curseg->next_blkoff &&
		!get_valid_blocks(sbi, curseg->segno, false) &&
		!get_ckpt_valid_blocks(sbi, curseg->segno))
		return;
	*/

alloc:
	old_segno = curseg->segno;
	SIT_I(sbi)->s_ops->allocate_segment(sbi, type, true);
	//locate_dirty_segment(sbi, old_segno);
}

void f2fs_allocate_new_segment(struct f2fs_sb_info *sbi, int type)
{
	down_write(&SIT_I(sbi)->sentry_lock);
	__allocate_new_segment(sbi, type);
	up_write(&SIT_I(sbi)->sentry_lock);
}

void f2fs_allocate_new_segments(struct f2fs_sb_info *sbi)
{
	int i;

	down_write(&SIT_I(sbi)->sentry_lock);
	for (i = CURSEG_HOT_DATA; i <= CURSEG_COLD_DATA; i++)
		__allocate_new_segment(sbi, i);
	up_write(&SIT_I(sbi)->sentry_lock);
}

/*
static const struct segment_allocation default_salloc_ops = {
	.allocate_segment = allocate_segment_by_default,
};
*/

static const struct segment_allocation IFLBA_salloc_ops = {
#ifdef INTERVAL_MANAGER
	.allocate_segment = allocate_segment_in_interval,
#else
	.allocate_segment = append_only_allocate_segment,
#endif
};

bool f2fs_exist_trim_candidates(struct f2fs_sb_info *sbi,
						struct cp_control *cpc)
{
	__u64 trim_start = cpc->trim_start;
	bool has_candidate = false;

	down_write(&SIT_I(sbi)->sentry_lock);
	for (; cpc->trim_start <= cpc->trim_end; cpc->trim_start++) {
		
		//if (add_discard_addrs(sbi, cpc, true)) {
		//	has_candidate = true;
		//	break;
		//}
	}
	up_write(&SIT_I(sbi)->sentry_lock);

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

next:
	issued = 0;

	mutex_lock(&dcc->cmd_lock);
	if (unlikely(dcc->rbtree_check))
		f2fs_bug_on(sbi, !f2fs_check_rb_tree_consistence(sbi,
							&dcc->root, false));

	dc = (struct discard_cmd *)f2fs_lookup_rb_tree_ret(&dcc->root,
					NULL, start,
					(struct rb_entry **)&prev_dc,
					(struct rb_entry **)&next_dc,
					&insert_p, &insert_parent, true, NULL);
	if (!dc)
		dc = next_dc;

	blk_start_plug(&plug);

	while (dc && dc->lstart <= end) {
		struct rb_node *node;
		int err = 0;

		if (dc->len < dpolicy->granularity)
			goto skip;

		if (dc->state != D_PREP) {
			list_move_tail(&dc->list, &dcc->fstrim_list);
			goto skip;
		}

		err = __submit_discard_cmd(sbi, dpolicy, dc, &issued);

		if (issued >= dpolicy->max_requests) {
			start = dc->lstart + dc->len;

			if (err)
				__remove_discard_cmd(sbi, dc);

			blk_finish_plug(&plug);
			mutex_unlock(&dcc->cmd_lock);
			trimmed += __wait_all_discard_cmd(sbi, NULL);
			congestion_wait(BLK_RW_ASYNC, DEFAULT_IO_TIMEOUT);
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
	mutex_unlock(&dcc->cmd_lock);

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
	unsigned long long trimmed = 0;
	int err = 0;
	bool need_align = f2fs_lfs_mode(sbi) && __is_large_section(sbi);
	
	printk("%s: not expected!!!!!!!", __func__);
	f2fs_bug_on(sbi, 1);

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
		start_segno = rounddown(start_segno, sbi->segs_per_sec);
		end_segno = roundup(end_segno + 1, sbi->segs_per_sec) - 1;
	}

	cpc.reason = CP_DISCARD;
	cpc.trim_minlen = max_t(__u64, 1, F2FS_BYTES_TO_BLK(range->minlen));
	cpc.trim_start = start_segno;
	cpc.trim_end = end_segno;

	cpc.excess_prefree = false;

	if (sbi->discard_blks == 0)
		goto out;

	down_write(&sbi->gc_lock);
	err = f2fs_write_checkpoint(sbi, &cpc);
	up_write(&sbi->gc_lock);
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

static bool __has_curseg_space(struct f2fs_sb_info *sbi,
					struct curseg_info *curseg)
{
	return curseg->next_blkoff < f2fs_usable_blks_in_seg(sbi,
							curseg->segno);
}

int f2fs_rw_hint_to_seg_type(enum rw_hint hint)
{
	switch (hint) {
	case WRITE_LIFE_SHORT:
		return CURSEG_HOT_DATA;
	case WRITE_LIFE_EXTREME:
		return CURSEG_COLD_DATA;
	default:
		return CURSEG_WARM_DATA;
	}
}

/* This returns write hints for each segment type. This hints will be
 * passed down to block layer. There are mapping tables which depend on
 * the mount option 'whint_mode'.
 *
 * 1) whint_mode=off. F2FS only passes down WRITE_LIFE_NOT_SET.
 *
 * 2) whint_mode=user-based. F2FS tries to pass down hints given by users.
 *
 * User                  F2FS                     Block
 * ----                  ----                     -----
 *                       META                     WRITE_LIFE_NOT_SET
 *                       HOT_NODE                 "
 *                       WARM_NODE                "
 *                       COLD_NODE                "
 * ioctl(COLD)           COLD_DATA                WRITE_LIFE_EXTREME
 * extension list        "                        "
 *
 * -- buffered io
 * WRITE_LIFE_EXTREME    COLD_DATA                WRITE_LIFE_EXTREME
 * WRITE_LIFE_SHORT      HOT_DATA                 WRITE_LIFE_SHORT
 * WRITE_LIFE_NOT_SET    WARM_DATA                WRITE_LIFE_NOT_SET
 * WRITE_LIFE_NONE       "                        "
 * WRITE_LIFE_MEDIUM     "                        "
 * WRITE_LIFE_LONG       "                        "
 *
 * -- direct io
 * WRITE_LIFE_EXTREME    COLD_DATA                WRITE_LIFE_EXTREME
 * WRITE_LIFE_SHORT      HOT_DATA                 WRITE_LIFE_SHORT
 * WRITE_LIFE_NOT_SET    WARM_DATA                WRITE_LIFE_NOT_SET
 * WRITE_LIFE_NONE       "                        WRITE_LIFE_NONE
 * WRITE_LIFE_MEDIUM     "                        WRITE_LIFE_MEDIUM
 * WRITE_LIFE_LONG       "                        WRITE_LIFE_LONG
 *
 * 3) whint_mode=fs-based. F2FS passes down hints with its policy.
 *
 * User                  F2FS                     Block
 * ----                  ----                     -----
 *                       META                     WRITE_LIFE_MEDIUM;
 *                       HOT_NODE                 WRITE_LIFE_NOT_SET
 *                       WARM_NODE                "
 *                       COLD_NODE                WRITE_LIFE_NONE
 * ioctl(COLD)           COLD_DATA                WRITE_LIFE_EXTREME
 * extension list        "                        "
 *
 * -- buffered io
 * WRITE_LIFE_EXTREME    COLD_DATA                WRITE_LIFE_EXTREME
 * WRITE_LIFE_SHORT      HOT_DATA                 WRITE_LIFE_SHORT
 * WRITE_LIFE_NOT_SET    WARM_DATA                WRITE_LIFE_LONG
 * WRITE_LIFE_NONE       "                        "
 * WRITE_LIFE_MEDIUM     "                        "
 * WRITE_LIFE_LONG       "                        "
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
	if (F2FS_OPTION(sbi).whint_mode == WHINT_MODE_USER) {
		if (type == DATA) {
			if (temp == WARM)
				return WRITE_LIFE_NOT_SET;
			else if (temp == HOT)
				return WRITE_LIFE_SHORT;
			else if (temp == COLD)
				return WRITE_LIFE_EXTREME;
		} else {
			return WRITE_LIFE_NOT_SET;
		}
	} else if (F2FS_OPTION(sbi).whint_mode == WHINT_MODE_FS) {
		if (type == DATA) {
			if (temp == WARM)
				return WRITE_LIFE_LONG;
			else if (temp == HOT)
				return WRITE_LIFE_SHORT;
			else if (temp == COLD)
				return WRITE_LIFE_EXTREME;
		} else if (type == NODE) {
			if (temp == WARM || temp == HOT)
				return WRITE_LIFE_NOT_SET;
			else if (temp == COLD)
				return WRITE_LIFE_NONE;
		} else if (type == META) {
			return WRITE_LIFE_MEDIUM;
		}
	}
	return WRITE_LIFE_NOT_SET;
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
		struct inode *inode = fio->page->mapping->host;

		if (S_ISDIR(inode->i_mode))
			return CURSEG_HOT_DATA;
		else
			return CURSEG_COLD_DATA;
	} else {
		if (IS_DNODE(fio->page) && is_cold_node(fio->page))
			return CURSEG_WARM_NODE;
		else
			return CURSEG_COLD_NODE;
	}
}

static int __get_segment_type_6(struct f2fs_io_info *fio)
{
	if (fio->type == DATA) {
		struct inode *inode = fio->page->mapping->host;

		if (is_cold_data(fio->page)) {
			if (fio->sbi->am.atgc_enabled)
				return CURSEG_ALL_DATA_ATGC;
			else
				return CURSEG_COLD_DATA;
		}
		if (file_is_cold(inode) || f2fs_need_compress_data(inode))
			return CURSEG_COLD_DATA;
		if (file_is_hot(inode) ||
				is_inode_flag_set(inode, FI_HOT_DATA) ||
				f2fs_is_atomic_file(inode) ||
				f2fs_is_volatile_file(inode))
			return CURSEG_HOT_DATA;
		return f2fs_rw_hint_to_seg_type(inode->i_write_hint);
	} else {
		if (IS_DNODE(fio->page))
			return is_cold_node(fio->page) ? CURSEG_WARM_NODE :
						CURSEG_HOT_NODE;
		return CURSEG_COLD_NODE;
	}
}

static int __get_segment_type(struct f2fs_io_info *fio)
{
	int type = 0;

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
		type = __get_segment_type_6(fio);
		break;
		//f2fs_bug_on(fio->sbi, true);
	}

	if (IS_HOT(type))
		fio->temp = HOT;
	else if (IS_WARM(type))
		fio->temp = WARM;
	else
		fio->temp = COLD;
	return type;
}

/*
 * This function should be resided under the curseg_mutex lock
 */
static void __add_sum_entry(struct f2fs_sb_info *sbi, int type,
					struct f2fs_summary *sum)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	void *addr = curseg->sum_blk;
	addr += curseg->next_blkoff * sizeof(struct f2fs_summary);
	memcpy(addr, sum, sizeof(struct f2fs_summary));
}

void f2fs_allocate_data_block(struct f2fs_sb_info *sbi, struct page *page,
		block_t old_blkaddr, block_t *new_blkaddr,
		struct f2fs_summary *sum, int type,
		struct f2fs_io_info *fio)
{
	struct sit_info *sit_i = SIT_I(sbi);
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	//unsigned long long old_mtime;
	bool from_gc = (type == CURSEG_ALL_DATA_ATGC);
	//struct seg_entry *se = NULL;
	uint64_t old_slot_idx, new_slot_idx;
#ifdef MIGRATION_HANDLING_LATENCY
	unsigned long long tstart, tend;
	unsigned long long tstart_, tend_;
#endif

	down_read(&SM_I(sbi)->curseg_lock);

	mutex_lock(&curseg->curseg_mutex);
	down_write(&sit_i->sentry_lock);

#ifdef MIGRATION_HANDLING_LATENCY
	tstart_ = OS_TimeGetUS();
#endif
	//static int type_cnt[NR_PERSISTENT_LOG] = {0, 0, 0, 0, 0, 0};

	/*if (type_cnt[type] % (4096*2) == 0){
		printk("%s: type %s allocated", __func__, 
				(type == CURSEG_HOT_DATA)? "HOT DATA" :
				(type == CURSEG_WARM_DATA)? "WARM DATA" :
				(type == CURSEG_COLD_DATA)? "COLD DATA" :
				(type == CURSEG_HOT_NODE)? "HOT NODE" :
				(type == CURSEG_WARM_NODE)? "WARM NODE" :
				(type == CURSEG_COLD_NODE)? "COLD NODE" :
				"Unknown Type"
		      );
	}
	type_cnt[type] += 1;*/


	if (from_gc) {
		panic("f2fs_allocate_data_block(): from_gc = 1 not expected!!");
		//f2fs_bug_on(sbi, GET_SEGNO(sbi, old_blkaddr) == NULL_SEGNO);
		//se = get_seg_entry(sbi, GET_SEGNO(sbi, old_blkaddr));
		//sanity_check_seg_type(sbi, se->type);
		//f2fs_bug_on(sbi, IS_NODESEG(se->type));
	}
	*new_blkaddr = NEXT_FREE_BLKADDR(sbi, curseg);
	
	f2fs_bug_on(sbi, curseg->next_blkoff >= sbi->blocks_per_seg);

	//f2fs_wait_discard_bio(sbi, *new_blkaddr);

	/*
	 * __add_sum_entry should be resided under the curseg_mutex
	 * because, this function updates a summary entry in the
	 * current summary block.
	 */
	
	__add_sum_entry(sbi, type, sum);

	__refresh_next_blkoff(sbi, curseg);

	stat_inc_block_count(sbi, curseg);

	/*if (from_gc) {
		old_mtime = get_segment_mtime(sbi, old_blkaddr);
	} else {
		update_segment_mtime(sbi, old_blkaddr, 0);
		old_mtime = 0;
	}
	update_segment_mtime(sbi, *new_blkaddr, old_mtime);
	*/
	/*
	 * SIT information should be updated before segment allocation,
	 * since SSR needs latest valid block information.
	 */
	new_slot_idx = curseg->slot_idx;
	old_slot_idx = NULL_SLOTNO;

	update_sit_entry(sbi, *new_blkaddr, 1, &new_slot_idx);
	if (GET_SEGNO(sbi, old_blkaddr) != NULL_SEGNO){
		update_sit_entry(sbi, old_blkaddr, -1, &old_slot_idx);
	}

	if (!__has_curseg_space(sbi, curseg)) {
		if (from_gc)
			f2fs_bug_on(sbi, 1);
			//get_atssr_segment(sbi, type, se->type,
			//			AT_SSR, se->mtime);
		else
			sit_i->s_ops->allocate_segment(sbi, type, false);
	}
	/*
	 * segment dirty status should be updated after segment allocation,
	 * so we just need to update status only one time after previous
	 * segment being closed.
	 */
	locate_dirty_segment(sbi, GET_SEGNO(sbi, old_blkaddr), old_slot_idx);
	locate_dirty_segment(sbi, GET_SEGNO(sbi, *new_blkaddr), new_slot_idx);

	up_write(&sit_i->sentry_lock);

	if (page && IS_NODESEG(type)) {
		fill_node_footer_blkaddr(page, NEXT_FREE_BLKADDR(sbi, curseg));

		f2fs_inode_chksum_set(sbi, page);
	}

	if (F2FS_IO_ALIGNED(sbi))
		fio->retry = false;

	if (fio) {
		struct f2fs_bio_info *io;

		INIT_LIST_HEAD(&fio->list);
		fio->in_list = true;
		io = sbi->write_io[fio->type] + fio->temp;
		spin_lock(&io->io_lock);
		list_add_tail(&fio->list, &io->io_list);
		spin_unlock(&io->io_lock);
	}

	mutex_unlock(&curseg->curseg_mutex);

	up_read(&SM_I(sbi)->curseg_lock);
}

static void update_device_state(struct f2fs_io_info *fio)
{
	struct f2fs_sb_info *sbi = fio->sbi;
	unsigned int devidx;

	if (!f2fs_is_multi_device(sbi))
		return;

	devidx = f2fs_target_device_index(sbi, fio->new_blkaddr);

	/* update device state for fsync */
	f2fs_set_dirty_device(sbi, fio->ino, devidx, FLUSH_INO);

	/* update device state for checkpoint */
	if (!f2fs_test_bit(devidx, (char *)&sbi->dirty_device)) {
		spin_lock(&sbi->dev_lock);
		f2fs_set_bit(devidx, (char *)&sbi->dirty_device);
		spin_unlock(&sbi->dev_lock);
	}
}

static void do_write_page(struct f2fs_summary *sum, struct f2fs_io_info *fio)
{
	int type = __get_segment_type(fio);
	bool keep_order = (f2fs_lfs_mode(fio->sbi) && type == CURSEG_COLD_DATA);

	//struct node_info ni;
	//struct f2fs_sb_info *sbi = F2FS_P_SB(fio->page);	

	if (keep_order)
		down_read(&fio->sbi->io_order_lock);
reallocate:
	f2fs_allocate_data_block(fio->sbi, fio->page, fio->old_blkaddr,
			&fio->new_blkaddr, sum, type, fio);
#ifdef SHIVAL
//	if (fio->new_blkaddr >= 0x60000000 && fio->new_blkaddr < 0x80000000){
//		printk("%s: old blkaddr: 0x%lx new_blkaddr: 0x%lx", __func__, fio->old_blkaddr, fio->new_blkaddr);
//		dump_stack();
//	}
#endif
	if (GET_SEGNO(fio->sbi, fio->old_blkaddr) != NULL_SEGNO)
		invalidate_mapping_pages(META_MAPPING(fio->sbi),
					fio->old_blkaddr, fio->old_blkaddr);

	/* writeout dirty page into bdev */
	f2fs_submit_page_write(fio);
	if (fio->retry) {
		fio->old_blkaddr = fio->new_blkaddr;
		goto reallocate;
	}
	//
	//f2fs_get_node_info(sbi, nid_of_node(fio->page), &ni);
	/*if ((fio->new_blkaddr == nid7_addr) || (fio->old_blkaddr == nid7_addr) && nid7_addr != 0){
		printk("[JW DBG] %s: other detected!! fio type is NODE %d, ino: %u, oldblkaddr: %u, newblkaddr: %u,  \n",
				 __func__, fio->type == NODE, fio->ino, fio->old_blkaddr , fio->new_blkaddr);
	}
	if (nid_of_node(fio->page) == 7){
		printk("[JW DBG] %s: fio type is NODE %d, ino: %u, oldblkaddr: %u, newblkaddr: %u,  \n",
				 __func__, fio->type == NODE, fio->ino, fio->old_blkaddr , fio->new_blkaddr);
		if (fio->type == NODE){
			printk("\t nodefooter[nid:%u,ino%u,ofs:%u,cpver:%llu,blkaddr:%u]", 
			  nid_of_node(fio->page), ino_of_node(fio->page),
			  ofs_of_node(fio->page), cpver_of_node(fio->page),
			  next_blkaddr_of_node(fio->page));
			//panic("[JW DBG] %s: Just to check stackframe\n", __func__);
			//printk("[JW DBG] %s: Intended Bug\n", __func__);
			nid7_addr = fio->new_blkaddr;
			//f2fs_bug_on(sbi, 1);
		}
	}*/
	/*
	wrtcnt += 1;
	if (fio->type == NODE){
		node_wrtcnt += 1;
		if (node_wrtcnt % 50000 == 0)
			printk("[JW DBG] %s: %ds write: Node write cnt: %d : ino: %u, oldblkaddr: %u, newblkaddr: %u,  \n",
				 __func__, wrtcnt, node_wrtcnt, fio->ino, fio->old_blkaddr , fio->new_blkaddr);
		
	}

	else if (fio->type == DATA){
		data_wrtcnt += 1;
		if (data_wrtcnt % 50000 == 0)
			printk("[JW DBG] %s: %ds write: Data write cnt: %d : ino: %u, oldblkaddr: %u, newblkaddr: %u,  \n",
				 __func__, wrtcnt, data_wrtcnt, fio->ino, fio->old_blkaddr , fio->new_blkaddr);

	}
	if (wrtcnt%50000==0)//{
		printk("[JW DBG] %s: %ds write: is_NODE %d, ino: %u, oldblkaddr: %u, newblkaddr: %u,  \n",
				 __func__, wrtcnt, fio->type == NODE, fio->ino, fio->old_blkaddr , fio->new_blkaddr);
	//}*/
	update_device_state(fio);

	if (keep_order)
		up_read(&fio->sbi->io_order_lock);
}

void f2fs_do_write_meta_page(struct f2fs_sb_info *sbi, struct page *page,
					enum iostat_type io_type)
{
	struct f2fs_io_info fio = {
		.sbi = sbi,
		.type = META,
		.temp = HOT,
		.op = REQ_OP_WRITE,
		.op_flags = REQ_SYNC | REQ_META | REQ_PRIO,
		.old_blkaddr = page->index,
		.new_blkaddr = page->index,
		.page = page,
		.encrypted_page = NULL,
		.in_list = false,
	};

	if (unlikely(page->index >= MAIN_BLKADDR(sbi)))
		fio.op_flags &= ~REQ_META;

	set_page_writeback(page);
	ClearPageError(page);
	f2fs_submit_page_write(&fio);

	stat_inc_meta_count(sbi, page->index);
	f2fs_update_iostat(sbi, io_type, F2FS_BLKSIZE);
}

void f2fs_do_write_node_page(unsigned int nid, struct f2fs_io_info *fio)
{
	struct f2fs_summary sum;

	set_summary(&sum, nid, 0, 0);
	do_write_page(&sum, fio);

	f2fs_update_iostat(fio->sbi, fio->io_type, F2FS_BLKSIZE);
}

void f2fs_outplace_write_data(struct dnode_of_data *dn,
					struct f2fs_io_info *fio)
{
	struct f2fs_sb_info *sbi = fio->sbi;
	struct f2fs_summary sum;

	f2fs_bug_on(sbi, dn->data_blkaddr == NULL_ADDR);
	set_summary(&sum, dn->nid, dn->ofs_in_node, fio->version);
	do_write_page(&sum, fio);
	f2fs_update_data_blkaddr(dn, fio->new_blkaddr);

	f2fs_update_iostat(sbi, fio->io_type, F2FS_BLKSIZE);
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
		return -EFSCORRUPTED;
	}

	stat_inc_inplace_blocks(fio->sbi);

	if (fio->bio && !(SM_I(sbi)->ipu_policy & (1 << F2FS_IPU_NOCACHE)))
		err = f2fs_merge_page_bio(fio);
	else
		err = f2fs_submit_page_bio(fio);
	if (!err) {
		update_device_state(fio);
		f2fs_update_iostat(fio->sbi, fio->io_type, F2FS_BLKSIZE);
	}

	return err;
}

static inline int __f2fs_get_curseg(struct f2fs_sb_info *sbi,
						unsigned int segno)
{
	int i;

	for (i = CURSEG_HOT_DATA; i < NO_CHECK_TYPE; i++) {
		if (CURSEG_I(sbi, i)->segno == segno)
			break;
	}
	return i;
}

void f2fs_do_replace_block(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
				block_t old_blkaddr, block_t new_blkaddr,
				bool recover_curseg, bool recover_newaddr,
				bool from_gc)
{
	printk("%s: not expected!!!!!!!!!!", __func__);
	f2fs_bug_on(sbi, 1);
	uint64_t slot_idx1, slot_idx2; 
	//struct sit_info *sit_i = SIT_I(sbi);
	//struct curseg_info *curseg;
	//unsigned int segno, old_cursegno;
	//struct seg_entry *se;
	//int type;
	//unsigned short old_blkoff;

	//segno = GET_SEGNO(sbi, new_blkaddr);
	//se = get_seg_entry(sbi, segno);
	//type = se->type;

	//down_write(&SM_I(sbi)->curseg_lock);

	/*if (!recover_curseg) {
		// for recovery flow 
		if (se->valid_blocks == 0 && !IS_CURSEG(sbi, segno)) {
			if (old_blkaddr == NULL_ADDR)
				type = CURSEG_COLD_DATA;
			else
				type = CURSEG_WARM_DATA;
		}
	} else {
		if (IS_CURSEG(sbi, segno)) {
			// se->type is volatile as SSR allocation 
			type = __f2fs_get_curseg(sbi, segno);
			f2fs_bug_on(sbi, type == NO_CHECK_TYPE);
		} else {
			type = CURSEG_WARM_DATA;
		}
	}*/

	//f2fs_bug_on(sbi, !IS_DATASEG(type));
	//curseg = CURSEG_I(sbi, type);

	//mutex_lock(&curseg->curseg_mutex);
	//down_write(&sit_i->sentry_lock);

	//old_cursegno = curseg->segno;
	//old_blkoff = curseg->next_blkoff;

	/* change the current segment */
	/* make these lines comment because change_curseg() is called only for changing current segment. 
	if (segno != curseg->segno) {
		curseg->next_segno = segno;
		change_curseg(sbi, type, true);
	}

	curseg->next_blkoff = GET_BLKOFF_FROM_SEG0(sbi, new_blkaddr);
	__add_sum_entry(sbi, type, sum);
	*/
	if (!recover_curseg || recover_newaddr) {
		//if (!from_gc)
		//	update_segment_mtime(sbi, new_blkaddr, 0);
		slot_idx1 = NULL_SLOTNO;
		update_sit_entry(sbi, new_blkaddr, 1, &slot_idx1);
	}
	if (GET_SEGNO(sbi, old_blkaddr) != NULL_SEGNO) {
		invalidate_mapping_pages(META_MAPPING(sbi),
					old_blkaddr, old_blkaddr);
		//if (!from_gc)
			//update_segment_mtime(sbi, old_blkaddr, 0);
		slot_idx2 = NULL_SLOTNO;
		update_sit_entry(sbi, old_blkaddr, -1, &slot_idx2);
	}

	//locate_dirty_segment(sbi, GET_SEGNO(sbi, old_blkaddr));
	//locate_dirty_segment(sbi, GET_SEGNO(sbi, new_blkaddr));

	//locate_dirty_segment(sbi, old_cursegno);

	/*if (recover_curseg) {
		if (old_cursegno != curseg->segno) {
			curseg->next_segno = old_cursegno;
			change_curseg(sbi, type, true);
		}
		curseg->next_blkoff = old_blkoff;
	}*/

	//up_write(&sit_i->sentry_lock);
	//mutex_unlock(&curseg->curseg_mutex);
	//up_write(&SM_I(sbi)->curseg_lock);
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

void f2fs_wait_on_page_writeback(struct page *page,
				enum page_type type, bool ordered, bool locked)
{
	if (PageWriteback(page)) {
		struct f2fs_sb_info *sbi = F2FS_P_SB(page);

		/* submit cached LFS IO */
		f2fs_submit_merged_write_cond(sbi, NULL, page, 0, type);
		/* sbumit cached IPU IO */
		f2fs_submit_merged_ipu_write(sbi, NULL, page);
		if (ordered) {
			wait_on_page_writeback(page);
			f2fs_bug_on(sbi, locked && PageWriteback(page));
		} else {
			wait_for_stable_page(page);
		}
	}
}

void f2fs_wait_on_block_writeback(struct inode *inode, block_t blkaddr)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct page *cpage;

	if (!f2fs_post_read_required(inode))
		return;

	if (!__is_valid_data_blkaddr(blkaddr))
		return;

	cpage = find_lock_page(META_MAPPING(sbi), blkaddr);
	if (cpage) {
		f2fs_wait_on_page_writeback(cpage, DATA, true, true);
		f2fs_put_page(cpage, 1);
	}
}

void f2fs_wait_on_block_writeback_range(struct inode *inode, block_t blkaddr,
								block_t len)
{
	block_t i;

	for (i = 0; i < len; i++)
		f2fs_wait_on_block_writeback(inode, blkaddr + i);
}

static int read_compacted_summaries(struct f2fs_sb_info *sbi)
{
	struct f2fs_checkpoint *ckpt = F2FS_CKPT(sbi);
	struct curseg_info *seg_i;
	unsigned char *kaddr;
	struct page *page;
	block_t start;
	int i, j, offset;

	start = start_sum_block(sbi);

	page = f2fs_get_meta_page(sbi, start++);
	if (IS_ERR(page))
		return PTR_ERR(page);
	kaddr = (unsigned char *)page_address(page);

	/* Step 1: restore nat cache */
	seg_i = CURSEG_I(sbi, CURSEG_HOT_DATA);
	memcpy(seg_i->journal, kaddr, SUM_JOURNAL_SIZE);

	/* Step 2: restore sit cache */
	seg_i = CURSEG_I(sbi, CURSEG_COLD_DATA);
	memcpy(seg_i->journal, kaddr + SUM_JOURNAL_SIZE, SUM_JOURNAL_SIZE);
	offset = 2 * SUM_JOURNAL_SIZE;

	/* Step 3: restore summary entries */
	for (i = CURSEG_HOT_DATA; i <= CURSEG_COLD_DATA; i++) {
		unsigned short blk_off;
		unsigned int segno;

		seg_i = CURSEG_I(sbi, i);
		segno = le32_to_cpu(ckpt->cur_data_segno[i]);
		blk_off = le16_to_cpu(ckpt->cur_data_blkoff[i]);
		seg_i->next_segno = segno;
		reset_curseg(sbi, i, 0);
		seg_i->alloc_type = ckpt->alloc_type[i];
		seg_i->next_blkoff = blk_off;

		if (seg_i->alloc_type == SSR)
			blk_off = sbi->blocks_per_seg;

		for (j = 0; j < blk_off; j++) {
			struct f2fs_summary *s;
			s = (struct f2fs_summary *)(kaddr + offset);
			seg_i->sum_blk->entries[j] = *s;
			offset += SUMMARY_SIZE;
			if (offset + SUMMARY_SIZE <= PAGE_SIZE -
						SUM_FOOTER_SIZE)
				continue;

			f2fs_put_page(page, 1);
			page = NULL;

			page = f2fs_get_meta_page(sbi, start++);
			if (IS_ERR(page))
				return PTR_ERR(page);
			kaddr = (unsigned char *)page_address(page);
			offset = 0;
		}
	}
	f2fs_put_page(page, 1);
	return 0;
}

static int read_normal_summaries(struct f2fs_sb_info *sbi, int type)
{
	struct f2fs_checkpoint *ckpt = F2FS_CKPT(sbi);
	struct f2fs_summary_block *sum;
	struct curseg_info *curseg;
	struct page *new;
	unsigned short blk_off;
	unsigned int segno = 0;
	block_t blk_addr = 0;
	int err = 0;

	/* get segment number and block addr */
	if (IS_DATASEG(type)) {
		segno = le32_to_cpu(ckpt->cur_data_segno[type]);
		blk_off = le16_to_cpu(ckpt->cur_data_blkoff[type -
							CURSEG_HOT_DATA]);
		if (__exist_node_summaries(sbi))
			blk_addr = sum_blk_addr(sbi, NR_CURSEG_PERSIST_TYPE, type);
		else
			blk_addr = sum_blk_addr(sbi, NR_CURSEG_DATA_TYPE, type);
	} else {
		segno = le32_to_cpu(ckpt->cur_node_segno[type -
							CURSEG_HOT_NODE]);
		blk_off = le16_to_cpu(ckpt->cur_node_blkoff[type -
							CURSEG_HOT_NODE]);
		if (__exist_node_summaries(sbi))
			blk_addr = sum_blk_addr(sbi, NR_CURSEG_NODE_TYPE,
							type - CURSEG_HOT_NODE);
		else
			blk_addr = GET_SUM_BLOCK(sbi, segno);
		//GET_SUM_BLOCK part must be modified. I disabled updating summary block so getting sum block form SSA part would cause trash SUM block. But it's okay since The content of SSA is actually not used. Thus, holding trash sum block for node type as curseg doesn't really matter. 
	}

	new = f2fs_get_meta_page(sbi, blk_addr);
	if (IS_ERR(new))
		return PTR_ERR(new);
	sum = (struct f2fs_summary_block *)page_address(new);
	/*
	if (IS_NODESEG(type)) {
		if (__exist_node_summaries(sbi)) {
			struct f2fs_summary *ns = &sum->entries[0];
			int i;
			for (i = 0; i < sbi->blocks_per_seg; i++, ns++) {
				ns->version = 0;
				ns->ofs_in_node = 0;
			}
		} else {
			err = f2fs_restore_node_summary(sbi, segno, sum);
			if (err)
				goto out;
		}
	}
	*/

	/* set uncompleted segment to curseg */
	curseg = CURSEG_I(sbi, type);
	mutex_lock(&curseg->curseg_mutex);

	/* update journal info */
	down_write(&curseg->journal_rwsem);
	memcpy(curseg->journal, &sum->journal, SUM_JOURNAL_SIZE);
	up_write(&curseg->journal_rwsem);
	
	memcpy(curseg->sum_blk->entries, sum->entries, SUM_ENTRY_SIZE);
	memcpy(&curseg->sum_blk->footer, &sum->footer, SUM_FOOTER_SIZE);
	
	curseg->next_segno = segno;
	reset_curseg(sbi, type, 0);
	curseg->alloc_type = ckpt->alloc_type[type];
	curseg->next_blkoff = blk_off;
	mutex_unlock(&curseg->curseg_mutex);
//out:
	f2fs_put_page(new, 1);
	return err;
}

static int restore_curseg_summaries(struct f2fs_sb_info *sbi)
{
	struct f2fs_journal *sit_j = CURSEG_I(sbi, CURSEG_COLD_DATA)->journal;
	struct f2fs_journal *nat_j = CURSEG_I(sbi, CURSEG_HOT_DATA)->journal;
	int type = CURSEG_HOT_DATA;
	int err;

	if (is_set_ckpt_flags(sbi, CP_COMPACT_SUM_FLAG)) {
		int npages = f2fs_npages_for_summary_flush(sbi, true);

		if (npages >= 2)
			f2fs_ra_meta_pages(sbi, start_sum_block(sbi), npages,
							META_CP, true);

		/* restore for compacted data summary */
		err = read_compacted_summaries(sbi);
		if (err)
			return err;
		type = CURSEG_HOT_NODE;
	}

	if (__exist_node_summaries(sbi))
		f2fs_ra_meta_pages(sbi,
				sum_blk_addr(sbi, NR_CURSEG_PERSIST_TYPE, type),
				NR_CURSEG_PERSIST_TYPE - type, META_CP, true);

	for (; type <= CURSEG_COLD_NODE; type++) {
		err = read_normal_summaries(sbi, type);
		if (err)
			return err;
	}

	/* sanity check for summary blocks */
	if (nats_in_cursum(nat_j) > NAT_JOURNAL_ENTRIES ||
			sits_in_cursum(sit_j) > SIT_JOURNAL_ENTRIES) {
		f2fs_err(sbi, "invalid journal entries nats %u sits %u\n",
			 nats_in_cursum(nat_j), sits_in_cursum(sit_j));
		return -EINVAL;
	}

	return 0;
}

static void write_compacted_summaries(struct f2fs_sb_info *sbi, block_t blkaddr)
{
	struct page *page;
	unsigned char *kaddr;
	struct f2fs_summary *summary;
	struct curseg_info *seg_i;
	int written_size = 0;
	int i, j;

	page = f2fs_grab_meta_page(sbi, blkaddr++);
	kaddr = (unsigned char *)page_address(page);
	memset(kaddr, 0, PAGE_SIZE);

	/* Step 1: write nat cache */
	seg_i = CURSEG_I(sbi, CURSEG_HOT_DATA);
	memcpy(kaddr, seg_i->journal, SUM_JOURNAL_SIZE);
	written_size += SUM_JOURNAL_SIZE;

	/* Step 2: write sit cache */
	seg_i = CURSEG_I(sbi, CURSEG_COLD_DATA);
	memcpy(kaddr + written_size, seg_i->journal, SUM_JOURNAL_SIZE);
	written_size += SUM_JOURNAL_SIZE;

	/* Step 3: write summary entries */
	for (i = CURSEG_HOT_DATA; i <= CURSEG_COLD_DATA; i++) {
		unsigned short blkoff;
		seg_i = CURSEG_I(sbi, i);
		if (sbi->ckpt->alloc_type[i] == SSR)
			blkoff = sbi->blocks_per_seg;
		else
			blkoff = curseg_blkoff(sbi, i);

		for (j = 0; j < blkoff; j++) {
			if (!page) {
				page = f2fs_grab_meta_page(sbi, blkaddr++);
				kaddr = (unsigned char *)page_address(page);
				memset(kaddr, 0, PAGE_SIZE);
				written_size = 0;
			}
			summary = (struct f2fs_summary *)(kaddr + written_size);
			*summary = seg_i->sum_blk->entries[j];
			written_size += SUMMARY_SIZE;

			if (written_size + SUMMARY_SIZE <= PAGE_SIZE -
							SUM_FOOTER_SIZE)
				continue;

			set_page_dirty(page);
			f2fs_put_page(page, 1);
			page = NULL;
		}
	}
	if (page) {
		set_page_dirty(page);
		f2fs_put_page(page, 1);
	}
}

static void write_normal_summaries(struct f2fs_sb_info *sbi,
					block_t blkaddr, int type)
{
	int i, end;
	if (IS_DATASEG(type))
		end = type + NR_CURSEG_DATA_TYPE;
	else
		end = type + NR_CURSEG_NODE_TYPE;

	for (i = type; i < end; i++)
		write_current_sum_page(sbi, i, blkaddr + (i - type));
}

void f2fs_write_data_summaries(struct f2fs_sb_info *sbi, block_t start_blk)
{
	if (is_set_ckpt_flags(sbi, CP_COMPACT_SUM_FLAG))
		write_compacted_summaries(sbi, start_blk);
	else
		write_normal_summaries(sbi, start_blk, CURSEG_HOT_DATA);
}

void f2fs_write_node_summaries(struct f2fs_sb_info *sbi, block_t start_blk)
{
	write_normal_summaries(sbi, start_blk, CURSEG_HOT_NODE);
}

/* Write every discard bitmap journal to blk */
static block_t write_discard_bitmap_journals(struct f2fs_sb_info *sbi, block_t *blk)
{
	struct dynamic_discard_map_control *ddmc = SM_I(sbi)->ddmc_info;
	//struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	//struct list_head *head = &dcc->entry_list;
	struct list_head *head = &ddmc->discard_map_head;
	struct discard_entry *entry, *this;
	struct page *page = NULL;
	struct discard_journal_block *dst_dj_blk;
	struct discard_journal_block_info *dst_dj_blk_info;
	unsigned int didx = 0;
	block_t tmp_blkcnt = 0;
	/* write discard journal: bitmap-type */
	list_for_each_entry_safe(entry, this, head, list) {
		struct discard_journal_bitmap *dst_dj_map;
		if (!page) {
			page = f2fs_grab_meta_page(sbi, (*blk)++);
			dst_dj_blk = (struct discard_journal_block *)page_address(page);
			memset(dst_dj_blk, 0, sizeof(*dst_dj_blk));
			dst_dj_blk_info = &dst_dj_blk->dj_block_info;
			dst_dj_blk_info->type = (unsigned char) DJ_BLOCK_BITMAP;
			didx = 0;
			tmp_blkcnt++;
		}
		dst_dj_map = (struct discard_journal_bitmap *) &dst_dj_blk->bitmap_entries[didx++];
		dst_dj_map->start_blkaddr = cpu_to_le32(entry->start_blkaddr);
		//printk("[JW DBG] %s: cnt: %d, start_sector: %u, start_blkaddr%u\n", __func__, cnt, entry->start_blkaddr*8, entry->start_blkaddr);	
		memcpy(dst_dj_map->discard_map, entry->discard_map, DISCARD_BLOCK_MAP_SIZE);

		if (didx == DJ_BITMAP_ENTRIES_IN_DJ_BLOCK){
			dst_dj_blk_info->entry_cnt = cpu_to_le32(didx);
			set_page_dirty(page);
			f2fs_put_page(page, 1);
			didx = 0;
			page = NULL;
		}
		//release_discard_addr(entry);
	}
	if (didx > 0){
		dst_dj_blk_info->entry_cnt = cpu_to_le32(didx);
		set_page_dirty(page);
		f2fs_put_page(page, 1);
	}
	return tmp_blkcnt;
}

static void release_discard_range(struct discard_range_entry *entry)
{
#ifndef LM
	list_del(&entry->ddm_list);
#endif
	list_del(&entry->list);
	kmem_cache_free(discard_range_slab, entry);
}

/* Write every discard range journal to blk */
static block_t write_discard_range_journals(struct f2fs_sb_info *sbi, block_t *blk)
{
	struct dynamic_discard_map_control *ddmc = SM_I(sbi)->ddmc_info;
	struct list_head *dr_head = &ddmc->discard_range_head;
	struct list_head *issued_discard_head = &ddmc->issued_discard_head;
	struct discard_range_entry *entry, *this;
	struct page *page = NULL;
	struct discard_journal_block *dst_dj_blk;
	struct discard_journal_block_info *dst_dj_blk_info;
	unsigned int didx = 0;
	block_t tmp_blkcnt = 0;
	int i;

	/* write discard journal, which is not issued */
	list_for_each_entry_safe(entry, this, dr_head, list) {
		unsigned int dr_cnt_in_dre = entry->cnt;
		for (i = 0; i < dr_cnt_in_dre; i++){
			struct discard_range *dr;
			struct discard_journal_range *dst_dj_range;
			dr = (struct discard_range *) &entry->discard_range_array[i];

			if (!page) {
				page = f2fs_grab_meta_page(sbi, (*blk)++);
				dst_dj_blk = (struct discard_journal_block *)page_address(page);
				memset(dst_dj_blk, 0, sizeof(*dst_dj_blk));
				dst_dj_blk_info = &dst_dj_blk->dj_block_info;
				dst_dj_blk_info->type = (unsigned char) DJ_BLOCK_RANGE;
				didx = 0;
				tmp_blkcnt += 1;
			}
			dst_dj_range = (struct discard_journal_range *) &dst_dj_blk->range_entries[didx++];
			dst_dj_range->start_blkaddr = cpu_to_le32(dr->start_blkaddr);
			dst_dj_range->len = cpu_to_le32(dr->len);
			//printk("[JW DBG] %s: cnt: %d, start_sector: %u, start_blkaddr%u\n", __func__, cnt, entry->start_blkaddr*8, entry->start_blkaddr);	
	
			if (didx == DJ_RANGE_ENTRIES_IN_DJ_BLOCK){
				dst_dj_blk_info->entry_cnt = cpu_to_le32(didx);
				set_page_dirty(page);
				f2fs_put_page(page, 1);
				didx = 0;
				page = NULL;
			}
		}
		//release_discard_range(entry);
	}

	/* write pending discard journal, which is issued */
	list_for_each_entry_safe(entry, this, issued_discard_head, list) {
		unsigned int dr_cnt_in_dre = entry->cnt;
		for (i = 0; i < dr_cnt_in_dre; i++){
			struct discard_range *dr;
			struct discard_journal_range *dst_dj_range;
			dr = (struct discard_range *) &entry->discard_range_array[i];

			if (!page) {
				page = f2fs_grab_meta_page(sbi, (*blk)++);
				dst_dj_blk = (struct discard_journal_block *)page_address(page);
				memset(dst_dj_blk, 0, sizeof(*dst_dj_blk));
				dst_dj_blk_info = &dst_dj_blk->dj_block_info;
				dst_dj_blk_info->type = (unsigned char) DJ_BLOCK_RANGE;
				didx = 0;
				tmp_blkcnt += 1;
			}
			dst_dj_range = (struct discard_journal_range *) &dst_dj_blk->range_entries[didx++];
			dst_dj_range->start_blkaddr = cpu_to_le32(dr->start_blkaddr);
			dst_dj_range->len = cpu_to_le32(dr->len);
			//printk("[JW DBG] %s: cnt: %d, start_sector: %u, start_blkaddr%u\n", __func__, cnt, entry->start_blkaddr*8, entry->start_blkaddr);	
	
			if (didx == DJ_RANGE_ENTRIES_IN_DJ_BLOCK){
				dst_dj_blk_info->entry_cnt = cpu_to_le32(didx);
				set_page_dirty(page);
				f2fs_put_page(page, 1);
				didx = 0;
				page = NULL;
			}
		}
		//release_discard_range(entry);
	}

	if (didx > 0){
		dst_dj_blk_info->entry_cnt = cpu_to_le32(didx);
		set_page_dirty(page);
		f2fs_put_page(page, 1);
	}
	return tmp_blkcnt;
}


block_t f2fs_write_discard_journals(struct f2fs_sb_info *sbi, 
					block_t start_blk, block_t journal_limit_addr)
{
	struct dynamic_discard_map_control *ddmc = SM_I(sbi)->ddmc_info;
	unsigned int discard_bitmap_segcnt = (unsigned int) atomic_read(&ddmc->dj_seg_cnt);
	unsigned int discard_range_cnt = (unsigned int) atomic_read(&ddmc->dj_range_cnt);
	static int cnt = 0;
	block_t blk, tmp_dblkcnt;
	//block_t dblkcnt_check = (discard_bitmap_segcnt % DJ_BITMAP_ENTRIES_IN_DJ_BLOCK)? 
	//		discard_bitmap_segcnt / DJ_BITMAP_ENTRIES_IN_DJ_BLOCK + 1 : 
	//		discard_bitmap_segcnt / DJ_BITMAP_ENTRIES_IN_DJ_BLOCK;
	
	block_t total_dblkcnt, bitmap_dblkcnt, range_dblkcnt;
	bitmap_dblkcnt = DISCARD_JOURNAL_BITMAP_BLOCKS(discard_bitmap_segcnt);
	range_dblkcnt = DISCARD_JOURNAL_RANGE_BLOCKS(discard_range_cnt);
	total_dblkcnt = bitmap_dblkcnt + range_dblkcnt;

	cnt += 1; 
	if (start_blk + total_dblkcnt >= journal_limit_addr){
		//panic("[JW DBG] %s: discard seg exceeded cp pack capacity\n", __func__);
		printk("[JW DBG] %s: discard seg exceeded cp pack capacity\n", __func__);
		return 0;
	}
		
	if (is_set_ckpt_flags(sbi, CP_COMPACT_SUM_FLAG))
		panic("[JW DBG] %s: must not be compact ckpt", __func__);
	
    blk = start_blk;
	tmp_dblkcnt = 0;
	block_t drange_cnt, dmap_cnt;

	/* write discard journal: bitmap-type*/
	dmap_cnt = write_discard_bitmap_journals(sbi, &blk);

	/* write discard journal: range-type*/
	drange_cnt = write_discard_range_journals(sbi, &blk);

	tmp_dblkcnt = drange_cnt + dmap_cnt;
	if (tmp_dblkcnt != total_dblkcnt)
		printk("[JW DBG] %s: total discard journal blk cnts not matching: real djblkcnt: %d, expected djblkcnt: %d, real range: %d exp range: %d, real_dmap: %d, exp dmap: %d", __func__, tmp_dblkcnt, total_dblkcnt, drange_cnt, range_dblkcnt, dmap_cnt, bitmap_dblkcnt);

	return blk;
}

int f2fs_lookup_journal_in_cursum(struct f2fs_journal *journal, int type,
					unsigned int val, int alloc)
{
	int i;

	if (type == NAT_JOURNAL) {
		for (i = 0; i < nats_in_cursum(journal); i++) {
			if (le32_to_cpu(nid_in_journal(journal, i)) == val)
				return i;
		}
		if (alloc && __has_cursum_space(journal, 1, NAT_JOURNAL))
			return update_nats_in_cursum(journal, 1);
	} else if (type == SIT_JOURNAL) {
		for (i = 0; i < sits_in_cursum(journal); i++)
			if (le32_to_cpu(segno_in_journal(journal, i)) == val)
				return i;
		if (alloc && __has_cursum_space(journal, 1, SIT_JOURNAL))
			return update_sits_in_cursum(journal, 1);
	}
	return -1;
}

static struct page *get_current_sit_page(struct f2fs_sb_info *sbi,
					unsigned int segno)
{
	return f2fs_get_meta_page(sbi, current_sit_addr(sbi, segno));
}

static struct page *get_next_sit_page(struct f2fs_sb_info *sbi,
					unsigned int start)
{
	struct sit_info *sit_i = SIT_I(sbi);
	struct page *page;
	pgoff_t src_off, dst_off;

	src_off = current_sit_addr(sbi, start);
	dst_off = next_sit_addr(sbi, src_off);

	page = f2fs_grab_meta_page(sbi, dst_off);
	seg_info_to_sit_page(sbi, page, start);

	set_page_dirty(page);
	set_to_next_sit(sit_i, start);

	return page;
}

static struct sit_entry_set *grab_sit_entry_set(void)
{
	struct sit_entry_set *ses =
			f2fs_kmem_cache_alloc(sit_entry_set_slab, GFP_NOFS);

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
		if (ses->entry_cnt <= next->entry_cnt)
			break;

	list_move_tail(&ses->set_list, &next->set_list);
}

/* here, segno is slot idx in IPLFS */
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

/* here, segno is slot idx in IPLFS */
static void add_sits_in_set(struct f2fs_sb_info *sbi)
{
	struct f2fs_sm_info *sm_info = SM_I(sbi);
	struct list_head *set_list = &sm_info->sit_entry_set;
	unsigned long *bitmap = SIT_I(sbi)->dirty_sentries_bitmap;
	unsigned int segno;

	for_each_set_bit(segno, bitmap, MAIN_SEG_SLOTS(sbi))
		add_sit_entry(segno, set_list);
}

static void remove_sits_in_journal(struct f2fs_sb_info *sbi)
{
	struct curseg_info *curseg = CURSEG_I(sbi, CURSEG_COLD_DATA);
	struct f2fs_journal *journal = curseg->journal;
	int i;

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


static void recover_info_from_ddm(struct f2fs_sb_info *sbi, unsigned long long ddmkey, 
		unsigned int ddm_offset, unsigned long long *p_segno, unsigned int *p_offset)
{
	unsigned int segs_per_ddm = SM_I(sbi)->ddmc_info->segs_per_node;
	unsigned int blocks_per_seg = sbi->blocks_per_seg;
	unsigned int start_segno = ddmkey * segs_per_ddm;
	unsigned int delta_segno = ddm_offset / blocks_per_seg ;
	*p_segno = start_segno + delta_segno;
	*p_offset = ddm_offset % blocks_per_seg;
}


//static unsigned long *get_seg_dmap(struct f2fs_sb_info *sbi, unsigned int p_segno){
//	unsigned long *cur_map;
//	unsigned long *ckpt_map;
//	unsigned long *dmap;
//	struct seg_entry *se;
//	int entries = SIT_VBLOCK_MAP_SIZE / sizeof(unsigned long);
//	int i;
//
//	se = get_seg_entry(sbi, p_segno);
//	cur_map = (unsigned long *)se->cur_valid_map;
//	ckpt_map = (unsigned long *)se->ckpt_valid_map;
//	dmap = SIT_I(sbi)->tmp_map;
//
//	for (i = 0; i < entries; i++)
//		dmap[i] = (cur_map[i] ^ ckpt_map[i]) & ckpt_map[i];
//	return dmap;
//}

				
//static void check_discarded_addr(block_t start_baddr, int offs, block_t target_addr){
//	if (target_addr == start_baddr + offs)
//		printk("[JW DBG] %s: target addr %u is discarded!!\n",__func__, target_addr);
//}

static struct discard_range_entry *__create_discard_range_entry(void)
{
	struct discard_range_entry *dre;

	dre = f2fs_kmem_cache_alloc(discard_range_slab, GFP_NOFS);
	INIT_LIST_HEAD(&dre->list);
#ifndef LM
	INIT_LIST_HEAD(&dre->ddm_list);
#endif
	dre->cnt = 0;
	
	return dre;
}

static inline void update_discard_range_entry(struct discard_range_entry *dre, unsigned int target_idx, 
				block_t lstart, block_t len)
{
	struct discard_range *dr;
	dr = (struct discard_range *) &dre->discard_range_array[target_idx];
	dr->start_blkaddr = lstart;
	dr->len = len;
	dre->cnt += 1;
}

#ifndef LM
static void add_discard_range_journal(struct f2fs_sb_info *sbi, block_t lstart, block_t len, struct dynamic_discard_map *ddm)
{
	struct dynamic_discard_map_control *ddmc = SM_I(sbi)->ddmc_info;
	struct list_head *total_drange_head = &ddmc->discard_range_head;
	struct list_head *ddm_drange_list = &ddm->drange_journal_list;
	struct discard_range_entry *dre;
	unsigned int target_idx;

	if (list_empty(total_drange_head) && !list_empty(ddm_drange_list)){
		printk("[JW DBG] %s: total drange list is empty but ddms drange list not empty!!", __func__);
		return;
	}
	if (list_empty(ddm_drange_list) || 
		list_last_entry(ddm_drange_list, struct discard_range_entry, ddm_list)->cnt 
								== DISCARD_RANGE_MAX_NUM)
	{
		dre = __create_discard_range_entry();
		list_add_tail(&dre->list, total_drange_head);
		list_add_tail(&dre->ddm_list, ddm_drange_list);
	}
	dre = list_last_entry(ddm_drange_list, struct discard_range_entry, ddm_list);
	target_idx = dre->cnt;
	update_discard_range_entry(dre, target_idx, lstart, len);
	atomic_inc(&ddmc->dj_range_cnt);
}
#endif

#ifdef LM
static void add_discard_range(struct f2fs_sb_info *sbi, block_t lstart, block_t len)
{
	struct dynamic_discard_map_control *ddmc = SM_I(sbi)->ddmc_info;
	struct list_head *total_drange_head = &ddmc->discard_range_head;
	struct discard_range_entry *dre;
	unsigned int target_idx;

	if (list_empty(total_drange_head) || 
		list_last_entry(total_drange_head, struct discard_range_entry, list)->cnt 
								== DISCARD_RANGE_MAX_NUM)
	{
		dre = __create_discard_range_entry();
		list_add_tail(&dre->list, total_drange_head);
	}
	dre = list_last_entry(total_drange_head, struct discard_range_entry, list);
	target_idx = dre->cnt;
	update_discard_range_entry(dre, target_idx, lstart, len);
	atomic_inc(&ddmc->dj_range_cnt);
}
#endif

static void remove_issued_discard_journals(struct f2fs_sb_info *sbi)
{
	struct dynamic_discard_map_control *ddmc = SM_I(sbi)->ddmc_info;
	struct list_head *issued_cmd_head = &ddmc->issued_discard_head;

	/* remove ddm's discard range journal entry */
	struct discard_range_entry *dre, *tmpdre;
	list_for_each_entry_safe(dre, tmpdre, issued_cmd_head, list) {
		atomic_set(&ddmc->dj_range_cnt, atomic_read(&ddmc->dj_range_cnt) - dre->cnt);
		// since release_discard_range deletes ddm_list, 
		// which is not used for issued discad cmds list, 
		// release_discard_range is not used in this case. 
		list_del(&dre->list);
		kmem_cache_free(discard_range_slab, dre);
	}

	/* remove ddm's discard bitmap journal entry */
	if (!list_empty(issued_cmd_head))
		printk("[JW DBG] %s: issued_cmd_head list not empty!!", __func__);
}


static void journal_issued_discard_cmd(struct f2fs_sb_info *sbi, block_t lstart, block_t len)
{
	struct dynamic_discard_map_control *ddmc = SM_I(sbi)->ddmc_info;
	struct list_head *head = &ddmc->issued_discard_head;
	struct discard_range_entry *dre;
	unsigned int target_idx;

	if (list_empty(head) ||
		list_last_entry(head, struct discard_range_entry, list)->cnt == DISCARD_RANGE_MAX_NUM)
	{
		dre = __create_discard_range_entry();
		list_add_tail(&dre->list, head);
	}
	dre = list_last_entry(head, struct discard_range_entry, list);
	target_idx = dre->cnt;
	update_discard_range_entry(dre, target_idx, lstart, len);
	atomic_inc(&ddmc->dj_range_cnt);
}

/* To save into discard journal, obtain previously issued but not yet submitted dicsard cmds */
static int journal_issued_discard_cmds(struct f2fs_sb_info *sbi)
{
	struct dynamic_discard_map_control *ddmc = SM_I(sbi)->ddmc_info;
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct list_head *pend_list;
	struct discard_cmd *dc, *tmp;
	int i, cmd_cnt;
	cmd_cnt = 0;
	
	struct list_head *issued_cmd_head = &ddmc->issued_discard_head;
	if (!list_empty(issued_cmd_head)){
		printk("[JW DBG] %s: not expected!!", __func__);
	}

	for (i = 0; i <= MAX_PLIST_NUM - 1; i++) {
		pend_list = &dcc->pend_list[i];

		mutex_lock(&dcc->cmd_lock);
		if (list_empty(pend_list))
			goto next;
		if (unlikely(dcc->rbtree_check))
			f2fs_bug_on(sbi, !f2fs_check_rb_tree_consistence(sbi,
							&dcc->root, false));
		list_for_each_entry_safe(dc, tmp, pend_list, list) {
			journal_issued_discard_cmd(sbi, dc->lstart, dc->len);
			cmd_cnt += 1;
		}
next:
		mutex_unlock(&dcc->cmd_lock);
	}
	return cmd_cnt;
}

static bool is_empty_ddm(struct f2fs_sb_info *sbi, struct dynamic_discard_map_control *ddmc,
					struct dynamic_discard_map *ddm)
{
        int max_blocks = sbi->blocks_per_seg * ddmc->segs_per_node;
	unsigned int start = 0, end = -1;
	unsigned long *ddmap = (unsigned long *)ddm->dc_map;

	start = __find_rev_next_bit(ddmap, max_blocks, end + 1);
        return start >= max_blocks;
}

static void remove_ddm_journals(struct f2fs_sb_info *sbi, struct dynamic_discard_map *ddm)
{
	struct list_head *drange_journal_head = &ddm->drange_journal_list;
	struct list_head *dmap_journal_head = &ddm->dmap_journal_list;
	struct dynamic_discard_map_control *ddmc = SM_I(sbi)->ddmc_info;

	/* remove ddm's discard range journal entry */
#ifdef LM
	f2fs_bug_on(sbi, !list_empty(drange_journal_head));
	f2fs_bug_on(sbi, !list_empty(dmap_journal_head));
#endif
#ifndef LM
	struct discard_range_entry *dre, *tmpdre;
	list_for_each_entry_safe(dre, tmpdre, drange_journal_head, ddm_list) {
		atomic_set(&ddmc->dj_range_cnt, atomic_read(&ddmc->dj_range_cnt) - dre->cnt);
		release_discard_range(dre);
	}
#endif
	/* remove ddm's discard bitmap journal entry */
	struct discard_entry *de, *tmpde;
	list_for_each_entry_safe(de, tmpde, dmap_journal_head, ddm_list) {
		release_discard_addr(de);
		atomic_dec(&ddmc->dj_seg_cnt);
	}
	if (!list_empty(drange_journal_head))
		printk("[JW DBG] %s: drange_journal_head list not empty!!", __func__);
	if (!list_empty(dmap_journal_head))
		printk("[JW DBG] %s: dmap_journal_head list not empty!!", __func__);
}

static void clear_ddm_bitmap(struct f2fs_sb_info *sbi, struct dynamic_discard_map *ddm, 
				unsigned int start_blkaddr, unsigned int end_blkaddr)
{
	unsigned int start_ofs, end_ofs, ddm_soff, ddm_eoff;
	unsigned long long s_segno, e_segno, s_ddmkey, e_ddmkey;
	int i;
	
	s_segno = GET_SEGNO(sbi, start_blkaddr);
	start_ofs = GET_BLKOFF_FROM_SEG0(sbi, start_blkaddr);
	e_segno = GET_SEGNO(sbi, end_blkaddr);
	end_ofs = GET_BLKOFF_FROM_SEG0(sbi, end_blkaddr);
	/* clear ddm's bitmap */
	get_ddm_info(sbi, s_segno, start_ofs, &s_ddmkey, &ddm_soff);
	get_ddm_info(sbi, e_segno, end_ofs, &e_ddmkey, &ddm_eoff);
	if (s_ddmkey != e_ddmkey)
		printk("[JW DBG] %s: ddm key not match!", __func__);
	
	for (i = ddm_soff; i <= ddm_eoff; i ++){
		if (!f2fs_test_and_clear_bit(i, ddm->dc_map))
			panic("[JW DBG] %s: weird. must be one but zero bit. offset: %d, ddmkey: %lld", __func__, i, ddm->key );
	}
}


//static int issue_small_discards_of_ddm(struct f2fs_sb_info *sbi, struct dynamic_discard_map *ddm,
//	       				int small_nr_issued)
//{
//	struct dynamic_discard_map_control *ddmc = SM_I(sbi)->ddmc_info;
//	struct list_head *ddm_dmap_list = &ddm->dmap_journal_list;
//	int i;
//	int nr_issued = 0;
//	bool small_force = (small_nr_issued > 0);
//
//        if (!f2fs_hw_support_discard(sbi)){
//		panic("Why HW not support discard!!");
//                return -1;
//        }
//        if (!f2fs_realtime_discard_enable(sbi)){
//                panic("Why discard not accepted?");
//                return -1;
//        }
//
//	//if (list_empty(&ddm->drange_journal_list) && list_empty(&ddm->dmap_journal_list)){
//		//printk("[JW DBG] %s: not expected!! ddm's drange journal and dmap journal are empty!!", __func__);	
//	//}
//	if (!small_force)
//		return 0;
//	
//	/* issue drange type: longer small discards */
//#ifndef LM
//	struct list_head *ddm_drange_list = &ddm->drange_journal_list;
//	struct discard_range_entry *dre, *tmpdre;
//	list_for_each_entry_safe(dre, tmpdre, ddm_drange_list, ddm_list) {
//		unsigned int dr_cnt = dre->cnt;
//		for (i = 0; i < dr_cnt; i++){
//			struct discard_range *dr;
//			dr = (struct discard_range *) &dre->discard_range_array[dr_cnt-i-1];
//			f2fs_issue_discard(sbi, dr->start_blkaddr, dr->len);
//			
//			clear_ddm_bitmap(sbi, ddm, dr->start_blkaddr, 
//						dr->start_blkaddr + dr->len - 1);
//			
//			dre->cnt -= 1;
//			atomic_dec(&ddmc->dj_range_cnt);
//			nr_issued += 1;
//			if (small_nr_issued <= nr_issued)
//				return nr_issued;
//		}
//		release_discard_range(dre);
//	}
//#endif
//
//	/* issue dmap type: small discards first */
//	struct discard_entry *de, *tmpde;
//	list_for_each_entry_safe(de, tmpde, ddm_dmap_list, ddm_list) {
//		unsigned int cur_pos = 0, next_pos, len;
//		bool is_valid = test_bit_le(0, de->discard_map);
//find_next:
//		if (is_valid) {
//			next_pos = find_next_zero_bit_le(de->discard_map,
//					sbi->blocks_per_seg, cur_pos);
//			len = next_pos - cur_pos;
//
//			f2fs_issue_discard(sbi, de->start_blkaddr + cur_pos,
//									len);
//			nr_issued += 1;
//
//			/* clear discard entry's bitmap */
//			for (i = cur_pos; i < next_pos; i++)
//				__clear_bit_le(i, (void *)de->discard_map);
//
//			/* clear ddm's bitmap */
//			clear_ddm_bitmap(sbi, ddm, de->start_blkaddr + cur_pos, 
//						de->start_blkaddr + next_pos - 1);
//		} else {
//			next_pos = find_next_bit_le(de->discard_map,
//					sbi->blocks_per_seg, cur_pos);
//		}
//	
//		cur_pos = next_pos;
//		is_valid = !is_valid;
//
//		if (cur_pos < sbi->blocks_per_seg){
//			if (small_nr_issued <= nr_issued){
//				return nr_issued;
//			} else {
//				goto find_next;
//			}
//		}
//		
//		release_discard_addr(de);
//		atomic_dec(&ddmc->dj_seg_cnt);
//
//		if (small_nr_issued <= nr_issued)
//			return nr_issued;
//	}
//	
//	if (!list_empty(&ddm->drange_journal_list)){
//		printk("[JW DBG] %s: not expected!! ddm's drange journal must be empty!!", __func__);	
//	       
//	}else if(!list_empty(&ddm->dmap_journal_list)){
//		printk("[JW DBG] %s: not expected!! ddm's dmap journal must be empty!!", __func__);	
//	}
//	
//	if (!is_empty_ddm(sbi, ddmc, ddm)){
//		printk("[JW DBG] %s: not expected!! ddm map must be empty", __func__);	
//	}
//	__remove_dynamic_discard_map(sbi, ddm);
//	return nr_issued;
//
//}



#define LONG_DISCARD_THRESHOLD 1024 
#ifndef LM
//static int flush_one_ddm(struct f2fs_sb_info *sbi, struct dynamic_discard_map_control *ddmc,
//					struct dynamic_discard_map *ddm, int print_history,
//					int small_nr_issued, bool issue_all)
//{
//        int max_blocks = sbi->blocks_per_seg * ddmc->segs_per_node;
//	unsigned int start = 0, end = -1;
//        struct discard_entry *de = NULL;
//	unsigned long *ddmap = (unsigned long *)ddm->dc_map;
//	unsigned long long ddmkey = ddm->key, tmp_ddmkey;
//	unsigned long long start_segno, end_segno; 
//	unsigned int start_offset, end_offset;
//        //struct list_head *head = &SM_I(sbi)->dcc_info->entry_list;
//	struct list_head *head = &ddmc->discard_map_head;
//	struct list_head *ddm_dmap_list = &ddm->dmap_journal_list;
//	int i;
//        bool first = true;
//	unsigned int last_target_segno;
//	unsigned int p_segno;
//	unsigned int start_in_seg, end_in_seg;
//	unsigned int offset_in_ddm;
//	unsigned static int cnt_list[128];
//	int nr_issued = 0;
//	bool small_force = (small_nr_issued > 0);
//	static int rmv_by_small_discard = 0;
//	if (print_history){
//		for (i = 0; i < 128; i ++){
//			cnt_list[i] = 0;
//		}
//	}
//	/*
//	if (!list_empty(&ddm->drange_journal_list))
//		printk("[JW DBG] %s: ddm's drange list must be empty!!", __func__);
//	if (!list_empty(&ddm->dmap_journal_list))
//		printk("[JW DBG] %s: ddm's dmap list must be empty!!", __func__);
//	*/
//	//unsigned int segcnt = 0;
//	//int localcnt = 0;
//        if (!f2fs_hw_support_discard(sbi)){
//		panic("Why HW not support discard!!");
//                return -1;
//        }
//        if (!f2fs_realtime_discard_enable(sbi)){
//                panic("Why discard not accepted?");
//                return -1;
//        }
//	while(1){
//                start = __find_rev_next_bit(ddmap, max_blocks, end + 1);
//                if (start >= max_blocks)
//                        break;
//
//                end = __find_rev_next_zero_bit(ddmap, max_blocks, start + 1);
//		
//		recover_info_from_ddm(sbi, ddmkey, start, &start_segno, &start_offset);
//		recover_info_from_ddm(sbi, ddmkey, end-1, &end_segno, &end_offset);
//		
//		/*set bitmap for each segment*/
//		unsigned int startLBA, endLBA, len;
//		startLBA = START_BLOCK(sbi, start_segno) + start_offset;
//		endLBA = START_BLOCK(sbi, end_segno) + end_offset;
//		len = endLBA - startLBA + 1;
//		
//		if (print_history){
//			if (len > LONG_DISCARD_THRESHOLD)
//				cnt_list[127] += 1;
//			else if (len > 0)
//				cnt_list[(len-1)/8] += 1;
//			else if (len <= 0)
//				printk("[JW DBG] %s: weird! len must be positive", __func__);
//			continue;
//		}
//		/* issue discard cmd to discard thread */
//		if (!print_history && !small_force){
//			/* Use discard range journal to reduce number of discard bitmap journal*/
//			if (len > 64){
//				//journal_discard_cmd(sbi, startLBA, len);
//				add_discard_range_journal(sbi, startLBA, len, ddm);
//				/* issue every long discard cmd */
//				if (len > 512 || issue_all){
//					for (i = start; i < end; i ++){
//						if (!f2fs_test_and_clear_bit(i, ddm->dc_map))
//							panic("[JW DBG] %s: weird. must be one but zero bit. offset: %d, segno: %d, ddmkey: %d", __func__, i, p_segno, ddmkey );
//	
//					}
//					
//					f2fs_issue_discard(sbi, startLBA, len);
//					nr_issued += 1;
//				}
//				continue;
//			}
//			else if (issue_all){
//				/* issue but dj_bitmap format*/
//				for (i = start; i < end; i ++){
//					if (!f2fs_test_and_clear_bit(i, ddm->dc_map))
//						panic("[JW DBG] %s: weird. must be one but zero bit. offset: %d, segno: %d, ddmkey: %d", __func__, i, p_segno, ddmkey );
//				}
//				f2fs_issue_discard(sbi, startLBA, len);
//				nr_issued += 1;
//			}
//		} else if (small_force){
//			/* issue small discard in ascending order. */
//			/* This helps to reduce dynamic discard map node having small discards. */
//			//if (small_nr_issued - nr_issued > 0){
//			for (i = start; i < end; i ++){
//				if (!f2fs_test_and_clear_bit(i, ddm->dc_map))
//					panic("[JW DBG] %s: weird. must be one but zero bit. offset: %d, segno: %d, ddmkey: %d", __func__, i, p_segno, ddmkey );
//
//			}
//			f2fs_issue_discard(sbi, startLBA, len);
//			nr_issued += 1;
//			continue;
//			//} else {
//			//	return nr_issued;
//			//}
//		}
//
//		start_in_seg = start_offset;
//		for (p_segno = start_segno; p_segno <= end_segno; p_segno++){
//			int dcmd_created = 0;
//
//			if (end_segno - p_segno){
//				end_in_seg = sbi->blocks_per_seg-1;
//			} else {
//				end_in_seg = end_offset;
//			}
//
//			if (first || last_target_segno != p_segno){
//				dcmd_created = 1;
//         			de = f2fs_kmem_cache_alloc(discard_entry_slab,
//                                                 GFP_F2FS_ZERO);
//        			de->start_blkaddr = START_BLOCK(sbi, p_segno);
//         			list_add_tail(&de->list, head);
//				list_add_tail(&de->ddm_list, ddm_dmap_list);
//				atomic_inc(&ddmc->dj_seg_cnt);
//				
//			}
//                	for (i = start_in_seg; i <= end_in_seg; i++){
//                		__set_bit_le(i, (void *)de->discard_map);
//			}
//			
//			last_target_segno = p_segno;
//			start_in_seg = 0;
//
//		}
//		first = false;
//        }
//
//	if (print_history){
//		for (i = 0; i < 128; i ++){
//			if (cnt_list[i] > 0)
//				printk("[JW DBG] %s: ddmkey: %u len: %u ~ %u: count: %u ", __func__, ddmkey, 8*i+1, 8*i+8, cnt_list[i]);
//		}
//		return 0;
//	}
//
//	if (small_force){
//		rmv_by_small_discard += 1;
//		
//		if (!is_empty_ddm(sbi, ddmc, ddm))
//			printk("[JW DBG] %s: 1: not empty ddm!!, must not be removed!\n", __func__, rmv_by_small_discard);
//
//		remove_ddm_journals(sbi, ddm);
//		__remove_dynamic_discard_map(sbi, ddm);
//	}
//	else if (!small_force){
//		if (is_empty_ddm(sbi, ddmc, ddm)){
//			__remove_dynamic_discard_map(sbi, ddm);
//		}
//	}
//	else if(issue_all){
//		if (!is_empty_ddm(sbi, ddmc, ddm))
//			printk("[JW DBG] %s: 3: not empty ddm!!, must not be removed!\n", __func__, rmv_by_small_discard);
//        	__remove_dynamic_discard_map(sbi, ddm);
//	}
//
//        return nr_issued;
//}
#endif

#ifdef ASYNC_SECTION_FREE
static inline void init_discard_cnt_entry(struct f2fs_sb_info *sbi, struct discard_cnt_entry *dce, 
		uint64_t segno)
{
	dce->segno = segno;

	dce->discard_blks = 0;
	
	INIT_HLIST_NODE(&dce->hnode);
	
	hash_add(discard_ht, &dce->hnode, dce->segno);
	
	SM_I(sbi)->dcnt_info->total_dce_cnt ++;
	//printk("%s: segno: %lu", __func__, segno);
}

static inline void remove_discard_cnt_entry(struct f2fs_sb_info *sbi, struct discard_cnt_entry *dce)
{
	SM_I(sbi)->dcnt_info->total_dce_cnt --;
	hash_del(&dce->hnode);
	kmem_cache_free(discard_cnt_entry_slab, dce);
}


static inline struct discard_cnt_entry *lookup_discard_hash(uint64_t key)
{
	struct hlist_head *head = &discard_ht[hash_min(key, HASH_BITS(discard_ht))];
	struct discard_cnt_entry *dce;

	hlist_for_each_entry(dce, head, hnode){
		//*height += 1;
		if (dce->segno == key)
			return dce;
	}
	return NULL;
}

static inline void inc_discard_cnt_entry(struct f2fs_sb_info *sbi, uint64_t segno, unsigned int len)
{
	struct discard_cnt_info *dc_info = SM_I(sbi)->dcnt_info;
	struct discard_cnt_entry *dce;
	
	spin_lock(&dc_info->lock);
	if ((dce = lookup_discard_hash(segno)) == NULL) {
		if (!(dce = f2fs_kmem_cache_alloc(discard_cnt_entry_slab, GFP_F2FS_ZERO))) {
			printk("%s: dce create fail", __func__);
			f2fs_bug_on(sbi, 1);
		}
		
		init_discard_cnt_entry(sbi, dce, segno);
	}

	//printk("%s: segno: %lu len: %u", __func__, segno, len);

	dce->discard_blks += len;
			
	f2fs_bug_on(sbi, dce->discard_blks > sbi->blocks_per_seg);

	spin_unlock(&dc_info->lock);
}

/* return true if all discards are completed */
static inline bool dec_discard_cnt_entry(struct f2fs_sb_info *sbi, uint64_t segno, unsigned int len)
{
	struct discard_cnt_info *dc_info = SM_I(sbi)->dcnt_info;
	struct discard_cnt_entry *dce;
	bool ret = false;
	
	//printk("%s: segno: %lu len: %u", __func__, segno, len);
	
	spin_lock(&dc_info->lock);
	if ((dce = lookup_discard_hash(segno)) == NULL) {
		printk("%s: segno: %lu something wrong!!!!!!!!!!!", __func__, segno);
		f2fs_bug_on(sbi, 1);
	}

	f2fs_bug_on(sbi, dce->discard_blks < len);
	dce->discard_blks -= len;

	if (dce->discard_blks == 0) {
		remove_discard_cnt_entry(sbi, dce);
		//printk("%s: removed!! segno: %lu len: %u", __func__, segno, len);
		ret = true;
	}
	
	spin_unlock(&dc_info->lock);
	
	return ret;
}

static void reflect_discard_cnt(struct f2fs_sb_info *sbi, block_t lstart, block_t len)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	unsigned int start_segno, end_segno, segno, segno_off;
	uint32_t slba = lstart, elba = lstart + len - 1, tmp_elba;
	enum dirty_type type_off = 0;

	void (*test_and_free_bit_handler) (struct f2fs_sb_info *sbi, unsigned int segno, bool inmem);

	start_segno = GET_SEGNO(sbi, slba);
	
	if (IS_MIGRATION_SEGNO(sbi, start_segno) || IS_META_SEGNO(sbi, start_segno))
		return;

	end_segno = GET_SEGNO(sbi, elba);

	if (start_segno >= sbi->START_SEGNO_INTERVAL_NODE) {
		/* NODE type */
		type_off = NR_DIRTY_DATA_TYPE;
		segno_off = sbi->START_SEGNO_INTERVAL_NODE;
		test_and_free_bit_handler = __set_test_and_free_node;
	} else {
		/* DATA type */
		segno_off = sbi->START_SEGNO_INTERVAL;
		test_and_free_bit_handler = __set_test_and_free;
	}

	for (segno = start_segno; segno <= end_segno; segno ++) {
		
		tmp_elba = min(elba, START_BLOCK(sbi, segno + 1) -1);
		len = tmp_elba - slba + 1;
		
		f2fs_bug_on(sbi, len > sbi->blocks_per_seg);
		
		if (dec_discard_cnt_entry(sbi, segno, len)) {
			/* free seg if seg is ready for it */
			mutex_lock(&dirty_i->seglist_lock);
			//f2fs_bug_on(sbi, ( (READY+type_off) != READY) && ((READY+type_off) != READY_NODE));
			//f2fs_bug_on(sbi, segno - segno_off > MAIN_SEGS_INTERVAL(sbi));
			//printk("%s: type: %u segno: %u segoff: %u", __func__, READY+type_off, segno, segno-segno_off);
			if (test_and_clear_bit(segno - segno_off, (dirty_i->dirty_segmap[READY + type_off]))) {
				dirty_i->nr_dirty[READY + type_off] --;
				//printk("%s: FREE!!! type: %u segno: %u segoff: %u", __func__, READY+type_off, segno, segno-segno_off);
				
				test_and_free_bit_handler(sbi, segno - segno_off, false);
				//printk("%s: to free: segno: %lu slba: 0x%lx", __func__, segno, START_BLOCK(sbi, segno));
			}
			mutex_unlock(&dirty_i->seglist_lock);
		}

		slba += len;

	}

}
#endif

//Notice!! This function always frees DDM. This can cause problem when number of blocks to be discarded is more than max_discards. The while loop stops when numblks to be discarded exceeds max_disacrds. This means DDM is freed while some of blks are not disacrded. This can cause orphan blocks. So this must be fixed. 
static int construct_ddm_journals(struct f2fs_sb_info *sbi, struct dynamic_discard_map *ddm,
					int *discard_limit, int* lcnt, int *scnt)
{
	struct dynamic_discard_map_control *ddmc = SM_I(sbi)->ddmc_info;
    int max_blocks = sbi->blocks_per_seg * ddmc->segs_per_node;
	unsigned int start = 0, end = -1;
	unsigned long *ddmap = (unsigned long *)ddm->dc_map;
	unsigned long long ddmkey = ddm->key;
	unsigned long long start_segno, end_segno; 
	unsigned int start_offset, end_offset;
    //struct list_head *head = &SM_I(sbi)->dcc_info->entry_list;
	int i;
	unsigned int p_segno;
#ifndef LM
    bool first = true;
	struct list_head *head = &ddmc->discard_map_head;
	struct list_head *ddm_dmap_list = &ddm->dmap_journal_list;
    struct discard_entry *de = NULL;
	unsigned int last_target_segno;
	unsigned int start_in_seg, end_in_seg;
	unsigned int offset_in_ddm;
#endif
	int nr_issued = 0;
	
	if (!list_empty(&ddm->drange_journal_list))
		printk("[JW DBG] %s: ddm's drange list must be empty!!", __func__);
	if (!list_empty(&ddm->dmap_journal_list))
		printk("[JW DBG] %s: ddm's dmap list must be empty!!", __func__);
	//unsigned int segcnt = 0;
	//int localcnt = 0;
	f2fs_bug_on(sbi, !f2fs_hw_support_discard(sbi));
	f2fs_bug_on(sbi, !f2fs_realtime_discard_enable(sbi));
	
	while(1){
	   	start = __find_rev_next_bit(ddmap, max_blocks, end + 1);
        if (start >= max_blocks)
			break;
		end = __find_rev_next_zero_bit(ddmap, max_blocks, start + 1);
		
		recover_info_from_ddm(sbi, ddmkey, start, &start_segno, &start_offset);
		recover_info_from_ddm(sbi, ddmkey, end-1, &end_segno, &end_offset);
		
		/*set bitmap for each segment*/
		unsigned int startLBA, endLBA, len;
		startLBA = START_BLOCK(sbi, start_segno) + start_offset;
		endLBA = START_BLOCK(sbi, end_segno) + end_offset;
		len = endLBA - startLBA + 1;
		
		/* issue discard cmd to discard thread */
		/* Use discard range journal to reduce number of discard bitmap journal*/

		/* issue every long discard cmd */
		/* Do not journal long discard cuz it is journalized when journaling pend_list */
		//if (len > ddmc->long_threshold){
		//if (len > LONG_DISCARD_THRESHOLD && *discard_limit > 0){
		for (i = start; i < end; i ++){
			if (!f2fs_test_and_clear_bit(i, ddm->dc_map))
				panic("[JW DBG] %s: weird. must be one but zero bit. offset: %d, segno: %d, ddmkey: %lld", __func__, i, p_segno, ddmkey );
	
		}
#ifdef LM
		add_discard_range(sbi, startLBA, len);
#else	
		f2fs_issue_discard(sbi, startLBA, len);
#endif
		nr_issued += 1;
		*discard_limit -= 1;
		*lcnt += 1;
		//f2fs_bug_on(sbi, GET_SEGNO(sbi, startLBA) != start_segno);
		//f2fs_bug_on(sbi, GET_SEGNO(sbi, endLBA) != end_segno);
#ifdef ASYNC_SECTION_FREE
		if (!IS_MIGRATION_SEGNO(sbi, start_segno) && !IS_META_SEGNO(sbi, start_segno)) {
			uint64_t tmp_segno, tmp_elba, tmp_slba;
			tmp_slba = startLBA;
			for (tmp_segno = start_segno; tmp_segno <= end_segno; tmp_segno ++){
				
				tmp_elba = min(endLBA, START_BLOCK(sbi, tmp_segno + 1) -1);
				//tmp_slba = max(startLBA, START_BLOCK(sbi, tmp_segno));
				len = tmp_elba - tmp_slba + 1;
				
				f2fs_bug_on(sbi, len > sbi->blocks_per_seg);
				
				inc_discard_cnt_entry(sbi, tmp_segno, len);

				tmp_slba += len;
			}
		}
#endif

		continue;
    }


    return nr_issued;
}


static void issue_all_discard_journals(struct f2fs_sb_info *sbi)
{
	struct dynamic_discard_map_control *ddmc = SM_I(sbi)->ddmc_info;
	struct list_head *total_dre_list = &ddmc->discard_range_head;
	struct list_head *total_dmap_list = &ddmc->discard_map_head;
	int i;

	struct discard_range_entry *dre, *tmpdre;
	list_for_each_entry_safe(dre, tmpdre, total_dre_list, list) {
		for (i = 0; i < dre->cnt; i++){
			struct discard_range *dr;
			dr = (struct discard_range *) &dre->discard_range_array[i];
			f2fs_issue_discard(sbi, dr->start_blkaddr, dr->len);
		}
		atomic_set(&ddmc->dj_range_cnt, atomic_read(&ddmc->dj_range_cnt) - dre->cnt);
		release_discard_range(dre);
	}
#ifdef LM
	f2fs_bug_on(sbi, !list_empty(total_dmap_list));
#endif
	struct discard_entry *de, *tmpde;
	list_for_each_entry_safe(de, tmpde, total_dmap_list, list) {
		unsigned int cur_pos = 0, next_pos, len, total_len = 0;
		bool is_valid = test_bit_le(0, de->discard_map);
find_next:
		if (is_valid) {
			next_pos = find_next_zero_bit_le(de->discard_map,
					sbi->blocks_per_seg, cur_pos);
			len = next_pos - cur_pos;

			f2fs_issue_discard(sbi, de->start_blkaddr + cur_pos, len);
			total_len += len;
		} else {
			next_pos = find_next_bit_le(de->discard_map,
					sbi->blocks_per_seg, cur_pos);
		}
		
		cur_pos = next_pos;
		is_valid = !is_valid;

		if (cur_pos < sbi->blocks_per_seg)
			goto find_next;

		release_discard_addr(de);
		atomic_dec(&ddmc->dj_seg_cnt);
	}
}

static int update_dirty_dynamic_discard_map(struct f2fs_sb_info *sbi, int *discard_limit)
{
	struct dynamic_discard_map_control *ddmc = SM_I(sbi)->ddmc_info;
	struct list_head *dirty_head = &ddmc->dirty_head;
	struct dynamic_discard_map *ddm, *tmpddm;
	int nr_issued = 0;
	int lcnt =0, scnt=0;//,  len = 0, ;
	
	list_for_each_entry_safe(ddm, tmpddm, dirty_head, dirty_list) {
		atomic_set(&ddm->is_dirty, 0);
		
		remove_ddm_journals(sbi, ddm);
		
		nr_issued += construct_ddm_journals(sbi, ddm, discard_limit, &lcnt, &scnt);
		
		list_del(&ddm->dirty_list);
		
		if (is_empty_ddm(sbi, ddmc, ddm)){
			if (!list_empty(&ddm->drange_journal_list)){
				printk("[JW DBG] %s: ddm's drange list isn't empty, impossible to remove ddm!!", __func__);
			} else if (!list_empty(&ddm->dmap_journal_list)){
				printk("[JW DBG] %s: ddm's dmap list isn't empty, impossible to remove ddm!!", __func__);
			} else{
				__remove_dynamic_discard_map(sbi, ddm);
			}
		}
	}
	//printk("%s: long hole: %d, short hole: %d", __func__, lcnt, scnt);
	
	//list_for_each_entry_safe(ddm, tmpddm, dirty_head, dirty_list) {
	//	printk("[JW DBG] %s: 4", __func__);
	//	list_del(&ddm->dirty_list);
	//	printk("[JW DBG] %s: 5", __func__);

	//}
	if (!list_empty(dirty_head))
		printk("[JW DBG] %s:  dirty list not empty!!", __func__);
		

	return nr_issued;
}

#define W_SIZE  5
void flush_dynamic_discard_maps(struct f2fs_sb_info *sbi, struct cp_control *cpc)
{
	struct dynamic_discard_map_control *ddmc = SM_I(sbi)->ddmc_info;
	struct dynamic_discard_map *ddm, *tmpddm;
	struct list_head *history_head_ddm = &ddmc->history_head;
	struct list_head *p;
	bool force = (cpc->reason & CP_DISCARD);
	bool issue_all = (cpc->reason & CP_UMOUNT);
        struct list_head *head = &SM_I(sbi)->dcc_info->entry_list;
	int i, tmp, nr_discard, nr_issued = 0, cur_dcmd_cnt, nr_small_discard, nr_issued_sum;
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	static int discard_limit = 0, prev_dcmd_cnt = 0, small_dcmd_cnt = 0;//, small_act_cnt = 0;	
	static int nr_issued_array[W_SIZE] = {0,0,0,0,0};
	struct migration_control *mgc = SM_I(sbi)->mgc_info;
	
	/*ar*/
	/* check submitted discard cmd and advise how many small discard will be submitted */
#ifdef MIGRATION_HANDLING_LATENCY
	static unsigned long long last_t = 0;
	unsigned long long cur_t;
	static int cp_cnt = 0;

	cp_cnt ++;	
	/*ar*/
	/* check submitted discard cmd and advise how many small discard will be submitted */
	cur_t = OS_TimeGetUS();
	if (cur_t - last_t > 5000000) {
		printk("%s: cur mg cnt handler: %d mg cnd pre comp: %d total: %d", __func__, atomic_read(&mgc->mg_entry_cnt),
			atomic_read(&mgc->mg_entry_cnt_pre_comp), 
			atomic_read(&mgc->total_mg_entry_cnt));

		//cur_dcmd_cnt = (int) atomic_read(&dcc->discard_cmd_cnt );
		//printk("%s: cur dcmd cnt: %d", __func__, cur_dcmd_cnt);
		printk("%s: cp cnt: %d", __func__, cp_cnt);
	
		printk("%s: total pgs: %d node pgs: %d data pgs: %d, trash pgs: %d dirty/update ratio: %u / %u", 
				__func__, 
				atomic_read(&mgc->total_pgs),
				atomic_read(&mgc->node_pgs), 
				atomic_read(&mgc->data_pgs), 
				atomic_read(&mgc->total_pgs) - atomic_read(&mgc->node_pgs) - atomic_read(&mgc->data_pgs),
				atomic_read(&mgc->dirty_node_pgs),
				atomic_read(&mgc->updated_node_pgs)
				);
		atomic_set(&mgc->total_pgs, 0);
		atomic_set(&mgc->node_pgs, 0);
		atomic_set(&mgc->data_pgs, 0);
		
	
		unsigned long long avg_dseg_time = (mgc->data_seg_cnt == 0)?
				0 : mgc->data_seg_time / mgc->data_seg_cnt;
		unsigned long long avg_nseg_time = (mgc->node_seg_cnt == 0)?
				0 : mgc->node_seg_time / mgc->node_seg_cnt;
		
		unsigned long long avg_dseg_p4_time = (mgc->data_seg_cnt == 0)?
				0 : mgc->_data_seg_time / mgc->data_seg_cnt;
		
		unsigned long long avg_dseg_p4_start_time = (mgc->data_seg_cnt == 0)?
				0 : mgc->data_seg_p4_start_time / mgc->data_seg_cnt;
		unsigned long long avg_dseg_p3_time = (mgc->data_seg_cnt == 0)?
				0 : mgc->data_seg_p3_time / mgc->data_seg_cnt;
		unsigned long long avg_dseg_p2_time = (mgc->data_seg_cnt == 0)?
				0 : mgc->data_seg_p2_time / mgc->data_seg_cnt;
		unsigned long long avg_dseg_is_alive_time = (mgc->data_seg_cnt == 0)?
				0 : mgc->data_seg_is_alive_time / mgc->data_seg_cnt;
		unsigned long long avg_dseg_p1_time = (mgc->data_seg_cnt == 0)?
				0 : mgc->data_seg_p1_time / mgc->data_seg_cnt;
		
		
		
		
		
		unsigned long long avg_dseg_p4_lck_time = (mgc->data_seg_cnt == 0)?
				0 : mgc->__data_seg_time / mgc->data_seg_cnt;
		
		unsigned long long avg_dseg_p4_node_read_time = (mgc->data_seg_cnt == 0)?
				0 : mgc->node_read_time / mgc->data_seg_cnt;
		
		unsigned long long avg_dseg_p4_nat_read_time = (mgc->data_seg_cnt == 0)?
				0 : mgc->nat_read_time / mgc->data_seg_cnt;
		
		unsigned long long avg_dseg_p4_ssa_update_time = (mgc->data_seg_cnt == 0)?
				0 : mgc->ssa_update_time / mgc->data_seg_cnt;
		
		unsigned long long avg_dseg_p4_ssa_update_lck_time = (mgc->data_seg_cnt == 0)?
				0 : mgc->ssa_update_lck_time / mgc->data_seg_cnt;
		
		unsigned long long avg_dseg_p4_sit_update_lck_time = (mgc->data_seg_cnt == 0)?
				0 : mgc->sit_update_lck_time / mgc->data_seg_cnt;
		
		unsigned long long avg_mge_proc_time = (mgc->mge_proc_cnt == 0)?
				0 : mgc->mge_proc_time / mgc->mge_proc_cnt;
		unsigned long long avg_mge_lck_time = (mgc->mge_proc_cnt == 0)?
				0 : mgc->mge_lck_time / mgc->mge_proc_cnt;
		unsigned long long avg_mge_lck2_time = (mgc->mge_proc_cnt == 0)?
				0 : mgc->mge_lck2_time / mgc->mge_proc_cnt;
	

		unsigned long long avg_mge_preproc_time = (mgc->mge_proc_cnt == 0)?
				0 : mgc->mge_preproc_time / mgc->mge_proc_cnt;
		unsigned long long avg_mge_preproc_get_ssa_time = (mgc->mge_proc_cnt == 0)?
				0 : mgc->mge_preproc_get_ssa_time / mgc->mge_proc_cnt;
		unsigned long long avg_mge_preproc_ssa_check_time = (mgc->mge_proc_cnt == 0)?
				0 : mgc->mge_preproc_ssa_check_time / mgc->mge_proc_cnt;
		
		unsigned long long avg_mge_preproc_ssa_check_get_time = (mgc->mge_proc_cnt == 0)?
				0 : mgc->mge_preproc_ssa_check_get_time / mgc->mge_proc_cnt;
		
		unsigned long long avg_mge_preproc_ssa_check_match_time = (mgc->mge_proc_cnt == 0)?
				0 : mgc->mge_preproc_ssa_check_match_time / mgc->mge_proc_cnt;

		unsigned long long avg_mge_seg_proc_time = (mgc->mge_proc_cnt == 0)?
				0 : (mgc->data_seg_time + mgc->node_seg_time) / mgc->mge_proc_cnt;
		unsigned int proc_seg_cnt = mgc->data_seg_cnt + mgc->node_seg_cnt;
		
	
		//printk("%s: data seg avg time: %llu us %llu ms data seg cnt: %llu", 
		//	__func__, avg_dseg_time, avg_dseg_time / 1000, mgc->data_seg_cnt);
		//
		
		printk("%s: [usec] data seg avg time: %llu p1: %llu p1.5: %llu p2: %llu p3: %llu p4: %llu", 
				__func__, avg_dseg_time, 
				avg_dseg_p1_time, 
				avg_dseg_is_alive_time, 
				avg_dseg_p2_time, 
				avg_dseg_p3_time, 
				avg_dseg_p4_time); 
		
		printk("%s: [usec] data seg avg time: %llu p4: %llu p4_lck: %llu node_read: %llu nat_read: %llu ssa_update: %llu ssa_update_lck: %llu sit_update_lck: %llu data seg cnt: %llu", 
			__func__, avg_dseg_time, avg_dseg_p4_time, avg_dseg_p4_lck_time,
			avg_dseg_p4_node_read_time, 
			avg_dseg_p4_nat_read_time, 
			avg_dseg_p4_ssa_update_time, 
			avg_dseg_p4_ssa_update_lck_time, 
			avg_dseg_p4_sit_update_lck_time, 
			mgc->data_seg_cnt);
		printk("%s: node seg avg time: %llu us %llu ms node seg cnt: %llu", 
			__func__, avg_nseg_time, avg_nseg_time / 1000, mgc->node_seg_cnt);
	
		printk("%s: [usec] mge proc avg time: %llu (preproc: %llu get_ssa: %llu ssa_check: %llu (get: %llu match: %llu) seg_proc: %llu seg_cnt: %llu) lck1: %llu lck2: %llu mge cnt: %llu", 
				__func__, avg_mge_proc_time, 
				avg_mge_preproc_time, 
				avg_mge_preproc_get_ssa_time, 
				avg_mge_preproc_ssa_check_time, 
				avg_mge_preproc_ssa_check_get_time, 
				avg_mge_preproc_ssa_check_match_time, 
				avg_mge_seg_proc_time, proc_seg_cnt, 
				avg_mge_lck_time, avg_mge_lck2_time, 
				mgc->mge_proc_cnt);	
		mgc->mge_proc_time = 0;
		mgc->mge_proc_cnt = 0;
		mgc->mge_lck_time = 0;
		mgc->mge_lck2_time = 0;

		mgc->data_seg_time = 0;
		mgc->node_seg_time = 0;
		
		mgc->nat_read_time = 0;
		mgc->nat_read_cnt = 0;
		
		mgc->ssa_update_time = 0;
		mgc->ssa_update_cnt = 0;
		
		mgc->ssa_update_lck_time = 0;
		mgc->ssa_update_lck_cnt = 0;
		
		mgc->sit_update_lck_time = 0;
		mgc->sit_update_lck_cnt = 0;
	
		mgc->mge_preproc_time = 0;
		mgc->mge_preproc_get_ssa_time = 0;
		mgc->mge_preproc_ssa_check_time = 0;
		mgc->mge_preproc_ssa_check_get_time = 0;
		mgc->mge_preproc_ssa_check_match_time = 0;


		mgc->data_seg_p4_start_time = 0;
		mgc->data_seg_p3_time  = 0;
		mgc->data_seg_p2_time  = 0;
		mgc->data_seg_p1_time  = 0;
		mgc->data_seg_is_alive_time = 0;

		
		unsigned long long _avg_dseg_time = (mgc->_data_seg_cnt == 0)?
				0 : mgc->_data_seg_time / mgc->_data_seg_cnt;
		unsigned long long _avg_nseg_time = (mgc->_node_seg_cnt == 0)?
				0 : mgc->_node_seg_time / mgc->_node_seg_cnt;
	
		printk("%s: data seg _avg time: %llu us %llu ms data seg cnt: %llu", 
			__func__, _avg_dseg_time, _avg_dseg_time / 1000, mgc->_data_seg_cnt);
		printk("%s: node seg _avg time: %llu us %llu ms node seg cnt: %llu", 
			__func__, _avg_nseg_time, _avg_nseg_time / 1000, mgc->_node_seg_cnt);
	
		mgc->_data_seg_time = 0;
		mgc->_node_seg_time = 0;
		mgc->_data_seg_cnt = 0;
		mgc->_node_seg_cnt = 0;


		unsigned long long avg_node_read_time = (mgc->node_read_cnt == 0)?
				0 : mgc->node_read_time / mgc->node_read_cnt;

		printk("%s: data seg node pg read avg time: %llu us %llu ms node pg read cnt: %llu", 
			__func__, avg_node_read_time, avg_node_read_time / 1000, mgc->node_read_cnt);
		
		mgc->node_read_time = 0;
		mgc->node_read_cnt = 0;
		
		unsigned long long __avg_dseg_time = (mgc->__data_seg_cnt == 0)?
				0 : mgc->__data_seg_time / mgc->__data_seg_cnt;
	
		printk("%s: data seg lck _avg time: %llu us %llu ms data seg cnt: %llu", 
			__func__, __avg_dseg_time, __avg_dseg_time / 1000, mgc->__data_seg_cnt);
	
		mgc->__data_seg_time = 0;
		mgc->__data_seg_cnt = 0;
	
		cp_cnt = 0;	
		
		//unsigned int upgs = (unsigned int) atomic_read(&mgc->updated_node_pgs);
		//unsigned int dpgs = (unsigned int) atomic_read(&mgc->dirty_node_pgs);
		//unsigned int dirty_ratio = (upgs > 0)?
		//	dpgs * 100 / upgs : 0;
		//unsigned int dirty_ratio_100 = (upgs > 0)?
		//	dpgs * 10000 / upgs : 0;
		//printk("%s: dirty ratio: %u percent X100 %u percent dirty node pgs: %u update node pgs: %u", 
		//		__func__, dirty_ratio, dirty_ratio_100, dpgs, upgs);	
		atomic_set(&mgc->dirty_node_pgs , 0);
		atomic_set(&mgc->updated_node_pgs , 0);
		//printk("%s: mg handling: data seg cnt: %llu node seg cnt: %llu", 
		//	__func__, mgc->data_seg_cnt, mgc->node_seg_cnt);
		mgc->data_seg_cnt = 0;
		mgc->node_seg_cnt = 0;
		
		last_t = cur_t;
	}
#endif

	//////////
#ifdef MG_HANDLER_WRITE_NODE
	unsigned int wpgs = (unsigned int) atomic_read(&sbi->written_node);
	unsigned int spgs = (unsigned int) atomic_read(&sbi->synced_node);
	unsigned int sync_ratio = (wpgs > 0)?
		spgs * 100 / wpgs : 0;
	unsigned int sync_ratio_100 = (wpgs > 0)?
		spgs * 10000 / wpgs : 0;
	printk("%s: sync ratio: %u percent X100 %u percent synced node pgs: %u written node pgs: %u", 
			__func__, sync_ratio, sync_ratio_100, spgs, wpgs);	
	atomic_set(&sbi->written_node , 0);
	atomic_set(&sbi->synced_node , 0);

	sbi->sync_ratio[sbi->sync_idx++] = sync_ratio;
	sbi->sync_idx %= N_SYNC_RATIO;
	sbi->sync_avg = 0;
	for (i = 0; i < N_SYNC_RATIO; i++) {
		sbi->sync_avg += sbi->sync_ratio[i];
	}
	sbi->sync_avg /= N_SYNC_RATIO;
	printk("%s: sync avg: %u", __func__, sbi->sync_avg);	
#endif
	/////////////
	nr_discard = prev_dcmd_cnt - cur_dcmd_cnt;
	if (nr_discard == 0 && prev_dcmd_cnt > 0){
		ddmc->long_threshold = 50000;
	}

	nr_issued_sum = 0;
	for (i=0; i < W_SIZE; i++){
		nr_issued_sum += nr_issued_array[i];
	}
	discard_limit = max(150000-nr_issued_sum, 0);
#ifdef FSYNC_LAT
	unsigned int node_wrt_cnt;
	unsigned long long node_wrt_lat;
	spin_lock(&sbi->lat_lock);
	node_wrt_cnt = sbi->fsync_node_wrt_cnt;
	node_wrt_lat = sbi->fsync_node_wrt_lat;
	sbi->fsync_node_wrt_cnt = 0;
	sbi->fsync_node_wrt_lat	= 0;
	spin_unlock(&sbi->lat_lock);
	if (node_wrt_cnt > 0){
		printk("%s: avg fsync wait lat: %u ns node_wrt_cnt: %u lat sum: %u", __func__, 
				node_wrt_lat / node_wrt_cnt, node_wrt_cnt, node_wrt_lat);
	}
#endif
#ifdef LM
	discard_limit = 0xffffffff;
#endif
	/* large discard */
	nr_issued += update_dirty_dynamic_discard_map(sbi, &discard_limit);
	
	/* UNMOUNT case */	
	if (issue_all){
		/* for umount, issue all discard journals, 
		 * because the journals have every discard blocks information*/
		remove_issued_discard_journals(sbi);
		journal_issued_discard_cmds(sbi);
		goto finish;
	}

	/* small discard */
	nr_small_discard = discard_limit;	
#ifdef LM
	f2fs_bug_on(sbi, !list_empty(history_head_ddm) );//
#endif

	list_for_each_entry_safe(ddm, tmpddm, history_head_ddm, history_list) {
		if (nr_small_discard > 0){
			f2fs_bug_on(sbi, 1);
			//tmp = issue_small_discards_of_ddm(sbi, ddm, nr_small_discard);
			//nr_small_discard -= tmp;
			//nr_issued += tmp;
			//small_dcmd_cnt += tmp;
		} else {
			break;
		}
	}

journal_issued_discard:
	/* Journal pending discard cmds */
	remove_issued_discard_journals(sbi);
	
	/* To save into discard journal, obtain issued but not completed dicsard cmds*/
	journal_issued_discard_cmds(sbi);
	
finish:
	tmp = (int) atomic_read(&dcc->discard_cmd_cnt );
	int tmp2 = (int) atomic_read(&ddmc->node_cnt);
	prev_dcmd_cnt = tmp;
	for (i = 0; i < W_SIZE-1; i++){
		nr_issued_array[i] = nr_issued_array[i+1];
	}
	nr_issued_array[W_SIZE-1] = nr_issued;
}

//static unsigned long *get_one_seg_bitmap_from_extended_ddm(struct f2fs_sb_info *sbi, 
//							struct dynamic_discard_map *ddm, 
//							unsigned long long ddmkey, 
//							unsigned long long segno)
//{
//        int entries = SIT_VBLOCK_MAP_SIZE / sizeof(unsigned long);
//	unsigned int segs_per_ddm = SM_I(sbi)->ddmc_info->segs_per_node;
//        unsigned int start_segno = ddmkey * segs_per_ddm;
//        unsigned int delta_segno = segno - start_segno;
//	unsigned long *dc_map = (unsigned long *) ddm->dc_map;
//
//	dc_map += entries * delta_segno;
//	
//	return dc_map;
//}


//static unsigned long *get_ddmap_from_extended_ddm_hash(struct f2fs_sb_info *sbi, 
//							unsigned long long segno)
//{
//	struct dynamic_discard_map *ddm;
//	unsigned long long ex_ddmkey, recovered_segno;
//	unsigned int ex_ddm_offset, recovered_offset;
//	unsigned long *dc_map;
//	unsigned int height;
//	
//	/*get extended ddm from segno*/
//	get_ddm_info(sbi, segno, 0, &ex_ddmkey, &ex_ddm_offset);
//        ddm = f2fs_lookup_hash(sbi, ex_ddmkey, &height);
//	if (ddm == NULL)
//		return NULL;
//	
//	/*recovery check*/
//	recover_info_from_ddm(sbi, ex_ddmkey, ex_ddm_offset, &recovered_segno, &recovered_offset);
//	if (recovered_segno != segno || recovered_offset != 0){
//		panic("get_ddmap_from_extended_ddm_hash: recover failed! ex vs recov : key %lld != %lld or offset %d != %d", ex_ddmkey, recovered_segno, ex_ddm_offset, recovered_offset);
//	}
//	dc_map = get_one_seg_bitmap_from_extended_ddm(sbi, ddm, ex_ddmkey, segno);
//	return dc_map;
//
//}



//static unsigned long *get_ddmap_from_extended_ddm_rb(struct f2fs_sb_info *sbi, 
//							unsigned long long segno)
//{
//	struct dynamic_discard_map_control *ddmc = SM_I(sbi)->ddmc_info;
//	struct rb_node **p, *parent = NULL;
//	struct rb_entry *re;
//	bool leftmost, exist;
//	struct dynamic_discard_map *ddm;
//	int height = 0;
//	unsigned long long ex_ddmkey, recovered_segno;
//	unsigned int ex_ddm_offset, recovered_offset;
//	unsigned long *dc_map;
//	/*get extended ddm from segno*/
//	get_ddm_info(sbi, segno, 0, &ex_ddmkey, &ex_ddm_offset);
//        p = f2fs_lookup_pos_rb_tree_ext(sbi, &ddmc->root, &parent, ex_ddmkey, &leftmost, &height, &exist);
//	if (!exist){
//		return NULL;
//	}
//	printk("%d", height);
//	re = rb_entry_safe(*p, struct rb_entry, rb_node);
//        ddm = dynamic_discard_map(re, struct dynamic_discard_map, rbe);
//	
//	/*recovery check*/
//	recover_info_from_ddm(sbi, ex_ddmkey, ex_ddm_offset, &recovered_segno, &recovered_offset);
//	if (recovered_segno != segno || recovered_offset != 0){
//		panic("get_ddmap_from_extended_ddm_rb: recover failed! ex vs recov : key %lld != %lld or offset %d != %d", ex_ddmkey, recovered_segno, ex_ddm_offset, recovered_offset);
//	}
//	dc_map = get_one_seg_bitmap_from_extended_ddm(sbi, ddm, ex_ddmkey, segno);
//	return dc_map;
//
//}


//static bool check_ddm_sanity(struct f2fs_sb_info *sbi, struct cp_control *cpc)
//{
//	int entries = SIT_VBLOCK_MAP_SIZE / sizeof(unsigned long);
//	int max_blocks = sbi->blocks_per_seg;
//	unsigned long long segno = (unsigned long long) cpc->trim_start;
//	struct seg_entry *se = get_seg_entry(sbi, cpc->trim_start);
//	unsigned long *cur_map = (unsigned long *)se->cur_valid_map;
//	unsigned long *ckpt_map = (unsigned long *)se->ckpt_valid_map;
//	unsigned long *discard_map = (unsigned long *)se->discard_map;
//	unsigned long *dmap = SIT_I(sbi)->tmp_map;
//
//	unsigned int start = 0, end = -1, start_ddm = 0, end_ddm = -1;
//	bool force = (cpc->reason & CP_DISCARD);
//	int i;
//	unsigned long *ddmap;
//	bool ori_blk_exst = true;
//
//	if (force)
//		panic("FITRIM occurs!!!\n");
//
//
//	if (se->valid_blocks == max_blocks || !f2fs_hw_support_discard(sbi)){
//		return false;
//	} 
//	if (!force) {
//		if (!f2fs_realtime_discard_enable(sbi) || !se->valid_blocks ||
//			SM_I(sbi)->dcc_info->nr_discards >=
//				SM_I(sbi)->dcc_info->max_discards){
//			
//			return false;
//		}
//	}
//
//	
//	/* SIT_VBLOCK_MAP_SIZE should be multiple of sizeof(unsigned long) */
//	for (i = 0; i < entries; i++)
//		dmap[i] = force ? ~ckpt_map[i] & ~discard_map[i] :
//				(cur_map[i] ^ ckpt_map[i]) & ckpt_map[i];
//	
//	start = __find_rev_next_bit(dmap, max_blocks, end+1);
//	if (start >= max_blocks)
//		ori_blk_exst = false;
//
//	ddmap = get_ddmap_from_extended_ddm_hash(sbi, segno);
//	if (ddmap == NULL){
//		if (ori_blk_exst){
//			panic("check_ddm_sanity: no ddmap but ori_blk_exst");
//		}
//		return false;
//	}
//	
//	/* check existence of discarded block in original version dmap*/
//	while (SM_I(sbi)->dcc_info->nr_discards <=
//				SM_I(sbi)->dcc_info->max_discards) {
//		start = __find_rev_next_bit(dmap, max_blocks, end + 1);
//		if (start >= max_blocks)
//			break;
//		start_ddm = __find_rev_next_bit(ddmap, max_blocks, end_ddm + 1);
//
//		end = __find_rev_next_zero_bit(dmap, max_blocks, start + 1);
//		end_ddm = __find_rev_next_zero_bit(ddmap, max_blocks, start_ddm +1);
//
//		if (start != start_ddm || end != end_ddm)
//			panic("start end not match in add_discard_addrs");
//			//f2fs_bug_on(sbi, start != start_ddm || end != end_ddm);
//
//	}
//	return false;
//}



/*
 * CP calls this function, which flushes SIT entries including sit_journal,
 * and moves prefree segs to free segs.
 */
void f2fs_flush_sit_entries(struct f2fs_sb_info *sbi, struct cp_control *cpc)
{
	struct sit_info *sit_i = SIT_I(sbi);
	unsigned long *bitmap = sit_i->dirty_sentries_bitmap;
	struct curseg_info *curseg = CURSEG_I(sbi, CURSEG_COLD_DATA);
	struct f2fs_journal *journal = curseg->journal;
	struct sit_entry_set *ses, *tmp;
	struct list_head *head = &SM_I(sbi)->sit_entry_set;
	bool to_journal = !is_sbi_flag_set(sbi, SBI_IS_RESIZEFS);
	struct seg_entry *se;

	static unsigned long long last_t = 0;
	unsigned long long cur_t;
#ifdef MIGRATION_HANDLING_LATENCY
	unsigned long long tstart_, tend_;
#endif

	down_write(&sit_i->sentry_lock);
#ifdef MIGRATION_HANDLING_LATENCY
	tstart_ = OS_TimeGetUS();
#endif

	if (!sit_i->dirty_sentries)
		goto out;

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
	if (!__has_cursum_space(journal, sit_i->dirty_sentries, SIT_JOURNAL) ||
								!to_journal)
		remove_sits_in_journal(sbi);
	
	/*
	 * there are two steps to flush sit entries:
	 * #1, flush sit entries to journal in current cold data summary block.
	 * #2, flush sit entries to sit page.
	 */

	list_for_each_entry_safe(ses, tmp, head, set_list) {
		struct page *page = NULL;
		struct f2fs_sit_block *raw_sit = NULL;
		unsigned int start_segno = ses->start_segno;
		unsigned int end = min(start_segno + SIT_ENTRY_PER_BLOCK,
						(unsigned long)MAIN_SEG_SLOTS(sbi));
		unsigned int segno = start_segno;
	///////////////	
		f2fs_bug_on(sbi, start_segno > MAIN_SEG_SLOTS(sbi));

		if (to_journal &&
			!__has_cursum_space(journal, ses->entry_cnt, SIT_JOURNAL))
			to_journal = false;

		if (to_journal) {
			down_write(&curseg->journal_rwsem);
		} else {
			page = get_next_sit_page(sbi, start_segno);
			raw_sit = page_address(page);
		}

		// flush dirty sit entries in region of current sit set 
		for_each_set_bit_from(segno, bitmap, end) {
			int offset, sit_offset;
			//unsigned int dbg1, dbg2;
			f2fs_bug_on(sbi, segno > MAIN_SEG_SLOTS(sbi));

			se = get_seg_entry(sbi, segno);
#ifdef CONFIG_F2FS_CHECK_FS
			if (memcmp(se->cur_valid_map, se->cur_valid_map_mir,
						SIT_VBLOCK_MAP_SIZE))
				f2fs_bug_on(sbi, 1);
#endif

			if (to_journal) {
				offset = f2fs_lookup_journal_in_cursum(journal,
							SIT_JOURNAL, segno, 1);
				f2fs_bug_on(sbi, offset < 0);
				segno_in_journal(journal, offset) =
							cpu_to_le32(segno);
				//dbg1 = se->segno;
				//dbg2 = (&sit_in_journal(journal, offset))->segno;
				//printk("%s: 1 dbg1: %u dbg2: %u", __func__, dbg1, dbg2);
				if (!(se->segno == NULL_SEGNO && 
						(&sit_in_journal(journal, offset))->segno == NULL_SEGNO))
					seg_info_to_raw_sit(se,
						&sit_in_journal(journal, offset));
				//check_block_count(sbi, segno,
				//	&sit_in_journal(journal, offset));
			} else {
				sit_offset = SIT_ENTRY_OFFSET(sit_i, segno);
				//dbg1 = se->segno;
				//dbg2 = (&raw_sit->entries[sit_offset])->segno;
				//printk("%s: 2 dbg1: %u dbg2: %u", __func__, dbg1, dbg2);
				if (!(se->segno == NULL_SEGNO && 
						(&raw_sit->entries[sit_offset])->segno == NULL_SEGNO))
					seg_info_to_raw_sit(se,
							&raw_sit->entries[sit_offset]);
				//check_block_count(sbi, segno,
				//		&raw_sit->entries[sit_offset]);
			}

			__clear_bit(segno, bitmap);
			sit_i->dirty_sentries--;
			ses->entry_cnt--;
			//printk("%s: in loop dirty_sentries = %u", __func__, sit_i->dirty_sentries);
		}

		if (to_journal)
			up_write(&curseg->journal_rwsem);
		else
			f2fs_put_page(page, 1);

		f2fs_bug_on(sbi, ses->entry_cnt);
		release_sit_entry_set(ses);
	}
	//printk("%s: aft loop dirty_sentries = %u", __func__, sit_i->dirty_sentries);

	f2fs_bug_on(sbi, !list_empty(head));
	f2fs_bug_on(sbi, sit_i->dirty_sentries);
	
out:
	//if (cpc->reason & CP_DISCARD) {
	//	panic("%s: didn't expect CP_DISCARD\n", __func__);
	//	//__u64 trim_start = cpc->trim_start;

	//	//for (; cpc->trim_start <= cpc->trim_end; cpc->trim_start++){
	//	//	add_discard_addrs(sbi, cpc, false);
	//	//}

	//	//cpc->trim_start = trim_start;
	//}
	
	up_write(&sit_i->sentry_lock);
#ifdef MIGRATION_HANDLING_LATENCY
	tend_ = OS_TimeGetUS();
//	printk("%s duration: %llu usec", __func__, tend_-tstart_);
#endif

	struct free_segmap_info *free_i = FREE_I(sbi);

	set_prefree_as_free_segments(sbi);
	unsigned int total_inuse_sections = (MAIN_SECS_INTERVAL(sbi) - free_i->free_sections) + (MAIN_SECS_INTERVAL(sbi) - free_i->free_sections_node);

#ifdef PRINT_FREE_SEC
	cur_t = OS_TimeGetUS();
	//if (cur_t - last_t > 1000000) {
	if (cur_t - last_t > 5000000) {
		spin_lock(&free_i->segmap_lock);
		printk("section utilization of regular region: %lu %%", 
				100 * total_inuse_sections / MAIN_SECS_INTERVAL(sbi) 
		);
		spin_unlock(&free_i->segmap_lock);

		last_t = cur_t;
	}
#endif
}

#ifdef IPLFS_CALLBACK_IO

#ifdef MG_HANDLER_WRITE_NODE
static struct inode *find_gc_inode(struct gc_inode_list *gc_list, nid_t ino, struct inode_entry **ie_ret)
#else
static struct inode *find_gc_inode(struct gc_inode_list *gc_list, nid_t ino)
#endif
{
	struct inode_entry *ie;

	ie = radix_tree_lookup(&gc_list->iroot, ino);
#ifdef MG_HANDLER_WRITE_NODE
	*ie_ret = ie;
#endif
	if (ie)
		return ie->inode;
	return NULL;
}

static void add_gc_inode(struct gc_inode_list *gc_list, struct inode *inode)
{
	struct inode_entry *new_ie;

#ifdef MG_HANDLER_WRITE_NODE
	if (inode == find_gc_inode(gc_list, inode->i_ino, &new_ie)) {
		new_ie->cnt ++;
#else
	if (inode == find_gc_inode(gc_list, inode->i_ino)) {
#endif
		iput(inode);
		return;
	}
	new_ie = f2fs_kmem_cache_alloc(f2fs_inode_entry_slab, GFP_NOFS);
	new_ie->inode = inode;
#ifdef MG_HANDLER_WRITE_NODE
	new_ie->cnt = 1;
#endif

	f2fs_radix_tree_insert(&gc_list->iroot, inode->i_ino, new_ie);
	list_add_tail(&new_ie->list, &gc_list->ilist);
}

static void put_gc_inode(struct gc_inode_list *gc_list)
{
	struct inode_entry *ie, *next_ie;
	list_for_each_entry_safe(ie, next_ie, &gc_list->ilist, list) {
		radix_tree_delete(&gc_list->iroot, ie->inode->i_ino);
		iput(ie->inode);
		list_del(&ie->list);
		kmem_cache_free(f2fs_inode_entry_slab, ie);
	}
}

static inline bool check_dynamic_discard_map(struct f2fs_sb_info *sbi, block_t blk_addr)
{
	struct dynamic_discard_map *ddm;
	unsigned long long ddmkey;
	unsigned int offset_in_ddm;
	unsigned int height;
	unsigned int segno, offset;
	bool ret = false;

	segno = GET_SEGNO(sbi, blk_addr);
	offset = GET_BLKOFF_FROM_SEG0(sbi, blk_addr);

	mutex_lock(&SM_I(sbi)->ddmc_info->ddm_lock);
	
	get_ddm_info(sbi, segno, offset, &ddmkey, &offset_in_ddm);
	
	ddm = f2fs_lookup_hash(sbi, ddmkey, &height);
	if (!ddm){
		mutex_unlock(&SM_I(sbi)->ddmc_info->ddm_lock);
		return false;
	}
	
	ret = f2fs_test_bit(offset_in_ddm, ddm->dc_map);
	
	mutex_unlock(&SM_I(sbi)->ddmc_info->ddm_lock);
	
	return ret;
}

static void curseg_slot_to_prefree_candidate(struct f2fs_sb_info *sbi, struct curseg_info *curseg)
{
	struct slot_info *slot_i = SLT_I(sbi);
	struct slot_entry *slte;
	//struct seg_entry *se;
		
	if (curseg->slot_idx == NULL_SLOTNO)
			printk("%s: curseg->segno:%u curseg->slot_idx: %u", __func__, 
					curseg->segno, curseg->slot_idx);

	slte = get_slot_entry(sbi, curseg->slot_idx);
	//se = get_seg_entry(sbi, curseg->slot_idx);

	//unset_slot_inuse(slot_i, slte);
	//set_slot_prefree(slot_i, slte);
	//se->segno = NULL_SEGNO;
	if (slte->segno == NULL_SEGNO) {
		/* already prefreed */
		f2fs_bug_on(sbi, hash_hashed(&slte->hnode));
		f2fs_bug_on(sbi, list_empty(&slte->list));
	} else if (list_empty(&slte->list)) {
		/* add to prefree candidate */
		list_add_tail(&slte->list, &slot_i->prefree_candidate_list);
	}
}


static int __reflect_node_page_migration(struct page *page,
				block_t old_blkaddr, 
				block_t new_blkaddr, uint64_t old_slot_idx)
{
	struct f2fs_sb_info *sbi = F2FS_P_SB(page);
	nid_t nid;
	struct node_info ni;
	struct f2fs_summary sum;
	struct curseg_info *curseg = CURSEG_I(sbi, CURSEG_MIGRATION_NODE);
	unsigned int segno = GET_SEGNO(sbi, new_blkaddr);
	struct sit_info *sit_i = SIT_I(sbi);
	struct slot_info *slot_i = SLT_I(sbi);

	//trace_f2fs_writepage(page, NODE);
	//f2fs_lock_op(sbi);

	if (unlikely(f2fs_cp_error(sbi))) {
		if (is_sbi_flag_set(sbi, SBI_IS_CLOSE)) {
			printk("%s: unexpected!!", __func__);
			f2fs_bug_on(sbi, 1);
			ClearPageUptodate(page);
			dec_page_count(sbi, F2FS_DIRTY_NODES);
			unlock_page(page);
			return 0;
		}
		goto redirty_out;
	}

	if (unlikely(is_sbi_flag_set(sbi, SBI_POR_DOING))) {
		printk("%s: unexpected 2!!", __func__);
		f2fs_bug_on(sbi, 1);
		goto redirty_out;
	}

	//if (!is_sbi_flag_set(sbi, SBI_CP_DISABLED) &&
	//		wbc->sync_mode == WB_SYNC_NONE &&
	//		IS_DNODE(page) && is_cold_node(page)) {
	//	printk("%s: unexpected 3!!", __func__);
	//	f2fs_bug_on(sbi, 1);
	//	goto redirty_out;
	//}

	/* get old block addr of this node page */
	nid = nid_of_node(page);
	f2fs_bug_on(sbi, page->index != nid);

	if (f2fs_get_node_info(sbi, nid, &ni)) {
		printk("%s: unexpected 4!!", __func__);
		f2fs_bug_on(sbi, 1);
		goto redirty_out;
	}

	down_read(&sbi->node_write);

	/* This page is already truncated */
	if (unlikely(ni.blk_addr == NULL_ADDR)) {
		printk("%s: becomes null!!!!!!!!!!!!!", __func__);
		//f2fs_bug_on(sbi, 1);
		ClearPageUptodate(page);
		//dec_page_count(sbi, F2FS_DIRTY_NODES);
		up_read(&sbi->node_write);
		unlock_page(page);
		f2fs_unlock_op(sbi);
		return 0;
	}

	if (__is_valid_data_blkaddr(ni.blk_addr) &&
		check_dynamic_discard_map(sbi, ni.blk_addr)) {
		up_read(&sbi->node_write);
		goto redirty_out;
	}
	/*
	if (__is_valid_data_blkaddr(ni.blk_addr) &&
		!f2fs_is_valid_blkaddr(sbi, ni.blk_addr,
					DATA_GENERIC_ENHANCE)) {
		up_read(&sbi->node_write);
		goto redirty_out;
	}
	*/

	/* should add to global list before clearing PAGECACHE status */
	//if (f2fs_in_warm_node_list(sbi, page)) {
	//	printk("%s: unexpected!!!!!!!!!!!!", __func__);
	//	f2fs_bug_on(sbi, 1);
	//	//seq = f2fs_add_fsync_node_entry(sbi, page);
	//}

	//set_page_writeback(page);
	//ClearPageError(page);

	set_summary(&sum, nid, 0, 0);

	//f2fs_do_write_node_page(nid, &fio);

	/* update sum and seg count of new addr and seg count of old addr . */
	/*down_read(&SM_I(sbi)->curseg_lock);
	mutex_lock(&curseg->curseg_mutex);*/

	/* update new addr's sum*/
	if (segno != curseg->segno) {
		bool flush = true;
		
		down_write(&sit_i->sentry_lock);
		mutex_lock(&slot_i->lock);
		
		if (curseg->segno != NULL_SEGNO) {
			if (get_seg_entry(sbi, curseg->slot_idx)->valid_blocks == 0) {
				//0313 modify
				curseg_slot_to_prefree_candidate(sbi, curseg);
				//struct slot_entry *slte = get_slot_entry(sbi, curseg->slot_idx);
				//unset_slot_inuse(slot_i, slte);
				//set_slot_prefree(slot_i, slte);
				//get_seg_entry(sbi, curseg->slot_idx)->segno = NULL_SEGNO;
				//0313 modify end
				
				flush = false;
			}
		}
		
		mutex_unlock(&slot_i->lock);
		up_write(&sit_i->sentry_lock);

		curseg->next_segno = segno;
//#ifdef SHIVAL
//		__change_curseg(sbi, CURSEG_MIGRATION_NODE, true);
//#else
		//change_curseg(sbi, CURSEG_MIGRATION_NODE, flush);
		reset_migration_curseg(sbi, CURSEG_MIGRATION_NODE, flush);
//#endif
	}

	curseg->next_blkoff = new_blkaddr - START_BLOCK(sbi, segno);
	__add_sum_entry(sbi, CURSEG_MIGRATION_NODE, &sum);
	
	/* update seg count and ddm of old addr and new addr */
	down_write(&sit_i->sentry_lock);
	mutex_lock(&slot_i->lock);
	
	f2fs_bug_on(sbi, old_slot_idx == NULL_SLOTNO);
	f2fs_bug_on(sbi, curseg->slot_idx == NULL_SLOTNO);
	update_slot_entry(sbi, old_slot_idx, -1, GET_SEGNO(sbi, old_blkaddr));
	update_slot_entry(sbi, curseg->slot_idx, 1, curseg->segno);
	
	//update_sit_entry(sbi, old_blkaddr, -1, old_slot_idx);
	//update_sit_entry(sbi, new_blkaddr, 1, curseg->slot_idx);

#ifdef SHIVAL
	static int cnt = 0;
	static uint64_t old_segno_stack[10], new_segno_stack[10];
	static uint64_t old_slot_stack[10], new_slot_stack[10];
	static uint64_t old_addr_stack[10], new_addr_stack[10];
	if ( (old_blkaddr & 0xe0000000) == 0xe0000000 || 
			(new_blkaddr & 0xe0000000) == 0xe0000000) {
		printk("%s: old_segno: %lu old_addr: 0x%lx old_slot_idx: %lu new_segno: %lu new_addr: 0x%lx new_slot_idx: %lu", 
				__func__, GET_SEGNO(sbi, old_blkaddr), old_blkaddr, old_slot_idx, curseg->segno, new_blkaddr, curseg->slot_idx);
	}
	
	//if ( (old_blkaddr & 0xe0000000) == 0xe0000000 || 
	//		(new_blkaddr & 0xe0000000) == 0xe0000000) {
	//	old_segno_stack[cnt] = GET_SEGNO(sbi, old_blkaddr);
	//	new_segno_stack[cnt] = curseg->segno;
	//	old_slot_stack[cnt] = old_slot_idx;
	//	new_slot_stack[cnt] = curseg->slot_idx;
	//	old_addr_stack[cnt] = old_blkaddr;
	//	new_addr_stack[cnt] = new_blkaddr;
	//	cnt ++;
	//	if (cnt == 10) {
	//		printk("%s: \n \
	//		old_segno: %lu old_addr: 0x%lx old_slot_idx: %lu new_segno: %lu new_addr: 0x%lx new_slot_idx: %lu, \n \
	//		old_segno: %lu old_addr: 0x%lx old_slot_idx: %lu new_segno: %lu new_addr: 0x%lx new_slot_idx: %lu, \n \
	//		old_segno: %lu old_addr: 0x%lx old_slot_idx: %lu new_segno: %lu new_addr: 0x%lx new_slot_idx: %lu, \n \
	//		old_segno: %lu old_addr: 0x%lx old_slot_idx: %lu new_segno: %lu new_addr: 0x%lx new_slot_idx: %lu, \n \
	//		old_segno: %lu old_addr: 0x%lx old_slot_idx: %lu new_segno: %lu new_addr: 0x%lx new_slot_idx: %lu, \n \
	//		old_segno: %lu old_addr: 0x%lx old_slot_idx: %lu new_segno: %lu new_addr: 0x%lx new_slot_idx: %lu, \n \
	//		old_segno: %lu old_addr: 0x%lx old_slot_idx: %lu new_segno: %lu new_addr: 0x%lx new_slot_idx: %lu, \n \
	//		old_segno: %lu old_addr: 0x%lx old_slot_idx: %lu new_segno: %lu new_addr: 0x%lx new_slot_idx: %lu, \n \
	//		old_segno: %lu old_addr: 0x%lx old_slot_idx: %lu new_segno: %lu new_addr: 0x%lx new_slot_idx: %lu, \n \
	//		old_segno: %lu old_addr: 0x%lx old_slot_idx: %lu new_segno: %lu new_addr: 0x%lx new_slot_idx: %lu, \
	//		", __func__,
	//		old_segno_stack[0], old_addr_stack[0], old_slot_stack[0], new_segno_stack[0], new_addr_stack[0], new_slot_stack[0], 
	//		old_segno_stack[1], old_addr_stack[1], old_slot_stack[1], new_segno_stack[1], new_addr_stack[1], new_slot_stack[1], 
	//		old_segno_stack[2], old_addr_stack[2], old_slot_stack[2], new_segno_stack[2], new_addr_stack[2], new_slot_stack[2], 
	//		old_segno_stack[3], old_addr_stack[3], old_slot_stack[3], new_segno_stack[3], new_addr_stack[3], new_slot_stack[3], 
	//		old_segno_stack[4], old_addr_stack[4], old_slot_stack[4], new_segno_stack[4], new_addr_stack[4], new_slot_stack[4], 
	//		old_segno_stack[5], old_addr_stack[5], old_slot_stack[5], new_segno_stack[5], new_addr_stack[5], new_slot_stack[5], 
	//		old_segno_stack[6], old_addr_stack[6], old_slot_stack[6], new_segno_stack[6], new_addr_stack[6], new_slot_stack[6], 
	//		old_segno_stack[7], old_addr_stack[7], old_slot_stack[7], new_segno_stack[7], new_addr_stack[7], new_slot_stack[7], 
	//		old_segno_stack[8], old_addr_stack[8], old_slot_stack[8], new_segno_stack[8], new_addr_stack[8], new_slot_stack[8], 
	//		old_segno_stack[9], old_addr_stack[9], old_slot_stack[9], new_segno_stack[9], new_addr_stack[9], new_slot_stack[9]  
	//		);
	//		cnt = 0;
	//	}
	//}
#endif

	mutex_unlock(&slot_i->lock);
	
	__mark_sit_entry_dirty(sbi, old_slot_idx);
	__mark_sit_entry_dirty(sbi, curseg->slot_idx);

	locate_dirty_segment(sbi, GET_SEGNO(sbi, old_blkaddr), old_slot_idx);
	//locate_dirty_segment(sbi, curseg->segno, curseg->slot_idx);

	up_write(&sit_i->sentry_lock);

	//mutex_unlock(&curseg->curseg_mutex);
	//up_read(&SM_I(sbi)->curseg_lock);

	set_node_addr(sbi, &ni, new_blkaddr, is_fsync_dnode(page));
	
	//dec_page_count(sbi, F2FS_DIRTY_NODES);
	up_read(&sbi->node_write);

	unlock_page(page);

	if (unlikely(f2fs_cp_error(sbi))) {
		printk("%s: unexpected 6", __func__);
		f2fs_bug_on(sbi, 1);
		f2fs_submit_merged_write(sbi, NODE);
	}

	//f2fs_unlock_op(sbi);
	return 0;

redirty_out:
	printk("%s: unexpected!!", __func__);
	//f2fs_unlock_op(sbi);
	f2fs_bug_on(sbi, 1);
	//redirty_page_for_writepage(wbc, page);
	return AOP_WRITEPAGE_ACTIVATE;
}

//!!!!!!!!!!!!!!!!!!!!!!
//static int reflect_node_page_migration_light(nid_t nid, block_t old_blkaddr, 
//		block_t new_blkaddr, uint64_t old_slot_idx)
//{
//	int err = 0;
//
//	//f2fs_wait_on_page_writeback(node_page, NODE, true, true);
//
//	/* TODO: Do we need to dirty? */
//	//set_page_dirty(node_page);
//
//	//clear_page_dirty_for_io(node_page);
//	//if (!clear_page_dirty_for_io(node_page)) {
//	//	err = -EAGAIN;
//	//	goto out_page;
//	//}
//
//	if (__reflect_node_page_migration_light(nid, old_blkaddr, new_blkaddr, old_slot_idx)) {
//		printk("%s: unexpected!!!!", __func__);
//		err = -EAGAIN;
//		//unlock_page(node_page);
//	}
//	goto release_page;
//
////out_page:
//	//unlock_page(node_page);
//release_page:
//	//f2fs_put_page(node_page, 0);
//	return err;
//}

static int reflect_node_page_migration(struct f2fs_sb_info *sbi, struct page *node_page, block_t old_blkaddr, 
		block_t new_blkaddr, uint64_t old_slot_idx)
{
	int err = 0;
	struct migration_control *mgc = SM_I(sbi)->mgc_info;

	/* this line is commented for node migration optimization */
	//f2fs_wait_on_page_writeback(node_page, NODE, true, true);

	if (__reflect_node_page_migration(node_page, old_blkaddr, new_blkaddr, old_slot_idx)) {
		printk("%s: unexpected!!!!", __func__);
		err = -EAGAIN;
		unlock_page(node_page);
	}
#ifdef MIGRATION_HANDLING_LATENCY
	atomic_inc(&mgc->node_pgs);
#endif
	goto release_page;

out_page:
	unlock_page(node_page);
release_page:
	f2fs_put_page(node_page, 0);
	return err;
}

static inline bool migration_segment_match(struct f2fs_sb_info *sbi,
		unsigned int segno, unsigned char type);
static inline struct f2fs_summary_block *get_curseg_sum(struct f2fs_sb_info *sbi, 
		unsigned char type, unsigned int segno);


static inline int __f2fs_check_nid_range(struct f2fs_sb_info *sbi, nid_t nid)
{
	if (unlikely(nid < F2FS_ROOT_INO(sbi) || nid >= NM_I(sbi)->max_nid)) {
		return -EFSCORRUPTED;
	}
	return 0;
}

#ifdef NODE_READ_PIPELINE
static void preread_node_of_node_segment(struct f2fs_sb_info *sbi, struct f2fs_summary *sum, 
		unsigned int segno, struct migration_seg_info *ms_info, uint64_t slot_idx)
{
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 0;
	//bool fggc = true; /* TODO: Is this right for migration? */
	//int submitted = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno), i;
	uint64_t old_addr, new_addr;
	nid_t nid;
	struct page *node_page;
	struct node_info ni;
	int err = 0;
//#ifdef SHIVAL
	int tmpiii;
	int tmpoff;
	block_t old_addr_tmp;
	struct f2fs_summary *tmp_entry;
//#endif
	start_addr = START_BLOCK(sbi, segno);

next_step:
	entry = sum;
		
	//if (fggc && phase == 2)
	//	atomic_inc(&sbi->wb_sync_req[NODE]);
	
	for (i = 0; i < ms_info->nblks; i ++) {
		old_addr = le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + i].old_lba);
		new_addr = le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + i].new_lba);
		
		f2fs_bug_on(sbi, segno != GET_SEGNO(sbi, old_addr));

		off = old_addr - start_addr;
		entry = sum + off;
		
		nid = le32_to_cpu(entry->nid);
	
		if (check_dynamic_discard_map(sbi, old_addr))
			continue;
		//if (check_valid_map(sbi, segno, off) == 0)
		//	continue;
	
		if (phase == 0) {
			f2fs_ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
							META_NAT, true);
			continue;
		}
	
		if (nid && __f2fs_check_nid_range(sbi, nid)) {
			continue;
		}
		if (phase == 1) {
//#ifdef SHIVAL
			if (nid && f2fs_check_nid_range(sbi, nid)) {
				unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
				printk("%s nid problem!!!!!!!!", __func__);
				printk("%s: cid: %u prob addr: 0x%lx off: %u slot idx: %lu", 
						__func__, ms_info->mge->command_id, old_addr, off, slot_idx);
				for (tmpiii = 0; tmpiii < ms_info->nblks; tmpiii ++) {
					old_addr_tmp = le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + tmpiii].old_lba);
					printk("ofs in sum: %u old addr: 0x%lx new addr: 0x%lx", 
						old_addr_tmp - start_addr, old_addr_tmp, 
						le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + tmpiii].new_lba));
				}
				tmp_entry = sum;
				
				for (tmpoff = 0; tmpoff < usable_blks_in_seg; tmpoff++, tmp_entry++) {
					printk("idx: %d nid: 0x%x ofs_in_node: 0x%x", 
							tmpoff, le32_to_cpu(tmp_entry->nid), 
							le16_to_cpu(tmp_entry->ofs_in_node));
				}

			}
//#endif
			f2fs_ra_node_page(sbi, nid);
			continue;
		}
	
	}
	
	if (++phase < 2)
		goto next_step;
	
}
#endif

static void reflect_node_segment_migration(struct f2fs_sb_info *sbi, struct f2fs_summary *sum, 
		unsigned int segno, struct migration_seg_info *ms_info, uint64_t slot_idx)
{
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 0;
	//bool fggc = true; /* TODO: Is this right for migration? */
	//int submitted = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno), i;
	uint64_t old_addr, new_addr;
	nid_t nid;
	struct page *node_page;
	struct node_info ni;
	int err = 0;
//#ifdef SHIVAL
	int tmpiii;
	int tmpoff;
	block_t old_addr_tmp;
	struct f2fs_summary *tmp_entry;
//#endif
	start_addr = START_BLOCK(sbi, segno);

next_step:
	entry = sum;
		
	//if (fggc && phase == 2)
	//	atomic_inc(&sbi->wb_sync_req[NODE]);
	
	for (i = 0; i < ms_info->nblks; i ++) {
		old_addr = le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + i].old_lba);
		new_addr = le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + i].new_lba);
		
		if (segno != GET_SEGNO(sbi, old_addr)) {
			printk("%s: prob mge: idx: %u old addr: 0x%lx new addr: 0x%lx sidx: %u i: %u nblks: %u", 
				__func__, ms_info->start_idx + i, old_addr, new_addr, 
				ms_info->start_idx, i, ms_info->nblks);
			for (tmpiii = 0; tmpiii < ms_info->nblks; tmpiii ++) {
				old_addr_tmp = le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + tmpiii].old_lba);
				printk("%s: prob range mge: idx: %u old addr: 0x%lx new addr: 0x%lx", 
					__func__, ms_info->start_idx + tmpiii, old_addr_tmp, 
					le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + tmpiii].new_lba));
			}
			
			for (tmpiii = 0; tmpiii < 256; tmpiii ++) {
				old_addr_tmp = le64_to_cpu(ms_info->mge->mg_pairs[tmpiii].old_lba);
				printk("%s: mge profile: mgeidx: %u old addr: 0x%lx new addr: 0x%lx", 
					__func__, tmpiii, old_addr_tmp, 
					le64_to_cpu(ms_info->mge->mg_pairs[tmpiii].new_lba));
			}
		}
		f2fs_bug_on(sbi, segno != GET_SEGNO(sbi, old_addr));

		off = old_addr - start_addr;
		entry = sum + off;
		
		nid = le32_to_cpu(entry->nid);
	
		if (check_dynamic_discard_map(sbi, old_addr))
			continue;
		//if (check_valid_map(sbi, segno, off) == 0)
		//	continue;
	
		if (phase == 0) {
			f2fs_ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
							META_NAT, true);
			continue;
		}
	
		if (nid && __f2fs_check_nid_range(sbi, nid)) {
			continue;
		}
		if (phase == 1) {
//#ifdef SHIVAL
			if (nid && f2fs_check_nid_range(sbi, nid)) {
				unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
				printk("%s nid problem!!!!!!!!", __func__);
				printk("%s: cid: %u prob addr: 0x%lx off: %u slot idx: %lu", 
						__func__, ms_info->mge->command_id, old_addr, off, slot_idx);
				for (tmpiii = 0; tmpiii < ms_info->nblks; tmpiii ++) {
					old_addr_tmp = le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + tmpiii].old_lba);
					printk("ofs in sum: %u old addr: 0x%lx new addr: 0x%lx", 
						old_addr_tmp - start_addr, old_addr_tmp, 
						le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + tmpiii].new_lba));
				}
				tmp_entry = sum;
				
				for (tmpoff = 0; tmpoff < usable_blks_in_seg; tmpoff++, tmp_entry++) {
					printk("idx: %d nid: 0x%x ofs_in_node: 0x%x", 
							tmpoff, le32_to_cpu(tmp_entry->nid), 
							le16_to_cpu(tmp_entry->ofs_in_node));
				}

			}
//#endif
			f2fs_ra_node_page(sbi, nid);
			continue;
		}
	
		/* phase == 2 */
#ifdef SHIVAL
		if (nid && f2fs_check_nid_range(sbi, nid)) {
			printk("%s: unexpected problem!! nid: 0x%x old_addr: 0x%x, new_addr: 0x%x", 
					__func__, nid, old_addr, new_addr);
				unsigned int usable_blks_in_seg_ = f2fs_usable_blks_in_seg(sbi, segno);
				printk("%s: cid: %u prob addr: 0x%lx off: %u slot idx: %lu", 
						__func__, ms_info->mge->command_id, old_addr, off, slot_idx);
				for (tmpiii = 0; tmpiii < ms_info->nblks; tmpiii ++) {
					old_addr_tmp = le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + tmpiii].old_lba);
					printk("ofs in sum: %u old addr: 0x%lx new addr: 0x%lx", 
						old_addr_tmp - start_addr, old_addr_tmp, 
						le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + tmpiii].new_lba));
				}
				tmp_entry = sum;
				
				for (tmpoff = 0; tmpoff < usable_blks_in_seg_; tmpoff++, tmp_entry++) {
					printk("idx: %d nid: 0x%x ofs_in_node: 0x%x", 
							tmpoff, le32_to_cpu(tmp_entry->nid), 
							le16_to_cpu(tmp_entry->ofs_in_node));
				}
		}
#endif
		f2fs_lock_op_mg(sbi);
		node_page = f2fs_get_node_page(sbi, nid);
		if (IS_ERR(node_page)) {
			//printk("%s: unexpected  f2fs_get_node_page fail. nid: 0x%x", __func__, nid);
			f2fs_unlock_op_mg(sbi);
			continue;
		}
	
		/* block may become invalid during f2fs_get_node_page */
		if (check_dynamic_discard_map(sbi, old_addr)) {
			f2fs_put_page(node_page, 1);
			f2fs_unlock_op_mg(sbi);
			continue;
		}
		//if (check_valid_map(sbi, segno, off) == 0) {
		//	f2fs_put_page(node_page, 1);
		//	continue;
		//}
	
		if (f2fs_get_node_info(sbi, nid, &ni)) {
			printk("%s: unexpected!! f2fs_get_node_info fail", __func__);
			f2fs_put_page(node_page, 1);
			f2fs_unlock_op_mg(sbi);
			continue;
		}
	
		if (ni.blk_addr != start_addr + off) {
			//printk("%s: nid: %u calc ni lba: 0x%lx segno: %u old_addr: 0x%lx segno: %u slot_idx: %lu new_addr: 0x%lx segno: %u",
			//	   	__func__, nid,
			//		ni.blk_addr, GET_SEGNO(sbi, ni.blk_addr), old_addr, GET_SEGNO(sbi, old_addr),
			//		slot_idx,
			//		new_addr, GET_SEGNO(sbi, new_addr));

			//if (f2fs_get_node_info(sbi, nid_of_node(node_page), &ni)) {
			//	printk("%s: nid; %u nat blkaddr: 0x%lx old_addr: 0x%lx", 
			//			__func__, nid_of_node(node_page), ni.blk_addr, old_addr);
			//} else {
			//	printk("%s: not exist!! nid from sum: %u nid from page %u ", __func__, nid, nid_of_node(node_page));
			//}
			//struct page *sum_page = find_get_page(META_MAPPING(sbi),
			//			GET_SUM_BLOCK(sbi, slot_idx));
			//f2fs_put_page(sum_page, 0);
			//
			//if (!PageUptodate(sum_page) || unlikely(f2fs_cp_error(sbi))) {
			//	printk("%s: unexpected!! page not uptodated", __func__);
			//	//f2fs_bug_on(sbi, 1);
			//}
			//
			//struct f2fs_summary_block *sum1 = NULL, *sum2;	
			//struct f2fs_summary *sum1_e = NULL, *sum2_e;	
			//unsigned int cur_segno = GET_SEGNO(sbi, old_addr);
			//if (migration_segment_match(sbi, cur_segno, CURSEG_MIGRATION_NODE)) {
			//	sum1 = get_curseg_sum(sbi, CURSEG_MIGRATION_NODE, cur_segno);
			//	sum1_e = sum1->entries;
			//	sum1_e += off;
			//}
			//sum2 = page_address(sum_page);
			//sum2_e = sum2->entries;
			//sum2_e += off;
			//if (sum1 != NULL) {
			//	printk("sum1->nid: %u sum2->nid: %u", 
			//			le32_to_cpu(sum1_e->nid), 
			//			le32_to_cpu(sum2_e->nid));
			//} else {
			//	printk("sum2->nid: %u", 
			//			le32_to_cpu(sum2_e->nid));

			//}
			

			//f2fs_bug_on(sbi, 1);
			f2fs_put_page(node_page, 1);
			f2fs_unlock_op_mg(sbi);
			continue;
		}
	
		err = reflect_node_page_migration(sbi, node_page, old_addr, new_addr, slot_idx);
		if (err)
			printk("%s: unexpected errno: %d", __func__, err);
		//	submitted++;
		//stat_inc_node_blk_count(sbi, 1, gc_type);
		f2fs_unlock_op_mg(sbi);
	}
	
	if (++phase < 3)
		goto next_step;
	
	//if (fggc)
	//	atomic_dec(&sbi->wb_sync_req[NODE]);

	//return submitted;

}

#ifdef LM_NO_INODE_READ
static int __reflect_data_page_migration_light_no_inode(struct f2fs_sb_info *sbi,
		struct f2fs_mg_sum_info *sum_info, block_t new_blkaddr)
{
	struct dnode_of_data dn;
	struct node_info ni;
	int err = 0;
	struct f2fs_summary sum;
	struct curseg_info *curseg = CURSEG_I(sbi, CURSEG_MIGRATION_DATA);
	unsigned int segno = GET_SEGNO(sbi, new_blkaddr);
	struct sit_info *sit_i = SIT_I(sbi);
	struct slot_info *slot_i = SLT_I(sbi);
	nid_t nid = sum_info->nid;
	unsigned int ofs_in_node = sum_info->ofs_in_node;
	block_t old_blkaddr = sum_info->old_blkaddr;
	struct page *node_page;
#ifdef MIGRATION_HANDLING_LATENCY
	unsigned long long tstart, tend;
#endif
	
	//err = f2fs_get_dnode_of_data(&dn, bidx, LOOKUP_NODE);
#ifdef MIGRATION_HANDLING_LATENCY
	tstart = OS_TimeGetUS();
#endif
	node_page = f2fs_get_node_page(sbi, nid);
#ifdef MIGRATION_HANDLING_LATENCY
	tend = OS_TimeGetUS();
	mgc->node_read_time += (tend-tstart);
	mgc->node_read_cnt ++;
#endif
	if (!node_page){
		printk("%s: unexpected. nid: %u", __func__, nid);
		f2fs_bug_on(sbi, 1 );	
		goto out;
	}

	if (IS_INODE(node_page)) {
		printk("%s: not expected!!!!!!!!!!!!! node page is inode", __func__);
	}
	
	set_new_dnode(&dn, NULL, NULL, NULL, nid);
	dn.node_page = node_page;
	dn.ofs_in_node = sum_info->ofs_in_node;

	
	dn.data_blkaddr = data_blkaddr(NULL, node_page, ofs_in_node);
	
	if (old_blkaddr != dn.data_blkaddr) {
		printk("%s: not expected!!! data_blkaddr: 0x%lx old_blkaddr: 0x%lx", __func__,
				dn.data_blkaddr, old_blkaddr);
		f2fs_bug_on(sbi, 1 );	
	}

	/* check discard bitmap */
	if (__is_valid_data_blkaddr(dn.data_blkaddr) &&
		check_dynamic_discard_map(sbi, dn.data_blkaddr)) {
		printk("%s: unexpected. ", __func__);
		f2fs_bug_on(sbi, 1);
		err = -EFSCORRUPTED;
		goto out_writepage;
	}

	err = f2fs_get_node_info(sbi, nid, &ni);
	if (err) {
		printk("%s: not expected", __func__);
		goto out_writepage;
	}
#ifdef SHIVAL
	if (nid && f2fs_check_nid_range(sbi, nid)) {
		printk("%s: nid: %u ofs_in_node: %u new_blkaddr: 0x%lx",
				__func__, nid, ofs_in_node, 
				new_blkaddr);
		f2fs_bug_on(sbi, 1);
	}
#endif
	/* update sum */
	set_summary(&sum, nid, ofs_in_node, ni.version);

	/* update sum and seg count of new addr and seg count of old addr . */
	down_read(&SM_I(sbi)->curseg_lock);
	mutex_lock(&curseg->curseg_mutex);

	if (segno != curseg->segno) {
		bool flush = true;
		
		down_write(&sit_i->sentry_lock);
		mutex_lock(&slot_i->lock);

		if (curseg->segno != NULL_SEGNO) {
			if (get_seg_entry(sbi, curseg->slot_idx)->valid_blocks == 0) {
				//0313 modify
				curseg_slot_to_prefree_candidate(sbi, curseg);
				//struct slot_entry *slte = get_slot_entry(sbi, curseg->slot_idx);
				//unset_slot_inuse(slot_i, slte);
				//set_slot_prefree(slot_i, slte);
				//get_seg_entry(sbi, curseg->slot_idx)->segno = NULL_SEGNO;
				//0313 modify end
				flush = false;
			}
		}
		
		mutex_unlock(&slot_i->lock);
		up_write(&sit_i->sentry_lock);

		curseg->next_segno = segno;
//#ifdef SHIVAL
//		__change_curseg(sbi, CURSEG_MIGRATION_DATA, true);
//#else
		//change_curseg(sbi, CURSEG_MIGRATION_DATA, flush);
		reset_migration_curseg(sbi, CURSEG_MIGRATION_DATA, flush);
//#endif
	}

	curseg->next_blkoff = new_blkaddr - START_BLOCK(sbi, segno);
	__add_sum_entry(sbi, CURSEG_MIGRATION_DATA, &sum);

	/* update seg count and ddm of old addr and new addr */
	down_write(&sit_i->sentry_lock);
	mutex_lock(&slot_i->lock);
	
	f2fs_bug_on(sbi, sum_info->slot_idx == NULL_SLOTNO);
	f2fs_bug_on(sbi, curseg->slot_idx == NULL_SLOTNO);
	update_slot_entry(sbi, sum_info->slot_idx, -1, GET_SEGNO(sbi, sum_info->old_blkaddr));
	update_slot_entry(sbi, curseg->slot_idx, 1, curseg->segno);

	//update_sit_entry(sbi, sum_info->old_blkaddr, -1, sum_info->slot_idx);
	//update_sit_entry(sbi, new_blkaddr, 1, curseg->slot_idx);
	mutex_unlock(&slot_i->lock);
	
	__mark_sit_entry_dirty(sbi, sum_info->slot_idx);
	__mark_sit_entry_dirty(sbi, curseg->slot_idx);

	locate_dirty_segment(sbi, GET_SEGNO(sbi, sum_info->old_blkaddr), sum_info->slot_idx);
	//locate_dirty_segment(sbi, curseg->segno, curseg->slot_idx);
	
	up_write(&sit_i->sentry_lock);
	
	f2fs_update_data_blkaddr_no_inode(&dn, new_blkaddr);
	//f2fs_update_data_blkaddr(&dn, new_blkaddr);
	
	mutex_unlock(&curseg->curseg_mutex);
	up_read(&SM_I(sbi)->curseg_lock);
	
	//trace_f2fs_do_write_data_page(page, OPU);
	//set_inode_flag(inode, FI_APPEND_WRITE);
	//if (page->index == 0)
	//	set_inode_flag(inode, FI_FIRST_BLOCK_WRITTEN);
out_writepage:
	//f2fs_put_dnode(&dn);
	if (node_page)
		f2fs_put_page(node_page, 1);
out:
	//f2fs_unlock_op(sbi);
	return err;
}
#endif

static int __reflect_data_page_migration_light(struct f2fs_sb_info *sbi, struct inode *inode,
		struct f2fs_mg_sum_info *sum_info, block_t new_blkaddr, block_t bidx)
{
	//struct inode *inode = page->mapping->host;
	struct dnode_of_data dn;
	struct node_info ni;
	int err = 0;
	struct f2fs_summary sum;
	struct curseg_info *curseg = CURSEG_I(sbi, CURSEG_MIGRATION_DATA);
	unsigned int segno = GET_SEGNO(sbi, new_blkaddr);
	struct sit_info *sit_i = SIT_I(sbi);
	struct slot_info *slot_i = SLT_I(sbi);
	struct migration_control *mgc = SM_I(sbi)->mgc_info;
#ifdef MIGRATION_HANDLING_LATENCY
	unsigned long long tstart, tend;
	unsigned long long tstart_, tend_;
#endif

	set_new_dnode(&dn, inode, NULL, NULL, 0);
	
	//f2fs_lock_op(sbi);
//	if (!f2fs_trylock_op(sbi)){
//		printk("%s: unexpected!!!!!!!!!!!!!!!!!!!!!!!!", __func__);
//		f2fs_bug_on(sbi, 1);
//		return -EAGAIN;
//	}
	//printk("%s start", __func__);

#ifdef MIGRATION_HANDLING_LATENCY
	tstart = OS_TimeGetUS();
#endif
	err = f2fs_get_dnode_of_data(&dn, bidx, LOOKUP_NODE);
#ifdef MIGRATION_HANDLING_LATENCY
	tend = OS_TimeGetUS();
	mgc->node_read_time += (tend-tstart);
	mgc->node_read_cnt ++;
#endif
	if (err)
		goto out;

	/* This page is already truncated */
	if (dn.data_blkaddr == NULL_ADDR) {
		printk("%s: unexpected. old blkaddr is NULL.", __func__);
		f2fs_bug_on(sbi, 1);
		goto out_writepage;
	} else if (dn.data_blkaddr != sum_info->old_blkaddr) {
	   	if (dn.nid != sum_info->nid 
			|| dn.ofs_in_node != sum_info->ofs_in_node) {
			printk("%s: unexpected. blk addr are different. \
				dn lba: 0x%lx mg old lba: 0x%lx \
				dn.nid: %u sum nid: %u \
				dn ofs in node: %u sum ofs in node: %u", __func__,
				dn.data_blkaddr, sum_info->old_blkaddr,
			   	dn.nid, sum_info->nid,
				dn.ofs_in_node, sum_info->ofs_in_node
			  );
			f2fs_bug_on(sbi, 1);
		}
		goto out_writepage;
		return 0;
	}

//got_it:
	/* check discard bitmap */
	//if (__is_valid_data_blkaddr(dn.data_blkaddr) &&
	//	check_dynamic_discard_map(sbi, dn.data_blkaddr)) {
	//	printk("%s: unexpected. ", __func__);
	//	f2fs_bug_on(sbi, 1);
	//	err = -EFSCORRUPTED;
	//	goto out_writepage;
	//}

	//if (__is_valid_data_blkaddr(fio->old_blkaddr) &&
	//	!f2fs_is_valid_blkaddr(fio->sbi, fio->old_blkaddr,
	//					DATA_GENERIC_ENHANCE)) {
	//	err = -EFSCORRUPTED;
	//	goto out_writepage;
	//}

#ifdef MIGRATION_HANDLING_LATENCY
	tstart = OS_TimeGetUS();
#endif
	err = f2fs_get_node_info(sbi, dn.nid, &ni);
#ifdef MIGRATION_HANDLING_LATENCY
	tend = OS_TimeGetUS();
	mgc->nat_read_time += (tend-tstart);
	mgc->nat_read_cnt ++;
#endif
	if (err) {
		printk("%s: not expected", __func__);
		goto out_writepage;
	}
#ifdef SHIVAL
	if (dn.nid && f2fs_check_nid_range(sbi, dn.nid)) {
		printk("%s: nid: %u ofs_in_node: %u new_blkaddr: 0x%lx",
				__func__, dn.nid, dn.ofs_in_node, 
				new_blkaddr);
		f2fs_bug_on(sbi, 1);
	}
#endif
	/* update sum */
	set_summary(&sum, dn.nid, dn.ofs_in_node, ni.version);

	/* update sum and seg count of new addr and seg count of old addr . */
	
	/*down_read(&SM_I(sbi)->curseg_lock);
	mutex_lock(&curseg->curseg_mutex);*/

#ifdef MIGRATION_HANDLING_LATENCY
	tstart = OS_TimeGetUS();
#endif

	if (segno != curseg->segno) {
		bool flush = true;
		
//#ifdef MIGRATION_HANDLING_LATENCY
//		tstart_ = OS_TimeGetUS();
//#endif
		//down_write_prioritized(&sit_i->sentry_lock);
		down_write(&sit_i->sentry_lock);
		mutex_lock(&slot_i->lock);
//#ifdef MIGRATION_HANDLING_LATENCY
//		tend_ = OS_TimeGetUS();
//		mgc->ssa_update_lck_time += (tend_-tstart_);
//		mgc->ssa_update_lck_cnt ++;
//#endif

		if (curseg->segno != NULL_SEGNO) {
			if (get_seg_entry(sbi, curseg->slot_idx)->valid_blocks == 0) {
				//0313 modify
				curseg_slot_to_prefree_candidate(sbi, curseg);
				//struct slot_entry *slte = get_slot_entry(sbi, curseg->slot_idx);
				//unset_slot_inuse(slot_i, slte);
				//set_slot_prefree(slot_i, slte);
				//get_seg_entry(sbi, curseg->slot_idx)->segno = NULL_SEGNO;
				//0313 modify end
				flush = false;
			}
		}
		
		mutex_unlock(&slot_i->lock);
		up_write(&sit_i->sentry_lock);

		curseg->next_segno = segno;
//#ifdef SHIVAL
//		__change_curseg(sbi, CURSEG_MIGRATION_DATA, true);
//#else
		//change_curseg(sbi, CURSEG_MIGRATION_DATA, flush);
		reset_migration_curseg(sbi, CURSEG_MIGRATION_DATA, flush);
//#endif
	}

	curseg->next_blkoff = new_blkaddr - START_BLOCK(sbi, segno);
	__add_sum_entry(sbi, CURSEG_MIGRATION_DATA, &sum);

#ifdef MIGRATION_HANDLING_LATENCY
	tend = OS_TimeGetUS();
	mgc->ssa_update_time += (tend-tstart);
	mgc->ssa_update_cnt ++;
#endif

//#ifdef MIGRATION_HANDLING_LATENCY
//	tstart = OS_TimeGetUS();
//#endif
#ifdef MIGRATION_HANDLING_LATENCY
	tstart_ = OS_TimeGetUS();
#endif
	/* update seg count and ddm of old addr and new addr */
	down_write(&sit_i->sentry_lock);
	//down_write_prioritized(&sit_i->sentry_lock);
#ifdef MIGRATION_HANDLING_LATENCY
	tend_ = OS_TimeGetUS();
	mgc->ssa_update_lck_time += (tend_-tstart_);
	mgc->ssa_update_lck_cnt ++;
	if (tend_-tstart_ > 10000)
		printk("%s: sentry_lock time: %llu usec", __func__, tend_-tstart_);
#endif
//#ifdef MIGRATION_HANDLING_LATENCY
//	tend = OS_TimeGetUS();
//	mgc->sit_update_lck_time += (tend-tstart);
//	mgc->sit_update_lck_cnt ++;
//#endif
#ifdef MIGRATION_HANDLING_LATENCY
	tstart = OS_TimeGetUS();
#endif
	mutex_lock(&slot_i->lock);
#ifdef MIGRATION_HANDLING_LATENCY
	tend = OS_TimeGetUS();
	mgc->sit_update_lck_time += (tend-tstart);
	mgc->sit_update_lck_cnt ++;
#endif

	
	f2fs_bug_on(sbi, sum_info->slot_idx == NULL_SLOTNO);
	f2fs_bug_on(sbi, curseg->slot_idx == NULL_SLOTNO);
	update_slot_entry(sbi, sum_info->slot_idx, -1, GET_SEGNO(sbi, sum_info->old_blkaddr));
	update_slot_entry(sbi, curseg->slot_idx, 1, curseg->segno);

	//update_sit_entry(sbi, sum_info->old_blkaddr, -1, sum_info->slot_idx);
	//update_sit_entry(sbi, new_blkaddr, 1, curseg->slot_idx);
	mutex_unlock(&slot_i->lock);
	
	__mark_sit_entry_dirty(sbi, sum_info->slot_idx);
	__mark_sit_entry_dirty(sbi, curseg->slot_idx);

	locate_dirty_segment(sbi, GET_SEGNO(sbi, sum_info->old_blkaddr), sum_info->slot_idx);
	//locate_dirty_segment(sbi, curseg->segno, curseg->slot_idx);
	
	up_write(&sit_i->sentry_lock);
#ifdef SHIVAL	
	if ((sum_info->old_blkaddr & 0xe0000000) == 0xe0000000 ||
			(new_blkaddr & 0xe0000000) == 0xe0000000)
		printk("%s: old_blkaddr: 0x%lx new_blkaddr: 0x%lx", __func__, 
				sum_info->old_blkaddr, new_blkaddr);
#endif
	//f2fs_update_data_blkaddr(&dn, new_blkaddr);
	f2fs_update_data_blkaddr_test(&dn, new_blkaddr);

#ifdef MG_HANDLER_WRITE_NODE
	bool node_is_inode = (dn.node_page == dn.inode_page);
	//static int cnt_ = 0;
	if (sum_info->write_node) {
		//unsigned int upgs = (unsigned int) atomic_read(&mgc->updated_node_pgs);
		//unsigned int dpgs = (unsigned int) atomic_read(&mgc->dirty_node_pgs);
		//unsigned int dirty_ratio = (upgs > 0)? dpgs * 10000 / upgs : 0;
		////if (get_pages(sbi, F2FS_DIRTY_NODES) <= (sbi->blocks_per_seg*75/10) ){
		///* for workload like fio */
		//if (sbi->prev_cp_reason == CP_REASON_EXCESS_PREFREE && 
		//		dirty_ratio < 50) 
		//	goto out_writepage;
		///* for workload like fileserver */
		//if (sbi->prev_cp_reason == CP_REASON_EXCESS_DIRTY_NODE)
		//	goto out_writepage;
		//if (dirty_ratio < 50 &&
		//		get_pages(sbi, F2FS_DIRTY_NODES) <= (sbi->blocks_per_seg*75/10)) //for workload like fio
		//	goto out_writepage;

		//if(sbi->prev_cp_reason != CP_REASON_EXCESS_DIRTY_NODE) {
		if(sbi->sync_avg > 30) {
		//if(1) {
		//	 get_pages(sbi, F2FS_DIRTY_NODES) > (sbi->blocks_per_seg*75/10) ){
		//if (sum_info->write_node) {

			//if (cnt_ % 1000 == 0) {
			//	printk("%s: write node works", __func__);
			//}
			//cnt_ ++;
			bool submitted = false;
			int ret;
			struct writeback_control wbc = {
				.sync_mode = WB_SYNC_NONE,
				.nr_to_write = 1,
				.for_reclaim = 0,
			};
				
			//f2fs_wait_on_page_writeback(dn.node_page, NODE, true, true);
				
			if (!clear_page_dirty_for_io(dn.node_page)) {
				printk("%s: unexpected!!!!!!! clear page dirty failed", __func__);
			}
			
			ret = mg_write_node_page(dn.node_page, &submitted, &wbc, dn.nid, &ni);
			if (ret || !submitted) {
				printk("%s: unexpected!!!!!!! ret: %d submitted: %d", __func__, ret, submitted);
			} else {
				//printk("%s: start 1", __func__);
				f2fs_put_page(dn.node_page, 0);
				if (dn.inode_page && !node_is_inode)
					f2fs_put_page(dn.inode_page, 0);
				//printk("%s: end 1", __func__);
				//printk("%s: dirty node pages: %u prev cp reason: %u", 
				//	__func__, get_pages(sbi, F2FS_DIRTY_NODES), 
				//	sbi->prev_cp_reason);
				goto out;
			}
		}
	}
#endif
	/*mutex_unlock(&curseg->curseg_mutex);
	up_read(&SM_I(sbi)->curseg_lock);*/
	
	//trace_f2fs_do_write_data_page(page, OPU);
	//set_inode_flag(inode, FI_APPEND_WRITE);
	//if (page->index == 0)
	//	set_inode_flag(inode, FI_FIRST_BLOCK_WRITTEN);
out_writepage:
#ifdef MG_HANDLER_WRITE_NODE
//	if (dn.inode_page && !node_is_inode)
//		f2fs_put_page(dn.inode_page, 0);
#else
#endif
	//printk("%s bef put dnode ", __func__);
#ifdef PIN_NODE_PAGE
	f2fs_unlock_dnode(&dn);
#else
	f2fs_put_dnode(&dn);
#endif
	//printk("%s aft put dnode", __func__);
out:
	//f2fs_unlock_op(sbi);
	return err;
}

#ifdef LM_NO_INODE_READ
static int reflect_data_page_migration_light_no_inode(struct f2fs_sb_info *sbi, 
							struct f2fs_mg_sum_info *sum_info, block_t new_blkaddr)
{
	//struct page *page;
	struct migration_control *mgc = SM_I(sbi)->mgc_info;
	int err = 0;

	f2fs_lock_op(sbi);
	//page = f2fs_get_lock_data_page(inode, bidx, true);
	//if (IS_ERR(page)){
	//	f2fs_unlock_op(F2FS_I_SB(inode));
	//	return PTR_ERR(page);
	//}

	if (check_dynamic_discard_map(sbi, sum_info->old_blkaddr)) {
		//err = -ENOENT;
		goto out;
	}

	//if (!check_valid_map(F2FS_I_SB(inode), segno, off)) {
	//	err = -ENOENT;
	//	goto out;
	//}
	
	//bool is_dirty = PageDirty(page);

retry:	
	//f2fs_wait_on_page_writeback(page, DATA, true, true);

	/* TODO: seems no need to set dirty. */
	//set_page_dirty(page);
	//if (clear_page_dirty_for_io(page)) {
	//	inode_dec_dirty_pages(inode);
	//	f2fs_remove_dirty_inode(inode);
	//}
	
	//set_cold_data(page);

	err = __reflect_data_page_migration_light_no_inode(sbi, sum_info, new_blkaddr);
	if (err) {
		//clear_cold_data(page);
		printk("%s: unexpected!!!!!!!! err: %d", __func__, err);
		//f2fs_bug_on(sbi, 1);
		if (err == -ENOMEM) {
			congestion_wait(BLK_RW_ASYNC,
					DEFAULT_IO_TIMEOUT);
			goto retry;
		}
		//if (is_dirty)
		//	set_page_dirty(page);
	}
#ifdef MIGRATION_HANDLING_LATENCY
	atomic_inc(&mgc->data_pgs);
#endif
out:
	//f2fs_put_page(page, 1);
	f2fs_unlock_op(sbi);
	return err;
}
#endif

static int reflect_data_page_migration_light(struct f2fs_sb_info *sbi, 
		struct inode *inode, block_t bidx,
							struct f2fs_mg_sum_info *sum_info, block_t new_blkaddr)
{
	//struct page *page;
	struct migration_control *mgc = SM_I(sbi)->mgc_info;
	int err = 0;
#ifdef MIGRATION_HANDLING_LATENCY
	unsigned long long tstart, tend;
#endif

#ifdef MIGRATION_HANDLING_LATENCY
	tstart = OS_TimeGetUS();
#endif

	f2fs_lock_op_mg(sbi);

#ifdef MIGRATION_HANDLING_LATENCY
	tend = OS_TimeGetUS();
	mgc->__data_seg_time += (tend-tstart);
	mgc->__data_seg_cnt ++;
#endif
	//page = f2fs_get_lock_data_page(inode, bidx, true);
	//if (IS_ERR(page)){
	//	f2fs_unlock_op(F2FS_I_SB(inode));
	//	return PTR_ERR(page);
	//}

	if (check_dynamic_discard_map(sbi, sum_info->old_blkaddr)) {
		//err = -ENOENT;
		goto out;
	}

	//if (!check_valid_map(F2FS_I_SB(inode), segno, off)) {
	//	err = -ENOENT;
	//	goto out;
	//}
	
	//bool is_dirty = PageDirty(page);

retry:	
	//f2fs_wait_on_page_writeback(page, DATA, true, true);

	/* TODO: seems no need to set dirty. */
	//set_page_dirty(page);
	//if (clear_page_dirty_for_io(page)) {
	//	inode_dec_dirty_pages(inode);
	//	f2fs_remove_dirty_inode(inode);
	//}
	
	//set_cold_data(page);

	err = __reflect_data_page_migration_light(sbi, inode, sum_info, new_blkaddr, bidx);
	//err = 0;
	if (err) {
		//clear_cold_data(page);
		printk("%s: unexpected!!!!!!!! err: %d", __func__, err);
		//f2fs_bug_on(sbi, 1);
		if (err == -ENOMEM) {
			congestion_wait(BLK_RW_ASYNC,
					DEFAULT_IO_TIMEOUT);
			goto retry;
		}
		//if (is_dirty)
		//	set_page_dirty(page);
	}
#ifdef MIGRATION_HANDLING_LATENCY
	atomic_inc(&mgc->data_pgs);
#endif
out:
	//f2fs_put_page(page, 1);
	f2fs_unlock_op_mg(sbi);
	return err;
}

//static int reflect_data_page_migration(struct inode *inode, block_t bidx,
//							struct f2fs_mg_sum_info *sum_info, block_t new_blkaddr)
//{
//	struct page *page;
//	int err = 0;
//
//	f2fs_lock_op(F2FS_I_SB(inode));
//	page = f2fs_get_lock_data_page(inode, bidx, true);
//	if (IS_ERR(page)){
//		f2fs_unlock_op(F2FS_I_SB(inode));
//		return PTR_ERR(page);
//	}
//
//	if (check_dynamic_discard_map(F2FS_I_SB(inode), sum_info->old_blkaddr)) {
//		//err = -ENOENT;
//		goto out;
//	}
//
//	//if (!check_valid_map(F2FS_I_SB(inode), segno, off)) {
//	//	err = -ENOENT;
//	//	goto out;
//	//}
//	
//	bool is_dirty = PageDirty(page);
//
//retry:	
//	f2fs_wait_on_page_writeback(page, DATA, true, true);
//
//	/* TODO: seems no need to set dirty. */
//	//set_page_dirty(page);
//	//if (clear_page_dirty_for_io(page)) {
//	//	inode_dec_dirty_pages(inode);
//	//	f2fs_remove_dirty_inode(inode);
//	//}
//	
//	//set_cold_data(page);
//	if (page->index != bidx) {
//		printk("%s: unexpected!!! page->index: %u bidx: %u ", __func__, page->index, bidx);
//		f2fs_bug_on(F2FS_I_SB(inode), 1);
//	}
//
//	err = __reflect_data_page_migration(F2FS_I_SB(inode), page, sum_info, new_blkaddr);
//	if (err) {
//		//clear_cold_data(page);
//		printk("%s: unexpected!!!!!!!! err: %d", __func__, err);
//		//f2fs_bug_on(sbi, 1);
//		if (err == -ENOMEM) {
//			congestion_wait(BLK_RW_ASYNC,
//					DEFAULT_IO_TIMEOUT);
//			goto retry;
//		}
//		if (is_dirty)
//			set_page_dirty(page);
//	}
//out:
//	f2fs_put_page(page, 1);
//	f2fs_unlock_op(F2FS_I_SB(inode));
//	return err;
//}

static bool check_node_alive(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct node_info *dni, block_t blkaddr, struct page *node_page)
{
	nid_t nid;
	unsigned int ofs_in_node;
	block_t source_blkaddr;

	nid = le32_to_cpu(sum->nid);
	ofs_in_node = le16_to_cpu(sum->ofs_in_node);

//#ifdef SHIVAL
//		if (nid && f2fs_check_nid_range(sbi, nid)) {
//			printk("%s: unexpected problem!! nid: 0x%x", __func__, nid);
//		}
//#endif
//	node_page = f2fs_get_node_page(sbi, nid);
//	if (IS_ERR(node_page)) {
//		printk("%s: unexpected!! f2fs_get_node_page fail. nid: 0x%x", __func__, nid);
//		return false;
//	}
//
//	if (f2fs_get_node_info(sbi, nid, dni)) {
//		f2fs_put_page(node_page, 1);
//		return false;
//	}

	if (sum->version != dni->version) {
		f2fs_warn(sbi, "%s: valid data with mismatched node version.",
			  __func__);
		set_sbi_flag(sbi, SBI_NEED_FSCK);
	}

	source_blkaddr = data_blkaddr(NULL, node_page, ofs_in_node);
	//f2fs_put_page(node_page, 1);

	if (source_blkaddr != blkaddr) {
#ifdef CONFIG_F2FS_CHECK_FS
		unsigned int segno = GET_SEGNO(sbi, blkaddr);
		unsigned long offset = GET_BLKOFF_FROM_SEG0(sbi, blkaddr);

		if (unlikely(check_valid_map(sbi, segno, offset))) {
			if (!test_and_set_bit(segno, SIT_I(sbi)->invalid_segmap)) {
				f2fs_err(sbi, "mismatched blkaddr %u (source_blkaddr %u) in seg %u\n",
						blkaddr, source_blkaddr, segno);
				f2fs_bug_on(sbi, 1);
			}
		}
#endif
		return false;
	}
	return true;
}

static bool is_alive(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct node_info *dni, block_t blkaddr, unsigned int *nofs)
{
	struct page *node_page;
	nid_t nid;
	unsigned int ofs_in_node;
	block_t source_blkaddr;

	nid = le32_to_cpu(sum->nid);
	ofs_in_node = le16_to_cpu(sum->ofs_in_node);

//#ifdef SHIVAL
//		if (nid && f2fs_check_nid_range(sbi, nid)) {
//			printk("%s: unexpected problem!! nid: 0x%x", __func__, nid);
//		}
//#endif
	node_page = f2fs_get_node_page(sbi, nid);
	if (IS_ERR(node_page)) {
		//printk("%s: unexpected!! f2fs_get_node_page fail. nid: 0x%x maxnid: 0x%x errno: %d %ld", __func__, nid,
		//		NM_I(sbi)->max_nid, PTR_ERR(node_page), PTR_ERR(node_page));
		//dump_stack();
		return false;
	}

	if (f2fs_get_node_info(sbi, nid, dni)) {
		f2fs_put_page(node_page, 1);
		return false;
	}

	if (sum->version != dni->version) {
		f2fs_warn(sbi, "%s: valid data with mismatched node version.",
			  __func__);
		set_sbi_flag(sbi, SBI_NEED_FSCK);
	}

	*nofs = ofs_of_node(node_page);
	source_blkaddr = data_blkaddr(NULL, node_page, ofs_in_node);
	f2fs_put_page(node_page, 1);

	if (source_blkaddr != blkaddr) {
#ifdef CONFIG_F2FS_CHECK_FS
		unsigned int segno = GET_SEGNO(sbi, blkaddr);
		unsigned long offset = GET_BLKOFF_FROM_SEG0(sbi, blkaddr);

		if (unlikely(check_valid_map(sbi, segno, offset))) {
			if (!test_and_set_bit(segno, SIT_I(sbi)->invalid_segmap)) {
				f2fs_err(sbi, "mismatched blkaddr %u (source_blkaddr %u) in seg %u\n",
						blkaddr, source_blkaddr, segno);
				f2fs_bug_on(sbi, 1);
			}
		}
#endif
		return false;
	}
	return true;
}

static int ra_data_block(struct inode *inode, pgoff_t index)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct address_space *mapping = inode->i_mapping;
	struct dnode_of_data dn;
	struct page *page;
	struct extent_info ei = {0, 0, 0};
	struct f2fs_io_info fio = {
		.sbi = sbi,
		.ino = inode->i_ino,
		.type = DATA,
		.temp = COLD,
		.op = REQ_OP_READ,
		.op_flags = 0,
		.encrypted_page = NULL,
		.in_list = false,
		.retry = false,
	};
	int err;

	page = f2fs_grab_cache_page(mapping, index, true);
	if (!page)
		return -ENOMEM;

	if (f2fs_lookup_extent_cache(inode, index, &ei)) {
		dn.data_blkaddr = ei.blk + index - ei.fofs;
		if (unlikely(!f2fs_is_valid_blkaddr(sbi, dn.data_blkaddr,
						DATA_GENERIC_ENHANCE_READ))) {
			err = -EFSCORRUPTED;
			goto put_page;
		}
		goto got_it;
	}

	set_new_dnode(&dn, inode, NULL, NULL, 0);
	err = f2fs_get_dnode_of_data(&dn, index, LOOKUP_NODE);
	if (err)
		goto put_page;
	f2fs_put_dnode(&dn);

	if (!__is_valid_data_blkaddr(dn.data_blkaddr)) {
		err = -ENOENT;
		goto put_page;
	}
	if (unlikely(!f2fs_is_valid_blkaddr(sbi, dn.data_blkaddr,
						DATA_GENERIC_ENHANCE))) {
		err = -EFSCORRUPTED;
		goto put_page;
	}
got_it:
	/* read page */
	fio.page = page;
	fio.new_blkaddr = fio.old_blkaddr = dn.data_blkaddr;

	/*
	 * don't cache encrypted data into meta inode until previous dirty
	 * data were writebacked to avoid racing between GC and flush.
	 */
	f2fs_wait_on_page_writeback(page, DATA, true, true);

	f2fs_wait_on_block_writeback(inode, dn.data_blkaddr);

	fio.encrypted_page = f2fs_pagecache_get_page(META_MAPPING(sbi),
					dn.data_blkaddr,
					FGP_LOCK | FGP_CREAT, GFP_NOFS);
	if (!fio.encrypted_page) {
		err = -ENOMEM;
		goto put_page;
	}

	err = f2fs_submit_page_bio(&fio);
	if (err)
		goto put_encrypted_page;
	f2fs_put_page(fio.encrypted_page, 0);
	f2fs_put_page(page, 1);

	f2fs_update_iostat(sbi, FS_DATA_READ_IO, F2FS_BLKSIZE);
	f2fs_update_iostat(sbi, FS_GDATA_READ_IO, F2FS_BLKSIZE);

	return 0;
put_encrypted_page:
	f2fs_put_page(fio.encrypted_page, 1);
put_page:
	f2fs_put_page(page, 1);
	return err;
}

//static void reflect_data_segment_migration_light(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
//		struct gc_inode_list *gc_list, unsigned int segno, struct migration_seg_info *ms_info,
//	   	uint64_t old_slot_idx, unsigned int *segno_buf, unsigned int *nblks_buf, int buf_cnt)
//{
//	struct super_block *sb = sbi->sb;
//	struct f2fs_summary *entry;
//	block_t start_addr, old_addr, new_addr;
//	int off, i;
//	int phase = 0;
//
//	start_addr = START_BLOCK(sbi, segno);
//
//next_step:
//	entry = sum;
//		
//	//for (off = 0; off < usable_blks_in_seg; off++, entry++) {
//	for (i = 0; i < ms_info->nblks; i ++) {
//		old_addr = le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + i].old_lba);
//		new_addr = le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + i].new_lba);
//	
//		if (segno != GET_SEGNO(sbi, old_addr)) {
//			printk("%s: ERR!! idx: %d arg segno: %u segno: %u", __func__, 
//					ms_info->start_idx + i, segno, GET_SEGNO(sbi, old_addr));
//
//
//			for (i = 0; i < ms_info->mge->nr; i ++) { 
//
//				block_t old_addr = le64_to_cpu(ms_info->mge->mg_pairs[i].old_lba);
//				printk("%s: idx: %d segno: %u old_addr: 0x%llx", __func__, i, 
//						GET_SEGNO(sbi, old_addr), old_addr);
//			}
//
//			for (i = 0; i < buf_cnt; i ++) {
//				printk("%s: idx: %d segno: %u nblks: %u", __func__, i, 
//						segno_buf[i], nblks_buf[i]);
//
//			}
//			
//		}
//		f2fs_bug_on(sbi, segno != GET_SEGNO(sbi, old_addr));
//		
//		off = old_addr - start_addr;
//		entry = sum + off;
//		
//		struct page *data_page;
//		struct inode *inode;
//		struct node_info dni; /* dnode info for the data */
//		unsigned int ofs_in_node, nofs;
//		block_t start_bidx;
//		nid_t nid = le32_to_cpu(entry->nid);
//	
//		if (check_dynamic_discard_map(sbi, old_addr))
//			continue;
//		//if (check_valid_map(sbi, segno, off) == 0)
//		//	continue;
//
//		if (phase == 0) {
//			f2fs_ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
//							META_NAT, true);
//			continue;
//		}
//
//		if (phase == 1) {
//#ifdef SHIVAL
//			if (nid && f2fs_check_nid_range(sbi, nid)) {
//				unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
//				printk("%s nid problem!!!!!!!!", __func__);
//				printk("%s: cid: %u prob addr: 0x%lx off: %u startblkaddr: 0x%lx slot idx: %lu", 
//						__func__, ms_info->mge->command_id, old_addr, off, start_addr, old_slot_idx);
//				block_t old_addr_tmp;
//				int tmpiii;
//				for (tmpiii = 0; tmpiii < ms_info->nblks; tmpiii ++) {
//					old_addr_tmp = le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + tmpiii].old_lba);
//					printk("ofs in sum: %u old addr: 0x%lx new addr: 0x%lx", 
//						old_addr_tmp - start_addr, old_addr_tmp, 
//						le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + tmpiii].new_lba));
//				}
//				int tmpoff;
//				struct f2fs_summary *tmp_entry = sum;
//				
//				for (tmpoff = 0; tmpoff < usable_blks_in_seg; tmpoff++, tmp_entry++) {
//					printk("idx: %d nid: 0x%x ofs_in_node: 0x%x", 
//							tmpoff, le32_to_cpu(tmp_entry->nid), 
//							le16_to_cpu(tmp_entry->ofs_in_node));
//				}
//			}
//
//#endif
//			f2fs_ra_node_page(sbi, nid);
//			continue;
//		}
//
//		/* Get an inode by ino with checking validity */
//		//if (!is_alive(sbi, entry, &dni, old_addr, &nofs))
//		//	continue;
//
//		/*if (phase == 2) {
//#ifdef SHIVAL
//			if (nid && f2fs_check_nid_range(sbi, nid))
//				printk("%s nid problem 2!!!!!!!!", __func__);
//#endif
//			f2fs_ra_node_page(sbi, dni.ino);
//			continue;
//		}*/
//
//		//ofs_in_node = le16_to_cpu(entry->ofs_in_node);
//			
//		struct f2fs_mg_sum_info sum_info = {
//			//.nid = nid,
//			//.ofs_in_node = ofs_in_node,
//			.old_blkaddr = old_addr,
//			.slot_idx = old_slot_idx,
//			.sum_entry = entry,
//		};
//				
//		err = reflect_data_page_migration_light(sbi, &sum_info, new_addr);
//
//		//if (phase == 3) {
//		//	inode = f2fs_iget(sb, dni.ino);
//		//	if (IS_ERR(inode) || is_bad_inode(inode)) {
//		//		set_sbi_flag(sbi, SBI_NEED_FSCK);
//		//		continue;
//		//	}
//
//		//	if (!down_write_trylock(
//		//		&F2FS_I(inode)->i_gc_rwsem[WRITE])) {
//		//		printk("%s unexpected. i_gc_rwsem failed,", __func__);
//		//		iput(inode);
//		//		sbi->skipped_gc_rwsem++;
//		//		continue;
//		//	}
//
//		//	start_bidx = f2fs_start_bidx_of_node(nofs, inode) +
//		//						ofs_in_node;
//
//		//	if (f2fs_post_read_required(inode)) {
//		//		printk("%sL unexpected, post_read_required ", __func__);
//		//		int err = ra_data_block(inode, start_bidx);
//
//		//		up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
//		//		if (err) {
//		//			iput(inode);
//		//			continue;
//		//		}
//		//		add_gc_inode(gc_list, inode);
//		//		continue;
//		//	}
//
//		//	data_page = f2fs_get_read_data_page(inode,
//		//				start_bidx, REQ_RAHEAD, true);
//		//	up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
//		//	if (IS_ERR(data_page)) {
//		//		printk("%s: unexpected!! f2fs_get_read_data_page failed ", __func__);
//		//		iput(inode);
//		//		continue;
//		//	}
//
//		//	f2fs_put_page(data_page, 0);
//		//	add_gc_inode(gc_list, inode);
//		//	continue;
//		//}
//
//		/* phase 4 */
//		//inode = find_gc_inode(gc_list, dni.ino);
//		//if (inode) {
//		//	struct f2fs_inode_info *fi = F2FS_I(inode);
//		//	bool locked = false;
//		//	int err = 0;
//
//		//	if (S_ISREG(inode->i_mode)) {
//		//		if (!down_write_trylock(&fi->i_gc_rwsem[READ])) {
//		//			printk("%s: unexpected!! i_gc_rwsem read trylock failed", __func__);
//		//			continue;
//		//		}
//		//		if (!down_write_trylock(
//		//				&fi->i_gc_rwsem[WRITE])) {
//		//			printk("%s: unexpected!! i_gc_rwsem write trylock failed", __func__);
//		//			sbi->skipped_gc_rwsem++;
//		//			up_write(&fi->i_gc_rwsem[READ]);
//		//			continue;
//		//		}
//		//		locked = true;
//		//		//printk("%s: UNEXPECTED!!!!!!!!!!!!!", __func__);
//
//		//		/* wait for all inflight aio data */
//		//		inode_dio_wait(inode);
//		//	}
//
//		//	start_bidx = f2fs_start_bidx_of_node(nofs, inode)
//		//						+ ofs_in_node;
//		//	struct f2fs_mg_sum_info sum_info = {
//		//		.nid = nid,
//		//		.ofs_in_node = ofs_in_node,
//		//		.old_blkaddr = old_addr,
//		//		.slot_idx = old_slot_idx,
//		//	};
//
//		//	if (f2fs_post_read_required(inode)) {
//		//		f2fs_bug_on(sbi, 1);
//		//		//err = move_data_block(inode, start_bidx,
//		//		//			segno, old_addr - start_addr);
//		//	}
//		//	else
//		//		err = reflect_data_page_migration(inode, start_bidx,
//		//						&sum_info, new_addr);
//		//	if (err) {
//		//		printk("%s: reflect data page migration failed errno: %d", __func__, err);
//		//	}
//
//		//	if (locked) {
//		//		up_write(&fi->i_gc_rwsem[WRITE]);
//		//		up_write(&fi->i_gc_rwsem[READ]);
//		//	}
//
//		//	//stat_inc_data_blk_count(sbi, 1, gc_type);
//		//} else {
//		//	printk("%s: no inode. unexpected!!", __func__);
//		//}
//	}
//
//	if (++phase < 3)
//		goto next_step;
//}

#ifdef LM_NO_INODE_READ
static void reflect_data_segment_migration_light_no_inode(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct gc_inode_list *gc_list, unsigned int segno, struct migration_seg_info *ms_info,
	   	uint64_t old_slot_idx, unsigned int *segno_buf, unsigned int *nblks_buf, int buf_cnt)
{
	struct migration_control *mgc = SM_I(sbi)->mgc_info;
	struct super_block *sb = sbi->sb;
	struct f2fs_summary *entry;
	block_t start_addr, old_addr, new_addr;
	int off, i;
	int phase = 0;
#ifdef MIGRATION_HANDLING_LATENCY
	unsigned long long tstart, tend;
#endif

	start_addr = START_BLOCK(sbi, segno);

next_step:
	entry = sum;
		
	//for (off = 0; off < usable_blks_in_seg; off++, entry++) {
	for (i = 0; i < ms_info->nblks; i ++) {
		old_addr = le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + i].old_lba);
		new_addr = le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + i].new_lba);
	
		if (segno != GET_SEGNO(sbi, old_addr)) {
			printk("%s: ERR!! idx: %d arg segno: %u segno: %u", __func__, 
					ms_info->start_idx + i, segno, GET_SEGNO(sbi, old_addr));


			for (i = 0; i < ms_info->mge->nr; i ++) { 

				block_t old_addr = le64_to_cpu(ms_info->mge->mg_pairs[i].old_lba);
				printk("%s: idx: %d segno: %u old_addr: 0x%llx", __func__, i, 
						GET_SEGNO(sbi, old_addr), old_addr);
			}

			for (i = 0; i < buf_cnt; i ++) {
				printk("%s: idx: %d segno: %u nblks: %u", __func__, i, 
						segno_buf[i], nblks_buf[i]);

			}
			
		}
		f2fs_bug_on(sbi, segno != GET_SEGNO(sbi, old_addr));
		
		off = old_addr - start_addr;
		entry = sum + off;
		
		struct page *data_page;
		struct inode *inode;
		struct node_info dni; /* dnode info for the data */
		unsigned int ofs_in_node, nofs;
		block_t start_bidx;
		nid_t nid = le32_to_cpu(entry->nid);
	
		if (check_dynamic_discard_map(sbi, old_addr))
			continue;
		//if (check_valid_map(sbi, segno, off) == 0)
		//	continue;

		if (phase == 0) {
			f2fs_ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
							META_NAT, true);
			continue;
		}

		if (phase == 1) {
//#ifdef SHIVAL
			if (nid && f2fs_check_nid_range(sbi, nid)) {
				unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
				printk("%s nid problem!!!!!!!! segno: %lu", __func__, segno);
				printk("%s: cid: %u prob addr: 0x%lx off: %u startblkaddr: 0x%lx slot idx: %lu", 
						__func__, ms_info->mge->command_id, old_addr, off, start_addr, old_slot_idx);

				block_t old_addr_tmp;
				int tmpiii;
				for (tmpiii = 0; tmpiii < ms_info->nblks; tmpiii ++) {
					old_addr_tmp = le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + tmpiii].old_lba);
					printk("ofs in sum: %u old addr: 0x%lx new addr: 0x%lx", 
						old_addr_tmp - start_addr, old_addr_tmp, 
						le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + tmpiii].new_lba));
				}
				int tmpoff;
				struct f2fs_summary *tmp_entry = sum;
				
				for (tmpoff = 0; tmpoff < usable_blks_in_seg; tmpoff++, tmp_entry++) {
					printk("idx: %d nid: 0x%x ofs_in_node: 0x%x", 
							tmpoff, le32_to_cpu(tmp_entry->nid), 
							le16_to_cpu(tmp_entry->ofs_in_node));
				}
			}

//#endif
			f2fs_ra_node_page(sbi, nid);
			continue;
		}

		/* Get an inode by ino with checking validity */
		if (!is_alive(sbi, entry, &dni, old_addr, &nofs))
			continue;

		//if (phase == 2) {
		//	f2fs_ra_node_page(sbi, dni.ino);
		//	continue;
		//}

		ofs_in_node = le16_to_cpu(entry->ofs_in_node);

		if (phase == 2) {

			inode = f2fs_try_iget(sb, dni.ino);
			if (inode == NULL || IS_ERR(inode) || is_bad_inode(inode)) {
				//set_sbi_flag(sbi, SBI_NEED_FSCK);
				//printk("%s: iget fail ino: %u is_err: %d is_bad_inode: %d", 
				//		__func__, dni.ino, 
				//		IS_ERR(inode), is_bad_inode(inode));
				continue;
			}

			if (!down_write_trylock(
				&F2FS_I(inode)->i_gc_rwsem[WRITE])) {
				printk("%s unexpected. i_gc_rwsem failed,", __func__);
				iput(inode);
				sbi->skipped_gc_rwsem++;
				continue;
			}


			if (f2fs_post_read_required(inode)) {
				start_bidx = f2fs_start_bidx_of_node(nofs, inode) +
									ofs_in_node;
				printk("%sL unexpected, post_read_required ", __func__);
				int err = ra_data_block(inode, start_bidx);

				up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
				if (err) {
					iput(inode);
					continue;
				}
				add_gc_inode(gc_list, inode);
				continue;
			}

			//data_page = f2fs_get_read_data_page(inode,
			//			start_bidx, REQ_RAHEAD, true);
			up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
			//if (IS_ERR(data_page)) {
			//	printk("%s: unexpected!! f2fs_get_read_data_page failed ", __func__);
			//	iput(inode);
			//	continue;
			//}

			//f2fs_put_page(data_page, 0);
			add_gc_inode(gc_list, inode);
			continue;
		}

		/* phase 3 */
		inode = find_gc_inode(gc_list, dni.ino);
		
		struct f2fs_mg_sum_info sum_info = {
			.nid = nid,
			.ofs_in_node = ofs_in_node,
			.old_blkaddr = old_addr,
			.slot_idx = old_slot_idx,
		};
			
		int err = 0;

		if (inode) {
			struct f2fs_inode_info *fi = F2FS_I(inode);
			bool locked = false;

			if (S_ISREG(inode->i_mode)) {
				if (!down_write_trylock(&fi->i_gc_rwsem[READ])) {
					printk("%s: unexpected!! i_gc_rwsem read trylock failed", __func__);
					continue;
				}
				if (!down_write_trylock(
						&fi->i_gc_rwsem[WRITE])) {
					printk("%s: unexpected!! i_gc_rwsem write trylock failed", __func__);
					sbi->skipped_gc_rwsem++;
					up_write(&fi->i_gc_rwsem[READ]);
					continue;
				}
				locked = true;
				//printk("%s: UNEXPECTED!!!!!!!!!!!!!", __func__);

				/* wait for all inflight aio data */
				inode_dio_wait(inode);
			}

			start_bidx = f2fs_start_bidx_of_node(nofs, inode)
								+ ofs_in_node;
			
			if (f2fs_post_read_required(inode)) {
				f2fs_bug_on(sbi, 1);
			}
			
#ifdef MIGRATION_HANDLING_LATENCY
			tstart = OS_TimeGetUS();
#endif
			err = reflect_data_page_migration_light(sbi, inode, start_bidx,
								&sum_info, new_addr);
#ifdef MIGRATION_HANDLING_LATENCY
			tend = OS_TimeGetUS();
			mgc->_data_seg_time += (tend-tstart);
			mgc->_data_seg_cnt ++;
#endif
			if (locked) {
				up_write(&fi->i_gc_rwsem[WRITE]);
				up_write(&fi->i_gc_rwsem[READ]);
			}

		} else {
#ifdef MIGRATION_HANDLING_LATENCY
			tstart = OS_TimeGetUS();
#endif
			err = reflect_data_page_migration_light_no_inode(sbi, 
							&sum_info, new_addr);
#ifdef MIGRATION_HANDLING_LATENCY
			tend = OS_TimeGetUS();
			mgc->_data_seg_time += (tend-tstart);
			mgc->_data_seg_cnt ++;
#endif
		}
		
		if (err) {
			printk("%s: reflect data page migration failed errno: %d", __func__, err);
		}

		//stat_inc_data_blk_count(sbi, 1, gc_type);
	}

	if (++phase < 3)
		goto next_step;
}
#endif

static void reflect_data_segment_migration_light(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct gc_inode_list *gc_list, unsigned int segno, struct migration_seg_info *ms_info,
	   	uint64_t old_slot_idx, unsigned int *segno_buf, unsigned int *nblks_buf, int buf_cnt)
{
	struct migration_control *mgc = SM_I(sbi)->mgc_info;
	struct super_block *sb = sbi->sb;
	struct f2fs_summary *entry;
	block_t start_addr, old_addr, new_addr;
	int off, i;
	int phase = 0;
#ifdef MIGRATION_HANDLING_LATENCY
	unsigned long long tstart, tend;
#endif
	bool is_alive_;

	start_addr = START_BLOCK(sbi, segno);

next_step:
	entry = sum;
		
	//for (off = 0; off < usable_blks_in_seg; off++, entry++) {
	for (i = 0; i < ms_info->nblks; i ++) {
		old_addr = le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + i].old_lba);
		new_addr = le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + i].new_lba);
	
		if (segno != GET_SEGNO(sbi, old_addr)) {
			printk("%s: ERR!! idx: %d arg segno: %u segno: %u", __func__, 
					ms_info->start_idx + i, segno, GET_SEGNO(sbi, old_addr));


			for (i = 0; i < ms_info->mge->nr; i ++) { 

				block_t old_addr = le64_to_cpu(ms_info->mge->mg_pairs[i].old_lba);
				printk("%s: idx: %d segno: %u old_addr: 0x%llx", __func__, i, 
						GET_SEGNO(sbi, old_addr), old_addr);
			}

			for (i = 0; i < buf_cnt; i ++) {
				printk("%s: idx: %d segno: %u nblks: %u", __func__, i, 
						segno_buf[i], nblks_buf[i]);

			}
			
		}
		f2fs_bug_on(sbi, segno != GET_SEGNO(sbi, old_addr));
		
		off = old_addr - start_addr;
		entry = sum + off;
		
		struct page *data_page;
		struct inode *inode;
		struct node_info dni; /* dnode info for the data */
		unsigned int ofs_in_node, nofs;
		block_t start_bidx;
		nid_t nid = le32_to_cpu(entry->nid);
	
		if (check_dynamic_discard_map(sbi, old_addr))
			continue;
		//if (check_valid_map(sbi, segno, off) == 0)
		//	continue;

		if (phase == 0) {
			f2fs_ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
							META_NAT, true);
			continue;
		}

		/* TODO: need to be fixed */
		if (unlikely(nid < F2FS_ROOT_INO(sbi) || nid >= NM_I(sbi)->max_nid)) {
			printk("%s: weird!!! addr: 0x%llx", __func__, old_addr);
			continue;
		}

		if (phase == 1) {
#ifdef MIGRATION_HANDLING_LATENCY
			tstart = OS_TimeGetUS();
#endif
//#ifdef SHIVAL
			if (nid && f2fs_check_nid_range(sbi, nid)) {
				unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
				printk("%s nid problem!!!!!!!! segno: %lu", __func__, segno);
				printk("%s: cid: %u prob addr: 0x%lx off: %u startblkaddr: 0x%lx slot idx: %lu", 
						__func__, ms_info->mge->command_id, old_addr, off, start_addr, old_slot_idx);

				block_t old_addr_tmp;
				int tmpiii;
				for (tmpiii = 0; tmpiii < ms_info->nblks; tmpiii ++) {
					old_addr_tmp = le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + tmpiii].old_lba);
					printk("ofs in sum: %u old addr: 0x%lx new addr: 0x%lx", 
						old_addr_tmp - start_addr, old_addr_tmp, 
						le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + tmpiii].new_lba));
				}
				int tmpoff;
				struct f2fs_summary *tmp_entry = sum;
				
				for (tmpoff = 0; tmpoff < usable_blks_in_seg; tmpoff++, tmp_entry++) {
					printk("idx: %d nid: 0x%x ofs_in_node: 0x%x", 
							tmpoff, le32_to_cpu(tmp_entry->nid), 
							le16_to_cpu(tmp_entry->ofs_in_node));
				}
			}

//#endif
			f2fs_ra_node_page(sbi, nid);
#ifdef MIGRATION_HANDLING_LATENCY
			tend = OS_TimeGetUS();
			mgc->data_seg_p1_time += (tend-tstart);
#endif
			continue;
		}

		/* Get an inode by ino with checking validity */
//		if (!is_alive(sbi, entry, &dni, old_addr, &nofs))
//			continue;
#ifdef MIGRATION_HANDLING_LATENCY
		tstart = OS_TimeGetUS();
#endif
		is_alive_ = is_alive(sbi, entry, &dni, old_addr, &nofs)? true : false;
#ifdef MIGRATION_HANDLING_LATENCY
		tend = OS_TimeGetUS();
		mgc->data_seg_is_alive_time += (tend-tstart);
#endif
		if (!is_alive_)
			continue;
		

		if (phase == 2) {
#ifdef MIGRATION_HANDLING_LATENCY
			tstart = OS_TimeGetUS();
#endif
//#ifdef SHIVAL
			if (nid && f2fs_check_nid_range(sbi, nid))
				printk("%s nid problem 2!!!!!!!!", __func__);
//#endif
			f2fs_ra_node_page(sbi, dni.ino);
#ifdef MIGRATION_HANDLING_LATENCY
			tend = OS_TimeGetUS();
			mgc->data_seg_p2_time += (tend-tstart);
#endif
			continue;
		}

		ofs_in_node = le16_to_cpu(entry->ofs_in_node);

		if (phase == 3) {
#ifdef MIGRATION_HANDLING_LATENCY
			tstart = OS_TimeGetUS();
#endif
			inode = f2fs_iget(sb, dni.ino);
			if (IS_ERR(inode) || is_bad_inode(inode)) {
				//set_sbi_flag(sbi, SBI_NEED_FSCK);
				//printk("%s: iget fail ino: %u is_err: %d is_bad_inode: %d", 
				//		__func__, dni.ino, 
				//		IS_ERR(inode), is_bad_inode(inode));
				continue;
			}

			if (!down_write_trylock(
				&F2FS_I(inode)->i_gc_rwsem[WRITE])) {
				printk("%s unexpected. i_gc_rwsem failed,", __func__);
				iput(inode);
				sbi->skipped_gc_rwsem++;
				continue;
			}


			if (f2fs_post_read_required(inode)) {
				start_bidx = f2fs_start_bidx_of_node(nofs, inode) +
									ofs_in_node;
				printk("%sL unexpected, post_read_required ", __func__);
				int err = ra_data_block(inode, start_bidx);

				up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
				if (err) {
					iput(inode);
					continue;
				}
				add_gc_inode(gc_list, inode);
				continue;
			}

			//data_page = f2fs_get_read_data_page(inode,
			//			start_bidx, REQ_RAHEAD, true);
			up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
			//if (IS_ERR(data_page)) {
			//	printk("%s: unexpected!! f2fs_get_read_data_page failed ", __func__);
			//	iput(inode);
			//	continue;
			//}

			//f2fs_put_page(data_page, 0);
			add_gc_inode(gc_list, inode);
#ifdef MIGRATION_HANDLING_LATENCY
			tend = OS_TimeGetUS();
			mgc->data_seg_p3_time += (tend-tstart);
#endif
			continue;
		}

		/* phase 4 */
#ifdef MIGRATION_HANDLING_LATENCY
		tstart = OS_TimeGetUS();
#endif
#ifdef MG_HANDLER_WRITE_NODE
		struct inode_entry *tmp_ie;
		bool write_node;
		write_node = false;
		inode = find_gc_inode(gc_list, dni.ino, &tmp_ie);
		tmp_ie->cnt --;
		if (tmp_ie->cnt == 0){
			write_node = true;
		}
#else
		inode = find_gc_inode(gc_list, dni.ino);
#endif
		if (inode) {
			struct f2fs_inode_info *fi = F2FS_I(inode);
			bool locked = false;
			int err = 0;

			if (S_ISREG(inode->i_mode)) {
				if (!down_write_trylock(&fi->i_gc_rwsem[READ])) {
					printk("%s: unexpected!! i_gc_rwsem read trylock failed", __func__);
					continue;
				}
				if (!down_write_trylock(&fi->i_gc_rwsem[WRITE])) {
					printk("%s: unexpected!! i_gc_rwsem write trylock failed", __func__);
					sbi->skipped_gc_rwsem++;
					up_write(&fi->i_gc_rwsem[READ]);
					continue;
				}
				locked = true;
				//printk("%s: UNEXPECTED!!!!!!!!!!!!!", __func__);

				/* wait for all inflight aio data */
				inode_dio_wait(inode);
			}

			start_bidx = f2fs_start_bidx_of_node(nofs, inode)
								+ ofs_in_node;
			struct f2fs_mg_sum_info sum_info = {
				.nid = nid,
				.ofs_in_node = ofs_in_node,
				.old_blkaddr = old_addr,
				.slot_idx = old_slot_idx,
#ifdef MG_HANDLER_WRITE_NODE
				.write_node = write_node,
#endif
			};

			if (f2fs_post_read_required(inode)) {
				f2fs_bug_on(sbi, 1);
				//err = move_data_block(inode, start_bidx,
				//			segno, old_addr - start_addr);
			}
			else {
#ifdef MIGRATION_HANDLING_LATENCY
				tend = OS_TimeGetUS();
				mgc->data_seg_p4_start_time += (tend-tstart);
#endif
#ifdef MIGRATION_HANDLING_LATENCY
				tstart = OS_TimeGetUS();
#endif
				err = reflect_data_page_migration_light(sbi, inode, start_bidx,
								&sum_info, new_addr);
#ifdef MIGRATION_HANDLING_LATENCY
				tend = OS_TimeGetUS();
				mgc->_data_seg_time += (tend-tstart);
				mgc->_data_seg_cnt ++;
#endif
			}
			if (err) {
				printk("%s: reflect data page migration failed errno: %d", __func__, err);
			}

			if (locked) {
				up_write(&fi->i_gc_rwsem[WRITE]);
				up_write(&fi->i_gc_rwsem[READ]);
			}

			//stat_inc_data_blk_count(sbi, 1, gc_type);
		} else {
			printk("%s: no inode. unexpected!!", __func__);
		}
	}

	if (++phase < 5)
		goto next_step;
}


#ifdef NODE_READ_PIPELINE
static void preread_node_of_data_segment(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct gc_inode_list *gc_list, unsigned int segno, struct migration_seg_info *ms_info,
	   	uint64_t old_slot_idx, unsigned int *segno_buf, unsigned int *nblks_buf, int buf_cnt)
{
	struct migration_control *mgc = SM_I(sbi)->mgc_info;
	struct super_block *sb = sbi->sb;
	struct f2fs_summary *entry;
	block_t start_addr, old_addr, new_addr;
	int off, i;
	int phase = 0;
//#ifdef MIGRATION_HANDLING_LATENCY
//	unsigned long long tstart, tend;
//#endif
	bool is_alive_;

	start_addr = START_BLOCK(sbi, segno);

next_step:
	entry = sum;
		
	//for (off = 0; off < usable_blks_in_seg; off++, entry++) {
	for (i = 0; i < ms_info->nblks; i ++) {
		old_addr = le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + i].old_lba);
		new_addr = le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + i].new_lba);
	
		if (segno != GET_SEGNO(sbi, old_addr)) {
			printk("%s: ERR!! idx: %d arg segno: %u segno: %u", __func__, 
					ms_info->start_idx + i, segno, GET_SEGNO(sbi, old_addr));


			for (i = 0; i < ms_info->mge->nr; i ++) { 

				block_t old_addr = le64_to_cpu(ms_info->mge->mg_pairs[i].old_lba);
				printk("%s: idx: %d segno: %u old_addr: 0x%llx", __func__, i, 
						GET_SEGNO(sbi, old_addr), old_addr);
			}

			for (i = 0; i < buf_cnt; i ++) {
				printk("%s: idx: %d segno: %u nblks: %u", __func__, i, 
						segno_buf[i], nblks_buf[i]);

			}
			
		}
		f2fs_bug_on(sbi, segno != GET_SEGNO(sbi, old_addr));
		
		off = old_addr - start_addr;
		entry = sum + off;
		
		struct page *data_page;
		struct inode *inode;
		struct node_info dni; /* dnode info for the data */
		unsigned int ofs_in_node, nofs;
		block_t start_bidx;
		nid_t nid = le32_to_cpu(entry->nid);
	
		if (check_dynamic_discard_map(sbi, old_addr))
			continue;
		//if (check_valid_map(sbi, segno, off) == 0)
		//	continue;

		if (phase == 0) {
			f2fs_ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
							META_NAT, true);
			continue;
		}

		/* TODO: need to be fixed */
		if (unlikely(nid < F2FS_ROOT_INO(sbi) || nid >= NM_I(sbi)->max_nid)) {
			printk("%s: weird!!! addr: 0x%llx", __func__, old_addr);
			continue;
		}

		if (phase == 1) {
//#ifdef MIGRATION_HANDLING_LATENCY
//			tstart = OS_TimeGetUS();
//#endif
//#ifdef SHIVAL
			if (nid && f2fs_check_nid_range(sbi, nid)) {
				unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
				printk("%s nid problem!!!!!!!! segno: %lu", __func__, segno);
				printk("%s: cid: %u prob addr: 0x%lx off: %u startblkaddr: 0x%lx slot idx: %lu", 
						__func__, ms_info->mge->command_id, old_addr, off, start_addr, old_slot_idx);

				block_t old_addr_tmp;
				int tmpiii;
				for (tmpiii = 0; tmpiii < ms_info->nblks; tmpiii ++) {
					old_addr_tmp = le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + tmpiii].old_lba);
					printk("ofs in sum: %u old addr: 0x%lx new addr: 0x%lx", 
						old_addr_tmp - start_addr, old_addr_tmp, 
						le64_to_cpu(ms_info->mge->mg_pairs[ms_info->start_idx + tmpiii].new_lba));
				}
				int tmpoff;
				struct f2fs_summary *tmp_entry = sum;
				
				for (tmpoff = 0; tmpoff < usable_blks_in_seg; tmpoff++, tmp_entry++) {
					printk("idx: %d nid: 0x%x ofs_in_node: 0x%x", 
							tmpoff, le32_to_cpu(tmp_entry->nid), 
							le16_to_cpu(tmp_entry->ofs_in_node));
				}
			}

//#endif
			f2fs_ra_node_page(sbi, nid);
//#ifdef MIGRATION_HANDLING_LATENCY
//			tend = OS_TimeGetUS();
//			mgc->data_seg_p1_time += (tend-tstart);
//#endif
			continue;
		}

		/* Get an inode by ino with checking validity */
//		if (!is_alive(sbi, entry, &dni, old_addr, &nofs))
//			continue;
	}

	if (++phase < 2)
		goto next_step;
}
#endif

static inline bool migration_segment_match(struct f2fs_sb_info *sbi,
		unsigned int segno, unsigned char type)
{
	if (!IS_MIGRATION_SEG(type))
		return false;
	
	/* migration curseg lock is not required in case of single migration thread */
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	return curseg->segno == segno;
}

static inline struct f2fs_summary_block *get_curseg_sum(struct f2fs_sb_info *sbi, 
		unsigned char type, unsigned int segno)
{
	/* migration curseg lock is not required in case of single migration thread */
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	
	f2fs_bug_on(sbi, curseg->segno != segno);
	
	/* We just return sum blk pointer in curseg without memcpy.
	 Error does not occur in case of single migration thread */
	return curseg->sum_blk;
}

#define TMP_STACK_SZ 512
static void preread_old_migration_sum_blk(struct f2fs_sb_info *sbi, struct mg_entry *mge)
{
       struct migration_control *mgc = SM_I(sbi)->mgc_info;
    struct sit_info *sit_i = SIT_I(sbi);
       struct slot_info *slot_i = SLT_I(sbi);
       struct page *sum_page;
       struct f2fs_summary_block *sum;
       unsigned int cur_segno, past_segno = NULL_SEGNO;
       unsigned int segno_buf[TMP_STACK_SZ];
       unsigned int nblks_buf[TMP_STACK_SZ];
       uint64_t slot_idx[TMP_STACK_SZ];
       unsigned int typebuf[TMP_STACK_SZ], type;
       //bool sum_read_buf[TMP_STACK_SZ], sum_readed;
       uint64_t cur_slot_idx;
       int i, buf_cnt = -1, nblks, start_idx = 0;
       struct slot_entry *slte;

       for (i = 0; i < mge->nr; i ++) { 
               block_t old_addr = le64_to_cpu(mge->mg_pairs[i].old_lba);
               //block_t new_addr = le64_to_cpu(mge->mg_pairs[i].old_lba);
               cur_segno = GET_SEGNO(sbi, old_addr);

               if (cur_segno == past_segno) {
                       nblks_buf[buf_cnt] += 1;
                       continue;
               }

               /* get slot index of segno from hash table */
               buf_cnt ++;
               nblks_buf[buf_cnt] = 1;
               
               down_write(&sit_i->sentry_lock);
               mutex_lock(&slot_i->lock);
               
               if ((slte = lookup_slot_hash(cur_segno)) == NULL) { 
                       mutex_unlock(&slot_i->lock);
                       up_write(&sit_i->sentry_lock);
                       segno_buf[buf_cnt] = NULL_SEGNO;
                       past_segno = cur_segno;

                       continue;
               }
               cur_slot_idx = slte->slot_idx;
               mutex_unlock(&slot_i->lock);

               past_segno = cur_segno;

               segno_buf[buf_cnt] = cur_segno;
               slot_idx[buf_cnt] = cur_slot_idx;
               //if (cur_slot_idx == NULL_SLOTNO) {
               //      printk("%s: cur_segno: %u cur_slot_idx: %u", 
               //                      __func__, cur_segno, cur_slot_idx);
               //}
               type = get_seg_entry(sbi, cur_slot_idx)->type;
               
               up_write(&sit_i->sentry_lock);
               
               typebuf[buf_cnt] = type;
               f2fs_ra_meta_pages(sbi, GET_SUM_BLOCK(sbi, cur_slot_idx),
                                       1, META_SSA, true);
       }
       
}

static void preread_new_migration_sum_blk(struct f2fs_sb_info *sbi, struct mg_entry *mge)
{
       struct migration_control *mgc = SM_I(sbi)->mgc_info;
    struct sit_info *sit_i = SIT_I(sbi);
       struct slot_info *slot_i = SLT_I(sbi);
       struct page *sum_page;
       struct f2fs_summary_block *sum;
       unsigned int cur_segno, past_segno = NULL_SEGNO;
       unsigned int segno_buf[TMP_STACK_SZ];
       unsigned int nblks_buf[TMP_STACK_SZ];
       uint64_t slot_idx[TMP_STACK_SZ];
       unsigned int typebuf[TMP_STACK_SZ], type;
       //bool sum_read_buf[TMP_STACK_SZ], sum_readed;
       uint64_t cur_slot_idx;
       int i, buf_cnt = -1, nblks, start_idx = 0;
       struct slot_entry *slte;

       for (i = 0; i < mge->nr; i ++) { 
               block_t new_addr = le64_to_cpu(mge->mg_pairs[i].new_lba);
               cur_segno = GET_SEGNO(sbi, new_addr);

               if (cur_segno == past_segno) {
                       nblks_buf[buf_cnt] += 1;
                       continue;
               }

               /* get slot index of segno from hash table */
               buf_cnt ++;
               nblks_buf[buf_cnt] = 1;
               
               down_write(&sit_i->sentry_lock);
               mutex_lock(&slot_i->lock);
               
               if ((slte = lookup_slot_hash(cur_segno)) == NULL) { 
                       mutex_unlock(&slot_i->lock);
                       up_write(&sit_i->sentry_lock);
                       segno_buf[buf_cnt] = NULL_SEGNO;
                       past_segno = cur_segno;

                       continue;
               }
               cur_slot_idx = slte->slot_idx;
               mutex_unlock(&slot_i->lock);

               past_segno = cur_segno;

               segno_buf[buf_cnt] = cur_segno;
               slot_idx[buf_cnt] = cur_slot_idx;
               //if (cur_slot_idx == NULL_SLOTNO) {
               //      printk("%s: cur_segno: %u cur_slot_idx: %u", 
               //                      __func__, cur_segno, cur_slot_idx);
               //}
               type = get_seg_entry(sbi, cur_slot_idx)->type;
               
               up_write(&sit_i->sentry_lock);
               
               typebuf[buf_cnt] = type;
               f2fs_ra_meta_pages(sbi, GET_SUM_BLOCK(sbi, cur_slot_idx),
                                       1, META_SSA, true);
       }
       
}

#ifdef NODE_READ_PIPELINE
static void preread_node_of_migration_entry(struct f2fs_sb_info *sbi, struct mg_entry *mge)
{
	struct migration_control *mgc = SM_I(sbi)->mgc_info;
   	struct sit_info *sit_i = SIT_I(sbi);
	struct slot_info *slot_i = SLT_I(sbi);
	struct page *sum_page;
	struct f2fs_summary_block *sum;
	unsigned int cur_segno, past_segno = NULL_SEGNO;
	unsigned int segno_buf[TMP_STACK_SZ];
	unsigned int nblks_buf[TMP_STACK_SZ];
	uint64_t slot_idx[TMP_STACK_SZ];
	unsigned int typebuf[TMP_STACK_SZ], type;
	//bool sum_read_buf[TMP_STACK_SZ], sum_readed;
	uint64_t cur_slot_idx;
	int i, buf_cnt = -1, nblks, start_idx = 0;
	struct slot_entry *slte;
//#ifdef MIGRATION_HANDLING_LATENCY
//	unsigned long long tstart, tend;
//	unsigned long long tstart_, tend_;
//	//unsigned long long tstart__, tend__;
//	static int mge_id = 0;
//	mge_id ++;
//#endif
	struct gc_inode_list gc_list = {
		.ilist = LIST_HEAD_INIT(gc_list.ilist),
		.iroot = RADIX_TREE_INIT(gc_list.iroot, GFP_NOFS),
	};

	for (i = 0; i < mge->nr; i ++) { 
//#ifdef MIGRATION_HANDLING_LATENCY
//		atomic_inc(&mgc->total_pgs);
//#endif
//#ifdef MIGRATION_HANDLING_LATENCY
//		tstart = OS_TimeGetUS();
//#endif
		block_t old_addr = le64_to_cpu(mge->mg_pairs[i].old_lba);
		//block_t new_addr = le64_to_cpu(mge->mg_pairs[i].old_lba);
		cur_segno = GET_SEGNO(sbi, old_addr);

		if (cur_segno == past_segno) {
			nblks_buf[buf_cnt] += 1;
			continue;
		}

		/* get slot index of segno from hash table */
		buf_cnt ++;
		nblks_buf[buf_cnt] = 1;
		
		down_write(&sit_i->sentry_lock);
		mutex_lock(&slot_i->lock);
		
		if ((slte = lookup_slot_hash(cur_segno)) == NULL) { 
			mutex_unlock(&slot_i->lock);
			up_write(&sit_i->sentry_lock);
			
			segno_buf[buf_cnt] = NULL_SEGNO;
			past_segno = cur_segno;

			continue;
		}
		cur_slot_idx = slte->slot_idx;
		mutex_unlock(&slot_i->lock);

		past_segno = cur_segno;

		segno_buf[buf_cnt] = cur_segno;
		slot_idx[buf_cnt] = cur_slot_idx;
		type = get_seg_entry(sbi, cur_slot_idx)->type;
		
		up_write(&sit_i->sentry_lock);
		
		typebuf[buf_cnt] = type;
//#ifdef MIGRATION_HANDLING_LATENCY
//		tend = OS_TimeGetUS();
//		mgc->mge_preproc_time += (tend-tstart);
//#endif

	}

	buf_cnt ++;

	for (i = 0; i < buf_cnt; i ++) {
		cur_segno = segno_buf[i];
		nblks = nblks_buf[i];
		if (cur_segno == NULL_SEGNO)
			goto next;
		cur_slot_idx = slot_idx[i];
		type = typebuf[i];
//#ifdef MIGRATION_HANDLING_LATENCY
//		tstart = OS_TimeGetUS();
//#endif

		sum_page = f2fs_get_sum_page(sbi, cur_slot_idx);
		
		if (IS_ERR(sum_page)) {
			f2fs_bug_on(sbi, 1);
		}

		unlock_page(sum_page);

		sum_page = find_get_page(META_MAPPING(sbi),
					GET_SUM_BLOCK(sbi, cur_slot_idx));
		f2fs_put_page(sum_page, 0);
//#ifdef MIGRATION_HANDLING_LATENCY
//		tend_ = OS_TimeGetUS();
//		mgc->mge_preproc_ssa_check_get_time += (tend_-tstart);
//#endif
		
		if (!PageUptodate(sum_page) || unlikely(f2fs_cp_error(sbi))) {
			printk("%s: unexpected!! page not uptodated", __func__);
			f2fs_bug_on(sbi, 1);
		}
		
		
//#ifdef MIGRATION_HANDLING_LATENCY
//		tstart_ = OS_TimeGetUS();
//#endif
		sum = page_address(sum_page);
		if (migration_segment_match(sbi, cur_segno, type)) {
			memcpy(sum, get_curseg_sum(sbi, type, cur_segno), sizeof(struct f2fs_summary_block));
		} 
//#ifdef MIGRATION_HANDLING_LATENCY
//		tend_ = OS_TimeGetUS();
//		mgc->mge_preproc_ssa_check_match_time += (tend_-tstart_);
//		//mgc->mge_preproc_seg_cnt ++;
//#endif
	
		type = IS_DATASEG(type)	? SUM_TYPE_DATA : SUM_TYPE_NODE;
		if (type != GET_SUM_TYPE((&sum->footer))) {
			down_write(&sit_i->sentry_lock);
			mutex_lock(&slot_i->lock);
			
			if ((slte = lookup_slot_hash(cur_segno)) == NULL) { 
				mutex_unlock(&slot_i->lock);
				up_write(&sit_i->sentry_lock);
				goto skip;	
			}

			/* prefree candidate case! */
			if (!list_empty(&slte->list)){
				int tmptmptmp;	
				block_t old_addr_, new_addr_;
				for (tmptmptmp = 0; tmptmptmp < nblks; tmptmptmp ++) {
					old_addr_ = le64_to_cpu(mge->mg_pairs[start_idx + tmptmptmp].old_lba);
					new_addr_ = le64_to_cpu(mge->mg_pairs[start_idx + tmptmptmp].new_lba);
					printk("%s: precandidate MGE! old_addr: 0x%llx new_addr: 0x%llx", __func__, 
							old_addr_, new_addr_);
				}
				mutex_unlock(&slot_i->lock);
				up_write(&sit_i->sentry_lock);
				goto skip;
			}


			f2fs_err(sbi, "Inconsistent segment (%u) type [%d, %d] in SSA and SIT",
				 cur_segno, type, GET_SUM_TYPE((&sum->footer)));
			printk("%s slte: segno: %u slot_idx: %u written blks: %u", __func__,
					slte->segno, slte->slot_idx, slte->written_blks);

			struct seg_entry* tmp_se = get_seg_entry(sbi, slte->slot_idx);
			printk("%s se segno: %u written blks: %u type: %u slba: 0x%lx is_mg_segno: %d", __func__,
					tmp_se->segno, tmp_se->valid_blocks, tmp_se->type, START_BLOCK(sbi, cur_segno), 
					IS_MIGRATION_SEGNO(sbi, cur_segno));
			int i_;
			struct seg_entry *se_;
			struct slot_entry *slte_;
			for (i_ = 0; i_ < MAIN_SEG_SLOTS(sbi); i_ ++) {
				slte_ = get_slot_entry(sbi, i_);
				se_ = get_seg_entry(sbi, i_);
				printk("%s: %dth slte_segno: %u slotno: %u written_blks: %u se_segno: %u vblks: %u type: %u", 
						__func__,
						i_, slte_->segno, slte_->slot_idx, slte_->written_blks, 
						se_->segno, se_->valid_blocks, se_->type);
			}
			mutex_unlock(&slot_i->lock);
			up_write(&sit_i->sentry_lock);
//#ifdef SHIVAL

			unsigned char type_ = IS_DATASEG(get_seg_entry(sbi, cur_slot_idx)->type) ?
						SUM_TYPE_DATA : SUM_TYPE_NODE;
			printk("%s: type_ %d type_real: %d slot idx: %u ", __func__, 
					type_, 
					get_seg_entry(sbi, cur_slot_idx)->type,
					cur_slot_idx);
			struct curseg_info *curseg = CURSEG_I(sbi, get_seg_entry(sbi, cur_slot_idx)->type);
			down_read(&SM_I(sbi)->curseg_lock);
			mutex_lock(&curseg->curseg_mutex);
			if (curseg->slot_idx == cur_slot_idx || curseg->segno == cur_segno)
				printk("%s: catchya!!! curseg slotidx: %u segno: %u, slot_idx: %u segno: %u", 
						__func__, curseg->slot_idx, curseg->segno, cur_slot_idx, cur_segno);

			mutex_unlock(&curseg->curseg_mutex);
			up_read(&SM_I(sbi)->curseg_lock);
			int i_tmp;
			for (i_tmp = 0; i_tmp < mge->nr; i_tmp ++) { 
				block_t old_addr__ = le64_to_cpu(mge->mg_pairs[i_tmp].old_lba);
				printk("%s: idx: %d segno: %u old_addr: 0x%llx", __func__, i_tmp, 
						GET_SEGNO(sbi, old_addr__), old_addr__);
			}
			for (i_tmp = 0; i_tmp < buf_cnt; i_tmp ++) {
				printk("%s: idx: %d segno: %u nblks: %u", __func__, i_tmp, 
						segno_buf[i_tmp], nblks_buf[i_tmp]);

			}
//#endif
			set_sbi_flag(sbi, SBI_NEED_FSCK);
			printk("%s: FSCK!!!!!!!!!!!!!", __func__);
			f2fs_stop_checkpoint(sbi, false);
			goto skip;
		}
		
		struct migration_seg_info mg_seg_info = {
			.mge = mge,
			.start_idx = start_idx,
			.nblks = nblks,
		};
		
//#ifdef MIGRATION_HANDLING_LATENCY
//		tend = OS_TimeGetUS();
//		mgc->mge_preproc_ssa_check_time += (tend-tstart);
//		//mgc->mge_preproc_seg_cnt ++;
//#endif
		
		if (type == SUM_TYPE_NODE) {
//#ifdef MIGRATION_HANDLING_LATENCY
//			tstart = OS_TimeGetUS();
//#endif
			//printk("%s: start node seg", __func__);
			preread_node_of_node_segment(sbi, sum->entries, cur_segno,
								&mg_seg_info, cur_slot_idx);
			//printk("%s: end node seg", __func__);
//#ifdef MIGRATION_HANDLING_LATENCY
//			tend = OS_TimeGetUS();
//			mgc->node_seg_time += (tend-tstart);
//			mgc->node_seg_cnt ++;
//#endif
		}
		else {
//#ifdef MIGRATION_HANDLING_LATENCY
//			tstart = OS_TimeGetUS();
//#endif
			//printk("%s: start data seg", __func__);
			preread_node_of_data_segment(sbi, sum->entries, &gc_list,
							cur_segno, &mg_seg_info, cur_slot_idx, segno_buf, nblks_buf, buf_cnt);
			//printk("%s: end data seg", __func__);
//#ifdef MIGRATION_HANDLING_LATENCY
//			tend = OS_TimeGetUS();
//			mgc->data_seg_time += (tend-tstart);
//			mgc->data_seg_cnt ++;
//#endif
		}
		
skip:
		f2fs_put_page(sum_page, 0);
next:
		start_idx += nblks;
	}

	
	put_gc_inode(&gc_list);
}
#endif

static void reflect_migration_entry(struct f2fs_sb_info *sbi, struct mg_entry *mge)
{
	struct migration_control *mgc = SM_I(sbi)->mgc_info;
   	struct sit_info *sit_i = SIT_I(sbi);
	struct slot_info *slot_i = SLT_I(sbi);
	struct page *sum_page;
	struct f2fs_summary_block *sum;
	unsigned int cur_segno, past_segno = NULL_SEGNO;
	unsigned int segno_buf[TMP_STACK_SZ];
	unsigned int nblks_buf[TMP_STACK_SZ];
	uint64_t slot_idx[TMP_STACK_SZ];
	unsigned int typebuf[TMP_STACK_SZ], type;
	//bool sum_read_buf[TMP_STACK_SZ], sum_readed;
	uint64_t cur_slot_idx;
	int i, buf_cnt = -1, nblks, start_idx = 0;
	struct slot_entry *slte;
#ifdef MIGRATION_HANDLING_LATENCY
	unsigned long long tstart, tend;
	unsigned long long tstart_, tend_;
	//unsigned long long tstart__, tend__;
	static int mge_id = 0;
	mge_id ++;
#endif
	struct gc_inode_list gc_list = {
		.ilist = LIST_HEAD_INIT(gc_list.ilist),
		.iroot = RADIX_TREE_INIT(gc_list.iroot, GFP_NOFS),
	};

#ifdef SHIVAL
//	f2fs_bug_on(sbi, mge->nr != 256);
//	block_t old_addr_;
//	block_t new_addr_;
//	for (i = 0; i < mge->nr; i ++) {
//		old_addr_ = le64_to_cpu(mge->mg_pairs[i].old_lba);
//		new_addr_ = le64_to_cpu(mge->mg_pairs[i].new_lba);
//		printk("%s: cid: %u \n \
//				%dth old_addr: 0x%lx new_addr: 0x%lx \n \
//				%dth old_addr: 0x%lx new_addr: 0x%lx \n \
//				%dth old_addr: 0x%lx new_addr: 0x%lx \n \
//				%dth old_addr: 0x%lx new_addr: 0x%lx \n \
//				%dth old_addr: 0x%lx new_addr: 0x%lx \n \
//				%dth old_addr: 0x%lx new_addr: 0x%lx \n \
//				%dth old_addr: 0x%lx new_addr: 0x%lx \n \
//				%dth old_addr: 0x%lx new_addr: 0x%lx \n \
//				%dth old_addr: 0x%lx new_addr: 0x%lx \n \
//				%dth old_addr: 0x%lx new_addr: 0x%lx \n \
//				%dth old_addr: 0x%lx new_addr: 0x%lx \n \
//				%dth old_addr: 0x%lx new_addr: 0x%lx \n \
//				%dth old_addr: 0x%lx new_addr: 0x%lx \n \
//				%dth old_addr: 0x%lx new_addr: 0x%lx \n \
//				%dth old_addr: 0x%lx new_addr: 0x%lx \n \
//				%dth old_addr: 0x%lx new_addr: 0x%lx \n \
//				%dth old_addr: 0x%lx new_addr: 0x%lx \n \
//				%dth old_addr: 0x%lx new_addr: 0x%lx \n \
//				%dth old_addr: 0x%lx new_addr: 0x%lx \n \
//				", __func__, mge->command_id, 
//				0, le64_to_cpu(mge->mg_pairs[0].old_lba), le64_to_cpu(mge->mg_pairs[0].new_lba),
//				, le64_to_cpu(mge->mg_pairs[].old_lba), le64_to_cpu(mge->mg_pairs[].new_lba),
//			);
//	}

#endif

	for (i = 0; i < mge->nr; i ++) { 
#ifdef MIGRATION_HANDLING_LATENCY
		atomic_inc(&mgc->total_pgs);
#endif
#ifdef MIGRATION_HANDLING_LATENCY
		tstart = OS_TimeGetUS();
#endif
		block_t old_addr = le64_to_cpu(mge->mg_pairs[i].old_lba);
		//block_t new_addr = le64_to_cpu(mge->mg_pairs[i].old_lba);
		cur_segno = GET_SEGNO(sbi, old_addr);

		if (cur_segno == past_segno) {
			nblks_buf[buf_cnt] += 1;
			continue;
		}

		/* get slot index of segno from hash table */
		buf_cnt ++;
		nblks_buf[buf_cnt] = 1;
		
		down_write(&sit_i->sentry_lock);
		mutex_lock(&slot_i->lock);
		
		if ((slte = lookup_slot_hash(cur_segno)) == NULL) { 
			//cur_segno = NULL_SEGNO;
			mutex_unlock(&slot_i->lock);
			up_write(&sit_i->sentry_lock);
			
			segno_buf[buf_cnt] = NULL_SEGNO;
			past_segno = cur_segno;
			//int is_ddm = 0;
			//if (check_dynamic_discard_map(sbi, old_addr)) {
			//	is_ddm = 1;
			//	//f2fs_bug_on(sbi, 1);
			//}
			
			//printk("%s: not found segno: %u addr: 0x%llx inddm: %d", 
			//		__func__, cur_segno, old_addr, is_ddm);

			continue;
		}
		cur_slot_idx = slte->slot_idx;
		mutex_unlock(&slot_i->lock);

		past_segno = cur_segno;

		segno_buf[buf_cnt] = cur_segno;
		slot_idx[buf_cnt] = cur_slot_idx;
		//if (cur_slot_idx == NULL_SLOTNO) {
		//	printk("%s: cur_segno: %u cur_slot_idx: %u", 
		//			__func__, cur_segno, cur_slot_idx);
		//}
		type = get_seg_entry(sbi, cur_slot_idx)->type;
		
		up_write(&sit_i->sentry_lock);
		
		typebuf[buf_cnt] = type;
		//typebuf[buf_cnt] = IS_DATASEG(get_seg_entry(sbi, cur_slot_idx)->type) ?
		//				SUM_TYPE_DATA : SUM_TYPE_NODE;
	
		/* Corner Case */
		/* Segment match between next old addr and curseg causes un-updated sum block. */
		//if (migration_segment_match(sbi, cur_segno, type)) {
		//	/* No need to read dummy sum page since it is on curseg */
		//	sum_read_buf[buf_cnt] = false;
		//	printk("%s: catchya!!! old_addr: 0x%lx slot_idx: %u segno: %u", 
		//			__func__, old_addr, cur_slot_idx, cur_segno);
		//	continue;
		//}
#ifdef MIGRATION_HANDLING_LATENCY
		tend = OS_TimeGetUS();
		mgc->mge_preproc_time += (tend-tstart);
#endif

		//sum_read_buf[buf_cnt] = true;
		/* No curseg lock required. No flict with curseg summary. */
//		sum_page = f2fs_get_sum_page(sbi, cur_slot_idx);
		

//		if (IS_ERR(sum_page)) {
//			f2fs_bug_on(sbi, 1);
//		}

//		unlock_page(sum_page);
	}

	buf_cnt ++;

	for (i = 0; i < buf_cnt; i ++) {
		cur_segno = segno_buf[i];
		nblks = nblks_buf[i];
		if (cur_segno == NULL_SEGNO)
			goto next;
		cur_slot_idx = slot_idx[i];
		type = typebuf[i];
		//sum_readed = sum_read_buf[i];
		
		//if (get_seg_entry(sbi, cur_slot_idx)->type == CURSEG_MIGRATION_NODE) {
		//	struct curseg_info *curseg = CURSEG_I(sbi, CURSEG_MIGRATION_NODE);
		//	down_read(&SM_I(sbi)->curseg_lock);
		//	mutex_lock(&curseg->curseg_mutex);
		//	if (curseg->slot_idx == cur_slot_idx)
		//		printk("%s: catchya!!! curseg slotidx: %u segno: %u, slot_idx: %u segno: %u", 
		//				__func__, curseg->slot_idx, curseg->segno, cur_slot_idx, cur_segno);

		//	mutex_unlock(&curseg->curseg_mutex);
		//	up_read(&SM_I(sbi)->curseg_lock);
		//}

		//if (!sum_readed) {
#ifdef MIGRATION_HANDLING_LATENCY
		tstart = OS_TimeGetUS();
#endif

		sum_page = f2fs_get_sum_page(sbi, cur_slot_idx);
		
		if (IS_ERR(sum_page)) {
			f2fs_bug_on(sbi, 1);
		}

		unlock_page(sum_page);

		sum_page = find_get_page(META_MAPPING(sbi),
					GET_SUM_BLOCK(sbi, cur_slot_idx));
		f2fs_put_page(sum_page, 0);
#ifdef MIGRATION_HANDLING_LATENCY
		tend_ = OS_TimeGetUS();
		mgc->mge_preproc_ssa_check_get_time += (tend_-tstart);
		//mgc->mge_preproc_seg_cnt ++;
	//	printk("%s: mgeID: %d ssa get time: %llu usec segno: %llu secno: %llu slba: 0x%llx", 
	//			__func__, mge_id, tend_-tstart, 
	//			cur_segno, 
	//			cur_segno / sbi->segs_per_sec, 
	//			START_BLOCK(sbi, cur_segno)
	//			);
#endif
		
		if (!PageUptodate(sum_page) || unlikely(f2fs_cp_error(sbi))) {
			printk("%s: unexpected!! page not uptodated", __func__);
			f2fs_bug_on(sbi, 1);
		}
		
		
#ifdef MIGRATION_HANDLING_LATENCY
		tstart_ = OS_TimeGetUS();
#endif
		sum = page_address(sum_page);
		if (migration_segment_match(sbi, cur_segno, type)) {
			//memcpy(sum, get_curseg_sum(sbi, type, cur_segno), SUM_ENTRY_SIZE);
			memcpy(sum, get_curseg_sum(sbi, type, cur_segno), sizeof(struct f2fs_summary_block));
			//printk("%s:  MATCH!!!!!!!!!!!!! type: %u segno: %u", __func__, 
			//		type, cur_segno);
		} 
	//	else {
	//		sum = page_address(sum_page);
	//	}
#ifdef MIGRATION_HANDLING_LATENCY
		tend_ = OS_TimeGetUS();
		mgc->mge_preproc_ssa_check_match_time += (tend_-tstart_);
		//mgc->mge_preproc_seg_cnt ++;
#endif
	
		type = IS_DATASEG(type)	? SUM_TYPE_DATA : SUM_TYPE_NODE;
		if (type != GET_SUM_TYPE((&sum->footer))) {
			down_write(&sit_i->sentry_lock);
			mutex_lock(&slot_i->lock);
			
			if ((slte = lookup_slot_hash(cur_segno)) == NULL) { 
				mutex_unlock(&slot_i->lock);
				up_write(&sit_i->sentry_lock);
				goto skip;	
			}

			/* prefree candidate case! */
			if (!list_empty(&slte->list)){
				int tmptmptmp;	
				block_t old_addr_, new_addr_;
				for (tmptmptmp = 0; tmptmptmp < nblks; tmptmptmp ++) {
					old_addr_ = le64_to_cpu(mge->mg_pairs[start_idx + tmptmptmp].old_lba);
					new_addr_ = le64_to_cpu(mge->mg_pairs[start_idx + tmptmptmp].new_lba);
					printk("%s: precandidate MGE! old_addr: 0x%llx new_addr: 0x%llx", __func__, 
							old_addr_, new_addr_);
				}
				mutex_unlock(&slot_i->lock);
				up_write(&sit_i->sentry_lock);
				goto skip;
			}


			f2fs_err(sbi, "Inconsistent segment (%u) type [%d, %d] in SSA and SIT",
				 cur_segno, type, GET_SUM_TYPE((&sum->footer)));
			printk("%s slte: segno: %u slot_idx: %u written blks: %u", __func__,
					slte->segno, slte->slot_idx, slte->written_blks);

			struct seg_entry* tmp_se = get_seg_entry(sbi, slte->slot_idx);
			printk("%s se segno: %u written blks: %u type: %u slba: 0x%lx is_mg_segno: %d", __func__,
					tmp_se->segno, tmp_se->valid_blocks, tmp_se->type, START_BLOCK(sbi, cur_segno), 
					IS_MIGRATION_SEGNO(sbi, cur_segno));
			int i_;
			struct seg_entry *se_;
			struct slot_entry *slte_;
			for (i_ = 0; i_ < MAIN_SEG_SLOTS(sbi); i_ ++) {
				slte_ = get_slot_entry(sbi, i_);
				se_ = get_seg_entry(sbi, i_);
				printk("%s: %dth slte_segno: %u slotno: %u written_blks: %u se_segno: %u vblks: %u type: %u", 
						__func__,
						i_, slte_->segno, slte_->slot_idx, slte_->written_blks, 
						se_->segno, se_->valid_blocks, se_->type);
				//printk("%s: %dth se slotno: %u vblks: %u type: %u", __func__,
				//		i, se->segno, se->valid_blocks, se->type);
			}
			mutex_unlock(&slot_i->lock);
			up_write(&sit_i->sentry_lock);
//#ifdef SHIVAL

			unsigned char type_ = IS_DATASEG(get_seg_entry(sbi, cur_slot_idx)->type) ?
						SUM_TYPE_DATA : SUM_TYPE_NODE;
			printk("%s: type_ %d type_real: %d slot idx: %u ", __func__, 
					type_, 
					get_seg_entry(sbi, cur_slot_idx)->type,
					cur_slot_idx);
			struct curseg_info *curseg = CURSEG_I(sbi, get_seg_entry(sbi, cur_slot_idx)->type);
			down_read(&SM_I(sbi)->curseg_lock);
			mutex_lock(&curseg->curseg_mutex);
			if (curseg->slot_idx == cur_slot_idx || curseg->segno == cur_segno)
				printk("%s: catchya!!! curseg slotidx: %u segno: %u, slot_idx: %u segno: %u", 
						__func__, curseg->slot_idx, curseg->segno, cur_slot_idx, cur_segno);

			mutex_unlock(&curseg->curseg_mutex);
			up_read(&SM_I(sbi)->curseg_lock);
			int i_tmp;
			for (i_tmp = 0; i_tmp < mge->nr; i_tmp ++) { 
				block_t old_addr__ = le64_to_cpu(mge->mg_pairs[i_tmp].old_lba);
				printk("%s: idx: %d segno: %u old_addr: 0x%llx", __func__, i_tmp, 
						GET_SEGNO(sbi, old_addr__), old_addr__);
			}
			for (i_tmp = 0; i_tmp < buf_cnt; i_tmp ++) {
				printk("%s: idx: %d segno: %u nblks: %u", __func__, i_tmp, 
						segno_buf[i_tmp], nblks_buf[i_tmp]);

			}
//#endif
			set_sbi_flag(sbi, SBI_NEED_FSCK);
			printk("%s: FSCK!!!!!!!!!!!!!", __func__);
			f2fs_stop_checkpoint(sbi, false);
			goto skip;
		}
		
		struct migration_seg_info mg_seg_info = {
			.mge = mge,
			.start_idx = start_idx,
			.nblks = nblks,
		};
		
#ifdef MIGRATION_HANDLING_LATENCY
		tend = OS_TimeGetUS();
		mgc->mge_preproc_ssa_check_time += (tend-tstart);
		//mgc->mge_preproc_seg_cnt ++;
#endif
		
		if (type == SUM_TYPE_NODE) {
#ifdef MIGRATION_HANDLING_LATENCY
			tstart = OS_TimeGetUS();
#endif
			//printk("%s: start node seg", __func__);
			reflect_node_segment_migration(sbi, sum->entries, cur_segno,
								&mg_seg_info, cur_slot_idx);
			//printk("%s: end node seg", __func__);
#ifdef MIGRATION_HANDLING_LATENCY
			tend = OS_TimeGetUS();
			mgc->node_seg_time += (tend-tstart);
			mgc->node_seg_cnt ++;
#endif
		}
		else {
#ifdef MIGRATION_HANDLING_LATENCY
			tstart = OS_TimeGetUS();
#endif
#ifdef LM_NO_INODE_READ
			reflect_data_segment_migration_light_no_inode(sbi, sum->entries, &gc_list,
							cur_segno, &mg_seg_info, cur_slot_idx, segno_buf, nblks_buf, buf_cnt);
#else
			//printk("%s: start data seg", __func__);
			reflect_data_segment_migration_light(sbi, sum->entries, &gc_list,
							cur_segno, &mg_seg_info, cur_slot_idx, segno_buf, nblks_buf, buf_cnt);
			//printk("%s: end data seg", __func__);
#endif
#ifdef MIGRATION_HANDLING_LATENCY
			tend = OS_TimeGetUS();
			mgc->data_seg_time += (tend-tstart);
			mgc->data_seg_cnt ++;
#endif
		}
		
skip:
		f2fs_put_page(sum_page, 0);
next:
		start_idx += nblks;
	}

	
	put_gc_inode(&gc_list);
}

#ifdef NODE_READ_PIPELINE
static int node_prereader_thread(void *data)
{
	struct f2fs_sb_info *sbi = data;
	struct migration_control *mgc = SM_I(sbi)->mgc_info;
	wait_queue_head_t *q = &mgc->node_prereader_wait_queue;
//	struct discard_policy dpolicy;
	//unsigned int wait_ms = DEF_MIGRATION_WAIT_TIME;
	unsigned int wait_us = DEF_MIGRATION_WAIT_TIME_USEC;
	//int issued;
	//int mg_ent_cnt;
	struct mg_entry *mge;
	int comp_mg_cnt = 0;
#ifdef MIGRATION_HANDLING_LATENCY
	unsigned long long tstart = 0, tend = 0;
#endif
	//set_freezable();

	do {
	//	wait_event_interruptible_timeout(*q,
	//			kthread_should_stop() || 
	//			mgc->migration_wake,
	//			msecs_to_jiffies(wait_ms));
		wait_event_interruptible_timeout(*q,
				kthread_should_stop() || 
				mgc->node_prereader_wake,
				usecs_to_jiffies(wait_us));
		//wait_event_interruptible_timeout(*q,
		//		kthread_should_stop() || freezing(current) ||
		//		mgc->migration_wake,
		//		msecs_to_jiffies(wait_ms));

		if (mgc->node_prereader_wake)
			mgc->node_prereader_wake = 0;

		/* clean up pending candidates before going to sleep */
		/*
		if (atomic_read(&dcc->queued_discard))
			__wait_all_discard_cmd(sbi, NULL);
		*/
		//mg_ent_cnt = (int) atomic_read(&mgc->mg_entry_cnt);
		
handle_mge:
		mge = NULL;
		/* read mgc->entry_list */
		spin_lock(&mgc->node_prereader_entry_list_lock);

		if (!list_empty(&mgc->node_prereader_entry_list)) {
			mge = list_first_entry(&mgc->node_prereader_entry_list, struct mg_entry, list);
			list_del(&mge->list);
		}

		spin_unlock(&mgc->node_prereader_entry_list_lock);

		/* read mgc->entry_list, reflect to the ssa, sit. */
		if (mge) {
//#ifdef MIGRATION_HANDLING_LATENCY
//			tstart = OS_TimeGetUS();
//#endif
			preread_node_of_migration_entry(sbi, mge);
//#ifdef MIGRATION_HANDLING_LATENCY
//			tend = OS_TimeGetUS();
//			mgc->mge_proc_time += (tend-tstart);
//			mgc->mge_proc_cnt ++;
//#endif

			
			/* don't need lock cuz precompletion_list is held during checkpoint routine */
//#ifdef MIGRATION_HANDLING_LATENCY
//			tstart = OS_TimeGetUS();
//#endif
			//f2fs_lock_op(sbi);
//#ifdef MIGRATION_HANDLING_LATENCY
//			tend = OS_TimeGetUS();
//			mgc->mge_lck_time += (tend-tstart);
//#endif
//#ifdef MIGRATION_HANDLING_LATENCY
//			tstart = OS_TimeGetUS();
//#endif
//#ifdef MIGRATION_HANDLING_LATENCY
//			tend = OS_TimeGetUS();
//			mgc->mge_lck2_time += (tend-tstart);
//#endif
			spin_lock(&mgc->entry_list_lock);
			list_add_tail(&mge->list, &mgc->entry_list);
			spin_unlock(&mgc->entry_list_lock);

			//printk("%s: mg cid: %u to precom", __func__, mge->command_id);
			//f2fs_unlock_op(sbi);

			//kmem_cache_free(mg_entry_slab, mge);
			//atomic_dec(&mgc->mg_entry_cnt);

			wake_up_migration_thread(sbi);

			goto handle_mge;
		}

		//if (try_to_freeze())
		//	continue;
		if (f2fs_readonly(sbi->sb))
			continue;
		if (kthread_should_stop())
			return 0;
		if (is_sbi_flag_set(sbi, SBI_NEED_FSCK)) {
			printk("%s: unexpected!! SBI_NEED_FSCK", __func__);
			//wait_ms = dpolicy.max_interval;
			continue;
		}

		//if (sbi->gc_mode == GC_URGENT_HIGH)
		//	__init_discard_policy(sbi, &dpolicy, DPOLICY_FORCE, 1);

		//sb_start_intwrite(sbi->sb);
/*
		issued = __issue_discard_cmd(sbi, &dpolicy);
		if (issued > 0) {
			__wait_all_discard_cmd(sbi, &dpolicy);
			wait_ms = dpolicy.min_interval;
			//wait_ms = dpolicy.max_interval;
		} else if (issued == -1){
			wait_ms = f2fs_time_to_wait(sbi, DISCARD_TIME);
			if (!wait_ms)
				wait_ms = dpolicy.mid_interval;
		} else {
			wait_ms = dpolicy.max_interval;
		}
*/
		//sb_end_intwrite(sbi->sb);

	} while (!kthread_should_stop());
	return 0;
}
#endif

static int handle_migration_thread(void *data)
{
	struct f2fs_sb_info *sbi = data;
	struct migration_control *mgc = SM_I(sbi)->mgc_info;
	wait_queue_head_t *q = &mgc->migration_wait_queue;
//	struct discard_policy dpolicy;
	//unsigned int wait_ms = DEF_MIGRATION_WAIT_TIME;
	unsigned int wait_us = DEF_MIGRATION_WAIT_TIME_USEC;
	//int issued;
	//int mg_ent_cnt;
	struct mg_entry *mge;
	int comp_mg_cnt = 0;
#ifdef MIGRATION_HANDLING_LATENCY
	unsigned long long tstart = 0, tend = 0;
#endif
	//set_freezable();

	do {
	//	wait_event_interruptible_timeout(*q,
	//			kthread_should_stop() || 
	//			mgc->migration_wake,
	//			msecs_to_jiffies(wait_ms));
		wait_event_interruptible_timeout(*q,
				kthread_should_stop() || 
				mgc->migration_wake,
				usecs_to_jiffies(wait_us));
		//wait_event_interruptible_timeout(*q,
		//		kthread_should_stop() || freezing(current) ||
		//		mgc->migration_wake,
		//		msecs_to_jiffies(wait_ms));

		if (mgc->migration_wake)
			mgc->migration_wake = 0;

		/* clean up pending candidates before going to sleep */
		/*
		if (atomic_read(&dcc->queued_discard))
			__wait_all_discard_cmd(sbi, NULL);
		*/
		//mg_ent_cnt = (int) atomic_read(&mgc->mg_entry_cnt);
		
handle_mge:
		mge = NULL;
		/* read mgc->entry_list */
		spin_lock(&mgc->entry_list_lock);

		if (!list_empty(&mgc->entry_list)) {
			mge = list_first_entry(&mgc->entry_list, struct mg_entry, list);
			list_del(&mge->list);
		}

		spin_unlock(&mgc->entry_list_lock);

		/* read mgc->entry_list, reflect to the ssa, sit. */
		if (mge) {
			//printk("%s: bef reflect mge", __func__);
#ifdef MIGRATION_HANDLING_LATENCY
			tstart = OS_TimeGetUS();
#endif
			reflect_migration_entry(sbi, mge);
#ifdef MIGRATION_HANDLING_LATENCY
			tend = OS_TimeGetUS();
			mgc->mge_proc_time += (tend-tstart);
			mgc->mge_proc_cnt ++;
#endif
			//comp_mg_cnt ++;
			//if (comp_mg_cnt % 10000 == 0) 
			//	printk("%s: mg completed cnt: %d", __func__, comp_mg_cnt);

			
			/* don't need lock cuz precompletion_list is held during checkpoint routine */
#ifdef MIGRATION_HANDLING_LATENCY
			tstart = OS_TimeGetUS();
#endif
			f2fs_lock_op(sbi);
#ifdef MIGRATION_HANDLING_LATENCY
			tend = OS_TimeGetUS();
			mgc->mge_lck_time += (tend-tstart);
#endif
#ifdef MIGRATION_HANDLING_LATENCY
			tstart = OS_TimeGetUS();
#endif
			spin_lock(&mgc->precompletion_lock);	
#ifdef MIGRATION_HANDLING_LATENCY
			tend = OS_TimeGetUS();
			mgc->mge_lck2_time += (tend-tstart);
#endif
			list_add_tail(&mge->list, &mgc->precompletion_list);	
			atomic_dec(&mgc->mg_entry_cnt);
			atomic_inc(&mgc->mg_entry_cnt_pre_comp);
			//printk("%s: mg cid: %u to precom", __func__, mge->command_id);
			spin_unlock(&mgc->precompletion_lock);	
			f2fs_unlock_op(sbi);

			//kmem_cache_free(mg_entry_slab, mge);
			//atomic_dec(&mgc->mg_entry_cnt);
			goto handle_mge;
		}

		//if (try_to_freeze())
		//	continue;
		if (f2fs_readonly(sbi->sb))
			continue;
		if (kthread_should_stop())
			return 0;
		if (is_sbi_flag_set(sbi, SBI_NEED_FSCK)) {
			printk("%s: unexpected!! SBI_NEED_FSCK", __func__);
			//wait_ms = dpolicy.max_interval;
			continue;
		}

		//if (sbi->gc_mode == GC_URGENT_HIGH)
		//	__init_discard_policy(sbi, &dpolicy, DPOLICY_FORCE, 1);

		//sb_start_intwrite(sbi->sb);
/*
		issued = __issue_discard_cmd(sbi, &dpolicy);
		if (issued > 0) {
			__wait_all_discard_cmd(sbi, &dpolicy);
			wait_ms = dpolicy.min_interval;
			//wait_ms = dpolicy.max_interval;
		} else if (issued == -1){
			wait_ms = f2fs_time_to_wait(sbi, DISCARD_TIME);
			if (!wait_ms)
				wait_ms = dpolicy.mid_interval;
		} else {
			wait_ms = dpolicy.max_interval;
		}
*/
		//sb_end_intwrite(sbi->sb);

	} while (!kthread_should_stop());
	return 0;
}

void init_mg_entry(struct mg_entry *mge, struct nvme_mg_cmd *cmd)
{
	mge->command_id = cmd->command_id;
	mge->nsid = le32_to_cpu(cmd->nsid);
	mge->nr = le32_to_cpu(cmd->nr) + 1;
	//printk("%s: cid: %u ccid: %u", __func__, 
	//		mge->command_id, cmd->command_id);
	//printk("%s: sizeof mgpairs: %lu", __func__, sizeof(mge->mg_pairs));
	//memcpy(mge->mg_pairs, cmd->mg_pairs, sizeof(mge->mg_pairs));
	mge->mg_pairs = cmd->mg_batch_ptr;
	INIT_LIST_HEAD(&mge->list);
}

void add_migration_cmd(void *_sbi, void *_cmd)
{
	struct nvme_mg_cmd *cmd = (struct nvme_mg_cmd *) _cmd;
	struct f2fs_sb_info *sbi = (struct f2fs_sb_info *) _sbi;
	struct migration_control *mgc = SM_I(sbi)->mgc_info;
	struct mg_entry *mge;
	if (!(mge = f2fs_kmem_cache_alloc(mg_entry_slab, GFP_F2FS_ZERO))) {
		printk("%s: mge create fail", __func__);
		f2fs_bug_on(sbi, 1);
	}

	init_mg_entry(mge, cmd);

	preread_old_migration_sum_blk(sbi, mge);
	preread_new_migration_sum_blk(sbi, mge);

#ifdef NODE_READ_PIPELINE
	spin_lock(&mgc->node_prereader_entry_list_lock);
	list_add_tail(&mge->list, &mgc->node_prereader_entry_list);
	spin_unlock(&mgc->node_prereader_entry_list_lock);
#else
	spin_lock(&mgc->entry_list_lock);
	list_add_tail(&mge->list, &mgc->entry_list);
	spin_unlock(&mgc->entry_list_lock);
#endif

	atomic_inc(&mgc->mg_entry_cnt);
	atomic_inc(&mgc->total_mg_entry_cnt);
	//printk("%s: mg_entry_cnt: %d", __func__, atomic_read(&mgc->mg_entry_cnt));
	//kmem_cache_free(discard_entry_slab, entry);
//		list_add_tail(&fio->list, &io->io_list);
}

static int setup_reverse_queue(struct f2fs_sb_info *sbi)
{
	struct gendisk *bi_disk;
	struct request_queue *q;
	int i, ret;

	if (f2fs_is_multi_device(sbi)) {
		for (i = 0; i < sbi->s_ndevs; i ++ ) {
			bi_disk = FDEV(i).bdev->bd_disk;
			q = bi_disk->queue;
#ifdef NODE_READ_PIPELINE
			if ((ret = submit_bio_setup_rev_queue(q, sbi, wake_up_prereader_thread, add_migration_cmd))){
#else
			if ((ret = submit_bio_setup_rev_queue(q, sbi, wake_up_migration_thread, add_migration_cmd))){
#endif
				printk("%s: error. ret: %d", __func__, ret);
				return ret;
			}
		}
	} else {
		bi_disk = sbi->sb->s_bdev->bd_disk;
		q = bi_disk->queue;
#ifdef NODE_READ_PIPELINE
		if ((ret = submit_bio_setup_rev_queue(q, sbi, wake_up_prereader_thread, add_migration_cmd))){
#else
		if ((ret = submit_bio_setup_rev_queue(q, sbi, wake_up_migration_thread, add_migration_cmd))){
#endif
			printk("%s: error. ret: %d", __func__, ret);
			f2fs_bug_on(sbi, 1);
			return ret;
		}
		sbi->q = q;
	}
	return ret;
}

static int create_migration_io_control(struct f2fs_sb_info *sbi)
{
	dev_t dev = sbi->sb->s_bdev->bd_dev;
	struct migration_control *mgc;
	int err = 0, i;

	if (SM_I(sbi)->mgc_info) {
		mgc = SM_I(sbi)->mgc_info;
		goto init_thread;
	}

	mgc = f2fs_kzalloc(sbi, sizeof(struct migration_control), GFP_KERNEL);
	if (!mgc)
		return -ENOMEM;

	INIT_LIST_HEAD(&mgc->entry_list);
	spin_lock_init(&mgc->entry_list_lock);

	INIT_LIST_HEAD(&mgc->completion_list);
	INIT_LIST_HEAD(&mgc->precompletion_list);
	spin_lock_init(&mgc->completion_lock);
	spin_lock_init(&mgc->precompletion_lock);
	
	atomic_set(&mgc->mg_entry_cnt, 0);
	atomic_set(&mgc->total_mg_entry_cnt, 0);
	atomic_set(&mgc->mg_entry_cnt_pre_comp, 0);
	
	init_waitqueue_head(&mgc->migration_wait_queue);

#ifdef NODE_READ_PIPELINE
	INIT_LIST_HEAD(&mgc->node_prereader_entry_list);
	spin_lock_init(&mgc->node_prereader_entry_list_lock);
	init_waitqueue_head(&mgc->node_prereader_wait_queue);
#endif


#ifdef MIGRATION_HANDLING_LATENCY
	atomic_set(&mgc->total_pgs, 0);
	atomic_set(&mgc->node_pgs, 0);
	atomic_set(&mgc->data_pgs, 0);

	atomic_set(&mgc->dirty_node_pgs , 0);
	atomic_set(&mgc->updated_node_pgs , 0);
	
	mgc->data_seg_time = 0;
	mgc->node_seg_time = 0;
	mgc->data_seg_cnt = 0;
	mgc->node_seg_cnt = 0;
	mgc->_data_seg_time = 0;
	mgc->_node_seg_time = 0;
	mgc->_data_seg_cnt = 0;
	mgc->_node_seg_cnt = 0;
	
	mgc->__data_seg_time = 0;
	mgc->__data_seg_cnt = 0;

	mgc->node_read_time = 0;
	mgc->node_read_cnt = 0;
	
	mgc->nat_read_time = 0;
	mgc->nat_read_cnt = 0;
	
	mgc->ssa_update_time = 0;
	mgc->ssa_update_cnt = 0;
		
	mgc->ssa_update_lck_time = 0;
	mgc->ssa_update_lck_cnt = 0;
		
	mgc->sit_update_lck_time = 0;
	mgc->sit_update_lck_cnt = 0;


	mgc->mge_proc_time = 0;
	mgc->mge_proc_cnt = 0;
	mgc->mge_lck_time = 0;
	mgc->mge_lck2_time = 0;

	mgc->mge_preproc_time = 0;
	mgc->mge_preproc_get_ssa_time = 0;
	mgc->mge_preproc_ssa_check_time = 0;
	mgc->mge_preproc_ssa_check_get_time = 0;
	mgc->mge_preproc_ssa_check_match_time = 0;


	mgc->data_seg_p4_start_time = 0;
	mgc->data_seg_p3_time  = 0;
	mgc->data_seg_p2_time  = 0;
	mgc->data_seg_p1_time  = 0;
	mgc->data_seg_is_alive_time = 0;
#endif

	SM_I(sbi)->mgc_info = mgc;

init_thread:
	mgc->iplfs_migration_handler = kthread_run(handle_migration_thread, sbi,
				"f2fs_migration-%u:%u", MAJOR(dev), MINOR(dev));
	if (IS_ERR(mgc->iplfs_migration_handler)) {
		err = PTR_ERR(mgc->iplfs_migration_handler);
		kfree(mgc);
		SM_I(sbi)->mgc_info = NULL;
		return err;
	}

#ifdef NODE_READ_PIPELINE
	mgc->migration_node_prereader = kthread_run(node_prereader_thread, sbi,
				"f2fs_node_prereader-%u:%u", MAJOR(dev), MINOR(dev));
	if (IS_ERR(mgc->migration_node_prereader)) {
		err = PTR_ERR(mgc->migration_node_prereader);
		kfree(mgc);
		SM_I(sbi)->mgc_info = NULL;
		return err;
	}
#endif

	setup_reverse_queue(sbi);

	return err;
}

#endif


static int build_sit_info(struct f2fs_sb_info *sbi)
{
	struct f2fs_super_block *raw_super = F2FS_RAW_SUPER(sbi);
	struct sit_info *sit_i;
	unsigned int sit_segs, start;
	char *src_bitmap, *bitmap;
	unsigned int main_bitmap_size, sit_bitmap_size;// bitmap_size, 

	/* allocate memory for SIT information */
	sit_i = f2fs_kzalloc(sbi, sizeof(struct sit_info), GFP_KERNEL);
	if (!sit_i)
		return -ENOMEM;

	SM_I(sbi)->sit_info = sit_i;

	sit_i->sentries =
		f2fs_kvzalloc(sbi, array_size(sizeof(struct seg_entry),
					      MAIN_SEG_SLOTS(sbi)),
			      GFP_KERNEL);
	//sit_i->sentries =
	//	f2fs_kvzalloc(sbi, array_size(sizeof(struct seg_entry),
	//				      MAIN_SEGS(sbi)),
	//		      GFP_KERNEL);
	if (!sit_i->sentries)
		return -ENOMEM;
	
	main_bitmap_size = f2fs_bitmap_size(MAIN_SEG_SLOTS(sbi));
	sit_i->dirty_sentries_bitmap = f2fs_kvzalloc(sbi, main_bitmap_size,
								GFP_KERNEL);
	printk("%s: dirty_sentries_bitmap size: %u", __func__, main_bitmap_size);
	if (!sit_i->dirty_sentries_bitmap)
		return -ENOMEM;

	//main_bitmap_size = f2fs_bitmap_size(MAIN_SEGS(sbi));
	//sit_i->dirty_sentries_bitmap = f2fs_kvzalloc(sbi, main_bitmap_size,
	//							GFP_KERNEL);
	//if (!sit_i->dirty_sentries_bitmap)
	//	return -ENOMEM;

//#ifdef CONFIG_F2FS_CHECK_FS
//	bitmap_size = MAIN_SEGS(sbi) * SIT_VBLOCK_MAP_SIZE * 4;
//#else
//	bitmap_size = MAIN_SEGS(sbi) * SIT_VBLOCK_MAP_SIZE * 3;
//#endif
	//sit_i->bitmap = f2fs_kvzalloc(sbi, bitmap_size, GFP_KERNEL);
	//if (!sit_i->bitmap)
	//	return -ENOMEM;
//
//	bitmap = sit_i->bitmap;
//
//	for (start = 0; start < MAIN_SEGS(sbi); start++) {
//		sit_i->sentries[start].cur_valid_map = bitmap;
//		bitmap += SIT_VBLOCK_MAP_SIZE;
//
//		sit_i->sentries[start].ckpt_valid_map = bitmap;
//		bitmap += SIT_VBLOCK_MAP_SIZE;
//
//#ifdef CONFIG_F2FS_CHECK_FS
//		sit_i->sentries[start].cur_valid_map_mir = bitmap;
//		bitmap += SIT_VBLOCK_MAP_SIZE;
//#endif
//
//		sit_i->sentries[start].discard_map = bitmap;
//		bitmap += SIT_VBLOCK_MAP_SIZE;
//	}

	sit_i->tmp_map = f2fs_kzalloc(sbi, SIT_VBLOCK_MAP_SIZE, GFP_KERNEL);
	if (!sit_i->tmp_map)
		return -ENOMEM;

	//if (__is_large_section(sbi)) {
	//	sit_i->sec_entries =
	//		f2fs_kvzalloc(sbi, array_size(sizeof(struct sec_entry),
	//					      MAIN_SECS(sbi)),
	//			      GFP_KERNEL);
	//	if (!sit_i->sec_entries)
	//		return -ENOMEM;
	//}

	/* get information related with SIT */
	sit_segs = le32_to_cpu(raw_super->segment_count_sit) >> 1;

	/* setup SIT bitmap from ckeckpoint pack */
	sit_bitmap_size = __bitmap_size(sbi, SIT_BITMAP);
	src_bitmap = __bitmap_ptr(sbi, SIT_BITMAP);

	sit_i->sit_bitmap = kmemdup(src_bitmap, sit_bitmap_size, GFP_KERNEL);
	if (!sit_i->sit_bitmap)
		return -ENOMEM;

#ifdef CONFIG_F2FS_CHECK_FS
	sit_i->sit_bitmap_mir = kmemdup(src_bitmap,
					sit_bitmap_size, GFP_KERNEL);
	if (!sit_i->sit_bitmap_mir)
		return -ENOMEM;

	sit_i->invalid_segmap = f2fs_kvzalloc(sbi,
					main_bitmap_size, GFP_KERNEL);
	if (!sit_i->invalid_segmap)
		return -ENOMEM;
#endif

	/* init SIT information */
	//sit_i->s_ops = &default_salloc_ops;
	sit_i->s_ops = &IFLBA_salloc_ops;

	sit_i->sit_base_addr = le32_to_cpu(raw_super->sit_blkaddr);
	sit_i->sit_blocks = sit_segs << sbi->log_blocks_per_seg;
	sit_i->written_valid_blocks = 0;
	sit_i->bitmap_size = sit_bitmap_size;
	sit_i->dirty_sentries = 0;
	sit_i->sents_per_block = SIT_ENTRY_PER_BLOCK;
	sit_i->elapsed_time = le64_to_cpu(sbi->ckpt->elapsed_time);
	sit_i->mounted_time = ktime_get_boottime_seconds();
	init_rwsem(&sit_i->sentry_lock);
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

	//bitmap_size = f2fs_bitmap_size(MAIN_SEGS(sbi));
	bitmap_size = f2fs_bitmap_size(MAIN_SEGS_INTERVAL(sbi));
	free_i->free_segmap = f2fs_kvmalloc(sbi, bitmap_size, GFP_KERNEL);
	if (!free_i->free_segmap)
		return -ENOMEM;
	
	free_i->free_segmap_node = f2fs_kvmalloc(sbi, bitmap_size, GFP_KERNEL);
	if (!free_i->free_segmap_node)
		return -ENOMEM;
	
	printk("%s: MAIN_SEGS: %u MAIN_SECS: %u MAIN_SEGS_INTERVAL: %u MAIN_SECS_INTERVAL: %u segs_per_sec: %u",
		   __func__, 
		   MAIN_SEGS(sbi), MAIN_SECS(sbi),  
		   MAIN_SEGS_INTERVAL(sbi), MAIN_SECS_INTERVAL(sbi), sbi->segs_per_sec);

	sec_bitmap_size = f2fs_bitmap_size(MAIN_SECS_INTERVAL(sbi));
	free_i->free_secmap = f2fs_kvmalloc(sbi, sec_bitmap_size, GFP_KERNEL);
	if (!free_i->free_secmap)
		return -ENOMEM;
	
	free_i->free_secmap_node = f2fs_kvmalloc(sbi, sec_bitmap_size, GFP_KERNEL);
	if (!free_i->free_secmap_node)
		return -ENOMEM;

	/* set all segments as dirty temporarily */
	memset(free_i->free_segmap, 0xff, bitmap_size);
	memset(free_i->free_secmap, 0xff, sec_bitmap_size);
	
	memset(free_i->free_segmap_node, 0xff, bitmap_size);
	memset(free_i->free_secmap_node, 0xff, sec_bitmap_size);

	/* init free segmap information */
	free_i->start_segno = GET_SEGNO_FROM_SEG0(sbi, MAIN_BLKADDR(sbi));
	free_i->free_segments = 0;
	free_i->free_sections = 0;
	
	free_i->free_segments_node = 0;
	free_i->free_sections_node = 0;

	free_i->total_inuse_sections = 0;

	spin_lock_init(&free_i->segmap_lock);
	return 0;
}

//static inline void JW_set_curseg_in_single_superzone(struct f2fs_sb_info *sbi)
//{
//	int i;
//	unsigned int zone = 0,superzone, segno, secno, first_zone;
//	struct summary_footer *sum_footer;
//	unsigned short seg_type;
//	struct curseg_info *curseg;
//	//initial case
//	//zone = GET_ZONE_FROM_SEG(sbi, GET_SEGNO(sbi, MAIN_BLKADDR(sbi)));
//	//superzone = GET_SUPERZONE_FROM_ZONE(sbi, zone);
//	//printk("%s: start zone %d!!!!!", __func__, superzone);
//	down_read(&SM_I(sbi)->curseg_zone_lock);
//        
//	//superzone = 1;
//	//zone = superzone * ZONES_PER_SUPERZONE;
//	//first_zone = GET_ZONE_FROM_SEG(sbi, &SM_I(sbi)->start_segno);
//	first_zone = GET_ZONE_FROM_SEG(sbi, GET_SEGNO(sbi, MAIN_BLKADDR(sbi)));
//	zone = first_zone;
//	//secno = zone * sbi->secs_per_zone;
//	//segno = secno * sbi->segs_per_sec;
//	int n_curseg = (NR_CURSEG_TYPE > 6)? 6: NR_CURSEG_TYPE;
//   	for (seg_type = 0; seg_type < n_curseg; seg_type++){
//		zone += 1; 
//                curseg = (struct curseg_info *)(SM_I(sbi)->curseg_array + seg_type);
//		curseg->zone = zone;
//		secno = curseg->zone * sbi->secs_per_zone;
//		curseg->segno = secno * sbi->segs_per_sec;
//
//		//curseg->next_segno = segno + seg_type;
//        
//		curseg->inited = true;
//		//curseg->segno = curseg->next_segno;
//
//		//curseg->zone = GET_ZONE_FROM_SEG(sbi, curseg->segno);
//		printk("%s: type %d  superzone: %d zone: %d curseg_zone: %d segno: %d !!!!!", __func__, seg_type, superzone, zone, curseg->zone, secno, curseg->segno);
//		curseg->next_blkoff = 0;
//		curseg->next_segno = NULL_SEGNO;
//		
//		sum_footer = &(curseg->sum_blk->footer);
//		memset(sum_footer, 0, sizeof(struct summary_footer));
//		
//		sanity_check_seg_type(sbi, seg_type);
//		
//		if (IS_DATASEG(seg_type))
//		        SET_SUM_TYPE(sum_footer, SUM_TYPE_DATA);
//		if (IS_NODESEG(seg_type))
//		        SET_SUM_TYPE(sum_footer, SUM_TYPE_NODE);
//	}
//	
//        
//	
//	up_read(&SM_I(sbi)->curseg_zone_lock);
//	//printk("type: %d, startblkaddr: 0x%x, segno: %d", type, START_BLOCK(sbi, curseg->segno), curseg->segno);
//	//return (struct curseg_info *) (SM_I(sbi)->curseg_array + type);
//	return;
//}

static inline void D2FS_set_segment_two_partition(struct f2fs_sb_info *sbi)
{
	struct sit_info *sit_i = SIT_I(sbi);
	struct free_segmap_info *free_i = FREE_I(sbi);
	unsigned int superzone, segno, secno, start_blkaddr;
	struct summary_footer *sum_footer;
	unsigned short type, start_type;
	struct curseg_info *curseg;
#ifdef IPLFS_CALLBACK_IO
	struct seg_entry *se;
#endif	

	//printk("%s: start ", __func__);
	//printk("[JWDBG] %s: seg0 blkaddr: 0x%x blocks Per seg: %d",
	//	 __func__, SEG0_BLKADDR(sbi), sbi->blocks_per_seg);
	int nr_curseg_type = 3; 
	start_type = CURSEG_HOT_DATA;
AGAIN:	
	down_read(&SM_I(sbi)->curseg_zone_lock);
	for (type = start_type; type < start_type + nr_curseg_type; type++) {
		
		curseg = (struct curseg_info *)(SM_I(sbi)->curseg_array + type);
		start_blkaddr = (start_type + 1) * BLKS_PER_SUPERZONE 
			+ SEG0_BLKADDR(sbi) % sbi->blocks_per_seg + (type - start_type) * sbi->segs_per_sec * sbi->blocks_per_seg;
		//start_blkaddr = (type+1) * BLKS_PER_SUPERZONE + SEG0_BLKADDR(sbi) % sbi->blocks_per_seg;
		
		segno = GET_SEGNO(sbi, start_blkaddr);
		if (start_blkaddr != START_BLOCK(sbi, segno)){
			//printk("[JWDBG] %s: start blk does not match. start_blkaddr: 0x%x START_BLOCK: 0x%x", __func__, start_blkaddr, START_BLOCK(sbi, segno));
			f2fs_bug_on(sbi, 1);
		}
		
		curseg->next_segno = segno;
		curseg->inited = true;
        curseg->segno = curseg->next_segno;
		curseg->zone = GET_ZONE_FROM_SEG(sbi, curseg->segno);

		if (start_type == CURSEG_HOT_DATA){	
			//printk("%s: CURSEG_HOT_DATA: START_SEGNO_INTERVAL: %u segno: %u", __func__, sbi->START_SEGNO_INTERVAL,
			//		curseg->segno);
			f2fs_bug_on(sbi, test_bit((curseg->segno - sbi->START_SEGNO_INTERVAL), free_i->free_segmap));
		}
		else if (start_type == CURSEG_HOT_NODE) {
			//printk("%s: CURSEG_HOT_NODE: START_SEGNO_INTERVAL_NODE: %u segno: %u", __func__, sbi->START_SEGNO_INTERVAL_NODE,
			//		curseg->segno);
			//sbi->START_SEGNO_INTERVAL_NODE = curseg->segno;
			f2fs_bug_on(sbi, test_bit((curseg->segno - sbi->START_SEGNO_INTERVAL_NODE), free_i->free_segmap_node));
		}
		//__set_inuse(sbi, curseg->segno);
		
		secno = GET_SEC_FROM_SEG(sbi, segno);
		superzone = GET_SUPERZONE_FROM_ZONE(sbi, curseg->zone);
		
		//printk("%s: type %d  superzone: %d curseg_zone: %d secno: %d segno: %d !!!!!", __func__, type, superzone, curseg->zone, secno, segno);
		
		curseg->next_blkoff = 0;
		curseg->next_segno = NULL_SEGNO;
		
		sum_footer = &(curseg->sum_blk->footer);
		memset(sum_footer, 0, sizeof(struct summary_footer));
		
		sanity_check_seg_type(sbi, type);
#ifdef IPLFS_CALLBACK_IO
		curseg->slot_idx = get_new_slot(sbi, curseg->segno);
		
		se = get_seg_entry(sbi, curseg->slot_idx);
		se->segno = curseg->segno;
		
		__set_sit_entry_type(sbi, type, curseg->slot_idx, 1);
#endif
		if (IS_DATASEG(type))
			SET_SUM_TYPE(sum_footer, SUM_TYPE_DATA);
		
		if (IS_NODESEG(type))
		    SET_SUM_TYPE(sum_footer, SUM_TYPE_NODE);
		
		//printk("type: %d, start_blkaddr: 0x%x startblkaddr: 0x%x, segno: %d", type, start_blkaddr, START_BLOCK(sbi, curseg->segno), curseg->segno);
	}
	up_read(&SM_I(sbi)->curseg_zone_lock);

    for (type = start_type; type <= start_type + nr_curseg_type; type++) {
            struct curseg_info *curseg_t = CURSEG_I(sbi, type);
#ifdef SINGLE_INTERVAL
			if (start_type == CURSEG_HOT_DATA) 
				__set_test_and_inuse(sbi, curseg_t->segno - sbi->START_SEGNO_INTERVAL);
			else if (start_type == CURSEG_HOT_NODE)
				__set_test_and_inuse_node(sbi, curseg_t->segno - sbi->START_SEGNO_INTERVAL_NODE);
#else
			__set_test_and_inuse(sbi, curseg_t->segno);
#endif
	}
	if (start_type == CURSEG_HOT_DATA) {
		start_type = CURSEG_HOT_NODE;
		goto AGAIN;
	}
	//printk("%s: end ", __func__);
	return;
}

static inline void D2FS_set_segment(struct f2fs_sb_info *sbi)
{
	struct sit_info *sit_i = SIT_I(sbi);
	struct free_segmap_info *free_i = FREE_I(sbi);
	unsigned int superzone, segno, secno, start_blkaddr;
	struct summary_footer *sum_footer;
	unsigned short type;
	struct curseg_info *curseg;
#ifdef IPLFS_CALLBACK_IO
	struct seg_entry *se;
#endif	

	down_read(&SM_I(sbi)->curseg_zone_lock);
	printk("[JWDBG] %s: seg0 blkaddr: 0x%x blocks Per seg: %d",
		 __func__, SEG0_BLKADDR(sbi), sbi->blocks_per_seg);
	int nr_curseg_type = 6; 
	
	for (type = CURSEG_HOT_DATA; type < CURSEG_HOT_DATA + nr_curseg_type; type++) {
		
		curseg = (struct curseg_info *)(SM_I(sbi)->curseg_array + type);
		start_blkaddr = (CURSEG_HOT_DATA + 1) * BLKS_PER_SUPERZONE 
			+ SEG0_BLKADDR(sbi) % sbi->blocks_per_seg + type * sbi->segs_per_sec * sbi->blocks_per_seg;
		//start_blkaddr = (type+1) * BLKS_PER_SUPERZONE + SEG0_BLKADDR(sbi) % sbi->blocks_per_seg;
		
		segno = GET_SEGNO(sbi, start_blkaddr);
		if (start_blkaddr != START_BLOCK(sbi, segno)){
			printk("[JWDBG] %s: start blk does not match. start_blkaddr: 0x%x START_BLOCK: 0x%x", __func__, start_blkaddr, START_BLOCK(sbi, segno));
			f2fs_bug_on(sbi, 1);
		}
		
		curseg->next_segno = segno;
		curseg->inited = true;
        curseg->segno = curseg->next_segno;
		curseg->zone = GET_ZONE_FROM_SEG(sbi, curseg->segno);
	
		f2fs_bug_on(sbi, test_bit((curseg->segno - sbi->START_SEGNO_INTERVAL), free_i->free_segmap));
		//__set_inuse(sbi, curseg->segno);
		
		secno = GET_SEC_FROM_SEG(sbi, segno);
		superzone = GET_SUPERZONE_FROM_ZONE(sbi, curseg->zone);
		
		printk("%s: type %d  superzone: %d curseg_zone: %d secno: %d segno: %d !!!!!", __func__, type, superzone, curseg->zone, secno, segno);
		
		curseg->next_blkoff = 0;
		curseg->next_segno = NULL_SEGNO;
		
		sum_footer = &(curseg->sum_blk->footer);
		memset(sum_footer, 0, sizeof(struct summary_footer));
		
		sanity_check_seg_type(sbi, type);
#ifdef IPLFS_CALLBACK_IO
		curseg->slot_idx = get_new_slot(sbi, curseg->segno);
		
		se = get_seg_entry(sbi, curseg->slot_idx);
		se->segno = curseg->segno;
		
		__set_sit_entry_type(sbi, type, curseg->slot_idx, 1);
#endif
		if (IS_DATASEG(type))
			SET_SUM_TYPE(sum_footer, SUM_TYPE_DATA);
		
		if (IS_NODESEG(type))
		    SET_SUM_TYPE(sum_footer, SUM_TYPE_NODE);
		
		printk("type: %d, start_blkaddr: 0x%x startblkaddr: 0x%x, segno: %d", type, start_blkaddr, START_BLOCK(sbi, curseg->segno), curseg->segno);
	}
	up_read(&SM_I(sbi)->curseg_zone_lock);

    for (type = CURSEG_HOT_DATA; type <= CURSEG_COLD_NODE; type++) {
            struct curseg_info *curseg_t = CURSEG_I(sbi, type);
#ifdef SINGLE_INTERVAL
			__set_test_and_inuse(sbi, curseg_t->segno - sbi->START_SEGNO_INTERVAL);
#else
			__set_test_and_inuse(sbi, curseg_t->segno);
#endif
	}
	return;
}


#if (SUPERZONE == 1)
static inline void IPLFS_set_zone(struct f2fs_sb_info *sbi)
{
	unsigned int superzone, segno, secno, start_blkaddr;
	struct summary_footer *sum_footer;
	unsigned short type;
	struct curseg_info *curseg;
#ifdef IPLFS_CALLBACK_IO
	struct seg_entry *se;
#endif	

	down_read(&SM_I(sbi)->curseg_zone_lock);
	printk("[JWDBG] %s: seg0 blkaddr: 0x%x blocks Per seg: %d",
		 __func__, SEG0_BLKADDR(sbi), sbi->blocks_per_seg);
	int nr_curseg_type = 6; 
	
	for (type = CURSEG_HOT_DATA; type < CURSEG_HOT_DATA + nr_curseg_type; type++) {
		
		curseg = (struct curseg_info *)(SM_I(sbi)->curseg_array + type);
		start_blkaddr = (type + 1) * BLKS_PER_SUPERZONE + SEG0_BLKADDR(sbi) % sbi->blocks_per_seg;
		//start_blkaddr = (type+1) * BLKS_PER_SUPERZONE + SEG0_BLKADDR(sbi) % sbi->blocks_per_seg;
		
		segno = GET_SEGNO(sbi, start_blkaddr);
		if (start_blkaddr != START_BLOCK(sbi, segno)){
			printk("[JWDBG] %s: start blk does not match. start_blkaddr: 0x%x START_BLOCK: 0x%x", __func__, start_blkaddr, START_BLOCK(sbi, segno));
			f2fs_bug_on(sbi, 1);
		}
		
		curseg->next_segno = segno;
		curseg->inited = true;
        curseg->segno = curseg->next_segno;
		curseg->zone = GET_ZONE_FROM_SEG(sbi, curseg->segno);
		
		secno = GET_SEC_FROM_SEG(sbi, segno);
		superzone = GET_SUPERZONE_FROM_ZONE(sbi, curseg->zone);
		
		printk("%s: type %d  superzone: %d curseg_zone: %d secno: %d segno: %d !!!!!", __func__, type, superzone, curseg->zone, secno, segno);
		
		curseg->next_blkoff = 0;
		curseg->next_segno = NULL_SEGNO;
		
		sum_footer = &(curseg->sum_blk->footer);
		memset(sum_footer, 0, sizeof(struct summary_footer));
		
		sanity_check_seg_type(sbi, type);
#ifdef IPLFS_CALLBACK_IO
		curseg->slot_idx = get_new_slot(sbi, curseg->segno);
		
		se = get_seg_entry(sbi, curseg->slot_idx);
		se->segno = curseg->segno;
		
		__set_sit_entry_type(sbi, type, curseg->slot_idx, 1);
#endif
		if (IS_DATASEG(type))
			SET_SUM_TYPE(sum_footer, SUM_TYPE_DATA);
		
		if (IS_NODESEG(type))
		    SET_SUM_TYPE(sum_footer, SUM_TYPE_NODE);
		
		printk("type: %d, start_blkaddr: 0x%x startblkaddr: 0x%x, segno: %d", type, start_blkaddr, START_BLOCK(sbi, curseg->segno), curseg->segno);
	}
	up_read(&SM_I(sbi)->curseg_zone_lock);
	return;
}
#endif

static int build_curseg(struct f2fs_sb_info *sbi)               
{                                                               
       struct curseg_info *array;                               
       int i;                                                   
                                                                
       array = f2fs_kzalloc(sbi, array_size(NR_CURSEG_TYPE,     
                                       sizeof(*array)), GFP_KERNEL);
       if (!array)                                              
               return -ENOMEM;                                  

       SM_I(sbi)->curseg_array = array;

       for (i = 0; i < NO_CHECK_TYPE; i++) {
               mutex_init(&array[i].curseg_mutex);
               array[i].sum_blk = f2fs_kzalloc(sbi, PAGE_SIZE, GFP_KERNEL);
               if (!array[i].sum_blk)
                       return -ENOMEM;
               init_rwsem(&array[i].journal_rwsem);
               array[i].journal = f2fs_kzalloc(sbi,
                               sizeof(struct f2fs_journal), GFP_KERNEL);
               if (!array[i].journal)
                       return -ENOMEM;
               if (i < NR_PERSISTENT_LOG)
                       array[i].seg_type = CURSEG_HOT_DATA + i;
               else if (i == CURSEG_COLD_DATA_PINNED)
                       array[i].seg_type = CURSEG_COLD_DATA;
               else if (i == CURSEG_ALL_DATA_ATGC)
                       array[i].seg_type = CURSEG_COLD_DATA;
               array[i].segno = NULL_SEGNO;
               array[i].next_blkoff = 0;
               array[i].inited = false;
#ifdef IPLFS_CALLBACK_IO
               array[i].slot_idx = NULL_SLOTNO;
#endif
       }
       return restore_curseg_summaries(sbi);
}

static int build_sit_entries(struct f2fs_sb_info *sbi)
{
       struct sit_info *sit_i = SIT_I(sbi);
       struct curseg_info *curseg = CURSEG_I(sbi, CURSEG_COLD_DATA);
       struct f2fs_journal *journal = curseg->journal;
       struct seg_entry *se;
       struct f2fs_sit_entry sit;
       //int sit_blk_cnt = SIT_BLK_CNT(sbi);
       int slot_blk_cnt = SLOT_BLK_CNT(sbi);
       unsigned int i, start, end;
       unsigned int readed, start_blk = 0;
       int err = 0;
       block_t total_node_blocks = 0;
	   int pcnt = 0;

	   //printk("%s: MAIN_SEG_SLOTS: %d %u", __func__, MAIN_SEG_SLOTS(sbi), MAIN_SEG_SLOTS(sbi));
       do {
               readed = f2fs_ra_meta_pages(sbi, start_blk, BIO_MAX_PAGES,
                                                       META_SIT, true);
			   if (readed == 0 && pcnt < 5) {
				   printk("%s: start_blk: %u MAIN_SEG_SLOTS: %u slot_blk_cnt: %u", 
						   __func__, start_blk, MAIN_SEG_SLOTS(sbi), 
						   slot_blk_cnt);
				   pcnt ++;
			   }
               start = start_blk * sit_i->sents_per_block;
               end = (start_blk + readed) * sit_i->sents_per_block;

               //for (; start < end && start < MAIN_SEGS(sbi); start++) {
               for (; start < end && start < MAIN_SEG_SLOTS(sbi); start++) {
                       struct f2fs_sit_block *sit_blk;
                       struct page *page;

                       se = &sit_i->sentries[start];
                       page = get_current_sit_page(sbi, start);
                       if (IS_ERR(page))
                               return PTR_ERR(page);
                       sit_blk = (struct f2fs_sit_block *)page_address(page);
                       sit = sit_blk->entries[SIT_ENTRY_OFFSET(sit_i, start)];
                       f2fs_put_page(page, 1);

                       //err = check_block_count(sbi, start, &sit);
                       //if (err)
                       //        return err;
                       seg_info_from_raw_sit(se, &sit);
                       if (IS_NODESEG(se->type))
                               total_node_blocks += se->valid_blocks;

                       /* build discard map only one time */
                       if (is_set_ckpt_flags(sbi, CP_TRIMMED_FLAG)) {
                               //memset(se->discard_map, 0xff,
                               //        SIT_VBLOCK_MAP_SIZE);
                       } else {
                              // memcpy(se->discard_map,
                              //         se->cur_valid_map,
                              //         SIT_VBLOCK_MAP_SIZE);
                               sbi->discard_blks +=
                                       sbi->blocks_per_seg -
                                       se->valid_blocks;
                       }

                       //if (__is_large_section(sbi))
                       //        get_sec_entry(sbi, start)->valid_blocks +=
                       //                                se->valid_blocks;
               }
               start_blk += readed;
       } while (start_blk < slot_blk_cnt);

	   down_read(&curseg->journal_rwsem);
       for (i = 0; i < sits_in_cursum(journal); i++) {
               unsigned int old_valid_blocks;

#ifdef IPLFS_CALLBACK_IO
			   uint64_t slot_idx, segno, tmp;
			   if (i == 0 || i == 3) {
				   slot_idx = (uint64_t) le32_to_cpu(segno_in_journal(journal, i));
				   if (slot_idx >= MAIN_SEG_SLOTS(sbi)) {
					   f2fs_err(sbi, "Wrong journal entry on segno %u",
						   slot_idx);
					   err = -EFSCORRUPTED;
					   break;
				   }
				   
				   sit = sit_in_journal(journal, i);
				   
				   //printk("%s: i: %u segno: %llu se segno: %llu vblock: %u", __func__, 
					//	   i, slot_idx, GET_SIT_SEGNO(&sit), GET_SIT_VBLOCKS(&sit));
				   
				   segno = GET_SIT_SEGNO(&sit);

				   if (GET_SIT_VBLOCKS(&sit) > 0) { 
					   //tmp = select_new_slot(sbi, segno, slot_idx);
					   //f2fs_bug_on(sbi, tmp != slot_idx);
					   
					   se = &sit_i->sentries[slot_idx];
					   
					   old_valid_blocks = se->valid_blocks;
					   
					   if (IS_NODESEG(se->type))
						   total_node_blocks -= old_valid_blocks;
					   
					   seg_info_from_raw_sit(se, &sit);
					   
					   if (IS_NODESEG(se->type))
						   total_node_blocks += se->valid_blocks;

               		   if (is_set_ckpt_flags(sbi, CP_TRIMMED_FLAG)) {
               		           //memset(se->discard_map, 0xff, SIT_VBLOCK_MAP_SIZE);
               		   } else {
               		           //memcpy(se->discard_map, se->cur_valid_map,
               		           //                        SIT_VBLOCK_MAP_SIZE);
               		           sbi->discard_blks += old_valid_blocks;
               		           sbi->discard_blks -= se->valid_blocks;
               		   }
				   }

				//   printk("%s: i: %u vblock: %u, segno: %llu, slot idx: %llu", __func__, 
				//		   i, se->valid_blocks, se->segno, slot_idx);

				   continue;

			   }
#endif
               start = le32_to_cpu(segno_in_journal(journal, i));
			   
			   if (start >= MAIN_SEGS(sbi)) {
               //if (start >= MAIN_SEG_SLOTS(sbi)) {
                       f2fs_err(sbi, "Wrong journal entry on segno %u",
                                start);
                       err = -EFSCORRUPTED;
                       break;
               }

               se = &sit_i->sentries[start];
               sit = sit_in_journal(journal, i);

               old_valid_blocks = se->valid_blocks;
               if (IS_NODESEG(se->type))
                       total_node_blocks -= old_valid_blocks;

               //err = check_block_count(sbi, start, &sit);
               //if (err)
               //        break;
               seg_info_from_raw_sit(se, &sit);
               if (IS_NODESEG(se->type))
                       total_node_blocks += se->valid_blocks;

               if (is_set_ckpt_flags(sbi, CP_TRIMMED_FLAG)) {
                       //memset(se->discard_map, 0xff, SIT_VBLOCK_MAP_SIZE);
               } else {
                       //memcpy(se->discard_map, se->cur_valid_map,
                       //                        SIT_VBLOCK_MAP_SIZE);
                       sbi->discard_blks += old_valid_blocks;
                       sbi->discard_blks -= se->valid_blocks;
               }

               //if (__is_large_section(sbi)) {
               //        get_sec_entry(sbi, start)->valid_blocks +=
               //                                        se->valid_blocks;
               //        get_sec_entry(sbi, start)->valid_blocks -=
               //                                        old_valid_blocks;
               //}
       }
       up_read(&curseg->journal_rwsem);

       if (!err && total_node_blocks != valid_node_count(sbi)) {
               f2fs_err(sbi, "SIT is corrupted node# %u vs %u",
                        total_node_blocks, valid_node_count(sbi));
               err = -EFSCORRUPTED;
       }

       return err;
}

static int build_discard_cnt_info(struct f2fs_sb_info *sbi)
{
	struct discard_cnt_info *dc_i;
	dc_i = f2fs_kzalloc(sbi, sizeof(struct discard_cnt_info), GFP_KERNEL);
	if (!dc_i)
		return -ENOMEM;
	
	dc_i->total_dce_cnt = 0;
	dc_i->last_total_dce_cnt = 0;
    
	spin_lock_init(&dc_i->lock);
	
	SM_I(sbi)->dcnt_info = dc_i;

	return 0;
}

static int build_slot_info(struct f2fs_sb_info *sbi)
{
	struct slot_info *slot_i;
	slot_i = f2fs_kzalloc(sbi, sizeof(struct slot_info), GFP_KERNEL);
	if (!slot_i)
		return -ENOMEM;
	
	slot_i->slot_entries = f2fs_kvzalloc(sbi, array_size(sizeof(struct slot_entry),
					      MAIN_SEG_SLOTS(sbi)), GFP_KERNEL);
	if (!(slot_i->slot_entries)) {
	   printk("%s: slot_entries create fail", __func__);
	   f2fs_bug_on(sbi, 1);
		return -ENOMEM;
	}


	slot_i->total_slot_cnt = 0;
	atomic_set(&slot_i->free_slot_cnt, 0);
	atomic_set(&slot_i->prefree_slot_cnt, 0);
	atomic_set(&slot_i->inuse_slot_cnt, 0);
	
	INIT_LIST_HEAD(&slot_i->free_list);
	INIT_LIST_HEAD(&slot_i->prefree_list);
	
	INIT_LIST_HEAD(&slot_i->prefree_candidate_list);
    
	mutex_init(&slot_i->lock);
	
	SM_I(sbi)->slot_info = slot_i;

	return 0;
}

static int build_slot_entries(struct f2fs_sb_info *sbi)
{
   	struct sit_info *sit_i = SIT_I(sbi);
    struct slot_info *slot_i = SLT_I(sbi);
    struct seg_entry *se;
    unsigned int slot_idx;
    int err = 0;
	struct slot_entry *slte;

	for (slot_idx = 0; slot_idx < MAIN_SEG_SLOTS(sbi); slot_idx ++ ) {
		
		slte = &slot_i->slot_entries[slot_idx];
	    se = &sit_i->sentries[slot_idx];
	   
	    init_slot_entry(slot_i, slte, se->segno, slot_idx, se->valid_blocks);

	    if (se->valid_blocks > 0) {
	 	   printk("%s: valid seg entry exists. vblk: %u segno: %llu", 
	 			   __func__, se->valid_blocks, se->segno);
		   if (slte->segno == NULL_SEGNO || lookup_slot_hash(se->segno)) {
			   f2fs_bug_on(sbi, slte->segno == NULL_SEGNO);
			   f2fs_bug_on(sbi, lookup_slot_hash(se->segno));
			   err = -EFSCORRUPTED;
			   return err;
		   }
	 	   
	 	   set_slot_inuse(slot_i, slte);
	    
	    } else {
			if (slte->segno != NULL_SEGNO) {
				printk("%s: valid seg entry exists. vblk: %u segno: %llu", 
	 			   __func__, se->valid_blocks, se->segno);
				f2fs_bug_on(sbi, slte->segno != NULL_SEGNO);
			   err = -EFSCORRUPTED;
			   return err;
		   }
	 	   set_slot_free(slot_i, slte);
	    }
	}
	return err;
}

static void init_free_segmap(struct f2fs_sb_info *sbi)
{
       unsigned int start;
       int type;
       struct seg_entry *sentry;
       
	   for (start = 0; start < MAIN_SEGS_INTERVAL(sbi); start++) {
		   __set_free(sbi, start);
		   __set_free_node(sbi, start);
       }

       for (start = 0; start < MAIN_SEG_SLOTS(sbi); start++) {
               if (f2fs_usable_blks_in_seg(sbi, start) == 0)
                       continue;
               sentry = get_seg_entry(sbi, start);
               if (sentry->valid_blocks){
				   if (!IS_MIGRATION_SEGNO(sbi, sentry->segno)){
#ifdef SINGLE_INTERVAL
					   if (sentry->segno >= sbi->START_SEGNO_INTERVAL_NODE){
						   __set_inuse_node(sbi, sentry->segno - sbi->START_SEGNO_INTERVAL_NODE);
					   } else if (sentry->segno >= sbi->START_SEGNO_INTERVAL){
						   __set_inuse(sbi, sentry->segno - sbi->START_SEGNO_INTERVAL);
					   } else {
						//   printk("%s: !!!!!!!!! segno: %lu slot idx: %lu", __func__, 
						//	   sentry->segno, start);
					   }
#else 
					   __set_inuse(sbi, sentry->segno);
#endif
				   }
			   }
               else
				   SIT_I(sbi)->written_valid_blocks +=
                                               sentry->valid_blocks;
       }

       /* set use the current segments */
       //for (type = CURSEG_HOT_DATA; type <= CURSEG_COLD_NODE; type++) {
       //        struct curseg_info *curseg_t = CURSEG_I(sbi, type);
       //        __set_test_and_inuse(sbi, curseg_t->segno);
       //}
}

//static void init_free_segmap(struct f2fs_sb_info *sbi)
//{
//       unsigned int start;
//       int type;
//       struct seg_entry *sentry;
//
//       for (start = 0; start < MAIN_SEGS(sbi); start++) {
//               if (f2fs_usable_blks_in_seg(sbi, start) == 0)
//                       continue;
//               sentry = get_seg_entry(sbi, start);
//               if (!sentry->valid_blocks)
//                       __set_free(sbi, start);
//               else
//                       SIT_I(sbi)->written_valid_blocks +=
//                                               sentry->valid_blocks;
//       }
//
//       /* set use the current segments */
//       for (type = CURSEG_HOT_DATA; type <= CURSEG_COLD_NODE; type++) {
//               struct curseg_info *curseg_t = CURSEG_I(sbi, type);
//               __set_test_and_inuse(sbi, curseg_t->segno);
//       }
//}

static void init_dirty_segmap(struct f2fs_sb_info *sbi)
{
       struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	   struct slot_info *slot_i = SLT_I(sbi);
       struct free_segmap_info *free_i = FREE_I(sbi);
       unsigned int segno = 0, offset = 0, secno, slot_idx;
       block_t valid_blocks, usable_blks_in_seg;
       block_t blks_per_sec = BLKS_PER_SEC(sbi);
	   struct slot_entry *slte;

	   bool node_partition = false;
	   printk("%s: start ", __func__);
AGAIN:
       segno = 0, offset = 0;

	   while (1) {
               /* find dirty segment based on free segmap */
			   if (node_partition)
				   segno = find_next_inuse_node(free_i, MAIN_SEGS_INTERVAL(sbi), offset);
			   else
				   segno = find_next_inuse(free_i, MAIN_SEGS_INTERVAL(sbi), offset);

			   if (segno >= MAIN_SEGS_INTERVAL(sbi))
                       break;
               offset = segno + 1;
			   
#ifdef SINGLE_INTERVAL
			   if (node_partition)
				   segno += sbi->START_SEGNO_INTERVAL_NODE;
			   else
				   segno += sbi->START_SEGNO_INTERVAL;
#endif
			   mutex_lock(&slot_i->lock);
			   
			   /* translate segno into slot index */
			   /* get slot index of segno from hash table */
	   		   slte = lookup_slot_hash(segno);
			   if (slte == NULL) {
				   printk("%s: blkaddr: 0x%lx segno: %u", __func__,
						   START_BLOCK(sbi, segno), segno);
				   unsigned int tmp_ii;
			
				   for (tmp_ii = 0; tmp_ii < MAIN_SEG_SLOTS(sbi); tmp_ii ++) {
					   slte = get_slot_entry(sbi, tmp_ii);
					   printk("%s: slot idx: %u slte slot_idx %lu segno: %lu", __func__, tmp_ii, 
						slte->slot_idx, slte->segno);
				   }
				   f2fs_bug_on(sbi, 1);
			   }
			   slot_idx = slte->slot_idx;
			   mutex_unlock(&slot_i->lock);


               valid_blocks = get_valid_blocks(sbi, slot_idx, false);
               usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
               if (valid_blocks == usable_blks_in_seg || !valid_blocks)
                       continue;
               if (valid_blocks > usable_blks_in_seg) {
                       f2fs_bug_on(sbi, 1);
                       continue;
               }
               mutex_lock(&dirty_i->seglist_lock);
			   __locate_dirty_segment(sbi, segno, DIRTY);
			   mutex_unlock(&dirty_i->seglist_lock);
       }

	   if (!node_partition) {
		   node_partition = true;
		   goto AGAIN;
	   }
	   
	   printk("%s: end ", __func__);
       //if (!__is_large_section(sbi))
       //        return;

       //mutex_lock(&dirty_i->seglist_lock);
       //for (segno = 0; segno < MAIN_SEGS(sbi); segno += sbi->segs_per_sec) {
       //        valid_blocks = get_valid_blocks(sbi, segno, true);
       //        secno = GET_SEC_FROM_SEG(sbi, segno);

       //        if (!valid_blocks || valid_blocks == blks_per_sec)
       //                continue;
       //        if (IS_CURSEC(sbi, secno))
       //                continue;
       //        set_bit(secno, dirty_i->dirty_secmap);
       //}
       //mutex_unlock(&dirty_i->seglist_lock);
}


//static void init_dirty_segmap(struct f2fs_sb_info *sbi)
//{
//       struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
//       struct free_segmap_info *free_i = FREE_I(sbi);
//       unsigned int segno = 0, offset = 0, secno;
//       block_t valid_blocks, usable_blks_in_seg;
//       block_t blks_per_sec = BLKS_PER_SEC(sbi);
//
//       while (1) {
//               /* find dirty segment based on free segmap */
//               segno = find_next_inuse(free_i, MAIN_SEGS(sbi), offset);
//               if (segno >= MAIN_SEGS(sbi))
//                       break;
//               offset = segno + 1;
//               valid_blocks = get_valid_blocks(sbi, segno, false);
//               usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
//               if (valid_blocks == usable_blks_in_seg || !valid_blocks)
//                       continue;
//               if (valid_blocks > usable_blks_in_seg) {
//                       f2fs_bug_on(sbi, 1);
//                       continue;
//               }
//               mutex_lock(&dirty_i->seglist_lock);
//               //__locate_dirty_segment(sbi, segno, DIRTY);
//               mutex_unlock(&dirty_i->seglist_lock);
//       }
//
//       if (!__is_large_section(sbi))
//               return;
//
//       mutex_lock(&dirty_i->seglist_lock);
//       for (segno = 0; segno < MAIN_SEGS(sbi); segno += sbi->segs_per_sec) {
//               valid_blocks = get_valid_blocks(sbi, segno, true);
//               secno = GET_SEC_FROM_SEG(sbi, segno);
//
//               if (!valid_blocks || valid_blocks == blks_per_sec)
//                       continue;
//               if (IS_CURSEC(sbi, secno))
//                       continue;
//               set_bit(secno, dirty_i->dirty_secmap);
//       }
//       mutex_unlock(&dirty_i->seglist_lock);
//}

//static int init_victim_secmap(struct f2fs_sb_info *sbi)
//{
//       struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
//       unsigned int bitmap_size = f2fs_bitmap_size(MAIN_SECS(sbi));
//
//       dirty_i->victim_secmap = f2fs_kvzalloc(sbi, bitmap_size, GFP_KERNEL);
//       if (!dirty_i->victim_secmap)
//               return -ENOMEM;
//       return 0;
//}

//static int build_dirty_segmap(struct f2fs_sb_info *sbi)
//{
//       struct dirty_seglist_info *dirty_i;
//       unsigned int bitmap_size, i;
//
//       /* allocate memory for dirty segments list information */
//       dirty_i = f2fs_kzalloc(sbi, sizeof(struct dirty_seglist_info),
//                                                               GFP_KERNEL);
//       if (!dirty_i)
//               return -ENOMEM;
//
//       SM_I(sbi)->dirty_info = dirty_i;
//       mutex_init(&dirty_i->seglist_lock);
//
//       bitmap_size = f2fs_bitmap_size(MAIN_SEGS(sbi));
//
//       for (i = 0; i < NR_DIRTY_TYPE; i++) {
//               dirty_i->dirty_segmap[i] = f2fs_kvzalloc(sbi, bitmap_size,
//                                                               GFP_KERNEL);
//               if (!dirty_i->dirty_segmap[i])
//                       return -ENOMEM;
//       }
//
//       if (__is_large_section(sbi)) {
//               bitmap_size = f2fs_bitmap_size(MAIN_SECS(sbi));
//               dirty_i->dirty_secmap = f2fs_kvzalloc(sbi,
//                                               bitmap_size, GFP_KERNEL);
//               if (!dirty_i->dirty_secmap)
//                       return -ENOMEM;
//       }
//
//       init_dirty_segmap(sbi);
//       return init_victim_secmap(sbi);
//}

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
       mutex_init(&dirty_i->seglist_lock);

       bitmap_size = f2fs_bitmap_size(MAIN_SEGS_INTERVAL(sbi));

       for (i = 0; i < NR_DIRTY_TYPE; i++) {
               dirty_i->dirty_segmap[i] = f2fs_kvzalloc(sbi, bitmap_size,
                                                               GFP_KERNEL);
               if (!dirty_i->dirty_segmap[i])
                       return -ENOMEM;
       }

       //if (__is_large_section(sbi)) {
       //        bitmap_size = f2fs_bitmap_size(MAIN_SECS(sbi));
       //        dirty_i->dirty_secmap = f2fs_kvzalloc(sbi,
       //                                        bitmap_size, GFP_KERNEL);
       //        if (!dirty_i->dirty_secmap)
       //                return -ENOMEM;
       //}

       init_dirty_segmap(sbi);
       return 0;

}

//static int sanity_check_curseg(struct f2fs_sb_info *sbi)
//{
//       int i;
//
//       /*
//        * In LFS/SSR curseg, .next_blkoff should point to an unused blkaddr;
//        * In LFS curseg, all blkaddr after .next_blkoff should be unused.
//        */
//       for (i = 0; i < NR_PERSISTENT_LOG; i++) {
//               struct curseg_info *curseg = CURSEG_I(sbi, i);
//               struct seg_entry *se = get_seg_entry(sbi, curseg->segno);
//               unsigned int blkofs = curseg->next_blkoff;
//
//               sanity_check_seg_type(sbi, curseg->seg_type);
//
//               if (f2fs_test_bit(blkofs, se->cur_valid_map))
//                       goto out;
//
//               if (curseg->alloc_type == SSR)
//                       continue;
//
//               for (blkofs += 1; blkofs < sbi->blocks_per_seg; blkofs++) {
//                       if (!f2fs_test_bit(blkofs, se->cur_valid_map))
//                               continue;
//out:
//                       f2fs_err(sbi,
//                                "Current segment's next free block offset is inconsistent with bitmap, logtype:%u, segno:%u, type:%u, next_blkoff:%u, blkofs:%u",
//                                i, curseg->segno, curseg->alloc_type,
//                                curseg->next_blkoff, blkofs);
//                       return -EFSCORRUPTED;
//               }
//       }
//       return 0;
//}

#ifdef CONFIG_BLK_DEV_ZONED

static int check_zone_write_pointer(struct f2fs_sb_info *sbi,
                                   struct f2fs_dev_info *fdev,
                                   struct blk_zone *zone)
{
       unsigned int wp_segno, wp_blkoff, zone_secno, zone_segno, segno;
       block_t zone_block, wp_block, last_valid_block;
       unsigned int log_sectors_per_block = sbi->log_blocksize - SECTOR_SHIFT;
       int i, s, b, ret;
       struct seg_entry *se;

       if (zone->type != BLK_ZONE_TYPE_SEQWRITE_REQ)
               return 0;

       wp_block = fdev->start_blk + (zone->wp >> log_sectors_per_block);
       wp_segno = GET_SEGNO(sbi, wp_block);
       wp_blkoff = wp_block - START_BLOCK(sbi, wp_segno);
       zone_block = fdev->start_blk + (zone->start >> log_sectors_per_block);
       zone_segno = GET_SEGNO(sbi, zone_block);
       zone_secno = GET_SEC_FROM_SEG(sbi, zone_segno);

       if (zone_segno >= MAIN_SEGS(sbi))
               return 0;

       /*
        * Skip check of zones cursegs point to, since
        * fix_curseg_write_pointer() checks them.
        */
       for (i = 0; i < NO_CHECK_TYPE; i++)
               if (zone_secno == GET_SEC_FROM_SEG(sbi,
                                                  CURSEG_I(sbi, i)->segno))
                       return 0;

       /*
        * Get last valid block of the zone.
        */
       last_valid_block = zone_block - 1;
       for (s = sbi->segs_per_sec - 1; s >= 0; s--) {
               segno = zone_segno + s;
               se = get_seg_entry(sbi, segno);
               for (b = sbi->blocks_per_seg - 1; b >= 0; b--)
                       //if (f2fs_test_bit(b, se->cur_valid_map)) {
                       //        last_valid_block = START_BLOCK(sbi, segno) + b;
                       //        break;
                       //}
               if (last_valid_block >= zone_block)
                       break;
       }

       /*
        * If last valid block is beyond the write pointer, report the
        * inconsistency. This inconsistency does not cause write error
        * because the zone will not be selected for write operation until
        * it get discarded. Just report it.
        */

       if (last_valid_block >= wp_block) {
               f2fs_notice(sbi, "Valid block beyond write pointer: "
                           "valid block[0x%x,0x%x] wp[0x%x,0x%x]",
                           GET_SEGNO(sbi, last_valid_block),
                           GET_BLKOFF_FROM_SEG0(sbi, last_valid_block),
                           wp_segno, wp_blkoff);
               return 0;
       }

       /*
        * If there is no valid block in the zone and if write pointer is
        * not at zone start, reset the write pointer.
        */
       if (last_valid_block + 1 == zone_block && zone->wp != zone->start) {
               f2fs_notice(sbi,
                           "Zone without valid block has non-zero write "
                           "pointer. Reset the write pointer: wp[0x%x,0x%x]",
                           wp_segno, wp_blkoff);
               ret = __f2fs_issue_discard_zone(sbi, fdev->bdev, zone_block,
                                       zone->len >> log_sectors_per_block);
               if (ret) {
                       f2fs_err(sbi, "Discard zone failed: %s (errno=%d)",
                                fdev->path, ret);
                       return ret;
               }
       }

       return 0;
}

//static struct f2fs_dev_info *get_target_zoned_dev(struct f2fs_sb_info *sbi,
//                                                 block_t zone_blkaddr)
//{
//       int i;
//
//       for (i = 0; i < sbi->s_ndevs; i++) {
//               if (!bdev_is_zoned(FDEV(i).bdev))
//                       continue;
//               if (sbi->s_ndevs == 1 || (FDEV(i).start_blk <= zone_blkaddr &&
//                               zone_blkaddr <= FDEV(i).end_blk))
//                       return &FDEV(i);
//       }
//
//       return NULL;
//}

//static int report_one_zone_cb(struct blk_zone *zone, unsigned int idx,
//                             void *data) {
//       memcpy(data, zone, sizeof(struct blk_zone));
//       return 0;
//}

//static int fix_curseg_write_pointer(struct f2fs_sb_info *sbi, int type)
//{
//       struct curseg_info *cs = CURSEG_I(sbi, type);
//       struct f2fs_dev_info *zbd;
//       struct blk_zone zone;
//       unsigned int cs_section, wp_segno, wp_blkoff, wp_sector_off;
//       block_t cs_zone_block, wp_block;
//       unsigned int log_sectors_per_block = sbi->log_blocksize - SECTOR_SHIFT;
//       sector_t zone_sector;
//       int err;
//
//       cs_section = GET_SEC_FROM_SEG(sbi, cs->segno);
//       cs_zone_block = START_BLOCK(sbi, GET_SEG_FROM_SEC(sbi, cs_section));
//
//       zbd = get_target_zoned_dev(sbi, cs_zone_block);
//       if (!zbd)
//               return 0;
//
//       /* report zone for the sector the curseg points to */
//       zone_sector = (sector_t)(cs_zone_block - zbd->start_blk)
//               << log_sectors_per_block;
//       err = blkdev_report_zones(zbd->bdev, zone_sector, 1,
//                                 report_one_zone_cb, &zone);
//       if (err != 1) {
//               f2fs_err(sbi, "Report zone failed: %s errno=(%d)",
//                        zbd->path, err);
//               return err;
//       }
//
//       if (zone.type != BLK_ZONE_TYPE_SEQWRITE_REQ)
//               return 0;
//
//       wp_block = zbd->start_blk + (zone.wp >> log_sectors_per_block);
//       wp_segno = GET_SEGNO(sbi, wp_block);
//       wp_blkoff = wp_block - START_BLOCK(sbi, wp_segno);
//       wp_sector_off = zone.wp & GENMASK(log_sectors_per_block - 1, 0);
//
//       if (cs->segno == wp_segno && cs->next_blkoff == wp_blkoff &&
//               wp_sector_off == 0)
//               return 0;
//
//       f2fs_notice(sbi, "Unaligned curseg[%d] with write pointer: "
//                   "curseg[0x%x,0x%x] wp[0x%x,0x%x]",
//                   type, cs->segno, cs->next_blkoff, wp_segno, wp_blkoff);
//
//       f2fs_notice(sbi, "Assign new section to curseg[%d]: "
//                   "curseg[0x%x,0x%x]", type, cs->segno, cs->next_blkoff);
//       allocate_segment_by_default(sbi, type, true);
//
//       /* check consistency of the zone curseg pointed to */
//       if (check_zone_write_pointer(sbi, zbd, &zone))
//               return -EIO;
//
//       /* check newly assigned zone */
//       cs_section = GET_SEC_FROM_SEG(sbi, cs->segno);
//       cs_zone_block = START_BLOCK(sbi, GET_SEG_FROM_SEC(sbi, cs_section));
//
//       zbd = get_target_zoned_dev(sbi, cs_zone_block);
//       if (!zbd)
//               return 0;
//
//       zone_sector = (sector_t)(cs_zone_block - zbd->start_blk)
//               << log_sectors_per_block;
//       err = blkdev_report_zones(zbd->bdev, zone_sector, 1,
//                                 report_one_zone_cb, &zone);
//       if (err != 1) {
//               f2fs_err(sbi, "Report zone failed: %s errno=(%d)",
//                        zbd->path, err);
//               return err;
//       }
//
//       if (zone.type != BLK_ZONE_TYPE_SEQWRITE_REQ)
//               return 0;
//
//       if (zone.wp != zone.start) {
//               f2fs_notice(sbi,
//                           "New zone for curseg[%d] is not yet discarded. "
//                           "Reset the zone: curseg[0x%x,0x%x]",
//                           type, cs->segno, cs->next_blkoff);
//               err = __f2fs_issue_discard_zone(sbi, zbd->bdev,
//                               zone_sector >> log_sectors_per_block,
//                               zone.len >> log_sectors_per_block);
//               if (err) {
//                       f2fs_err(sbi, "Discard zone failed: %s (errno=%d)",
//                                zbd->path, err);
//                       return err;
//               }
//       }
//
//       return 0;
//}

int f2fs_fix_curseg_write_pointer(struct f2fs_sb_info *sbi)
{
       int i;//, ret;
       panic("f2fs_fix_curseg_write_pointer(): not expected!!\n");

       for (i = 0; i < NR_PERSISTENT_LOG; i++) {
               //ret = fix_curseg_write_pointer(sbi, i);
               //if (ret)
               //        return ret;
       }

       return 0;
}

struct check_zone_write_pointer_args {
       struct f2fs_sb_info *sbi;
       struct f2fs_dev_info *fdev;
};

static int check_zone_write_pointer_cb(struct blk_zone *zone, unsigned int idx,
                                     void *data) {
       struct check_zone_write_pointer_args *args;
       args = (struct check_zone_write_pointer_args *)data;

       return check_zone_write_pointer(args->sbi, args->fdev, zone);
}

int f2fs_check_write_pointer(struct f2fs_sb_info *sbi)
{
       int i, ret;
       struct check_zone_write_pointer_args args;
       panic("f2fs_check_write_pointer(): not expected!!\n");
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

static bool is_conv_zone(struct f2fs_sb_info *sbi, unsigned int zone_idx,
                                               unsigned int dev_idx)
{
       if (!bdev_is_zoned(FDEV(dev_idx).bdev))
               return true;
       return !test_bit(zone_idx, FDEV(dev_idx).blkz_seq);
}


/* Return the zone index in the given device */
static unsigned int get_zone_idx(struct f2fs_sb_info *sbi, unsigned int secno,
                                       int dev_idx)
{
       block_t sec_start_blkaddr = START_BLOCK(sbi, GET_SEG_FROM_SEC(sbi, secno));

       return (sec_start_blkaddr - FDEV(dev_idx).start_blk) >>
                                               sbi->log_blocks_per_blkz;
}

/*
 * Return the usable segments in a section based on the zone's
 * corresponding zone capacity. Zone is equal to a section.
 */
static inline unsigned int f2fs_usable_zone_segs_in_sec(
               struct f2fs_sb_info *sbi, unsigned int segno)
{
       unsigned int dev_idx, zone_idx, unusable_segs_in_sec;

       dev_idx = f2fs_target_device_index(sbi, START_BLOCK(sbi, segno));
       zone_idx = get_zone_idx(sbi, GET_SEC_FROM_SEG(sbi, segno), dev_idx);

       /* Conventional zone's capacity is always equal to zone size */
       if (is_conv_zone(sbi, zone_idx, dev_idx))
               return sbi->segs_per_sec;

       /*
        * If the zone_capacity_blocks array is NULL, then zone capacity
        * is equal to the zone size for all zones
        */
       if (!FDEV(dev_idx).zone_capacity_blocks)
               return sbi->segs_per_sec;

       /* Get the segment count beyond zone capacity block */
       unusable_segs_in_sec = (sbi->blocks_per_blkz -
                               FDEV(dev_idx).zone_capacity_blocks[zone_idx]) >>
                               sbi->log_blocks_per_seg;
       return sbi->segs_per_sec - unusable_segs_in_sec;
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
       unsigned int zone_idx, dev_idx, secno;

       secno = GET_SEC_FROM_SEG(sbi, segno);
       seg_start = START_BLOCK(sbi, segno);
       dev_idx = f2fs_target_device_index(sbi, seg_start);
       zone_idx = get_zone_idx(sbi, secno, dev_idx);

       /*
        * Conventional zone's capacity is always equal to zone size,
        * so, blocks per segment is unchanged.
        */
       if (is_conv_zone(sbi, zone_idx, dev_idx))
               return sbi->blocks_per_seg;

       if (!FDEV(dev_idx).zone_capacity_blocks)
               return sbi->blocks_per_seg;

       sec_start_blkaddr = START_BLOCK(sbi, GET_SEG_FROM_SEC(sbi, secno));
       sec_cap_blkaddr = sec_start_blkaddr +
                               FDEV(dev_idx).zone_capacity_blocks[zone_idx];

       /*
        * If segment starts before zone capacity and spans beyond
        * zone capacity, then usable blocks are from seg start to
        * zone capacity. If the segment starts after the zone capacity,
        * then there are no usable blocks.
        */
       if (seg_start >= sec_cap_blkaddr)
               return 0;
       if (seg_start + sbi->blocks_per_seg > sec_cap_blkaddr)
               return sec_cap_blkaddr - seg_start;

       return sbi->blocks_per_seg;
}
#else
int f2fs_fix_curseg_write_pointer(struct f2fs_sb_info *sbi)
{
       return 0;
}

int f2fs_check_write_pointer(struct f2fs_sb_info *sbi)
{
       return 0;
}

static inline unsigned int f2fs_usable_zone_blks_in_seg(struct f2fs_sb_info *sbi,
                                                       unsigned int segno)
{
       return 0;
}

static inline unsigned int f2fs_usable_zone_segs_in_sec(struct f2fs_sb_info *sbi,
                                                       unsigned int segno)
{
       return 0;
}
#endif
unsigned int f2fs_usable_blks_in_seg(struct f2fs_sb_info *sbi,
                                       unsigned int segno)
{
       if (f2fs_sb_has_blkzoned(sbi)){
               panic("f2fs_usable_blks_in_seg(): f2fs_sb_has_blkzoned is True. not expected\n");
               return f2fs_usable_zone_blks_in_seg(sbi, segno);
       }
       return sbi->blocks_per_seg;
}

unsigned int f2fs_usable_segs_in_sec(struct f2fs_sb_info *sbi,
                                       unsigned int segno)
{
       if (f2fs_sb_has_blkzoned(sbi))
               return f2fs_usable_zone_segs_in_sec(sbi, segno);

       return sbi->segs_per_sec;
}

/*
 * Update min, max modified time for cost-benefit GC algorithm
 */
//static void init_min_max_mtime(struct f2fs_sb_info *sbi)
//{
//       struct sit_info *sit_i = SIT_I(sbi);
//       unsigned int segno;
//
//       down_write(&sit_i->sentry_lock);
//
//       sit_i->min_mtime = ULLONG_MAX;
//
//       for (segno = 0; segno < MAIN_SEGS(sbi); segno += sbi->segs_per_sec) {
//               unsigned int i;
//               unsigned long long mtime = 0;
//
//               for (i = 0; i < sbi->segs_per_sec; i++)
//                       mtime += get_seg_entry(sbi, segno + i)->mtime;
//
//               mtime = div_u64(mtime, sbi->segs_per_sec);
//
//               if (sit_i->min_mtime > mtime)
//                       sit_i->min_mtime = mtime;
//       }
//       sit_i->max_mtime = get_mtime(sbi, false);
//       sit_i->dirty_max_mtime = 0;
//       up_write(&sit_i->sentry_lock);
//}

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
	   printk("%s: segment_count: %u", __func__, sm_info->segment_count);
       sm_info->reserved_segments = le32_to_cpu(ckpt->rsvd_segment_count);
       sm_info->ovp_segments = le32_to_cpu(ckpt->overprov_segment_count);
       sm_info->main_segments = le32_to_cpu(raw_super->segment_count_main);
       sm_info->ssa_blkaddr = le32_to_cpu(raw_super->ssa_blkaddr);
       sm_info->rec_prefree_segments = sm_info->main_segments *
                                       DEF_RECLAIM_PREFREE_SEGMENTS / 100;
       sm_info->start_segno = GET_SEGNO_FROM_SEG0(sbi, MAIN_BLKADDR(sbi));
       if (sm_info->rec_prefree_segments > DEF_MAX_RECLAIM_PREFREE_SEGMENTS)
               sm_info->rec_prefree_segments = DEF_MAX_RECLAIM_PREFREE_SEGMENTS;

       if (!f2fs_lfs_mode(sbi))
               sm_info->ipu_policy = 1 << F2FS_IPU_FSYNC;
       sm_info->min_ipu_util = DEF_MIN_IPU_UTIL;
       sm_info->min_fsync_blocks = DEF_MIN_FSYNC_BLOCKS;
       sm_info->min_seq_blocks = sbi->blocks_per_seg * sbi->segs_per_sec;
       sm_info->min_hot_blocks = DEF_MIN_HOT_BLOCKS;
       sm_info->min_ssr_sections = reserved_sections(sbi);

       INIT_LIST_HEAD(&sm_info->sit_entry_set);

       init_rwsem(&sm_info->curseg_lock);
       init_rwsem(&sm_info->curseg_zone_lock);

       if (!f2fs_readonly(sbi->sb)) {
               err = f2fs_create_flush_cmd_control(sbi);
               if (err)
                       return err;
       }

	   sbi->START_SEGNO_INTERVAL = GET_SEGNO(sbi, 
			   (CURSEG_HOT_DATA + 1) * BLKS_PER_SUPERZONE + SEG0_BLKADDR(sbi) % sbi->blocks_per_seg
			   );
	   
	   sbi->START_SEGNO_INTERVAL_NODE = GET_SEGNO(sbi, 
			   (CURSEG_HOT_NODE + 1) * BLKS_PER_SUPERZONE + SEG0_BLKADDR(sbi) % sbi->blocks_per_seg
			   );

       err = create_discard_cmd_control(sbi);
       if (err)
               return err;

       err = create_dynamic_discard_map_control(sbi);
       if (err)
               return err;

#ifdef IPLFS_CALLBACK_IO
	   err = create_migration_io_control(sbi);
	   if (err)
			return err;
#endif

       err = build_sit_info(sbi);
       if (err)
               return err;
       
	   err = build_free_segmap(sbi);
       if (err)
               return err;

       err = build_curseg(sbi);
       if (err)
               return err;
	   printk("%s: build_curseg done", __func__);

       /* reinit free segmap based on SIT */
       err = build_sit_entries(sbi);
       if (err)
               return err;

	   printk("%s: build_sit_entries done", __func__);

       printk("%s: NR_CURSEG_TYPE: %d, CURSEG_HOT_DATA: %d", __func__, NR_CURSEG_TYPE, CURSEG_HOT_DATA);
	   printk("[JWDBG] %s: seg0_blkaddr = %d 0x%x blocks_per_seg: %d", 
			__func__, sm_info->seg0_blkaddr, sm_info->seg0_blkaddr, sbi->blocks_per_seg);

#ifdef IPLFS_CALLBACK_IO
       err = build_slot_info(sbi);
       if (err)
               return err;
	   
       err = build_slot_entries(sbi);
       if (err)
               return err;
	  
	   err = build_discard_cnt_info(sbi);
       if (err)
               return err;
#endif

       init_free_segmap(sbi);
       err = build_dirty_segmap(sbi);
       if (err)
             return err;

#if (SUPERZONE == 1)
	   D2FS_set_segment_two_partition(sbi);
	   //D2FS_set_segment(sbi);
       if (sm_info->seg0_blkaddr % (sbi)->blocks_per_seg != 0){
	       printk("[JWDBG] %s: seg0_blkaddr = %d 0x%x, blk_per_seg: %d", __func__, sm_info->seg0_blkaddr,
			       sm_info->seg0_blkaddr, sbi->blocks_per_seg);
	       //f2fs_bug_on(sbi, 1);
       }
#else
	   f2fs_bug_on(sbi, 1);
       //JW_set_curseg_in_single_superzone(sbi);
#endif

       /*err = sanity_check_curseg(sbi);
       if (err)
               return err;
       */
       //init_min_max_mtime(sbi);

       return 0;
}

/*
static void discard_dirty_segmap(struct f2fs_sb_info *sbi,
               enum dirty_type dirty_type)
{
       struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);

       mutex_lock(&dirty_i->seglist_lock);
       kvfree(dirty_i->dirty_segmap[dirty_type]);
       dirty_i->nr_dirty[dirty_type] = 0;
       mutex_unlock(&dirty_i->seglist_lock);
}

static void destroy_victim_secmap(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	kvfree(dirty_i->victim_secmap);
}

static void destroy_dirty_segmap(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	int i;

	if (!dirty_i)
		return;

	// discard pre-free/dirty segments list 
	for (i = 0; i < NR_DIRTY_TYPE; i++)
		discard_dirty_segmap(sbi, i);

	if (__is_large_section(sbi)) {
		mutex_lock(&dirty_i->seglist_lock);
		kvfree(dirty_i->dirty_secmap);
		mutex_unlock(&dirty_i->seglist_lock);
	}

	destroy_victim_secmap(sbi);
	SM_I(sbi)->dirty_info = NULL;
	kfree(dirty_i);
}
*/

static void destroy_curseg(struct f2fs_sb_info *sbi)
{
	struct curseg_info *array = SM_I(sbi)->curseg_array;
	int i;

	if (!array)
		return;
	SM_I(sbi)->curseg_array = NULL;
	for (i = 0; i < NR_CURSEG_TYPE; i++) {
		kfree(array[i].sum_blk);
		kfree(array[i].journal);
	}
	kfree(array);
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

	//if (sit_i->sentries)
	//	kvfree(sit_i->bitmap);
	kfree(sit_i->tmp_map);

	kvfree(sit_i->sentries);
	//kvfree(sit_i->sec_entries);
	kvfree(sit_i->dirty_sentries_bitmap);

	SM_I(sbi)->sit_info = NULL;
	kvfree(sit_i->sit_bitmap);
#ifdef CONFIG_F2FS_CHECK_FS
	kvfree(sit_i->sit_bitmap_mir);
	kvfree(sit_i->invalid_segmap);
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
	destroy_dynamic_discard_map_control(sbi);
	//destroy_dirty_segmap(sbi);
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

	inmem_entry_slab = f2fs_kmem_cache_create("f2fs_inmem_page_entry",
			sizeof(struct inmem_pages));
	if (!inmem_entry_slab)
		goto destroy_sit_entry_set;
	
	discard_map_slab = f2fs_kmem_cache_create("f2fs_discard_map",
			sizeof(struct dynamic_discard_map));
	if (!discard_map_slab)
		goto destroy_inmem_page_entry;
	
	discard_range_slab = f2fs_kmem_cache_create("f2fs_range_map",
			sizeof(struct discard_range_entry));
	if (!discard_range_slab)
		goto destroy_discard_map;

#ifdef IPLFS_CALLBACK_IO	
	mg_entry_slab = f2fs_kmem_cache_create("f2fs_migration_entry",
			sizeof(struct mg_entry));
	if (!mg_entry_slab)
		goto destroy_range_map;
	printk("%s: mes: %llx %p", __func__, mg_entry_slab, mg_entry_slab);

	slot_entry_slab = f2fs_kmem_cache_create("f2fs_slot_entry", 
			sizeof(struct slot_entry));
	if (!slot_entry_slab)
		goto destroy_mg_entry;
	
	discard_cnt_entry_slab = f2fs_kmem_cache_create("f2fs_discard_cnt_entry", 
			sizeof(struct discard_cnt_entry));
	if (!discard_cnt_entry_slab)
		goto destroy_mg_entry;
#endif

	return 0;

#ifdef IPLFS_CALLBACK_IO	
destroy_mg_entry:
	kmem_cache_destroy(mg_entry_slab);
destroy_range_map:
	kmem_cache_destroy(discard_range_slab);
#endif
destroy_discard_map:
	kmem_cache_destroy(discard_map_slab);
destroy_inmem_page_entry:
	kmem_cache_destroy(inmem_entry_slab);
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
	kmem_cache_destroy(inmem_entry_slab);
	kmem_cache_destroy(discard_map_slab);
	kmem_cache_destroy(discard_range_slab);
}
