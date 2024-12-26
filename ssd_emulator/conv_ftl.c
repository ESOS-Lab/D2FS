/**********************************************************************
 * Copyright (c) 2020-2023
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTIABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 **********************************************************************/

#include <linux/ktime.h>
#include <linux/sched/clock.h>
#include <linux/hashtable.h>

#include "nvmev.h"
#include "conv_ftl.h"
#include "bitmap.h"

#ifdef DISCARD_ENABLED
#include <linux/highmem.h>
#endif

#ifdef SHIVAL2
unsigned int total_valid_blks = 0;
#endif

#define BUF_CNT_ 10
static int mig_cnt = 0;

inline int wftl_test_bit(unsigned int nr, char *addr)
{
	int mask;

	addr += (nr >> 3);
	mask = 1 << (7 - (nr & 0x07));
	return mask & *addr;
}

inline void wftl_set_bit(unsigned int nr, char *addr)
{
	int mask;

	addr += (nr >> 3);
	mask = 1 << (7 - (nr & 0x07));
	*addr |= mask;
}

inline void wftl_clear_bit(unsigned int nr, char *addr)
{
	int mask;

	addr += (nr >> 3);
	mask = 1 << (7 - (nr & 0x07));
	*addr &= ~mask;
}

inline int wftl_test_and_set_bit(unsigned int nr, char *addr)
{
	int mask;
	int ret;

	addr += (nr >> 3);
	mask = 1 << (7 - (nr & 0x07));
	ret = mask & *addr;
	*addr |= mask;
	return ret;
}

inline int wftl_test_and_clear_bit(unsigned int nr, char *addr)
{
	int mask;
	int ret;

	addr += (nr >> 3);
	mask = 1 << (7 - (nr & 0x07));
	ret = mask & *addr;
	*addr &= ~mask;
	return ret;
}

inline void wftl_change_bit(unsigned int nr, char *addr)
{
	int mask;

	addr += (nr >> 3);
	mask = 1 << (7 - (nr & 0x07));
	*addr ^= mask;
}


inline uint64_t find_next_zero_bit_(char *bitmap, uint64_t nbits, uint64_t sidx)
{
	uint64_t idx;
	for (idx = sidx; idx < nbits; idx ++) {
		if (wftl_test_bit(idx, bitmap) == 0) {
			return idx;
		}
	}
	return idx;
}

#ifdef WAF
unsigned long long OS_TimeGetUS( void )
{
    struct timespec64 lTime;
    ktime_get_coarse_real_ts64(&lTime);
    return (lTime.tv_sec * 1000000 + div_u64(lTime.tv_nsec, 1000) );

}
#endif


void enqueue_writeback_io_req(int sqid, unsigned long long nsecs_target, struct buffer * write_buffer, unsigned int buffs_to_release);

static inline bool last_pg_in_wordline(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	return (ppa->g.pg % spp->pgs_per_oneshotpg) == (spp->pgs_per_oneshotpg - 1);
}

static bool should_gc(struct conv_ftl *conv_ftl)
{
	return (conv_ftl->lm.free_line_cnt <= conv_ftl->cp.gc_thres_lines);
}

static inline bool should_gc_high(struct conv_ftl *conv_ftl)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	return conv_ftl->lm.free_line_cnt <= conv_ftl->cp.gc_thres_lines_high;
}

#ifdef COUPLED_GC_MTL
static inline struct mg_entry * init_migration_entry(struct nvmev_ns *ns)
{
	struct mg_entry * mge;
	mge = kmem_cache_alloc(ns->mtl_migration_entry_slab, GFP_KERNEL);
	NVMEV_ASSERT(mge != NULL);
	mge->nr_log = 0;
	return mge;
}

static inline struct trans_entry * init_translation_entry(struct nvmev_ns *ns)
{
	struct trans_entry * te;
	te = kmem_cache_alloc(ns->mtl_translation_entry_slab, GFP_KERNEL);
	NVMEV_ASSERT(te != NULL);
	te->nr_log = 0;
	te->cur_idx = 0;
	return te;
}

void init_mtl_migration_mgmt(struct nvmev_ns *ns)
{
	ns->mtl_migration_entry_slab = kmem_cache_create("mtl_migration", sizeof(struct mg_entry), 
													0, SLAB_RECLAIM_ACCOUNT, NULL);
	NVMEV_ASSERT(ns->mtl_migration_entry_slab != NULL);
}

void init_mtl_translation_mgmt(struct nvmev_ns *ns)
{
	ns->mtl_translation_entry_slab = kmem_cache_create("mtl_translation", sizeof(struct trans_entry), 
													0, SLAB_RECLAIM_ACCOUNT, NULL);
	NVMEV_ASSERT(ns->mtl_translation_entry_slab != NULL);
}
#endif

#ifndef MULTI_PARTITION_FTL
static void forground_gc(struct conv_ftl *conv_ftl);
#elif defined COUPLED_GC_MTL
static void forground_gc(struct conv_ftl *conv_ftl, int no_partition, struct nvmev_result *ret);
#else
static void forground_gc(struct conv_ftl *conv_ftl, int no_partition);
#endif

#ifdef ZONE_MAPPING

#ifndef COUPLED_GC
static inline uint64_t get_logical_zoneno(struct conv_ftl *conv_ftl, uint64_t local_lpn)
{
	uint64_t pgs_per_zone = conv_ftl->ssd->sp.pgs_per_line;
	return (local_lpn - LOCAL_PARTITION_START_ADDR(local_lpn)) / pgs_per_zone;  /* TODO: should modify for window map */
}
#endif

static inline uint64_t get_zone_offset(struct conv_ftl *conv_ftl, uint64_t local_lpn)
{
	uint64_t pgs_per_zone = conv_ftl->ssd->sp.pgs_per_line;
	return local_lpn % pgs_per_zone - LOCAL_DATA_PARTITION_START_OFFSET;  /* TODO: need to substract if partition start addr is not aligned to zone.  */
}

static inline bool is_first_lpn_in_zone(struct conv_ftl *conv_ftl, uint64_t local_lpn)
{
	return get_zone_offset(conv_ftl, local_lpn) == 0;
}

static inline bool is_first_wp(struct write_pointer *wpp)
{
	return (wpp->pg == 0) && (wpp->lun == 0) && (wpp->ch == 0) && (wpp->pl == 0);
}

#ifdef COUPLED_GC

DEFINE_HASHTABLE(aimless_translator, HBITS_AIMLESS_TRANSLATOR);
#ifdef SEPARATE_GC_LOG
DEFINE_HASHTABLE(aimless_translator_node, HBITS_AIMLESS_TRANSLATOR);
#endif

#ifdef GC_LOG_MERGE
DEFINE_HASHTABLE(gc_log_merger, HBITS_AIMLESS_TRANSLATOR);
#ifdef SEPARATE_GC_LOG
DEFINE_HASHTABLE(gc_log_merger_node, HBITS_AIMLESS_TRANSLATOR);
#endif
#endif

#ifdef MIGRATION_IO
//DEFINE_HASHTABLE(inflight_gc_log_htable, HBITS_INFLIGHT_GC_LOG_HTABLE);
#endif

void init_gc_log_mgmt(struct gc_log_mgmt *gclm)
{
	gclm->gc_log_slab = kmem_cache_create("gc_log", sizeof(struct gc_log), 0, 
											SLAB_RECLAIM_ACCOUNT, NULL);
	NVMEV_ASSERT(gclm->gc_log_slab);

	list_init(&gclm->free_gc_log_list);
	list_init(&gclm->buffered_gc_log_list);
#ifdef SEPARATE_GC_LOG
	list_init(&gclm->buffered_gc_log_list_node);
#endif

	gclm->hbits = HBITS_AIMLESS_TRANSLATOR;
	hash_init(aimless_translator);
	
#ifdef SEPARATE_GC_LOG
	hash_init(aimless_translator_node);
#endif

#ifdef GC_LOG_MERGE
	hash_init(gc_log_merger);
#ifdef SEPARATE_GC_LOG
	hash_init(gc_log_merger_node);
#endif
#endif

	gclm->n_free = 0;
	gclm->n_buffered = 0;
	gclm->n_inflight = 0;
	gclm->n_total = 0;

	gclm->buffering_cnt = 0;
	gclm->buffering_trial_cnt = 0;

#ifdef MIGRATION_IO
	int i;
	//gclm->inflight_set_slab = kmem_cache_create("inflight_set", sizeof(struct inflight_set_entry), 0, 
	//										SLAB_RECLAIM_ACCOUNT, NULL);
	//NVMEV_ASSERT(gclm->inflight_set_slab);

	//gclm->mg_batch_slab = kmem_cache_create("mg_batch", sizeof(struct mg_batch_entry), 0, 
	//										SLAB_RECLAIM_ACCOUNT, NULL);
	//NVMEV_ASSERT(gclm->mg_batch_slab);

	gclm->next_command_id = 0;
	gclm->completed_command_id = 0;
	gclm->n_ise = 0;

	for (i = 0; i < NR_INFLIGHT_SET; i ++){
		gclm->ise_array[i].command_id = INVALID_COMMAND_ID;
		list_init(&gclm->ise_array[i].gc_log_list);
	}

	//hash_init(inflight_gc_log_htable);
#endif
#ifdef MG_HANDLER_DISABLED
	list_init(&gclm->unhandled_gc_log_list);
#endif
}

void init_gc_log(struct gc_log *gc_log)
{
	gc_log->old_lpn = INVALID_LPN;
	gc_log->new_lpn = INVALID_LPN;
	gc_log->status = GC_LOG_FREE;
	INIT_HLIST_NODE(&gc_log->hnode);
#ifdef GC_LOG_MERGE
	INIT_HLIST_NODE(&gc_log->hnode_merge);
#endif
}

/* create free mem page list. */
void build_free_gc_log(struct gc_log_mgmt *gclm)
{
	int i;
	struct gc_log *gc_log;
	for (i = 0; i < NO_INIT_GC_LOG; i ++){
		if ((gc_log = (struct gc_log *) kmem_cache_alloc(gclm->gc_log_slab, GFP_KERNEL)) == NULL)
			NVMEV_ASSERT(0);
		init_gc_log(gc_log);
#ifdef GC_LOG_PRINT
		printk("%s: free_gc_log_list: %p gc_log->list_elem: %p", __func__, 
				&gclm->free_gc_log_list, &gc_log->list_elem);
#endif
		list_push_back(&gclm->free_gc_log_list, &gc_log->list_elem);
	
		gclm->n_free ++ ;
		gclm->n_total ++ ;
	}
}

#ifdef MIGRATION_IO
void init_inflight_set(struct gc_log_mgmt *gclm, struct inflight_set_entry *ise, unsigned int *cid)
{
	ise->command_id = (gclm->next_command_id % MAX_CID);
	gclm->next_command_id ++ ;
	*cid = ise->command_id;
	
	//if (ise->command_id % 1000 == 0) {
	//	printk("%s: command id: %u idx: %u", __func__, ise->command_id, 
	//		ise->command_id % NR_INFLIGHT_SET);	

	//}
	//printk("%s: command id: %u idx: %u", __func__, ise->command_id, 
	//		ise->command_id % NR_INFLIGHT_SET);	
	list_init(&ise->gc_log_list);

	//list_init(&ise->list_elem);
	//INIT_HLIST_NODE(&ise->hnode);
	
	gclm->n_ise ++ ;
}

/*void init_mg_batch(struct mg_batch_entry *mgbe, unsigned int cid)
{
	mgbe->command_id = cid;
	mgbe->nr = 0;
}*/
#endif
#endif
static inline bool is_window_overflow(struct conv_ftl *conv_ftl, struct window_mgmt *wm)
{
	return (wm->tail_zoneno - wm->head_zoneno + 1 > wm->nzones_per_partition);
}

static inline bool wrong_head_tail_idx(struct conv_ftl *conv_ftl, struct window_mgmt *wm)
{
	int head_idx = wm->head_idx;
	int tail_idx = wm->tail_idx;
	return head_idx >= wm->nzones_per_partition || tail_idx >= wm->nzones_per_partition;
}

static inline void check_window_overflow(struct conv_ftl *conv_ftl, struct window_mgmt *wm)
{
	if (is_window_overflow(conv_ftl, wm) || wrong_head_tail_idx(conv_ftl, wm)){
		printk("%s: head_zoneno: %llu tail_zoneno: %llu window_sz: %lu headidx: %d tailidx: %d",
				__func__, wm->head_zoneno, wm->tail_zoneno, wm->nzones_per_partition,
				wm->head_idx, wm->tail_idx);
		NVMEV_ASSERT(!is_window_overflow(conv_ftl, wm));
	}
}

static inline bool inside_active_interval(struct window_mgmt *wm, uint64_t cur_zoneno)
{
	/* tail_zoneno + 1 is for write request */
	return true;
	/* TODO: Need to Activate if write is ordered */
	//return (wm->head_zoneno <= cur_zoneno) && (cur_zoneno <= wm->tail_zoneno + 1); 
}

static inline void check_inside_active_interval(struct conv_ftl *conv_ftl, struct window_mgmt *wm, uint64_t local_lpn)
{
	uint64_t cur_zoneno = NO_ZONE(conv_ftl, local_lpn);

	if (!inside_active_interval(wm, cur_zoneno)){
		printk("%s: head_zoneno: %llu tail_zoneno: %llu cur_zoneno: %llu window_sz: %lu",
				__func__,
				wm->head_zoneno, wm->tail_zoneno, cur_zoneno, wm->nzones_per_partition);
		NVMEV_ASSERT(0);
	}
}

static inline bool behind_active_interval(struct conv_ftl *conv_ftl, uint64_t local_lpn)
{
	uint64_t cur_zoneno;
	int no_partition = NO_LOCAL_PARTITION(local_lpn);
	struct window_mgmt *wm = &conv_ftl->wm[no_partition];

	cur_zoneno = NO_ZONE(conv_ftl, local_lpn);
//	printk("%s: not expected!!!!!!!", __func__);
	return cur_zoneno < wm->head_zoneno;
}

static inline uint64_t get_relational_zoneno(struct conv_ftl *conv_ftl, uint64_t local_lpn)
{
	uint64_t cur_zoneno, zone_ofs;
	int no_partition = NO_LOCAL_PARTITION(local_lpn);
	struct window_mgmt *wm = &conv_ftl->wm[no_partition];

	cur_zoneno = NO_ZONE(conv_ftl, local_lpn);
	zone_ofs = cur_zoneno - wm->head_zoneno;

	if (cur_zoneno < wm->head_zoneno || zone_ofs > wm->nzones_per_partition){
		//printk("%s: cur_zoneno: %llu head_zoneno: %llu, nzones_per_partition: %lu", 
		//		__func__, cur_zoneno, wm->head_zoneno, wm->nzones_per_partition);
	}

	return zone_ofs;
}

static inline uint64_t get_zone_idx(struct conv_ftl *conv_ftl, uint64_t local_lpn)
{
	uint64_t relational_zoneno, zidx;
	struct window_mgmt *wm = &conv_ftl->wm[NO_LOCAL_PARTITION(local_lpn)];
	
	check_inside_active_interval(conv_ftl, wm, local_lpn);

	relational_zoneno = get_relational_zoneno(conv_ftl, local_lpn);
	zidx = (wm->head_idx + relational_zoneno) % wm->nzones_per_partition;

	return zidx;
}

static inline void update_window_mgmt_for_write(struct conv_ftl *conv_ftl, uint64_t local_lpn)
{
	uint64_t cur_zoneno;
	int no_partition = NO_LOCAL_PARTITION(local_lpn);
	struct window_mgmt *wm = &conv_ftl->wm[no_partition];
	
	cur_zoneno = NO_ZONE(conv_ftl, local_lpn);

#ifndef DEACTIVATE_SLIDING_WINDOW
	if (cur_zoneno > wm->tail_zoneno){
#ifdef COUPLED_GC_PRINT
		printk("%s 1 type: %d head_zoneno: %llu tail_zoneno: %llu cur_zoneno: %llu next_local_lpn: %llu head_idx: %d tail_idx: %d", 
				__func__, no_partition, wm->head_zoneno, wm->tail_zoneno, cur_zoneno,
		  		wm->next_local_lpn, wm->head_idx, wm->tail_idx);
#endif

		wm->tail_idx += cur_zoneno - wm->tail_zoneno;
		wm->tail_idx %= wm->nzones_per_partition;
		wm->tail_zoneno = cur_zoneno;
#ifdef COUPLED_GC_PRINT
		printk("%s 2 type: %d head_zoneno: %llu tail_zoneno: %llu cur_zoneno: %llu next_local_lpn: %llu head_idx: %d tail_idx: %d", 
				__func__, no_partition, wm->head_zoneno, wm->tail_zoneno, cur_zoneno, 
		  		wm->next_local_lpn, wm->head_idx, wm->tail_idx);
#endif

		check_window_overflow(conv_ftl, wm);
	}
#else
	if (cur_zoneno - wm->head_zoneno + 1 > wm->nzones_per_partition) {
		printk("%s: overflow!!!!!!!!!!!! no_partition: %d", __func__, no_partition);
		NVMEV_ASSERT(0);
	}
#endif
	
}

static inline void update_window_mgmt_for_discard(struct conv_ftl *conv_ftl, uint64_t local_lpn)
{
	uint64_t cur_zoneno, n_map_entries;
	int no_partition = NO_LOCAL_PARTITION(local_lpn);
	struct window_mgmt *wm = &conv_ftl->wm[no_partition];
    n_map_entries = wm->nzones_per_partition;
	uint64_t start_zidx = get_zone_idx(conv_ftl, local_lpn);
	int i;
	struct ppa *maptbl = conv_ftl->maptbl[no_partition], *ppa;
	cur_zoneno = NO_ZONE(conv_ftl, local_lpn);
#ifndef DEACTIVATE_SLIDING_WINDOW
	if (cur_zoneno == wm->head_zoneno){
		for (i = 1; i < n_map_entries; i++){
			ppa = &maptbl[(start_zidx + i) % n_map_entries];
			if (ppa->ppa != INVALID_PPA)
				break;
		}
		//printk("%s: bef. head_zoneno: %llu", __func__, wm->head_zoneno);
#ifdef COUPLED_GC_PRINT
		printk("%s 1 type: %d head_zoneno: %llu tail_zoneno: %llu cur_zoneno: %llu next_local_lpn: %llu head_idx: %d tail_idx: %d", 
				__func__, no_partition, wm->head_zoneno, wm->tail_zoneno, cur_zoneno, 
		  		wm->next_local_lpn, wm->head_idx, wm->tail_idx);
#endif
		wm->head_zoneno += i;
		wm->head_idx += i;
		wm->head_idx %= n_map_entries;

		//printk("%s: aft. head_zoneno: %llu", __func__, wm->head_zoneno);

#ifdef COUPLED_GC_PRINT
		printk("%s 2 type: %d head_zoneno: %llu tail_zoneno: %llu cur_zoneno: %llu next_local_lpn: %llu head_idx: %d tail_idx: %d", 
				__func__, no_partition, wm->head_zoneno, wm->tail_zoneno, cur_zoneno,
		  		wm->next_local_lpn, wm->head_idx, wm->tail_idx);
#endif

		check_window_overflow(conv_ftl, wm);
	}
#else
	if (cur_zoneno - wm->head_zoneno + 1 > wm->nzones_per_partition) {
		printk("%s: overflow!! no_partition: %d cur_zoneno: %d head_zoneno: %d lpn: 0x%lx", 
				__func__, no_partition, cur_zoneno, wm->head_zoneno, local_lpn*4);
		NVMEV_ASSERT(0);
	}
#endif
	
}
//#endif

static inline bool out_of_partition(struct conv_ftl *conv_ftl, uint64_t local_lpn);

/* Before calling this function, behind_active_interval should be checked.*/
/* For invalidation, behind_active_interval is already called in read_handler. */
static inline struct ppa *get_zone_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t local_lpn)
{
	int no_partition = NO_LOCAL_PARTITION(local_lpn);

#ifndef COUPLED_GC
	uint64_t zidx = get_logical_zoneno(conv_ftl, local_lpn);
	if (out_of_partition(conv_ftl, local_lpn)){
		NVMEV_INFO("[JWDBG] %s: local_lpn: 0x%llx zidx: %lld nzp: %ld pgsPline: %ld PSA: 0x%llx\n",
					__func__, local_lpn, zidx, conv_ftl->nzones_per_partition,
					conv_ftl->ssd->sp.pgs_per_line, LOCAL_PARTITION_START_ADDR(local_lpn));
		NVMEV_ASSERT(0);
	}
#else
	uint64_t zidx = get_zone_idx(conv_ftl, local_lpn);
#endif

	return &conv_ftl->maptbl[no_partition][zidx];
}

static void print_dbg_get_zone_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t local_lpn, char* func)
{
	int no_partition = NO_LOCAL_PARTITION(local_lpn);

#ifndef COUPLED_GC
	uint64_t zidx = get_logical_zoneno(conv_ftl, local_lpn);
	if (out_of_partition(conv_ftl, local_lpn)){
		NVMEV_INFO("[JWDBG] %s  %s: local_lpn: 0x%llx zidx: %lld nzp: %ld pgsPline: %ld PSA: 0x%llx\n",
					__func__, local_lpn, zidx, conv_ftl->nzones_per_partition,
					conv_ftl->ssd->sp.pgs_per_line, LOCAL_PARTITION_START_ADDR(local_lpn));
		NVMEV_ASSERT(0);
	}
	printk("%s %s: no_partition: %d zidx: %d", func, __func__, no_partition, zidx);
#else
	uint64_t zidx = get_zone_idx(conv_ftl, local_lpn);
#endif

	return;
}

static inline bool is_first_ppa_in_zone(struct ppa *ppa)
{
	return ppa->g.pg == 0 && ppa->g.lun == 0 && ppa->g.ch == 0 && ppa->g.pl == 0;
	/* TODO: assume # of pl_per_lun is 1, fix later */
}

static inline void set_zone_maptbl_ent(struct conv_ftl *conv_ftl, struct ppa *zone_map_ent, 
														struct ppa *ppa)
{
	NVMEV_ASSERT(is_first_ppa_in_zone(ppa));
	*zone_map_ent = *ppa; 
}

static struct line *get_next_free_line(struct conv_ftl *conv_ftl);

static inline void check_addr(int a, int max);

static inline void set_rmap_zone_ent(struct conv_ftl *conv_ftl, uint64_t local_lpn,
		uint32_t io_type, int no_partition)
{
	struct write_pointer *wpp;
	struct ppa ppa;

	if (io_type == USER_IO) {
	    wpp = &conv_ftl->wp[no_partition];
	} else if (io_type == GC_IO) {
	    wpp = &conv_ftl->gc_wp;
	} else {
	    NVMEV_ASSERT(0);
	}
	wpp->curline->start_local_lpn = local_lpn;
	//if (conv_ftl->no_part == 2 || conv_ftl->no_part == 3) {
	/*
	if ((wpp->curline->start_local_lpn & 0xe0000000) == 0x20000000) {
		struct line_mgmt *lm = &conv_ftl->lm;
		//printk("%s: ftlno: %d line: %p start_local_lpn: 0x%llx free lcnt: %u victim lcnt: %u full lcnt: %u tt lcnt: %u is_same: %d", 
		//		__func__,
		//	   conv_ftl->no_part, wpp->curline, wpp->curline->start_local_lpn, 
		//	   lm->free_line_cnt, lm->victim_line_cnt, lm->full_line_cnt, 
		//	   lm->tt_lines, 
		//	   lm->free_line_cnt + lm->victim_line_cnt + lm->full_line_cnt == lm->tt_lines
		//	   );
		printk("%s: ftlno: %d line: %p start_local_lpn: 0x%llx", 
				__func__,
			   conv_ftl->no_part, wpp->curline, wpp->curline->start_local_lpn);
	}
	*/
	//}
}

static struct ppa get_new_zone_for_append(struct conv_ftl *conv_ftl, uint32_t io_type, int no_partition)
{
	struct write_pointer *wpp;
	struct ppa ppa;

	if (io_type == USER_IO) {
	    wpp = &conv_ftl->wp[no_partition];
	} else if (io_type == GC_IO) {
	    wpp = &conv_ftl->gc_wp;
	} else {
	    NVMEV_ASSERT(0);
	}

	ppa.ppa = 0;
	ppa.g.ch = wpp->ch;
	ppa.g.lun = wpp->lun;
	ppa.g.pg = wpp->pg;
	ppa.g.blk = wpp->blk;
	ppa.g.pl = wpp->pl;
	NVMEV_ASSERT(ppa.g.pl == 0);

	/* check wp is first when allocating new zone */
	NVMEV_ASSERT(is_first_wp(wpp));
	return ppa;
}

#ifndef COUPLED_GC_MTL
static struct ppa get_new_zone(struct conv_ftl *conv_ftl, uint32_t io_type, int no_partition)
#else
static struct ppa get_new_zone(struct conv_ftl *conv_ftl, uint32_t io_type, int no_partition, struct nvmev_result *ret)
#endif
{
	struct write_pointer *wpp;
	struct ppa ppa;
#ifndef GURANTEE_SEQ_WRITE
	struct ssdparams *spp = &conv_ftl->ssd->sp;
#endif

	if (io_type == USER_IO) {
	    wpp = &conv_ftl->wp[no_partition];
	} else if (io_type == GC_IO) {
	    wpp = &conv_ftl->gc_wp;
	} else {
	    NVMEV_ASSERT(0);
	}

#ifdef GURANTEE_SEQ_WRITE
	ppa.ppa = 0;
	ppa.g.ch = wpp->ch;
	ppa.g.lun = wpp->lun;
	ppa.g.pg = wpp->pg;
	ppa.g.blk = wpp->blk;
	ppa.g.pl = wpp->pl;
	NVMEV_ASSERT(ppa.g.pl == 0);

	/* check wp is first when allocating new zone */
	NVMEV_ASSERT(is_first_wp(wpp));
#else
	if (wpp->curline->wpc > 0) {
		/* allocate another new zone */
		/* do GC if there is not enough line */
#ifndef MULTI_PARTITION_FTL
		forground_gc(conv_ftl);
#elif defined COUPLED_GC_MTL
		forground_gc(conv_ftl, no_partition, ret);
#else
		forground_gc(conv_ftl, no_partition);
#endif
		wpp->curline = NULL;
		wpp->curline = get_next_free_line(conv_ftl);
		BUG_ON(!wpp->curline);
		wpp->blk = wpp->curline->id;
		if (!(wpp->blk >= 0 && wpp->blk < spp->blks_per_pl)){
			struct line_mgmt *lm = &conv_ftl->lm;
			printk("[JWDBG] %s: what the? line: %p wpblk: %u blks_per_pl: %d freelinecnt: %d\n",
					__func__, wpp->curline, wpp->blk, spp->blks_per_pl, lm->free_line_cnt);

		}
		check_addr(wpp->blk, spp->blks_per_pl);
	}
	ppa.ppa = 0;
	ppa.g.ch = wpp->ch;
	ppa.g.lun = wpp->lun;
	ppa.g.pg = wpp->pg;
	ppa.g.blk = wpp->blk;
	ppa.g.pl = wpp->pl;
	NVMEV_ASSERT(ppa.g.pl == 0);
#endif
	return ppa;
}

static inline struct ppa calc_ppa_in_zone(struct conv_ftl *conv_ftl, uint64_t local_lpn, 
											struct ppa *zone_map_ent)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;

	struct ppa ppa = *zone_map_ent;

	NVMEV_ASSERT(ppa.g.pg == 0);
	NVMEV_ASSERT(ppa.g.lun == 0);
	NVMEV_ASSERT(ppa.g.ch == 0);
	/* TODO: assume # of pl_per_lun is 1, fix later */
	NVMEV_ASSERT(ppa.g.pl == 0);

	uint64_t zone_ofs = get_zone_offset(conv_ftl, local_lpn);
	uint32_t pg_per_oneshotpg, channel, lun, oneshotpg;
#ifdef JWDBG_CONV_FTL
	//printk("[JWDBG] %s: local_lpn: 0x%llx zofs: 0x%llx, zone size: 0x%lx\n",
	//			 __func__, local_lpn, zone_ofs, conv_ftl->ssd->sp.pgs_per_line);
#endif

	pg_per_oneshotpg = zone_ofs & cpp->bitmask_pg_per_oneshotpg;
	channel = (zone_ofs / cpp->divider_channel) & cpp->bitmask_channel;
	lun = (zone_ofs / cpp->divider_lun) & cpp->bitmask_lun; 
	//oneshotpg = (zone_ofs / cpp->divider_oneshotpg) & cpp->bitmask_oneshotpg;
	oneshotpg = (zone_ofs / cpp->divider_oneshotpg) % cpp->remainder_oneshotpg;

#ifdef JWDBG_CONV_FTL
	//printk("[JWDBG] pg_per_ospg: 0x%x channel: 0x%x, lun: 0x%x zone size: 0x%x\n",
	//			 pg_per_oneshotpg, channel, lun, oneshotpg);
#endif	
	ppa.g.ch = channel;
	ppa.g.lun = lun;
	ppa.g.pg = (oneshotpg * spp->pgs_per_oneshotpg) + pg_per_oneshotpg;
	//ppa.g.pl = wpp->pl;
	NVMEV_ASSERT(ppa.g.pl == 0);
	check_addr(ppa.g.pg, spp->pgs_per_blk);
	check_addr(ppa.g.ch, spp->nchs);
	check_addr(ppa.g.lun, spp->luns_per_ch);
	check_addr(ppa.g.blk, spp->blks_per_pl);
	
	return ppa;
}

static inline struct ppa fake_ppa(void)
{
	struct ppa fake_ppa;
	fake_ppa.ppa = UNMAPPED_PPA;
	return fake_ppa;
}

static inline bool mapped_ppa(struct ppa *ppa);

static inline struct ppa read_zone_mapping_handler(struct conv_ftl *conv_ftl, uint64_t local_lpn) 
{
	/* For migrated lba from coupled gc. Let aimless translator handle with it */
	if (behind_active_interval(conv_ftl, local_lpn))
		return fake_ppa();

	struct ppa *zone_map_ent = get_zone_maptbl_ent(conv_ftl, local_lpn);

	if (!mapped_ppa(zone_map_ent)) {
		/* to handle with f2fs's forward recovery */
		//NVMEV_INFO("[JWDBG] %s: read unmapped zone!! req lpn: 0x%llx\n", __func__, 
		//			local_lpn*SSD_PARTITIONS);
		return fake_ppa();
	}
	return calc_ppa_in_zone(conv_ftl, local_lpn, zone_map_ent);
}
#endif

#ifndef MULTI_PARTITION_FTL
static inline struct ppa get_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t lpn)
{
	return conv_ftl->maptbl[lpn];
}
#else

static inline bool out_of_partition(struct conv_ftl *conv_ftl, uint64_t local_lpn)
{
	static int print = 1;
	//static int cnt = 0;
	int no_partition = NO_LOCAL_PARTITION(local_lpn);
	struct window_mgmt *wm = &conv_ftl->wm[no_partition];

	/* due to coupled gc */
	if (behind_active_interval(conv_ftl, local_lpn))
		return true;
#ifdef COUPLED_GC
	uint64_t logical_zoneno = get_relational_zoneno(conv_ftl, local_lpn);
#else
	uint64_t logical_zoneno = get_logical_zoneno(conv_ftl, local_lpn);
#endif

	//if (cnt % 100000 == 0) {
	////if (conv_ftl->no_part != 0) {
	//	printk("%s: n_ftl: %u head zoneno: %llu zone no: %llu nzones_per_partitoin: %llu pgs_per_zone: %llu", 
	//		__func__, conv_ftl->no_part, conv_ftl->wm[no_partition].head_zoneno, 
	//		NO_ZONE(conv_ftl, local_lpn), conv_ftl->nzones_per_partition,
	//		conv_ftl->ssd->sp.pgs_per_line);
	//}
	//cnt ++;

	if (!(logical_zoneno < wm->nzones_per_partition)){
		if (print){
			NVMEV_INFO("[JWDBG] %s: local_lpn: 0x%llx logical_zoneno: %llu nzones_per_partition: %lu pgsPline: %lu PSA: 0x%llx\n",
					__func__, local_lpn, logical_zoneno, wm->nzones_per_partition,
					conv_ftl->ssd->sp.pgs_per_line, PARTITION_START_ADDR(local_lpn));
		}
		print = 0;
		return true;
	}

	return false;
}

#ifdef ZONE_MAPPING
static inline bool out_of_meta_partition(struct conv_ftl *conv_ftl, uint64_t local_lpn)
{
	return (OFFSET_LOCAL_META_PARTITION(local_lpn) >= conv_ftl->npages_meta);
}

static inline struct ppa read_meta_page_mapping_handler(struct conv_ftl *conv_ftl, uint64_t local_lpn)
{
	if (out_of_meta_partition(conv_ftl, local_lpn)){
		/* to handle with device mount and file system mount. */
		return fake_ppa();
	}
	return conv_ftl->maptbl[META_PARTITION][OFFSET_LOCAL_META_PARTITION(local_lpn)];
}
#else
static inline struct ppa get_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t local_lpn)
{
	return conv_ftl->maptbl[NO_LOCAL_PARTITION(local_lpn)][OFFSET_LOCAL_META_PARTITION(local_lpn)];
}
#endif
#endif

static inline void set_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t local_lpn, struct ppa *ppa)
{
#ifndef MULTI_PARTITION_FTL
	NVMEV_ASSERT(local_lpn < conv_ftl->ssd->sp.tt_pgs);
	conv_ftl->maptbl[local_lpn] = *ppa;
#else
	conv_ftl->maptbl[NO_LOCAL_PARTITION(local_lpn)][OFFSET_LOCAL_META_PARTITION(local_lpn)] = *ppa;
#endif
}

static inline void invalidate_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t local_lpn, 
		struct ppa *ppa)
{
#ifndef MULTI_PARTITION_FTL
	NVMEV_ASSERT(local_lpn < conv_ftl->ssd->sp.tt_pgs);
	conv_ftl->maptbl[local_lpn].ppa = INVALID_PPA;
#else
	conv_ftl->maptbl[NO_LOCAL_PARTITION(local_lpn)][OFFSET_LOCAL_META_PARTITION(local_lpn)].ppa 
		= INVALID_PPA;
#endif
}

static inline struct line *get_line(struct conv_ftl *conv_ftl, struct ppa *ppa);

#ifdef ZONE_MAPPING
static inline bool invalidate_zone_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t local_lpn,
		struct ppa *ppa) 
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line *line;
	struct ppa *zone_map_ent;
	line = get_line(conv_ftl, ppa);
	unsigned int no_part = NO_LOCAL_PARTITION(local_lpn);
	
	if (line->ipc == spp->pgs_per_line) {
		zone_map_ent = get_zone_maptbl_ent(conv_ftl, local_lpn);
		//print_dbg_get_zone_maptbl_ent(conv_ftl, local_lpn, __func__);
		//printk("%s: part: %d start lpn: 0x%lx ppa: 0x%lx line: 0x%lx", __func__, 
		//	   conv_ftl->no_part, 
		//	   LPN_FROM_LOCAL_LPN(line->start_local_lpn, conv_ftl->no_part, conv_ftl->ns->nr_parts),
		//	   zone_map_ent->ppa, line);
		NVMEV_ASSERT(mapped_ppa(zone_map_ent));
		(*zone_map_ent).ppa = INVALID_PPA; 
		
		//conv_ftl->valid_zone_cnt[no_part] --;
		if (conv_ftl->valid_zone_cnt[no_part] > 0) 
			conv_ftl->valid_zone_cnt[no_part] --;

		if (conv_ftl->total_valid_zone_cnt > 0) 
			conv_ftl->total_valid_zone_cnt --;

		if (IS_GC_PARTITION(no_part)){
			NVMEV_ASSERT(0);
			struct window_mgmt *wm = &conv_ftl->wm[no_part];
			//printk("%s: hello??????????", __func__);
			if (wftl_test_and_clear_bit(get_zone_idx(conv_ftl, local_lpn), (char *) wm->zone_bitmap)) {
				//printk("%s: zidx: %u slpn: 0x%lx", 
				//	__func__, get_zone_idx(conv_ftl, local_lpn), 
				//	LPN_FROM_LOCAL_LPN(local_lpn, conv_ftl->no_part, conv_ftl->ns->nr_parts));
				wm->free_zone ++;
			} else {
				printk("%s: not expected!!!!!!!!!!!!!!!!! zidx: %lu", __func__, 
						get_zone_idx(conv_ftl, local_lpn));
				NVMEV_ASSERT(0);
			}
		}

		return true;
	}
	else if (line->ipc > spp->pgs_per_line){
		printk("%s: no way!!! unexpected!!!!!!!!! partno: %u", __func__, NO_LOCAL_PARTITION(local_lpn));
		NVMEV_ASSERT(0);
	}
	return false;
}

static inline void invalidate_zone_maptbl_ent_from_gc(struct conv_ftl *conv_ftl, uint64_t local_lpn, 
		struct line *line) 
{
	struct ppa *zone_map_ent;
	zone_map_ent = get_zone_maptbl_ent(conv_ftl, local_lpn);
	//NVMEV_ASSERT(mapped_ppa(zone_map_ent));
	//printk("%s: part: %d start lpn: 0x%lx line: 0x%lx line wpc: %d line ipc: %d", __func__, 
	//	   conv_ftl->no_part, 
	//	   LPN_FROM_LOCAL_LPN(local_lpn, conv_ftl->no_part, conv_ftl->ns->nr_parts), 
	//	   line, line->wpc, line->ipc);
	zone_map_ent->ppa = INVALID_PPA;

	unsigned int no_part = NO_LOCAL_PARTITION(local_lpn);

	if (IS_GC_PARTITION(no_part)) {
		struct window_mgmt *wm = &conv_ftl->wm[no_part];
		//wftl_clear_bit(get_zone_idx(conv_ftl, local_lpn), (char *) wm->zone_bitmap);
		uint64_t zidx = get_zone_idx(conv_ftl, local_lpn);
		if (wm->remain_cnt_array[zidx] == 0) {
			if (wftl_test_and_clear_bit(zidx, (char *) wm->zone_bitmap)) {
				wm->free_zone ++;
				//printk("%s: how are you?", __func__);
				//printk("%s: zidx: %u slpn: 0x%lx", 
				//	__func__, get_zone_idx(conv_ftl, local_lpn), 
				//	LPN_FROM_LOCAL_LPN(local_lpn, conv_ftl->no_part, conv_ftl->ns->nr_parts));
			} 
			//else {
			//	NVMEV_ASSERT(0);
			//}
		}
	}

	return;
}
#endif

static uint64_t ppa2pgidx(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	uint64_t pgidx;

	NVMEV_DEBUG("ppa2pgidx: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n",
	    ppa->g.ch, ppa->g.lun, ppa->g.pl, ppa->g.blk, ppa->g.pg);

	pgidx = ppa->g.ch  * spp->pgs_per_ch  + \
	        ppa->g.lun * spp->pgs_per_lun + \
	        ppa->g.pl  * spp->pgs_per_pl  + \
	        ppa->g.blk * spp->pgs_per_blk + \
	        ppa->g.pg;

	NVMEV_ASSERT(pgidx < spp->tt_pgs);

	return pgidx;
}

static inline uint64_t get_rmap_ent(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(conv_ftl, ppa);

	return conv_ftl->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(conv_ftl, ppa);

	conv_ftl->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
	return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
	return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
	((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
	return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
	((struct line *)a)->pos = pos;
}

#ifndef MULTI_PARTITION_FTL
static inline void consume_write_credit(struct conv_ftl *conv_ftl)
#else
static inline void consume_write_credit(struct conv_ftl *conv_ftl, int no_partition)
#endif
{
#ifndef MULTI_PARTITION_FTL
	conv_ftl->wfc.write_credits--;
#else
	conv_ftl->wfc[no_partition].write_credits--;
#endif
}

#ifndef MULTI_PARTITION_FTL
static inline void check_and_refill_write_credit(struct conv_ftl *conv_ftl)
#elif defined COUPLED_GC_MTL
static inline void check_and_refill_write_credit(struct conv_ftl *conv_ftl, int no_partition, struct nvmev_result *ret)
#else
static inline void check_and_refill_write_credit(struct conv_ftl *conv_ftl, int no_partition)
#endif
{
#ifndef MULTI_PARTITION_FTL
	struct write_flow_control * wfc = &(conv_ftl->wfc);
#else
	struct write_flow_control * wfc = &(conv_ftl->wfc[no_partition]);
#endif

	if (wfc->write_credits <= 0) {
#ifndef MULTI_PARTITION_FTL
	    forground_gc(conv_ftl);
#elif defined COUPLED_GC_MTL
	    forground_gc(conv_ftl, no_partition, ret);
#else
		forground_gc(conv_ftl, no_partition);
#endif
	    wfc->write_credits += wfc->credits_to_refill;
	}
}

static void init_lines(struct conv_ftl *conv_ftl)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *line;
	int i;

	lm->tt_lines = spp->blks_per_pl;
	NVMEV_ASSERT(lm->tt_lines == spp->tt_lines);
	lm->lines = vmalloc(sizeof(struct line) * lm->tt_lines);

	INIT_LIST_HEAD(&lm->free_line_list);
	lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
	        victim_line_get_pri, victim_line_set_pri,
	        victim_line_get_pos, victim_line_set_pos);
	INIT_LIST_HEAD(&lm->full_line_list);

	lm->free_line_cnt = 0;
	for (i = 0; i < lm->tt_lines; i++) {
	    line = &lm->lines[i];
	    line->id = i;
	    line->ipc = 0;
	    line->vpc = 0;
	    line->pos = 0;
#ifndef GURANTEE_SEQ_WRITE
		line->wpc = 0;
#endif
#ifdef ZONE_MAPPING
		line->start_local_lpn = INVALID_LPN;
#endif
	    /* initialize all the lines as free lines */
		list_add_tail(&line->entry, &lm->free_line_list);
	    lm->free_line_cnt++;
		//printk("[JWDBG] %s: %dth line range %p", __func__, i, line);
	}

	//printk("[JWDBG] %s: line range %p ~ %p", __func__, 
	 //   &lm->lines[0], &lm->lines[lm->tt_lines-1]);

	NVMEV_ASSERT(lm->free_line_cnt == lm->tt_lines);
	lm->victim_line_cnt = 0;
	lm->full_line_cnt = 0;
}

static void init_write_pointer(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	struct write_pointer *wpp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *curline = NULL;

#ifndef MULTI_PARTITION_FTL
	if (io_type == USER_IO) {
	    wpp = &conv_ftl->wp;
	} else if (io_type == GC_IO) {
	    wpp = &conv_ftl->gc_wp;
	} else {
	    NVMEV_ASSERT(0);
	}

	curline = list_first_entry(&lm->free_line_list, struct line, entry);
	list_del_init(&curline->entry);
	lm->free_line_cnt--;

	/* wpp->curline is always our next-to-write super-block */
	wpp->curline = curline;
	wpp->ch = 0;
	wpp->lun = 0;
	wpp->pg = 0;
	wpp->blk = curline->id;
	wpp->pl = 0;
#else
	int i, n_partitions;

	if (io_type == USER_IO) {
	    wpp = conv_ftl->wp;
	    n_partitions = NO_USER_PARTITION;
	} else if (io_type == GC_IO) {
	    wpp = &conv_ftl->gc_wp;
	    n_partitions = 1; /* gc partition */
	} else {
	    NVMEV_ASSERT(0);
	}

	for (i = 0; i < n_partitions; i ++){
		curline = list_first_entry(&lm->free_line_list, struct line, entry);
		list_del_init(&curline->entry);
		lm->free_line_cnt--;

		/* wpp->curline is always our next-to-write super-block */
		wpp[i].curline = curline;
		wpp[i].ch = 0;
		wpp[i].lun = 0;
		wpp[i].pg = 0;
		wpp[i].blk = curline->id;
		wpp[i].pl = 0;
	}
#endif
}

static void init_write_flow_control(struct conv_ftl *conv_ftl) {
	struct ssdparams *spp = &conv_ftl->ssd->sp;
#ifndef MULTI_PARTITION_FTL
	struct write_flow_control * wfc = &(conv_ftl->wfc);

	wfc->write_credits = spp->pgs_per_line;
	wfc->credits_to_refill = spp->pgs_per_line;
#else
	int no_type;
	struct write_flow_control * wfc;
	for (no_type = META_PARTITION; no_type < NO_USER_PARTITION; no_type ++){
		wfc = &(conv_ftl->wfc[no_type]);
		wfc->write_credits = spp->pgs_per_line;
		wfc->credits_to_refill = spp->pgs_per_line;
	}
#endif
}

static inline void check_addr(int a, int max)
{
	NVMEV_ASSERT(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct conv_ftl *conv_ftl)
{
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *curline = NULL;

#ifdef LINE_PRINT
	if (lm->free_line_cnt  < 2) {
		printk("%s: free line cnt: %u", __func__, lm->free_line_cnt);
		if (lm->free_line_cnt == 0)
			NVMEV_ASSERT(0);
	}
#endif
	curline = list_first_entry(&lm->free_line_list, struct line, entry);
	if (!curline) {
	    NVMEV_ERROR("No free lines left in VIRT !!!!\n");
	    return NULL;
	}

	list_del_init(&curline->entry);
	lm->free_line_cnt--;
	NVMEV_DEBUG("[%s] free_line_cnt %d\n",__FUNCTION__, lm->free_line_cnt);
#ifdef LINE_PRINT
	//printk("%s: new_line: %p free_line_cnt: %d", __func__, curline, lm->free_line_cnt);
#endif
	return curline;
}

#ifndef MULTI_PARTITION_FTL
static void advance_write_pointer(struct conv_ftl *conv_ftl, uint32_t io_type)
#else
#ifdef GURANTEE_SEQ_WRITE
static void advance_write_pointer(struct conv_ftl *conv_ftl, uint32_t io_type, int no_partition)
#else
static void advance_write_pointer(struct conv_ftl *conv_ftl, uint32_t io_type, int no_partition, 
										struct ppa *ppa)
#endif
#endif
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct write_pointer *wpp;
#ifndef MULTI_PARTITION_FTL
	if (io_type == USER_IO) {
	    wpp = &conv_ftl->wp;
	} else if (io_type == GC_IO) {
	    wpp = &conv_ftl->gc_wp;
	} else {
	    NVMEV_ASSERT(0);
	}
#else
	if (io_type == USER_IO) {
	    wpp = &conv_ftl->wp[no_partition];
	} else if (io_type == GC_IO) {
	    wpp = &conv_ftl->gc_wp;
	} else {
	    NVMEV_ASSERT(0);
	}
#endif

	NVMEV_DEBUG("current wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n",
	    wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg);

	check_addr(wpp->pg, spp->pgs_per_blk);
	wpp->pg++;
	if ((wpp->pg % spp->pgs_per_oneshotpg) != 0) goto out;

	wpp->pg -= spp->pgs_per_oneshotpg;
	check_addr(wpp->ch, spp->nchs);
	wpp->ch++;
	if (wpp->ch != spp->nchs) goto out;

	wpp->ch = 0;
	check_addr(wpp->lun, spp->luns_per_ch);
	wpp->lun++;
	/* in this case, we should go to next lun */
	if (wpp->lun != spp->luns_per_ch) goto out;

	wpp->lun = 0;
	/* go to next wordline in the block */
	wpp->pg += spp->pgs_per_oneshotpg;
	if (wpp->pg != spp->pgs_per_blk) goto out;

	wpp->pg = 0;
	/* move current line to {victim,full} line list */
	if (wpp->curline->vpc == spp->pgs_per_line) {
		/* all pgs are still valid, move to full line list */
		NVMEV_ASSERT(wpp->curline->ipc == 0);
		list_add_tail(&wpp->curline->entry, &lm->full_line_list);
		lm->full_line_cnt++;
		NVMEV_DEBUG("wpp: move line to full_line_list\n");
		//printk("[JWDBG] %s: line %p to full_line_list\n", __func__, wpp->curline);
	} else {
		NVMEV_DEBUG("wpp: line is moved to victim list\n");
		//printk("[JWDBG] %s: line %p to victim list\n", __func__, wpp->curline);
		NVMEV_ASSERT(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
		/* there must be some invalid pages in this line */
		NVMEV_ASSERT(wpp->curline->ipc > 0);
		pqueue_insert(lm->victim_line_pq, wpp->curline);
		lm->victim_line_cnt++;
	}
	/* current line is used up, pick another empty line */
	check_addr(wpp->blk, spp->blks_per_pl);
free_line:
	wpp->curline = NULL;
	wpp->curline = get_next_free_line(conv_ftl);
#ifdef ZONE_MAPPING
	if (wpp->curline->start_local_lpn != INVALID_LPN) {
		printk("%s: no_ftl: %d line: %p 0x%llx start_local_lpn: 0x%llx, dst_partno: %u io_type: %u ipc:%u vpc: %u wpc: %u", __func__,
			   conv_ftl->no_part, wpp->curline, wpp->curline, wpp->curline->start_local_lpn, 
			   no_partition, io_type, 
			   wpp->curline->ipc, wpp->curline->vpc, wpp->curline->wpc);
		printk("%s: tt_lines: %u free lcnt: %u victim lcnt: %u full lcnt: %u tt lcnt: %u sum lcnt: %u", 
				__func__, conv_ftl->lm.tt_lines, 
				conv_ftl->lm.free_line_cnt, conv_ftl->lm.victim_line_cnt, 
				conv_ftl->lm.full_line_cnt, conv_ftl->lm.tt_lines, 
				conv_ftl->lm.free_line_cnt + conv_ftl->lm.victim_line_cnt + conv_ftl->lm.full_line_cnt
				);
		dump_stack();
		goto free_line;
		//wpp->curline = get_next_free_line(conv_ftl);
	}
	NVMEV_ASSERT(wpp->curline->start_local_lpn == INVALID_LPN);
#endif
	BUG_ON(!wpp->curline);
	NVMEV_DEBUG("wpp: got new clean line %d\n", wpp->curline->id);

	wpp->blk = wpp->curline->id;
	check_addr(wpp->blk, spp->blks_per_pl);

	/* make sure we are starting from page 0 in the super block */
	NVMEV_ASSERT(wpp->pg == 0);
	NVMEV_ASSERT(wpp->lun == 0);
	NVMEV_ASSERT(wpp->ch == 0);
	/* TODO: assume # of pl_per_lun is 1, fix later */
	NVMEV_ASSERT(wpp->pl == 0);
out:
	NVMEV_DEBUG("advanced wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d (curline %d)\n",
			wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg, wpp->curline->id);
}

#ifndef MULTI_PARTITION_FTL
static struct ppa get_new_page(struct conv_ftl *conv_ftl, uint32_t io_type)
#else
static struct ppa get_new_page(struct conv_ftl *conv_ftl, uint32_t io_type, int no_partition)
#endif
{
	struct write_pointer *wpp;
	struct ppa ppa;

#ifndef MULTI_PARTITION_FTL
	if (io_type == USER_IO) {
	    wpp = &conv_ftl->wp;
	} else if (io_type == GC_IO) {
	    wpp = &conv_ftl->gc_wp;
	} else {
	    NVMEV_ASSERT(0);
	}
#else
	if (io_type == USER_IO) {
	    wpp = &conv_ftl->wp[no_partition];
	} else if (io_type == GC_IO) {
	    wpp = &conv_ftl->gc_wp;
	} else {
	    NVMEV_ASSERT(0);
	}
#endif
	ppa.ppa = 0;
	ppa.g.ch = wpp->ch;
	ppa.g.lun = wpp->lun;
	ppa.g.pg = wpp->pg;
	ppa.g.blk = wpp->blk;
	ppa.g.pl = wpp->pl;
	NVMEV_ASSERT(ppa.g.pl == 0);

	return ppa;
}

#ifdef MULTI_PARTITION_MTL
static void init_mtl(struct mtl_zone_entry *_mtl_zent)
{
	int i;
	_mtl_zent->zone_info.nr_inv_pgs = 0;
	_mtl_zent->zone_info.nr_v_pgs = 0;
	for (i = 0; i < PGS_PER_MTL_ZONE; i++){
		_mtl_zent->map_table[i] = INVALID_MAPPING;
	}
}
#endif

static void init_maptbl(struct conv_ftl *conv_ftl)
{
	int i;
	struct ssdparams *spp = &conv_ftl->ssd->sp;

#ifndef MULTI_PARTITION_FTL
	conv_ftl->maptbl = vmalloc(sizeof(struct ppa) * spp->tt_pgs);
	for (i = 0; i < spp->tt_pgs; i++) {
	    conv_ftl->maptbl[i].ppa = UNMAPPED_PPA;
	}
#else
	int no_type;
	#ifdef ZONE_MAPPING
		#ifdef JWDBG_CONV_FTL
	NVMEV_INFO("[JWDBG] %s: ssd total pgs: %ld capacity: %ld GB \n", 
				__func__, spp->tt_pgs, spp->tt_pgs * spp->pgsz / 1024/1024 / 1024);
		#endif
	/* metadata partition (page mapping) */
	unsigned long meta_pgs = conv_ftl->npages_meta;
		#ifdef EQUAL_IM_MEM
	unsigned long main_pgs = conv_ftl->npages_main;
	uint64_t mem_consumption = 0;
		#endif
	conv_ftl->maptbl[META_PARTITION] = vmalloc(sizeof(struct ppa) * meta_pgs);
	for (i = 0; i < meta_pgs; i++) {
	    conv_ftl->maptbl[META_PARTITION][i].ppa = UNMAPPED_PPA;
	}
	/* data partition and gc partition (zone mapping) */
	unsigned long nzones_per_partition = conv_ftl->nzones_per_partition;
	unsigned long nzones_per_gc_partition = conv_ftl->nzones_per_gc_partition;
	for (no_type = META_PARTITION + 1; no_type < NO_TYPE; no_type ++){
		if (IS_HOST_PARTITION(no_type)) {
			conv_ftl->maptbl[no_type] = vmalloc(sizeof(struct ppa) * nzones_per_partition);
		#ifdef EQUAL_IM_MEM
			mem_consumption += (sizeof(struct ppa) * nzones_per_partition);
		#endif
			for (i = 0; i < nzones_per_partition; i++) {
				conv_ftl->maptbl[no_type][i].zone_start_ppa = UNMAPPED_ZONENO;
			}
		} else if (IS_GC_PARTITION(no_type)) {
			conv_ftl->maptbl[no_type] = vmalloc(sizeof(struct ppa) * nzones_per_gc_partition);
		#ifdef EQUAL_IM_MEM
			mem_consumption += (sizeof(struct ppa) * nzones_per_gc_partition);
		#endif
			for (i = 0; i < nzones_per_gc_partition; i++) {
				conv_ftl->maptbl[no_type][i].zone_start_ppa = UNMAPPED_ZONENO;
			}
		}
	}

		#ifdef EQUAL_IM_MEM
	uint64_t IM_mem = 0;
	//for (no_type = META_PARTITION + 1; no_type < NO_TYPE_IM; no_type ++){
	//	IM_mem += (sizeof(struct ppa) * main_pgs);
	//}
	IM_mem += (sizeof(struct ppa) * main_pgs * (NO_TYPE_IM-1-3));
	//NVMEV_ASSERT(IM_mem > mem_consumption);
	if (IM_mem > mem_consumption) {
		printk("%s: redundant: %u MB", __func__, (IM_mem-mem_consumption)/1024/1024);
		conv_ftl->redundant = vmalloc(IM_mem - mem_consumption);
		NVMEV_ASSERT(conv_ftl->redundant != NULL);
	} else {
		printk("%s: mem consumption higher than IM", __func__);
	}
		#endif

	#else
	for (no_type = 0; no_type < NO_TYPE; no_type ++){
		conv_ftl->maptbl[no_type] = vmalloc(sizeof(struct ppa) * spp->tt_pgs);
		for (i = 0; i < spp->tt_pgs; i++) {
		    conv_ftl->maptbl[no_type][i].ppa = UNMAPPED_PPA;
		}
	}
	#endif
#endif
}

static void init_rmap(struct conv_ftl *conv_ftl)
{
	int i;
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	conv_ftl->rmap = vmalloc(sizeof(uint64_t) * spp->tt_pgs);
	for (i = 0; i < spp->tt_pgs; i++) {
	    conv_ftl->rmap[i] = INVALID_LPN;
	}
}

//#ifdef COUPLED_GC
static inline void init_window_mgmt(struct conv_ftl *conv_ftl)
{
	int i;
	uint64_t local_start_addr;
	uint64_t bitmap_size = (conv_ftl->nzones_per_gc_partition % sizeof(unsigned long))?
		conv_ftl->nzones_per_gc_partition / sizeof(unsigned long) + sizeof(unsigned long) :
		conv_ftl->nzones_per_gc_partition / sizeof(unsigned long);
	uint64_t remain_array_size = (conv_ftl->nzones_per_gc_partition * sizeof(uint16_t));

	for (i = META_PARTITION + 1; i < NO_USER_PARTITION; i ++){
		if (IS_HOST_PARTITION(i)) {
			conv_ftl->wm[i].nzones_per_partition = conv_ftl->nzones_per_partition;
			conv_ftl->wm[i].zone_bitmap = NULL;
			conv_ftl->wm[i].remain_cnt_array = NULL;
		}
		else if (IS_GC_PARTITION(i)) {
			conv_ftl->wm[i].nzones_per_partition = conv_ftl->nzones_per_gc_partition;
			if ((conv_ftl->wm[i].zone_bitmap = (unsigned long *) kzalloc(bitmap_size, GFP_KERNEL)) == NULL) {
				NVMEV_ASSERT(0);
			}
			conv_ftl->wm[i].free_zone = conv_ftl->nzones_per_gc_partition;
			if ((conv_ftl->wm[i].remain_cnt_array = 
						(uint16_t *) kzalloc(remain_array_size, GFP_KERNEL)) == NULL) {
				NVMEV_ASSERT(0);
			}
		}



		local_start_addr = LOCAL_PARTITION_START_ADDR_FROM_PARTITION_NO(i);
		conv_ftl->wm[i].next_local_lpn = local_start_addr;
		conv_ftl->wm[i].head_zoneno = NO_ZONE(conv_ftl, local_start_addr);
		conv_ftl->wm[i].head_idx = 0;
		conv_ftl->wm[i].tail_zoneno = NO_ZONE(conv_ftl, local_start_addr);
		conv_ftl->wm[i].tail_idx = 0;
		//printk("%s type: %d head_zoneno: %llu tail_zoneno: %llu next_local_lpn: %llu head_idx: %d tail_idx: %d nzones_per_partition: %lu", 
		//		__func__, i, conv_ftl->wm[i].head_zoneno, conv_ftl->wm[i].tail_zoneno, 
		//  		local_start_addr, conv_ftl->wm[i].head_idx, conv_ftl->wm[i].tail_idx, 
		//		conv_ftl->wm[i].nzones_per_partition);
#ifdef COUPLED_GC_PRINT
		//printk("%s type: %d head_zoneno: %llu tail_zoneno: %llu next_local_lpn: %llu head_idx: %d tail_idx: %d", 
		//		__func__, i, conv_ftl->wm[i].head_zoneno, conv_ftl->wm[i].tail_zoneno, 
		//  		local_start_addr, conv_ftl->wm[i].head_idx, conv_ftl->wm[i].tail_idx);
#endif
		if (IS_GC_PARTITION(i)) {
			wftl_set_bit(get_zone_idx(conv_ftl, conv_ftl->wm[i].next_local_lpn), 
					(char *) conv_ftl->wm[i].zone_bitmap);
			conv_ftl->wm[i].free_zone --;
		}
	}
}
//#endif

static void conv_init_ftl(struct conv_ftl *conv_ftl, struct convparams *cpp, struct ssd *ssd, uint32_t no_part, struct nvmev_ns *ns)
{
	/*copy convparams*/
	conv_ftl->cp = *cpp;

	conv_ftl->ssd = ssd;

	conv_ftl->no_part = no_part;
	conv_ftl->ns = ns;
	printk("%s: conv ftl: no_part: %u", __func__, conv_ftl->no_part);

#ifdef ZONE_MAPPING
	conv_ftl->npages_meta = NPAGES_META(ssd->sp);
#ifdef EQUAL_IM_MEM
	conv_ftl->npages_main = NPAGES_MAIN(ssd->sp); /* to equalize mem consumption with interval map*/
#endif
	conv_ftl->nzones_per_partition = NZONES_PER_PARTITION(ssd->sp);
	conv_ftl->nzones_per_gc_partition = NZONES_PER_GC_PARTITION(ssd->sp);
#endif

	/* initialize maptbl */
	NVMEV_INFO("initialize maptbl\n");
	init_maptbl(conv_ftl); // mapping table

	/* initialize rmap */
	NVMEV_INFO("initialize rmap\n");
	init_rmap(conv_ftl); // reverse mapping table (?)

	/* initialize all the lines */
	NVMEV_INFO("initialize lines\n");
	init_lines(conv_ftl);

	/* initialize write pointer, this is how we allocate new pages for writes */
	NVMEV_INFO("initialize write pointer\n");
	init_write_pointer(conv_ftl, USER_IO);
	init_write_pointer(conv_ftl, GC_IO);

	init_write_flow_control(conv_ftl);

//#ifdef COUPLED_GC
	init_window_mgmt(conv_ftl);
//#endif
	int i;
#ifdef MULTI_PARTITION_FTL
	for (i = 0; i < NO_USER_PARTITION; i ++) {
		conv_ftl->valid_zone_cnt[i] = 0;
		conv_ftl->gc_free_zone_cnt[i] = 0;
	}
#endif
	
	NVMEV_INFO("Init FTL Instance with %d channels(%ld pages)\n",  conv_ftl->ssd->sp.nchs, conv_ftl->ssd->sp.tt_pgs);
	conv_ftl->total_valid_zone_cnt = 0;

	return;
}

#ifndef ZONE_MAPPING
static void conv_init_params(struct convparams *cpp)
#else
static void conv_init_params(struct convparams *cpp, struct ssdparams *spp)
#endif
{
	cpp->op_area_pcent = OP_AREA_PERCENT;
	cpp->gc_thres_lines = 5; /* Need only two lines.(host write, gc)*/
	cpp->gc_thres_lines_high = 5; /* Need only two lines.(host write, gc)*/
	//cpp->gc_thres_lines = 2; /* Need only two lines.(host write, gc)*/
	//cpp->gc_thres_lines_high = 2; /* Need only two lines.(host write, gc)*/
	cpp->enable_gc_delay = 1;
	cpp->pba_pcent = (int)((1 + cpp->op_area_pcent) * 100);

#ifdef ZONE_MAPPING
	/* for calculating ppa in zone */
	printk("[JWDBG] %s: pgsz: %d ps_per_oneshotpg: %d nchs: %d luns_per_ch: %d pgs_per_blk: %d\n",
		__func__, spp->pgsz, spp->pgs_per_oneshotpg, spp->nchs, spp->luns_per_ch ,spp->pgs_per_blk);
	cpp->bitmask_pg_per_oneshotpg = spp->pgs_per_oneshotpg - 1;
	cpp->bitmask_channel = spp->nchs - 1;
	cpp->bitmask_lun = spp->luns_per_ch - 1;
	cpp->bitmask_oneshotpg = spp->pgs_per_blk / spp->pgs_per_oneshotpg - 1;
	cpp->remainder_oneshotpg = spp->pgs_per_blk / spp->pgs_per_oneshotpg;

	cpp->divider_channel = spp->pgs_per_oneshotpg;
	cpp->divider_lun	 = spp->pgs_per_oneshotpg * spp->nchs;
	cpp->divider_oneshotpg = spp->pgs_per_oneshotpg * spp->nchs * spp->luns_per_ch;
#endif

}

#ifdef MULTI_PARTITION_MTL
/* create free mem page list. */
void init_free_mem_page_list(struct nvmev_ns *ns)
{
	uint32_t nlba = ns->size / PAGE_SIZE;
	int i;
	MEM_PAGE_ENTRY *MPE;

	for (i = 0; i < nlba; i ++){
		MPE = (MEM_PAGE_ENTRY *) kmalloc(sizeof(MEM_PAGE_ENTRY), GFP_KERNEL);
		MPE->mem_addr = i * PAGE_SIZE;
		list_push_back(&ns->free_mem_page_list, &MPE->list_elem);
	}
}
#endif

void conv_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr, uint32_t cpu_nr_dispatcher)
{
	struct ssdparams spp;
	struct convparams cpp;
	struct conv_ftl *conv_ftls;
	struct ssd *ssd;
	uint64_t i;
	const uint32_t nr_parts = SSD_PARTITIONS;
#ifdef MULTI_PARTITION_MTL
	uint64_t ii, n_mtl_zones, n_mtl_meta_zones, n_mtl_gc_zones, 
			 gc_window_size, meta_window_size;
#endif

	ssd_init_params(&spp, size, nr_parts);

#ifndef ZONE_MAPPING
	conv_init_params(&cpp);
#else
	conv_init_params(&cpp, &spp);
#endif

	conv_ftls = kmalloc(sizeof(struct conv_ftl) * nr_parts, GFP_KERNEL);

	for (i = 0; i < nr_parts; i++) {
	    ssd = kmalloc(sizeof(struct ssd), GFP_KERNEL);
	    ssd_init(ssd, &spp, cpu_nr_dispatcher);
	    conv_init_ftl(&conv_ftls[i], &cpp, ssd, i, ns);
	}


	/* PCIe, Write buffer are shared by all instances*/
	for (i = 1; i < nr_parts; i++) {
	    kfree(conv_ftls[i].ssd->pcie);
	    kfree(conv_ftls[i].ssd->write_buffer);

	    conv_ftls[i].ssd->pcie = conv_ftls[0].ssd->pcie;
	    conv_ftls[i].ssd->write_buffer = conv_ftls[0].ssd->write_buffer;
	}

	ns->id = id;
	ns->csi = NVME_CSI_NVM;
	ns->nr_parts = nr_parts;
	ns->ftls = (void *)conv_ftls;
	ns->size = (uint64_t)((size * 100) / cpp.pba_pcent);
	ns->mapped = mapped_addr;
	/*register io command handler*/
	ns->proc_io_cmd = conv_proc_nvme_io_cmd;
#ifdef MIGRATION_IO
	ns->proc_rev_io_cmd = conv_proc_nvme_rev_io_cmd;
#endif

	//ns->n_gc_log_max = ns->size / PAGE_SIZE * sizeof(uint64_t) / sizeof(struct gc_log) 
	//	* RATIO_OF_GC_LOG_TO_PAGE_MAP / 100;
	ns->n_gc_log_max = ns->size / PAGE_SIZE * sizeof(uint32_t) / 
		( (sizeof(struct gc_log) - sizeof(struct hlist_node)) / 2)
		* RATIO_OF_GC_LOG_TO_PAGE_MAP / 100;
	//printk("%s: n_gc_log_max: %u gc_log size: %u", __func__, ns->n_gc_log_max, 
	//		sizeof(struct gc_log));

#ifdef COUPLED_GC
	struct gc_log_mgmt *gclm;
	gclm = kmalloc(sizeof(struct gc_log_mgmt), GFP_KERNEL);
	NVMEV_ASSERT(gclm != NULL);
	init_gc_log_mgmt(gclm);
	build_free_gc_log(gclm);
	ns->gclm = gclm;
#ifdef COUPLED_GC_MTL
	init_mtl_migration_mgmt(ns);
	init_mtl_translation_mgmt(ns);
#endif
#endif

//#ifdef MULTI_PARTITION_MTL

#ifdef EQUAL_IM_MEM
	uint64_t window_size_im, n_mtl_zones_im;
	window_size_im = ns->size * IM_WINDOW_EXT_RATE / PAGE_SIZE * sizeof(MTL_ENTRY);
	n_mtl_zones_im = (window_size_im % MTL_ZONE_SIZE)? 
	                 	window_size_im / MTL_ZONE_SIZE + 1:
						window_size_im / MTL_ZONE_SIZE;

	n_mtl_zones_im = n_mtl_zones_im + n_mtl_zones_im / 5;
	uint64_t mem_consump = 0, IM_mem = 0;
	IM_mem += (sizeof(struct mtl_zone_entry *) * n_mtl_zones_im * (NO_TYPE_IM-1 - 3)); // -3: -1 for cold data partition, -2 for 1/3 cold node, 1/3 hot node and 1/3 hot data
	IM_mem += (sizeof(struct mtl_zone_entry) * n_mtl_zones_im * (NO_TYPE_IM-1 - 3));
#endif

#ifdef MULTI_PARTITION_MTL
	/* JW: build memory mapping table */
	ns->window_size = ns->size * WINDOW_EXT_RATE / PAGE_SIZE * sizeof(MTL_ENTRY);

	/* JW: Devide mtls into MTL_ZONE_SIZE, since kmalloc can not allocate huge space */
	n_mtl_zones = (ns->window_size % MTL_ZONE_SIZE)? 
						ns->window_size / MTL_ZONE_SIZE + 1:
						ns->window_size / MTL_ZONE_SIZE;

	/* A bit enlarge n_mtl_zones to have same n_mtl_zones with gc partiiton. */
	n_mtl_zones = n_mtl_zones + n_mtl_zones / 5;


	ns->window_size = n_mtl_zones * MTL_ZONE_SIZE;
	ns->n_mtl_zones = n_mtl_zones;

	//meta_window_size = ns->size / META_PARTITION_RATE / PAGE_SIZE * sizeof(MTL_ENTRY);
	meta_window_size = ns->size / PAGE_SIZE * sizeof(MTL_ENTRY);
	n_mtl_meta_zones = (meta_window_size % MTL_ZONE_SIZE)?
						meta_window_size / MTL_ZONE_SIZE + 1:
						meta_window_size / MTL_ZONE_SIZE;

	gc_window_size = ns->size / PAGE_SIZE * sizeof(MTL_ENTRY);
	//n_mtl_gc_zones = (gc_window_size % MTL_ZONE_SIZE)?
	//					gc_window_size / MTL_ZONE_SIZE + 1:
	//					gc_window_size / MTL_ZONE_SIZE;
	
	//n_mtl_gc_zones = n_mtl_gc_zones + n_mtl_gc_zones / 2;
	//ns->n_mtl_gc_zones = n_mtl_gc_zones + n_mtl_gc_zones / 2;

	//n_mtl_gc_zones = n_mtl_zones + n_mtl_zones / 5;
	n_mtl_gc_zones = n_mtl_zones;
	ns->n_mtl_gc_zones = n_mtl_gc_zones;

    for (i = 0; i < NO_TYPE; i++){
		/* JW: init ith mtls */
		/* TODO: need to redesign for meta mtl */
		if (IS_META_PARTITION(i)) {
			if ((ns->mtls[i] = kmalloc(sizeof(struct mtl_zone_entry *) * n_mtl_meta_zones , GFP_KERNEL)) == NULL)
				NVMEV_ERROR("[JWDBG] kmalloc return NULL. %lld window chunk set sz: %lldKB\n", 
								i, n_mtl_zones * sizeof(void *)/1024 );
//
			/* JW: init mtls in unit of MTL_ZONE_SIZE */
			for (ii = 0; ii < n_mtl_meta_zones; ii++){
    			if ((ns->mtls[i][ii] = kmalloc(sizeof(struct mtl_zone_entry), GFP_KERNEL)) == NULL)
					NVMEV_ERROR("[JWDBG] kmalloc return NULL. %lld sz: %ldKB\n", i, sizeof(struct mtl_zone_entry)/1024);
				struct mtl_zone_entry *__mtl;
				__mtl =  ns->mtls[i][ii];
				init_mtl(ns->mtls[i][ii]);
				//printk("%s: init_mtl type: %llu mtl_zoneno: %llu mtl: %p nr_inv_pgs: %u", 
				//		__func__, i, ii, ns->mtls[i][ii], __mtl->zone_info.nr_inv_pgs);
			}

		} else if (IS_HOST_PARTITION(i)) {
			if ((ns->mtls[i] = kmalloc(sizeof(struct mtl_zone_entry *) * n_mtl_zones, GFP_KERNEL)) == NULL)
				NVMEV_ERROR("[JWDBG] kmalloc return NULL. %lld window chunk set sz: %lldKB\n", 
								i, n_mtl_zones * sizeof(void *)/1024 );

			/* JW: init mtls in unit of MTL_ZONE_SIZE */
			for (ii = 0; ii < n_mtl_zones; ii++){
    			if ((ns->mtls[i][ii] = kmalloc(sizeof(struct mtl_zone_entry), GFP_KERNEL)) == NULL)
					NVMEV_ERROR("[JWDBG] kmalloc return NULL. %lld sz: %ldKB\n", i, sizeof(struct mtl_zone_entry)/1024);
				struct mtl_zone_entry *__mtl;
				__mtl =  ns->mtls[i][ii];
				init_mtl(ns->mtls[i][ii]);
				//printk("%s: init_mtl type: %llu mtl_zoneno: %llu mtl: %p nr_inv_pgs: %u", 
				//		__func__, i, ii, ns->mtls[i][ii], __mtl->zone_info.nr_inv_pgs);
			}

#ifdef EQUAL_IM_MEM
			mem_consump += (sizeof(struct mtl_zone_entry *) * n_mtl_zones);
			mem_consump += (sizeof(struct mtl_zone_entry) * n_mtl_zones);
#endif
		} else if (IS_GC_PARTITION(i)) {
			if ((ns->mtls[i] = kmalloc(sizeof(struct mtl_zone_entry *) * n_mtl_gc_zones, GFP_KERNEL)) == NULL)
				NVMEV_ERROR("[JWDBG] kmalloc return NULL. %lld window chunk set sz: %lldKB\n", 
								i, n_mtl_gc_zones * sizeof(void *)/1024 );

			/* JW: init mtls in unit of MTL_ZONE_SIZE */
			for (ii = 0; ii < n_mtl_gc_zones; ii++){
    			if ((ns->mtls[i][ii] = kmalloc(sizeof(struct mtl_zone_entry), GFP_KERNEL)) == NULL)
					NVMEV_ERROR("[JWDBG] kmalloc return NULL. %lld sz: %ldKB\n", i, sizeof(struct mtl_zone_entry)/1024);
				struct mtl_zone_entry *__mtl;
				__mtl =  ns->mtls[i][ii];
				init_mtl(ns->mtls[i][ii]);
				//printk("%s: init_mtl type: %llu mtl_zoneno: %llu mtl: %p nr_inv_pgs: %u", 
				//		__func__, i, ii, ns->mtls[i][ii], __mtl->zone_info.nr_inv_pgs);
			}
#ifdef EQUAL_IM_MEM
			mem_consump += (sizeof(struct mtl_zone_entry *) * n_mtl_gc_zones);
			mem_consump += (sizeof(struct mtl_zone_entry) * n_mtl_gc_zones);
#endif

		}
		ns->start_zoneno[i] = 0;
    }

	/* JW: build free page list */
	list_init(&ns->free_mem_page_list);
	init_free_mem_page_list(ns);
#endif

	NVMEV_INFO("FTL physical space: %lld, logical space: %lld (physical/logical * 100 = %d)\n", size, ns->size, cpp.pba_pcent);
	//printk("%s: MTL_ZONE_SIZE: %lu MAX_KMALLOC_SIZE: %u sz_mtl_zone_info: %lu sz_MTL_ENTRY: %lu", 
	//		__func__, MTL_ZONE_SIZE, MAX_KMALLOC_SIZE, sizeof(struct mtl_zone_info), sizeof(MTL_ENTRY));

#ifdef WAF
	ns->last_t = 0;
	ns->write_volume_host = 0;
	ns->write_volume_gc = 0;
	ns->total_write_volume_host = 0;
	ns->total_write_volume_gc = 0;
#ifdef HOST_GC_OVERHEAD_ANALYSIS    
    ns->last_t_host_gc_analy = 0;
   	ns->req_cnt = 0;             
#endif                              
#endif
#ifdef CHIP_UTIL
	ns->last_t_chip_util = 0;
#endif

#ifdef MG_CMD_CNT
	ns->mg_cmd_cnt = 0;
	ns->total_mg_cmd_cnt = 0;
	ns->discarded_gc_log = 0;
#endif

#ifdef CMD_CNT
	ns->total_write_blks_host = 0; 
	ns->total_read_blks_host = 0; 
	ns->total_discard_cmds_host = 0; 
#endif
#ifdef CHIP_UTIL
	ns->nand_idle_t_sum = 0;
	ns->nand_active_t_sum = 0;
	ns->avg_nand_idle_t_sum_total = 0;
	ns->avg_nand_active_t_sum_total = 0;
#endif

#ifdef EQUAL_IM_MEM
	//uint64_t htable_mem = 2 * (1<<HBITS_AIMLESS_TRANSLATOR) * sizeof(struct hlist_head);
	//uint64_t gc_log_mem = ns->n_gc_log_max * sizeof(struct gc_log);
	//gc_log_mem += (gc_log_mem * 4 / 100);
	if (IM_mem > mem_consump) {
	//NVMEV_ASSERT(IM_mem > mem_consump + htable_mem + gc_log_mem);
		printk("%s: mtl redundant: %u MB", __func__, (IM_mem-mem_consump)/1024/1024);
		ns->mtl_redundant = vmalloc(IM_mem - mem_consump);
		NVMEV_ASSERT(ns->mtl_redundant != NULL);
	} else {
		printk("%s: mem consumption higher than IM", __func__);
	}

#endif

	return;
}

static inline bool valid_ppa(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	int ch = ppa->g.ch;
	int lun = ppa->g.lun;
	int pl = ppa->g.pl;
	int blk = ppa->g.blk;
	int pg = ppa->g.pg;
	//int sec = ppa->g.sec;

	if (ch < 0 || ch >= spp->nchs) return false;
	if (lun < 0 || lun >= spp->luns_per_ch) return false;
	if (pl < 0 || pl >= spp->pls_per_lun) return false;
	if (blk < 0 || blk >= spp->blks_per_pl) return false;
	if (pg < 0 || pg >= spp->pgs_per_blk) return false;

	return true;
}

static inline bool valid_lpn(struct conv_ftl *conv_ftl, uint64_t lpn)
{
#if (defined MULTI_PARTITION_FTL || defined ZONE_MAPPING)
	/* TODO */
	return true;
#else
	return (lpn < conv_ftl->ssd->sp.tt_pgs);
#endif
}

static inline bool mapped_ppa(struct ppa *ppa)
{
	return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct line *get_line(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	return &(conv_ftl->lm.lines[ppa->g.blk]);
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	bool was_full_line = false;
	struct line *line;

	/* update corresponding page status */
	pg = get_pg(conv_ftl->ssd, ppa);
	if (pg->status != PG_VALID){
		printk("[JWDBG] %s: ppa: 0x%llx pg status: %d", __func__, ppa->ppa, pg->status);
	}

	NVMEV_ASSERT(pg->status == PG_VALID);
	pg->status = PG_INVALID;

	/* update corresponding block status */
	blk = get_blk(conv_ftl->ssd, ppa);
	NVMEV_ASSERT(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
	blk->ipc++;
	NVMEV_ASSERT(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
	blk->vpc--;

#ifdef SHIVAL2
	total_valid_blks -= 1;
#endif

	/* update corresponding line status */
	line = get_line(conv_ftl, ppa);
	if (!(line->ipc >= 0 && line->ipc < spp->pgs_per_line)){
		printk("[JWDBG] %s: ipc is %d", __func__, line->ipc);
	}
	NVMEV_ASSERT(line->ipc >= 0 && line->ipc < spp->pgs_per_line);

#ifdef GURANTEE_SEQ_WRITE
	if ((line->wpc == spp->pgs_per_line && line->ipc == 0){
	    was_full_line = true;
	}
#endif
	if (line->vpc == spp->pgs_per_line) {
	    NVMEV_ASSERT(line->ipc == 0);
	    was_full_line = true;
	}
	line->ipc++;
	NVMEV_ASSERT(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
	/* Adjust the position of the victime line in the pq under over-writes */
	if (line->pos) {
	    /* Note that line->vpc will be updated by this call */
		//printk("[JWDBG] %s: line %p is be changed in victim", __func__, line);
	    pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
	} else {
	    line->vpc--;
	}

	if (was_full_line) {
	    /* move line: "full" -> "victim" */
		//printk("[JWDBG] %s: line %p from full to victim", __func__, line);
	    list_del_init(&line->entry);
	    lm->full_line_cnt--;
	    pqueue_insert(lm->victim_line_pq, line);
	    lm->victim_line_cnt++;
	}
}

static void check_mark_page_valid(struct conv_ftl *conv_ftl, struct ppa *ppa, uint64_t local_lpn)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	struct line *line;

	/* update page status */
	pg = get_pg(conv_ftl->ssd, ppa);
	//NVMEV_ASSERT(pg->status == PG_FREE);
	if (!(pg->status == PG_FREE)){
		NVMEV_INFO("[JWDBG] %s: local_lpn: 0x%llx lpn: 0x%llx segno: %lu ppa: %llx %llu status: %d\n", 
				__func__, local_lpn, local_lpn * 4, local_lpn * 4 / 512, ppa->ppa, ppa->ppa,
					pg->status);
		NVMEV_INFO("[JWDBG] %s: local_lpn: %llx %llu %lld ppa: %llx %llu status: %d\n", 
				__func__, LOCAL_PARTITION_START_ADDR(local_lpn), 
				LOCAL_PARTITION_START_ADDR(local_lpn), LOCAL_PARTITION_START_ADDR(local_lpn), 
				ppa->ppa, ppa->ppa,	pg->status);
		print_dbg_get_zone_maptbl_ent(conv_ftl, local_lpn, __func__);
	}
	NVMEV_ASSERT(pg->status == PG_FREE);

}

static void mark_page_valid(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	struct line *line;

	/* update page status */
	pg = get_pg(conv_ftl->ssd, ppa);
	//NVMEV_ASSERT(pg->status == PG_FREE);
	if (!(pg->status == PG_FREE))
		NVMEV_INFO("[JWDBG] %s: ppa: %llx %lld status: %d\n", __func__, ppa->ppa, ppa->ppa,
					pg->status);
	NVMEV_ASSERT(pg->status == PG_FREE);
	pg->status = PG_VALID;

	/* update corresponding block status */
	blk = get_blk(conv_ftl->ssd, ppa);
	NVMEV_ASSERT(blk->vpc >= 0 && blk->vpc < spp->pgs_per_blk);
	blk->vpc++;

	/* update corresponding line status */
	line = get_line(conv_ftl, ppa);
	NVMEV_ASSERT(line->vpc >= 0 && line->vpc < spp->pgs_per_line);
	line->vpc++;
#ifndef GURANTEE_SEQ_WRITE
	line->wpc++;
#endif
#ifdef SHIVAL2
	total_valid_blks += 1;
#endif
}

static void mark_block_free(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_block *blk = get_blk(conv_ftl->ssd, ppa);
	struct nand_page *pg = NULL;
	int i;

	for (i = 0; i < spp->pgs_per_blk; i++) {
	    /* reset page status */
	    pg = &blk->pg[i];
	    NVMEV_ASSERT(pg->nsecs == spp->secs_per_pg);
	    pg->status = PG_FREE;
	}

	/* reset block status */
	NVMEV_ASSERT(blk->npgs == spp->pgs_per_blk);
	blk->ipc = 0;
	blk->vpc = 0;
	blk->erase_cnt++;
}

//static void gc_read_page(struct conv_ftl *conv_ftl, struct ppa *ppa)
//{
//	struct ssdparams *spp = &conv_ftl->ssd->sp;
//	struct convparams *cpp = &conv_ftl->cp;
//	/* advance conv_ftl status, we don't care about how long it takes */
//	if (cpp->enable_gc_delay) {
//	    struct nand_cmd gcr;
//	    gcr.type = GC_IO;
//	    gcr.cmd = NAND_READ;
//	    gcr.stime = 0;
//	    gcr.xfer_size = spp->pgsz;
//	    gcr.interleave_pci_dma = false;
//	    gcr.ppa = ppa;
//	    ssd_advance_nand(conv_ftl->ssd, &gcr);
//	}
//}

#ifdef COUPLED_GC
static uint64_t gc_write_meta_page(struct conv_ftl *conv_ftl, struct ppa *old_ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	struct ppa new_ppa;
	uint64_t lpn = get_rmap_ent(conv_ftl, old_ppa);
	int no_partition = NO_LOCAL_PARTITION(lpn);

	NVMEV_ASSERT(valid_lpn(conv_ftl, lpn));
	new_ppa = get_new_page(conv_ftl, GC_IO, no_partition);

	/* update maptbl */
	set_maptbl_ent(conv_ftl, lpn, &new_ppa);

	/* update rmap */
	set_rmap_ent(conv_ftl, lpn, &new_ppa);

	mark_page_valid(conv_ftl, &new_ppa);

#ifdef SHIVAL2
	total_valid_blks -= 1;
#endif

	/* need to advance the write pointer here */
#ifdef GURANTEE_SEQ_WRITE
	advance_write_pointer(conv_ftl, GC_IO, no_partition);
#else
	struct ppa *trash_ppa;
	advance_write_pointer(conv_ftl, GC_IO, no_partition, trash_ppa);
#endif

	if (cpp->enable_gc_delay) {
	    struct nand_cmd gcw;
	    gcw.type = GC_IO;
	    gcw.cmd = NAND_NOP;
	    gcw.stime = 0;
	    gcw.interleave_pci_dma = false;
	    gcw.ppa = &new_ppa;
	    if (last_pg_in_wordline(conv_ftl, &new_ppa)) {
	        gcw.cmd = NAND_WRITE;
	        gcw.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg;
	    }
#ifdef CHIP_UTIL
	    ssd_advance_nand(conv_ftl->ssd, &gcw, 
				&(conv_ftl->ns->nand_idle_t_sum), &(conv_ftl->ns->nand_active_t_sum));
#else
	    ssd_advance_nand(conv_ftl->ssd, &gcw);
#endif
	}

	/* advance per-ch gc_endtime as well */
#if 0
	new_ch = get_ch(conv_ftl, &new_ppa);
	new_ch->gc_endtime = new_ch->next_ch_avail_time;

	new_lun = get_lun(conv_ftl, &new_ppa);
	new_lun->gc_endtime = new_lun->next_lun_avail_time;
#endif

	return 0;
}

struct ppa append_zone_mapping_handler(struct conv_ftl *conv_ftl, uint64_t local_lpn, int no_partition);
#ifndef GURANTEE_SEQ_WRITE
static void classify_line(struct conv_ftl *conv_ftl, uint32_t io_type, int no_partition, 
							struct ppa *ppa);
#endif

uint64_t get_next_free_zone(struct conv_ftl *conv_ftl, unsigned int no_partition, 
		struct window_mgmt *wm, uint64_t start_zidx)
{
	uint64_t nzones = wm->nzones_per_partition;
	unsigned long * zone_bitmap = wm->zone_bitmap;
	uint64_t ret_zidx;
	//static int pcnt[SSD_PARTITIONS][GC_PARTITION] ={0};

	if (start_zidx < nzones) {
		ret_zidx = find_next_zero_bit_((char *)zone_bitmap, nzones, start_zidx);
		if (ret_zidx < nzones){
			goto got_it;
		}
	}
	start_zidx = 0;
	
	ret_zidx = find_next_zero_bit_((char *)zone_bitmap, nzones, start_zidx);

	if (ret_zidx >= nzones){
		printk("%s: no free zone in GC partition!!", __func__);
		NVMEV_ASSERT(0);
	}

got_it:
	if (wftl_test_and_set_bit(ret_zidx, (char *)zone_bitmap) != 0) {
		printk("%s: something wrong!!! ", __func__);
		NVMEV_ASSERT(0);
	}
	wm->free_zone --;
	//pcnt[conv_ftl->no_part][no_partition] ++;
	//if (pcnt[conv_ftl->no_part][no_partition] % 500 == 0) {
	//	printk("%s: free_zone: %lu / %lu conv_part: %u no_partition: %u", 
	//			__func__, wm->free_zone, wm->nzones_per_partition, 
	//			conv_ftl->no_part, no_partition);
	//}
	
	return ret_zidx;
}

static inline struct gc_log *lookup_aimless_translator(struct conv_ftl *conv_ftl, uint64_t lpn);

/* map new local lpn to new ppa */
static inline struct ppa append_page(struct conv_ftl *conv_ftl, int no_partition, uint64_t *new_local_lpn)
{
	NVMEV_ASSERT(IS_MAIN_PARTITION(no_partition));
	struct ppa ppa;
	struct window_mgmt *wm = &conv_ftl->wm[no_partition];
	uint64_t cur_local_lpn = wm->next_local_lpn, next_local_lpn;
	int del;

	*new_local_lpn = cur_local_lpn;
	next_local_lpn = cur_local_lpn + 1;
	
	if (is_first_lpn_in_zone(conv_ftl, next_local_lpn)) {
		uint64_t start_zidx = get_zone_idx(conv_ftl, next_local_lpn), new_zidx, cur_start_local_lpn, 
				 cur_zidx;
		new_zidx = get_next_free_zone(conv_ftl, no_partition, &conv_ftl->wm[no_partition], start_zidx);
		cur_zidx = get_zone_idx(conv_ftl, cur_local_lpn);
		cur_start_local_lpn = next_local_lpn - conv_ftl->ssd->sp.pgs_per_line; 
		NVMEV_ASSERT(is_first_lpn_in_zone(conv_ftl, cur_start_local_lpn));

		del = new_zidx - cur_zidx;
		next_local_lpn = cur_start_local_lpn + del * (conv_ftl->ssd->sp.pgs_per_line);
		//printk("%s: new zidx: %u lpn: 0x%lx", 
		//	__func__, new_zidx, 
		//	LPN_FROM_LOCAL_LPN(next_local_lpn, conv_ftl->no_part, conv_ftl->ns->nr_parts));
		NVMEV_ASSERT(is_first_lpn_in_zone(conv_ftl, next_local_lpn));
		NVMEV_ASSERT(get_zone_idx(conv_ftl, next_local_lpn) == new_zidx);
		//printk("%s: next zidx: %lu nzones: %lu del: %d", __func__, 
		//		new_zidx, 
		//		conv_ftl->wm[no_partition].nzones_per_partition, del);
		//printk("%s: cur_local_lpn: 0x%lx cur_start_local_lpn: 0x%lx next_local_lpn: 0x%lx pgs_per_line: %d",
		//		__func__, cur_local_lpn, cur_start_local_lpn, next_local_lpn, conv_ftl->ssd->sp.pgs_per_line);
		//get_zone_idx(conv_ftl, next_local_lpn)
	}

	//printk("%s: cur_local_lpn: 0x%lx noftl: %u, lpn: 0x%lx pgs_per_line: %u", 
	//		__func__, cur_local_lpn, conv_ftl->no_part, 
	//		LPN_FROM_LOCAL_LPN(cur_local_lpn, conv_ftl->no_part, conv_ftl->ns->nr_parts), 
	//		conv_ftl->ssd->sp.pgs_per_line);

	conv_ftl->wm[no_partition].next_local_lpn = next_local_lpn;
	return append_zone_mapping_handler(conv_ftl, cur_local_lpn, no_partition);
}

static inline void __buffer_gc_log(struct gc_log_mgmt *gclm, struct gc_log *gc_log, 
									uint64_t old_lpn, uint64_t new_lpn)
{
	gc_log->old_lpn = old_lpn;
	gc_log->new_lpn = new_lpn;
	gc_log->status = GC_LOG_BUFFERED;
#ifdef GC_LOG_PRINT
	printk("%s: buffered_gc_log_list: %p gc_log->list_elem: %p", __func__, 
				&gclm->buffered_gc_log_list, &gc_log->list_elem);
#endif
	//printk("%s: 0x%lx 0x%lx",__func__, old_lpn, new_lpn);
#ifndef SEPARATE_GC_LOG
	list_push_back(&gclm->buffered_gc_log_list, &gc_log->list_elem);
#else
	unsigned int no_partition = NO_PARTITION(gc_log->old_lpn);
	if (no_partition == COLD_DATA_PARTITION || 
			no_partition == HOT_DATA_PARTITION) {
		list_push_back(&gclm->buffered_gc_log_list, &gc_log->list_elem);
	} else {
		list_push_back(&gclm->buffered_gc_log_list_node, &gc_log->list_elem);
	}
#endif
	gclm->n_buffered ++ ;
}

static inline void insert_into_aimless_translator(struct gc_log *gc_log)
{
#ifndef SEPARATE_GC_LOG
	hash_add(aimless_translator, &gc_log->hnode, gc_log->old_lpn);
#ifdef GC_LOG_MERGE
	hash_add(gc_log_merger, &gc_log->hnode_merge, gc_log->new_lpn);
#endif

#else
	unsigned int no_partition = NO_PARTITION(gc_log->old_lpn);
	if (no_partition == COLD_DATA_PARTITION || 
			no_partition == HOT_DATA_PARTITION) {
		hash_add(aimless_translator, &gc_log->hnode, gc_log->old_lpn);
#ifdef GC_LOG_MERGE
		hash_add(gc_log_merger, &gc_log->hnode_merge, gc_log->new_lpn);
#endif
	} else {
		hash_add(aimless_translator_node, &gc_log->hnode, gc_log->old_lpn);
#ifdef GC_LOG_MERGE
		hash_add(gc_log_merger_node, &gc_log->hnode_merge, gc_log->new_lpn);
#endif
	}

#endif


#ifdef GC_LOG_PRINT2
	//printk("%s: gc log: %p old lpn: 0x%llx new lpn: 0x%llx",
	//		__func__, gc_log, gc_log->old_lpn, gc_log->new_lpn);
#endif
}

#ifdef GC_LOG_MERGE
static inline struct gc_log *lookup_gc_log_merger(uint64_t lpn)
{
#ifndef SEPARATE_GC_LOG
	struct hlist_head *head = &gc_log_merger[hash_min(lpn, HASH_BITS(gc_log_merger))];
#else
	struct hlist_head *head;
	unsigned int no_partition = NO_PARTITION(lpn);
	if (no_partition == COLD_DATA_PARTITION || 
			no_partition == HOT_DATA_PARTITION) {
		head = &gc_log_merger[hash_min(lpn, HASH_BITS(gc_log_merger))];
	} else {
		head = &gc_log_merger_node[hash_min(lpn, HASH_BITS(gc_log_merger_node))];
	}

#endif
	struct gc_log *gc_log;

	hlist_for_each_entry(gc_log, head, hnode_merge){
		if (lpn == gc_log->new_lpn){
			//NVMEV_ASSERT(conv_ftl->no_part == gc_log->new_lpn % conv_ftl->ns->nr_parts);
			NVMEV_ASSERT(gc_log->status == GC_LOG_BUFFERED || gc_log->status == GC_LOG_INFLIGHT);
			return gc_log;
		}
	}
	return NULL;
}

static inline bool try_merge_gc_log(struct gc_log_mgmt *gclm, uint64_t old_lpn, uint64_t new_lpn)
{
	struct gc_log *gc_log;
	
	if ((gc_log = lookup_gc_log_merger(old_lpn)) == NULL) {
		return false;
	}

	if (gc_log->status == GC_LOG_INFLIGHT) { 
		return false;
	}

	NVMEV_ASSERT(gc_log->status == GC_LOG_BUFFERED);
	NVMEV_ASSERT(gc_log->new_lpn == old_lpn);


	gc_log->new_lpn = new_lpn;
	
	hash_del(&gc_log->hnode_merge);
#ifndef SEPARATE_GC_LOG
	hash_add(gc_log_merger, &gc_log->hnode_merge, gc_log->new_lpn);
#else
	unsigned int no_partition = NO_PARTITION(gc_log->old_lpn);
	if (no_partition == COLD_DATA_PARTITION || 
			no_partition == HOT_DATA_PARTITION) {
		hash_add(gc_log_merger, &gc_log->hnode_merge, gc_log->new_lpn);
	} else {
		hash_add(gc_log_merger_node, &gc_log->hnode_merge, gc_log->new_lpn);
	}
#endif
	//printk("%s: 0x%lx - 0x%lx - 0x%lx", __func__, gc_log->old_lpn, old_lpn, gc_log->new_lpn);

	return true;
}
#endif

static inline void buffer_gc_log(struct gc_log_mgmt *gclm, uint64_t old_lpn, uint64_t new_lpn, 
		unsigned int *is_merged)
{
	int i;
	struct gc_log *gc_log;
	struct list_elem *tmp_elem;
	
#ifdef GC_LOG_MEM
	gclm->buffering_trial_cnt ++ ;
#endif

#ifdef GC_LOG_MERGE
	if (try_merge_gc_log(gclm, old_lpn, new_lpn)){
		*is_merged = 1;
		return;
	}
#endif

	if (list_empty_(&(gclm->free_gc_log_list))){
#ifdef GC_LOG_PRINT
		printk("[JWDBG] %s: free gc log list empty!!", __func__);
#endif
#ifdef COUPLED_GC_DEBUG
		NVMEV_ASSERT(gclm->n_free == 0);
#endif
		if ((gc_log = (struct gc_log *) kmalloc(sizeof(struct gc_log), GFP_KERNEL)) == NULL)
			NVMEV_ASSERT(0);
		init_gc_log(gc_log); /* to init hash node */
		//printk()

		gclm->n_total ++ ;
	} else {
		tmp_elem = list_pop_front(&gclm->free_gc_log_list);
		gc_log = (struct gc_log *) list_entry(tmp_elem, struct gc_log, list_elem);
		NVMEV_ASSERT(gc_log->old_lpn == INVALID_LPN);
		NVMEV_ASSERT(gc_log->new_lpn == INVALID_LPN);
		if (gc_log->status != GC_LOG_FREE)
			printk("%s: gc log status: %d free type: %d n_free: %u n_buffered: %u n_inflight: %u n_total: %u", 
					__func__, gc_log->status, GC_LOG_FREE, gclm->n_free, gclm->n_buffered,
					gclm->n_inflight, gclm->n_total);
		//NVMEV_ASSERT(gc_log->status == GC_LOG_FREE);
		NVMEV_ASSERT((gc_log->status & 0xf) == GC_LOG_FREE);
		
		gclm->n_free -- ;
	}
	
	gclm->buffering_cnt ++ ;

	__buffer_gc_log(gclm, gc_log, old_lpn, new_lpn);
	insert_into_aimless_translator(gc_log);
#ifdef COUPLED_GC_DEBUG
	NVMEV_ASSERT(gclm->n_total == gclm->n_buffered + gclm->n_inflight + gclm->n_free);
#endif
}

static inline void free_gc_log(struct gc_log_mgmt *gclm, struct gc_log *gc_log)
{
	NVMEV_ASSERT(gc_log->status != GC_LOG_FREE);

	if (gc_log->status == GC_LOG_BUFFERED)
		gclm->n_buffered --;
	else if (gc_log->status == GC_LOG_INFLIGHT)
		gclm->n_inflight --;
	gclm->n_free ++ ;
	
	//printk("%s: 0x%lx 0x%lx",__func__, gc_log->old_lpn, gc_log->new_lpn);

#ifdef COUPLED_GC_DEBUG
	NVMEV_ASSERT(gclm->n_total == gclm->n_buffered + gclm->n_inflight + gclm->n_free);
#endif
	gc_log->status = GC_LOG_FREE;
	gc_log->old_lpn = INVALID_LPN;
	gc_log->new_lpn = INVALID_LPN;

	/* Remove from buffered list */
	list_remove(&gc_log->list_elem);
#ifdef GC_LOG_PRINT
	printk("%s: free_gc_log_list: %p gc_log->list_elem: %p", __func__, 
				&gclm->free_gc_log_list, &gc_log->list_elem);
#endif
	/* Push back to free list */
	list_push_back(&gclm->free_gc_log_list, &gc_log->list_elem);

	/* Remove from aimless translator */
	hash_del(&gc_log->hnode);
#ifdef GC_LOG_MERGE
	
	/* Remove from merger */
	hash_del(&gc_log->hnode_merge);
#endif
}

#ifdef MIGRATION_IO
static inline bool excess_buffered_gc_log(struct conv_ftl *conv_ftl)
{
	//return false;
	//return gclm->n_buffered > MIGRATION_THRESHOLD;
	struct gc_log_mgmt *gclm = conv_ftl->ns->gclm;
	//return gclm->n_buffered > MIGRATION_THRESHOLD;
	return gclm->n_buffered > conv_ftl->ns->n_gc_log_max;
}

void load_gc_log_inflight(struct conv_ftl *conv_ftl, struct gc_log_mgmt *gclm, struct nvmev_result *ret)
{
	int i, ii;
	struct inflight_set_entry *ise;
	//struct mg_batch_entry *mgbe;
	unsigned int command_id;
	struct list_elem *le;
	struct gc_log *gc_log;

	/* create a gc log set in a inflight list. The set consists of NR_MG_PAIR gc logs. */
	ise = &gclm->ise_array[gclm->next_command_id % NR_INFLIGHT_SET];
	//printk("%s: nxt cid: %u ise cid: %u", __func__, gclm->next_command_id, ise->command_id);
	NVMEV_ASSERT(ise->command_id == INVALID_COMMAND_ID);

	//kmem_cache_alloc(gclm->inflight_set_slab, GFP_KERNEL);
	init_inflight_set(gclm, ise, &command_id);
	
	//mgbe = kmem_cache_alloc(gclm->mg_batch_slab, GFP_KERNEL);
	//init_mg_batch(mgbe, command_id);

	/* group into a batch and insert to inflight list */
	for (ii = 0; ii < NR_MG_PAIR; ii ++) {
#ifdef SEPARATE_GC_LOG
		if (!list_empty_(&gclm->buffered_gc_log_list)) {
			le = list_pop_front(&gclm->buffered_gc_log_list);
		} else if (!list_empty_(&gclm->buffered_gc_log_list_node)) {
			le = list_pop_front(&gclm->buffered_gc_log_list_node);
		} else {
			printk("%s: something wrong!!! buffer cnt: %u inflight: %u", __func__, gclm->n_buffered,
					gclm->n_inflight);
			NVMEV_ASSERT(0);
		}
#else
		le = list_pop_front(&gclm->buffered_gc_log_list);
#endif
		gc_log = (struct gc_log *) list_entry(le, struct gc_log, list_elem);

		/* convert into an inflight gc log */
		gc_log->status = GC_LOG_INFLIGHT;
		gclm->n_inflight ++ ;
		gclm->n_buffered --;

		/* insert the gc log into inflight set */
		list_push_back(&ise->gc_log_list, le);
	
		/* reflect into the mg batch */
	//	mgbe->mg_pairs[mgbe->nr].old_lba = gc_log->old_lpn;
	//	mgbe->mg_pairs[mgbe->nr].new_lba = gc_log->new_lpn;
	//	mgbe->nr ++ ;
	}

	
	list_push_back(&ret->ise_list, &ise->list_elem);

	/* insert an inflight set into a inflight_gc_log_htable */
	//hash_add(inflight_gc_log_htable, &ise->hnode, ise->command_id);

	/* insert an mg batch into a mg batch list */
	//list_push_back(&ret->mg_batch_list, &mgbe->list_elem);

}
#endif

#ifdef COUPLED_GC_MTL

static uint64_t mig_old_stack[BUF_CNT_], mig_new_stack[BUF_CNT_], mig_cid_stack[BUF_CNT_];

void add_mtl_migration_log(struct nvmev_ns *ns, struct nvmev_result *ret, uint64_t old_lpn, uint64_t new_lpn)
{
	struct mg_entry *mg_entry;
	struct list *mlist = &ret->mtl_migration_list;
	
	//if ((old_lpn & 0x20000000) == 0x20000000) {
	//	printk("%s: old_lpn: 0x%llx new_lpn: 0x%llx", __func__, 
	//			old_lpn, new_lpn);
	//}
	
	if (list_empty_(mlist)){
		mg_entry = init_migration_entry(ns);
		list_push_back(mlist, &mg_entry->list_elem);
	}

	mg_entry = list_entry(list_last(mlist), struct mg_entry, list_elem);
	if (mg_entry->nr_log == NR_MAX_MIGRATION_LOG){
		mg_entry = init_migration_entry(ns);
		list_push_back(mlist, &mg_entry->list_elem);
	}

	mg_entry->log_buf[mg_entry->nr_log].old_lpn = old_lpn;
	mg_entry->log_buf[mg_entry->nr_log].new_lpn = new_lpn;
	mg_entry->nr_log ++;

	
	//if (1) {
	//if ((old_lpn & 0x20000000) == 0x20000000 || 
	//		(new_lpn & 0x60000000) == 0x60000000) {
	/*
	if ((old_lpn >> 29) == 0x1 || 
			(new_lpn >> 29) == 0x3) {
		mig_old_stack[mig_cnt] = old_lpn;
		mig_new_stack[mig_cnt] = new_lpn;
		mig_cid_stack[mig_cnt] = ret->order;
		mig_cnt ++;
		if (mig_cnt == BUF_CNT_) {
//#ifdef PLEASE
			printk("%s \n \
					cid: %u slpn: 0x%llx elpn: 0x%llx \n \
					cid: %u slpn: 0x%llx elpn: 0x%llx \n \
					cid: %u slpn: 0x%llx elpn: 0x%llx \n \
					cid: %u slpn: 0x%llx elpn: 0x%llx \n \
					cid: %u slpn: 0x%llx elpn: 0x%llx \n \
					cid: %u slpn: 0x%llx elpn: 0x%llx \n \
					cid: %u slpn: 0x%llx elpn: 0x%llx \n \
					cid: %u slpn: 0x%llx elpn: 0x%llx \n \
					cid: %u slpn: 0x%llx elpn: 0x%llx \n \
					cid: %u slpn: 0x%llx elpn: 0x%llx \
					", __func__,
					mig_cid_stack[0], mig_old_stack[0], mig_new_stack[0],
					mig_cid_stack[1], mig_old_stack[1], mig_new_stack[1],
					mig_cid_stack[2], mig_old_stack[2], mig_new_stack[2],
					mig_cid_stack[3], mig_old_stack[3], mig_new_stack[3],
					mig_cid_stack[4], mig_old_stack[4], mig_new_stack[4],
					mig_cid_stack[5], mig_old_stack[5], mig_new_stack[5],
					mig_cid_stack[6], mig_old_stack[6], mig_new_stack[6],
					mig_cid_stack[7], mig_old_stack[7], mig_new_stack[7],
					mig_cid_stack[8], mig_old_stack[8], mig_new_stack[8],
					mig_cid_stack[9], mig_old_stack[9], mig_new_stack[9]
					//old_stack[10], new_stack[10],
					//old_stack[11], new_stack[11],
					//old_stack[12], new_stack[12],
					//old_stack[13], new_stack[13],
					//old_stack[14], new_stack[14],
					//old_stack[15], new_stack[15],
					//old_stack[16], new_stack[16],
					//old_stack[17], new_stack[17],
					//old_stack[18], new_stack[18],
					//old_stack[19], new_stack[19]
			);
//#endif
			mig_cnt = 0;
		}
	}
	*/

}

void add_mtl_translation_log_for_read(struct nvmev_ns *ns, struct nvmev_result *ret, 
		uint64_t old_lpn, uint64_t new_lpn)
{
	struct trans_entry *te;
	struct list *tlist = &ret->mtl_read_translation_list[old_lpn % ns->nr_parts];

	if (list_empty_(tlist)){
		te = init_translation_entry(ns);
		list_push_back(tlist, &te->list_elem);
	}

	te = list_entry(list_last(tlist), struct trans_entry, list_elem);
	if (te->nr_log == NR_MAX_TRANSLATION_LOG){
		te = init_translation_entry(ns);
		list_push_back(tlist, &te->list_elem);
	}

	//printk("%s: te: %p idx: %llu old_lpn: 0x%llx new_lpn: 0x%llx",
	//		__func__, te, te->nr_log, old_lpn, new_lpn);
	te->log_buf[te->nr_log].old_lpn = old_lpn;
	te->log_buf[te->nr_log].new_lpn = new_lpn;
	te->nr_log ++;
}

void add_mtl_translation_log(struct nvmev_ns *ns, struct nvmev_result *ret, uint64_t old_lpn, uint64_t new_lpn)
{
	struct trans_entry *te;
	struct list *tlist = &ret->mtl_translation_list;
	if (list_empty_(tlist)){
		te = init_translation_entry(ns);
		list_push_back(tlist, &te->list_elem);
	}

	te = list_entry(list_last(tlist), struct trans_entry, list_elem);
	if (te->nr_log == NR_MAX_TRANSLATION_LOG){
		te = init_translation_entry(ns);
		list_push_back(tlist, &te->list_elem);
	}

	//printk("%s: te: %p idx: %llu old_lpn: 0x%llx new_lpn: 0x%llx",
	//		__func__, te, te->nr_log, old_lpn, new_lpn);
	te->log_buf[te->nr_log].old_lpn = old_lpn;
	te->log_buf[te->nr_log].new_lpn = new_lpn;
	te->nr_log ++;
}
#endif

static inline bool dma_mg_pool_is_full(struct gc_log_mgmt *gclm)
{
	uint64_t next_cid = gclm->next_command_id;
	uint64_t completed_cid = gclm->completed_command_id;
	unsigned int del;
	if (next_cid >= completed_cid) {
		del = next_cid - completed_cid;
		if (del >= NR_PGS_IN_MG_POOL-1)
			return true;
		return false;
	} else {
		printk("%s: not yet expected!!!!!!!!!", __func__);
		NVMEV_ASSERT(0);
		return true;
	}


}

#define MG_SEND_THRESHOLD(conv_ftl) (conv_ftl->nzones_per_partition * 80 / 100 )

static inline bool need_to_send_migration_command(struct conv_ftl *conv_ftl)
{
	if (excess_buffered_gc_log(conv_ftl))
		return true;	
	//if ((conv_ftl->ns->gclm->buffering_cnt % 1024 == 0) && 
	//	  (conv_ftl->ns->gclm->n_buffered > MIGRATION_LOGS_PER_CMD))
	//	return true;
	if  (conv_ftl->ns->gclm->n_buffered <= MIGRATION_LOGS_PER_CMD) {
		return false;
	}
	if (conv_ftl->valid_zone_cnt[HOT_DATA_PARTITION] >= MG_SEND_THRESHOLD(conv_ftl)) {
		//printk("%s: 1. zcnt: %u thre: %u", __func__, 
		//	conv_ftl->valid_zone_cnt[HOT_DATA_PARTITION], 
		//	MG_SEND_THRESHOLD(conv_ftl)	
		//);
		return true;
	}
	if (conv_ftl->valid_zone_cnt[HOT_NODE_PARTITION] >= MG_SEND_THRESHOLD(conv_ftl)) {
		//printk("%s: 2. zcnt: %u thre: %u", __func__, 
		//	conv_ftl->valid_zone_cnt[HOT_NODE_PARTITION], 
		//	MG_SEND_THRESHOLD(conv_ftl)	
		//		);
		return true;
	}

	if (conv_ftl->valid_zone_cnt[HOT_NODE_PARTITION] + conv_ftl->valid_zone_cnt[HOT_DATA_PARTITION]
		+ conv_ftl->gc_free_zone_cnt[HOT_NODE_PARTITION] + conv_ftl->gc_free_zone_cnt[HOT_DATA_PARTITION]
			> MG_SEND_THRESHOLD(conv_ftl)) {
		return true;
	}


	return false;

}


#ifndef COUPLED_GC_MTL
static uint64_t coupled_gc_write_page(struct conv_ftl *conv_ftl, struct ppa *old_ppa)
#else
static uint64_t coupled_gc_write_page(struct conv_ftl *conv_ftl, struct ppa *old_ppa, struct nvmev_result *ret, unsigned int *is_merged)
#endif
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	struct ppa new_ppa;
	uint64_t old_local_lpn, new_local_lpn;
	uint64_t old_lpn, new_lpn;
	old_local_lpn = get_rmap_ent(conv_ftl, old_ppa);
	int no_partition = NO_LOCAL_PARTITION(old_local_lpn), no_dst_partition;

	NVMEV_ASSERT(valid_lpn(conv_ftl, old_local_lpn));
	if (IS_DATA_PARTITION(no_partition)){
		no_dst_partition = COLD_DATA_PARTITION;
	} else if (IS_NODE_PARTITION(no_partition)){
		no_dst_partition = GC_PARTITION;
		//no_dst_partition = COLD_NODE_PARTITION;
	} else {
		NVMEV_ASSERT(0);
	}

	new_ppa = append_page(conv_ftl, no_dst_partition, &new_local_lpn);

	/* update rmap */
	set_rmap_ent(conv_ftl, new_local_lpn, &new_ppa);

	mark_page_valid(conv_ftl, &new_ppa);
#ifdef SHIVAL2
	total_valid_blks -= 1;
#endif

	/* need to advance the write pointer here */
#ifdef GURANTEE_SEQ_WRITE
	advance_write_pointer(conv_ftl, USER_IO, no_dst_partition);
#else
	struct ppa *trash_ppa;
	advance_write_pointer(conv_ftl, USER_IO, no_dst_partition, trash_ppa);
#endif
	if (cpp->enable_gc_delay) {
	    struct nand_cmd gcw;
	    gcw.type = GC_IO;
	    gcw.cmd = NAND_NOP;
	    gcw.stime = 0;
	    gcw.interleave_pci_dma = false;
	    gcw.ppa = &new_ppa;
	    if (last_pg_in_wordline(conv_ftl, &new_ppa)) {
	        gcw.cmd = NAND_WRITE;
	        gcw.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg;
	    }

#ifdef CHIP_UTIL
	    ssd_advance_nand(conv_ftl->ssd, &gcw, 
				&(conv_ftl->ns->nand_idle_t_sum), &(conv_ftl->ns->nand_active_t_sum));
#else
	    ssd_advance_nand(conv_ftl->ssd, &gcw);
#endif
	}

	/* advance per-ch gc_endtime as well */
#if 0
	new_ch = get_ch(conv_ftl, &new_ppa);
	new_ch->gc_endtime = new_ch->next_ch_avail_time;

	new_lun = get_lun(conv_ftl, &new_ppa);
	new_lun->gc_endtime = new_lun->next_lun_avail_time;
#endif
	old_lpn = LPN_FROM_LOCAL_LPN(old_local_lpn, conv_ftl->no_part, conv_ftl->ns->nr_parts);
	new_lpn = LPN_FROM_LOCAL_LPN(new_local_lpn, conv_ftl->no_part, conv_ftl->ns->nr_parts);
	buffer_gc_log(conv_ftl->ns->gclm, old_lpn, new_lpn, is_merged);
#ifdef MIGRATION_IO
	if (!dma_mg_pool_is_full(conv_ftl->ns->gclm)) {
		if (need_to_send_migration_command(conv_ftl)) {
				//) {
			load_gc_log_inflight(conv_ftl, conv_ftl->ns->gclm, ret);
			conv_ftl->ns->mg_cmd_cnt ++;
			conv_ftl->ns->total_mg_cmd_cnt ++;
		}
	}
#endif
#ifdef COUPLED_GC_MTL
	add_mtl_migration_log(conv_ftl->ns, ret, old_lpn, new_lpn);
#endif
	ret->migration_cnt += 1;
	return 0;
}

static inline struct gc_log *lookup_aimless_translator(struct conv_ftl *conv_ftl, uint64_t lpn)
{
#ifdef SEPARATE_GC_LOG
	struct hlist_head *head;
	unsigned int no_partition = NO_PARTITION(lpn);
	if (no_partition == COLD_DATA_PARTITION || 
			no_partition == HOT_DATA_PARTITION) {
		head = &aimless_translator[hash_min(lpn, HASH_BITS(aimless_translator))];
	} else {
		head = &aimless_translator_node[hash_min(lpn, HASH_BITS(aimless_translator_node))];
	}
#else
	struct hlist_head *head = &aimless_translator[hash_min(lpn, HASH_BITS(aimless_translator))];
#endif
	struct gc_log *gc_log;
	hlist_for_each_entry(gc_log, head, hnode){
		if (lpn == gc_log->old_lpn){
			NVMEV_ASSERT(conv_ftl->no_part == gc_log->old_lpn % conv_ftl->ns->nr_parts);
			NVMEV_ASSERT(gc_log->status == GC_LOG_BUFFERED || gc_log->status == GC_LOG_INFLIGHT);
			return gc_log;
		}
	}
	return NULL;
}

static inline struct ppa read_zone_mapping_handler(struct conv_ftl *conv_ftl, uint64_t local_lpn);

static inline struct ppa read_from_aimless_translator(struct conv_ftl *conv_ftl, uint64_t local_lpn, struct gc_log *gc_log_ret)
{
	struct ppa trans_ppa;
	struct gc_log *gc_log, *first_gc_log = NULL;;
	uint64_t trans_local_lpn;
	bool is_first = true, is_chained = false, first_is_inflight = false;
	uint64_t first_lpn = LPN_FROM_LOCAL_LPN(local_lpn, conv_ftl->no_part, conv_ftl->ns->nr_parts);
	uint64_t lpn = first_lpn;

	gc_log_ret->old_lpn = lpn;

lookup_translator:
	gc_log = lookup_aimless_translator(conv_ftl, lpn);
	if (gc_log != NULL){
		/* hit gc log */
		trans_local_lpn = LOCAL_LPN_FROM_LPN(gc_log->new_lpn, conv_ftl->ns->nr_parts);
		trans_ppa = read_zone_mapping_handler(conv_ftl, trans_local_lpn);
		/* TODO: need to handle with several copuled gc . 
		 need to check status, and decide whether free gc log or not.
		 ex. if status is inflight, do not free gc log. */
		if (!(mapped_ppa(&trans_ppa) && valid_ppa(conv_ftl, &trans_ppa))){
			lpn = gc_log->new_lpn;
			
			if (is_first){
				first_gc_log = gc_log;
				is_first = false;
				is_chained = true;
				if (gc_log->status == GC_LOG_INFLIGHT) 
					first_is_inflight = true;
			} else {
				/* free a gc log in the middle of a chain. */
				/* no need to check inflight state since middle of chane should not be send to host */
				if (gc_log->status != GC_LOG_INFLIGHT) {
#ifdef GC_LOG_MERGE
					/*buffered gc log should contain mapped ppa since its merged*/
					printk("%s: old_lpn: 0x%llx new_lpn: 0x%llx", __func__, 
							gc_log->old_lpn, gc_log->new_lpn);
					NVMEV_ASSERT(0);
#endif
					//free_gc_log(conv_ftl->ns->gclm, gc_log);
				}

			}
			
			goto lookup_translator;
		}
		
		/* update first gc log if gc log are chained and not yet in-flight */
		if (is_chained){
			/*TODO: polish by removing unnecessary code lines. is_chained == first_is_inflight with gc log merge */
			if (!first_is_inflight)
				NVMEV_ASSERT(0);
				//first_gc_log->new_lpn = gc_log->new_lpn;
			/* free the gc log at the end of chain. */
			//NVMEV_ASSERT(gc_log->status == GC_LOG_BUFFERED);
			//if (gc_log->status != GC_LOG_INFLIGHT)
			//	free_gc_log(conv_ftl->ns->gclm, gc_log);
			//gc_log = first_gc_log;
		}

		gc_log_ret->new_lpn = gc_log->new_lpn;
		
		//*gc_log_ret = gc_log;
		
		
		return trans_ppa;
	}
	return fake_ppa();
}

static inline void dec_zone_remain_cnt(struct conv_ftl *conv_ftl, uint64_t local_lpn)
{
	unsigned int no_partition = NO_LOCAL_PARTITION(local_lpn);
	struct window_mgmt *wm = &conv_ftl->wm[no_partition];
	uint64_t zidx = get_zone_idx(conv_ftl, local_lpn);

	NVMEV_ASSERT(wm->remain_cnt_array[zidx] > 0);
	wm->remain_cnt_array[zidx] --;
	if (wm->remain_cnt_array[zidx] == 0) {
		if (wftl_test_and_clear_bit(get_zone_idx(conv_ftl, local_lpn), (char *) wm->zone_bitmap)) {
			//printk("%s: zidx: %u slpn: 0x%lx", 
			//		__func__, get_zone_idx(conv_ftl, local_lpn), 
			//		LPN_FROM_LOCAL_LPN(local_lpn, conv_ftl->no_part, conv_ftl->ns->nr_parts));
			wm->free_zone ++;
		} else {
			NVMEV_ASSERT(0);
		}
	}
}

static inline bool single_gc_log_in_zone(struct conv_ftl *conv_ftl, struct gc_log *gc_log)
{
	struct list_elem *le_prev, *le_next;
	bool prev_gc_log_exist = false, next_gc_log_exist = false;
	struct gc_log *prev_gc_log, *next_gc_log;
	struct conv_ftl *conv_ftls = (struct conv_ftl *)conv_ftl->ns->ftls;
	unsigned int no_ftl;

	le_prev = list_before(&gc_log->list_elem);
	le_next = list_next(&gc_log->list_elem);

	if (le_prev != NULL) {
		prev_gc_log = (struct gc_log *) list_entry(le_prev, struct gc_log, list_elem);
		no_ftl = prev_gc_log->old_lpn % conv_ftl->ns->nr_parts;
		if (conv_ftl->no_part == no_ftl) {
			if (get_zone_idx(conv_ftl, gc_log->old_lpn / conv_ftl->ns->nr_parts) ==  
				get_zone_idx(&conv_ftls[no_ftl], prev_gc_log->old_lpn / conv_ftl->ns->nr_parts))
				return false;
				//prev_gc_log_exist = true;
		}
	}
	
	if (le_next != NULL) {
		next_gc_log = (struct gc_log *) list_entry(le_next, struct gc_log, list_elem);
		no_ftl = next_gc_log->old_lpn % conv_ftl->ns->nr_parts;
		if (conv_ftl->no_part == no_ftl) {
			if (get_zone_idx(conv_ftl, gc_log->old_lpn / conv_ftl->ns->nr_parts) ==  
				get_zone_idx(&conv_ftls[no_ftl], next_gc_log->old_lpn / conv_ftl->ns->nr_parts))
				return false;
				//next_gc_log_exist = true;
		}
	}

	return true;
}


/* read aimless translator and delete gc log */
static inline struct ppa pop_from_aimless_translator(struct conv_ftl *conv_ftl, uint64_t local_lpn,
														uint64_t *p_trans_local_lpn, struct gc_log *gc_log_ret)
{
	struct ppa trans_ppa;
	struct gc_log *gc_log;
	uint64_t trans_local_lpn;
	uint64_t first_lpn = LPN_FROM_LOCAL_LPN(local_lpn, conv_ftl->no_part, conv_ftl->ns->nr_parts);
	uint64_t lpn = first_lpn;

lookup_translator:
	gc_log = lookup_aimless_translator(conv_ftl, lpn);
	if (gc_log != NULL){
		/* hit gc log */
		trans_local_lpn = LOCAL_LPN_FROM_LPN(gc_log->new_lpn, conv_ftl->ns->nr_parts);
		trans_ppa = read_zone_mapping_handler(conv_ftl, trans_local_lpn);
		/* TODO: need to handle with several copuled gc . 
		 need to check status, and decide whether free gc log or not.
		 ex. if status is inflight, do not free gc log. */
		if (!(mapped_ppa(&trans_ppa) && valid_ppa(conv_ftl, &trans_ppa))){
			/* in-flight gc log case */

			//if (IS_GC_PARTITION(NO_PARTITION(gc_log->old_lpn)))
			//	dec_zone_remain_cnt(conv_ftl, LOCAL_LPN_FROM_LPN(gc_log->old_lpn, conv_ftl->ns->nr_parts));
			
			
			lpn = gc_log->new_lpn;
			//if ((gc_log->old_lpn & 0xe0000000) == 0x60000000 ||
			//		(gc_log->new_lpn  & 0xe0000000) == 0x60000000) 
			//printk("%s: free gc log: old lpn: 0x%llx new lpn: 0x%llx", 
			//			__func__, gc_log->old_lpn, gc_log->new_lpn);
			if (gc_log->status != GC_LOG_INFLIGHT){
				/* No more intermediate buffered gc log due to gc log merge */
				NVMEV_ASSERT(0);
				//free_gc_log(conv_ftl->ns->gclm, gc_log);
			}
			
			goto lookup_translator;
		}
		//NVMEV_ASSERT(mapped_ppa(&trans_ppa) && valid_ppa(conv_ftl, &trans_ppa));
		*p_trans_local_lpn = trans_local_lpn;
		gc_log_ret->old_lpn = first_lpn;
		gc_log_ret->new_lpn = gc_log->new_lpn;
		//if ((gc_log->old_lpn  & 0xe0000000) == 0x60000000 ||
		//			(gc_log->new_lpn  & 0xe0000000) == 0x60000000) 
		//printk("%s: free gc log: old lpn: 0x%llx new lpn: 0x%llx", 
		//			__func__, gc_log->old_lpn, gc_log->new_lpn);
		//if ((gc_log_ret->old_lpn  & 0xe0000000) == 0x60000000 ||
		//			(gc_log_ret->new_lpn  & 0xe0000000) == 0x60000000) 
		//printk("%s: change gc log: old lpn: 0x%llx new lpn: 0x%llx", 
		//			__func__, gc_log_ret->old_lpn, gc_log_ret->new_lpn);
		

		if (gc_log->status != GC_LOG_INFLIGHT) {
			conv_ftl->ns->discarded_gc_log ++;
			if (IS_GC_PARTITION(NO_PARTITION(gc_log->old_lpn))) {
				dec_zone_remain_cnt(conv_ftl, 
						LOCAL_LPN_FROM_LPN(gc_log->old_lpn, conv_ftl->ns->nr_parts));
			}
			if (single_gc_log_in_zone(conv_ftl, gc_log)) {

				if (conv_ftl->valid_zone_cnt[NO_PARTITION(gc_log->old_lpn)] > 0) 
					conv_ftl->valid_zone_cnt[NO_PARTITION(gc_log->old_lpn)] --;
				if (conv_ftl->total_valid_zone_cnt > 0) 
					conv_ftl->total_valid_zone_cnt --;
				//conv_ftl->valid_zone_cnt[NO_PARTITION(gc_log->old_lpn)] --;
			}
			free_gc_log(conv_ftl->ns->gclm, gc_log);
		}
		return trans_ppa;
	}

	*p_trans_local_lpn = INVALID_LPN;
	printk("%s: unexpected!!!!!!!!!!!!!!", __func__);	
	return fake_ppa();
}

/* move valid page data (already in DRAM) from victim line to a new page */
#ifndef COUPLED_GC_MTL
static uint64_t gc_write_page(struct conv_ftl *conv_ftl, struct ppa *old_ppa)
#else
static uint64_t gc_write_page(struct conv_ftl *conv_ftl, struct ppa *old_ppa, struct nvmev_result *ret,
		unsigned int *is_merged)
#endif
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	struct ppa new_ppa;
	uint64_t lpn = get_rmap_ent(conv_ftl, old_ppa);
	int no_partition = NO_LOCAL_PARTITION(lpn);

	if (IS_META_PARTITION(no_partition)){
		gc_write_meta_page(conv_ftl, old_ppa);
	} else if (IS_MAIN_PARTITION(no_partition)){
#ifndef COUPLED_GC_MTL
		coupled_gc_write_page(conv_ftl, old_ppa);
#else
		coupled_gc_write_page(conv_ftl, old_ppa, ret, is_merged);
#endif
	} else {
		printk("%s: [JWDBG] gc on gc partition not yet implemented", __func__);
		NVMEV_ASSERT(0);
	}
	return 0;
}

#else
/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct conv_ftl *conv_ftl, struct ppa *old_ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	struct ppa new_ppa;
	uint64_t lpn = get_rmap_ent(conv_ftl, old_ppa);
#ifdef MULTI_PARTITION_FTL
	int no_partition = NO_LOCAL_PARTITION(lpn);
#endif

	NVMEV_ASSERT(valid_lpn(conv_ftl, lpn));
#ifndef MULTI_PARTITION_FTL
	new_ppa = get_new_page(conv_ftl, GC_IO);
#else
	new_ppa = get_new_page(conv_ftl, GC_IO, no_partition);
#endif
	/* update maptbl */
	set_maptbl_ent(conv_ftl, lpn, &new_ppa);

	/* update rmap */
	set_rmap_ent(conv_ftl, lpn, &new_ppa);

	mark_page_valid(conv_ftl, &new_ppa);

#ifdef SHIVAL2
	total_valid_blks -= 1;
#endif

	/* need to advance the write pointer here */
#ifndef MULTI_PARTITION_FTL
	advance_write_pointer(conv_ftl, GC_IO);
#else
#ifdef GURANTEE_SEQ_WRITE
	advance_write_pointer(conv_ftl, GC_IO, no_partition);
#else
	struct ppa *trash_ppa;
	advance_write_pointer(conv_ftl, GC_IO, no_partition, trash_ppa);
#endif
#endif

	if (cpp->enable_gc_delay) {
	    struct nand_cmd gcw;
	    gcw.type = GC_IO;
	    gcw.cmd = NAND_NOP;
	    gcw.stime = 0;
	    gcw.interleave_pci_dma = false;
	    gcw.ppa = &new_ppa;
	    if (last_pg_in_wordline(conv_ftl, &new_ppa)) {
	        gcw.cmd = NAND_WRITE;
	        gcw.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg;
	    }

#ifdef CHIP_UTIL
	    ssd_advance_nand(conv_ftl->ssd, &gcw, 
				&(conv_ftl->ns->nand_idle_t_sum), &(conv_ftl->ns->nand_active_t_sum));
#else
	    ssd_advance_nand(conv_ftl->ssd, &gcw);
#endif
	}

	/* advance per-ch gc_endtime as well */
#if 0
	new_ch = get_ch(conv_ftl, &new_ppa);
	new_ch->gc_endtime = new_ch->next_ch_avail_time;

	new_lun = get_lun(conv_ftl, &new_ppa);
	new_lun->gc_endtime = new_lun->next_lun_avail_time;
#endif

	return 0;
}
#endif

static struct line *select_victim_line(struct conv_ftl *conv_ftl, bool force)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *victim_line = NULL;

	victim_line = pqueue_peek(lm->victim_line_pq);
	if (!victim_line) {
	    return NULL;
	}

	if (!force && (victim_line->vpc > (spp->pgs_per_line / 8))) {
	    return NULL;
	}

	pqueue_pop(lm->victim_line_pq);
	victim_line->pos = 0;
	lm->victim_line_cnt--;

	/* victim_line is a danggling node now */
	return victim_line;
}

/* here ppa identifies the block we want to clean */
//#ifndef COUPLED_GC_MTL
//static void clean_one_block(struct conv_ftl *conv_ftl, struct ppa *ppa)
//#else
//static void clean_one_block(struct conv_ftl *conv_ftl, struct ppa *ppa, struct nvmev_result *ret)
//#endif
//{
//	struct ssdparams *spp = &conv_ftl->ssd->sp;
//	struct nand_page *pg_iter = NULL;
//	int cnt = 0;
//	int pg;
//
//	for (pg = 0; pg < spp->pgs_per_blk; pg++) {
//	    ppa->g.pg = pg;
//	    pg_iter = get_pg(conv_ftl->ssd, ppa);
//	    /* there shouldn't be any free page in victim blocks */
//	    NVMEV_ASSERT(pg_iter->status != PG_FREE);
//	    if (pg_iter->status == PG_VALID) {
//	        gc_read_page(conv_ftl, ppa);
//	        /* delay the maptbl update until "write" happens */
//#ifndef COUPLED_GC_MTL
//			gc_write_page(conv_ftl, ppa);
//#else
//			gc_write_page(conv_ftl, ppa, ret);
//#endif
//	        cnt++;
//	    }
//	}
//
//	NVMEV_ASSERT(get_blk(conv_ftl->ssd, ppa)->vpc == cnt);
//}

/* here ppa identifies the block we want to clean */
#ifndef COUPLED_GC_MTL
static void clean_one_flashpg(struct conv_ftl *conv_ftl, struct ppa *ppa)
#else
static void clean_one_flashpg(struct conv_ftl *conv_ftl, struct ppa *ppa, struct nvmev_result *ret, 
		uint16_t *merge_cnt, uint16_t *vcnt)
#endif
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	struct nand_page *pg_iter = NULL;
	int cnt = 0, i = 0;
	uint64_t completed_time = 0;
	struct ppa ppa_copy = *ppa;

	for (i = 0; i < spp->pgs_per_flashpg; i++) {
	    pg_iter = get_pg(conv_ftl->ssd, &ppa_copy);
	    /* there shouldn't be any free page in victim blocks */
	    NVMEV_ASSERT(pg_iter->status != PG_FREE);
	    if (pg_iter->status == PG_VALID)
	        cnt++;

	    ppa_copy.g.pg++;
	}

	ppa_copy = *ppa;

	if (cnt <= 0) return;

	unsigned int is_merged;
	if (cpp->enable_gc_delay) {
		struct nand_cmd gcr;
		gcr.type = GC_IO;
		gcr.cmd = NAND_READ;
		gcr.stime = 0;
		gcr.xfer_size = spp->pgsz * cnt;
		gcr.interleave_pci_dma = false;
		gcr.ppa = &ppa_copy;
#ifdef CHIP_UTIL
	    completed_time = ssd_advance_nand(conv_ftl->ssd, &gcr, 
				&(conv_ftl->ns->nand_idle_t_sum), &(conv_ftl->ns->nand_active_t_sum));
#else
		completed_time = ssd_advance_nand(conv_ftl->ssd, &gcr);
#endif
	}

	for (i = 0; i < spp->pgs_per_flashpg; i++) {
		pg_iter = get_pg(conv_ftl->ssd, &ppa_copy);

		/* there shouldn't be any free page in victim blocks */
		if (pg_iter->status == PG_VALID) {
			/* delay the maptbl update until "write" happens */
#ifndef COUPLED_GC_MTL
			gc_write_page(conv_ftl, &ppa_copy);
#else
			*vcnt = *vcnt + 1;
			is_merged = 0;
			gc_write_page(conv_ftl, &ppa_copy, ret, &is_merged);
			*merge_cnt = *merge_cnt + is_merged;
#endif
		}

		ppa_copy.g.pg++;
	}
}

static void mark_line_free(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *line = get_line(conv_ftl, ppa);
	line->ipc = 0;
	line->vpc = 0;
#ifndef GURANTEE_SEQ_WRITE
	line->wpc = 0;
#endif
#ifdef ZONE_MAPPING
	uint64_t start_local_lpn = line->start_local_lpn;
	line->start_local_lpn = INVALID_LPN;
#endif
	/* move this line to free line list */
	list_add_tail(&line->entry, &lm->free_line_list);
	lm->free_line_cnt++;
#ifdef LINE_PRINT
	/*
	//printk("[JWDBG] %s: freed line: %p free_line_cnt: %d", __func__, line, lm->free_line_cnt);
	if ((start_local_lpn & 0xe0000000) == 0x20000000) 
	//if (conv_ftl->no_part == 2 || conv_ftl->no_part == 3) 
	//	printk("%s: ftlno: %d freed line: %p start_local_lpn: 0x%llx free lcnt: %u victim lcnt: %u full lcnt: %u tt lcnt: %u is_same: %d", 
	//			__func__, conv_ftl->no_part, 
	//			line,  
	//		start_local_lpn, lm->free_line_cnt, lm->victim_line_cnt, lm->full_line_cnt, 
	//		lm->tt_lines, lm->free_line_cnt + lm->victim_line_cnt + lm->full_line_cnt == lm->tt_lines);
		printk("%s: ftlno: %d freed line: %p start_local_lpn: 0x%llx", 
				__func__, conv_ftl->no_part, 
				line,  
			start_local_lpn);
	*/
#endif
}

#ifdef WAF

#define SEC_IN_USEC 1000000
#define MSEC_IN_USEC 1000
#define PRINT_TIME_SEC	1
#define WAF_TIME_INTERVAL	(PRINT_TIME_SEC * SEC_IN_USEC)
#define HOST_GC_OVERHEAD_ANALYSIS_TIME_INTERVAL	(PRINT_TIME_SEC * SEC_IN_USEC)
#define CHIP_UTIL_TIME_INTERVAL (PRINT_TIME_SEC * SEC_IN_USEC / 1)

static inline void print_WAF(struct nvmev_ns *ns)
{
	if (ns->write_volume_host) {
		//float waf = 
		//	(float) ((float) (ns->write_volume_gc + ns->write_volume_host)) / ns->write_volume_host;
		//float total_waf = 
		//	(float) ((float) (ns->total_write_volume_gc + ns->total_write_volume_host)) 
		//	/ ns->total_write_volume_host;
		unsigned int waf = 
			(100* (ns->write_volume_gc + ns->write_volume_host)) / ns->write_volume_host;
		unsigned int total_waf = 
			(100* (ns->total_write_volume_gc + ns->total_write_volume_host)) 
			/ ns->total_write_volume_host;
		//printk("%s: WAF: %u percent gc: %llu KB write_req: %llu KB total: %llu total WAF: %u percent", 
		//	__func__, waf, ns->write_volume_gc*4, ns->write_volume_host*4, 
		//	ns->total_write_volume_host*4, total_waf);
	}
}

//static inline void print_req_cnt(struct nvmev_ns *ns, unsigned long long t_intval) 
//{                                                                                  
//                                                                                   
//       printk("%s: user_req_cnt: %llu time_interval: %llu",                        
//               __func__, ns->req_cnt, t_intval);                                   
//}                                                                                  

#ifdef MG_CMD_CNT
static inline void print_MG_CMD_CNT(struct nvmev_ns *ns)
{
	if (ns->total_mg_cmd_cnt) {
		printk("%s: mg cmd submitted: %llu (per sec) total mg cmd: %llu", 
			__func__, ns->mg_cmd_cnt/PRINT_TIME_SEC, ns->total_mg_cmd_cnt);
	}
}
#endif

#ifdef CMD_CNT
static inline void print_CMD_CNT(struct nvmev_ns *ns)
{
	//printk("%s: total transfer volume. write: %d KB read: %d KB discard: %d",
	//	__func__, ns->total_write_blks_host * 4,
	//	ns->total_read_blks_host * 4, ns->total_discard_cmds_host * 4);
}
#endif

#ifdef GC_LOG_MEM
static inline void print_GC_LOG_MEM(struct nvmev_ns *ns)
{
	unsigned int n_valid_gc_log = ns->gclm->n_buffered + ns->gclm->n_inflight;

	/* disable merge so remove hlist node and to emulate 32bit, divide by 2 */
	unsigned int gc_log_size = sizeof(struct gc_log) - sizeof(struct hlist_node);
	gc_log_size /= 2;
	unsigned int gc_log_mem_MB = n_valid_gc_log * gc_log_size / 1024 / 1024;

	if (ns->gclm->buffering_trial_cnt) {
		printk("%s: mem: %u MB gc log cnt (buffer/inflight): %u ( %u / %u ) merge ratio: %u / %u %u percent total: %u discard: %u submit: %u", 
			__func__, gc_log_mem_MB, 
			n_valid_gc_log, ns->gclm->n_buffered, ns->gclm->n_inflight, 
			ns->gclm->buffering_trial_cnt - ns->gclm->buffering_cnt, 
			ns->gclm->buffering_trial_cnt, 
			(ns->gclm->buffering_trial_cnt - ns->gclm->buffering_cnt) * 100 
			/ ns->gclm->buffering_trial_cnt , 
			ns->gclm->buffering_trial_cnt, ns->discarded_gc_log, ns->total_mg_cmd_cnt * 256);
	}
	//int i = 0;
	//struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	//for (i = 0; i < 4; i ++) {
	//	struct conv_ftl *conv_ftl = &conv_ftls[i];
	//	printk("%s: partno: %d node: %u data: %u gc node: %u gc data: %u thre: %u", 
	//			__func__, i,
	//		conv_ftl->valid_zone_cnt[HOT_NODE_PARTITION], 
	//		conv_ftl->valid_zone_cnt[HOT_DATA_PARTITION],
	//		conv_ftl->gc_free_zone_cnt[HOT_NODE_PARTITION],
	//		conv_ftl->gc_free_zone_cnt[HOT_DATA_PARTITION],
	//		MG_SEND_THRESHOLD(conv_ftl)
	//	);
	//}
}
#endif

#ifdef CHIP_UTIL
static inline void print_CHIP_UTIL(struct nvmev_ns *ns)
{
	struct conv_ftl *conv_ftl = &ns->ftls[0];
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	static bool first_passed = false;

	uint64_t avg_nand_idle_t, avg_nand_active_t;

	avg_nand_idle_t = ns->nand_idle_t_sum / (spp->nchs * spp->luns_per_ch);
	avg_nand_active_t = ns->nand_active_t_sum / (spp->nchs * spp->luns_per_ch);
	if (!first_passed && avg_nand_active_t == 0)
		return;
	first_passed = true;
	ns->avg_nand_idle_t_sum_total += avg_nand_idle_t; 
	ns->avg_nand_active_t_sum_total += avg_nand_active_t; 
	ns->nand_idle_t_sum = 0;
	ns->nand_active_t_sum = 0; 
	if (avg_nand_idle_t + avg_nand_active_t > 0) { 
		//printk("%s: util: %llu %%  total util: %llu %% ", 
		//		__func__, 
		//		avg_nand_active_t * 100 / (avg_nand_idle_t + avg_nand_active_t),
		//		ns->avg_nand_active_t_sum_total * 100 
		//		/ (ns->avg_nand_idle_t_sum_total + ns->avg_nand_active_t_sum_total)
		//	  );
	}
}
#endif

static inline void try_print_WAF(struct nvmev_ns *ns) {
	unsigned long long cur_t = OS_TimeGetUS();
#ifdef HOST_GC_OVERHEAD_ANALYSIS                                                        
    if (cur_t - ns->last_t_host_gc_analy > HOST_GC_OVERHEAD_ANALYSIS_TIME_INTERVAL) {
            print_req_cnt(ns, cur_t - ns->last_t_host_gc_analy);                     
            ns->req_cnt = 0;                                                         
            ns->last_t_host_gc_analy = cur_t;                                        
    }                                                                                
#endif                                                                                  
    
#ifdef CHIP_UTIL
    if (cur_t - ns->last_t_chip_util > CHIP_UTIL_TIME_INTERVAL) {
		print_CHIP_UTIL(ns);
            ns->last_t_chip_util = cur_t;                                        
    }                                                                                
#endif

	if (cur_t - ns->last_t > WAF_TIME_INTERVAL) {
		print_WAF(ns);
#ifdef MG_CMD_CNT
		print_MG_CMD_CNT(ns);
		ns->mg_cmd_cnt = 0;
#endif

#ifdef CMD_CNT
		print_CMD_CNT(ns);
#endif

#ifdef GC_LOG_MEM
		print_GC_LOG_MEM(ns);
		ns->mg_cmd_cnt = 0;
#endif
		ns->last_t = cur_t;
		ns->write_volume_host = 0;
		ns->write_volume_gc = 0;
	}
}

#endif


static inline void set_zone_remain_cnt(struct conv_ftl *conv_ftl, uint64_t local_lpn, 

		uint16_t remain_cnt)
{
	unsigned int no_partition = NO_LOCAL_PARTITION(local_lpn);
	struct window_mgmt *wm = &conv_ftl->wm[no_partition];
	uint64_t zidx = get_zone_idx(conv_ftl, local_lpn);

	NVMEV_ASSERT(wm->remain_cnt_array[zidx] == 0);
	wm->remain_cnt_array[zidx] = remain_cnt;
}

static inline uint16_t get_zone_remain_cnt(struct conv_ftl *conv_ftl, uint64_t local_lpn)
{
	unsigned int no_partition = NO_LOCAL_PARTITION(local_lpn);
	struct window_mgmt *wm = &conv_ftl->wm[no_partition];
	uint64_t zidx = get_zone_idx(conv_ftl, local_lpn);

	return wm->remain_cnt_array[zidx];
}


#ifndef MULTI_PARTITION_FTL
static int do_gc(struct conv_ftl *conv_ftl, bool force)
#elif defined COUPLED_GC_MTL
static int do_gc(struct conv_ftl *conv_ftl, bool force, int no_partition, struct nvmev_result *ret)
#else
static int do_gc(struct conv_ftl *conv_ftl, bool force, int no_partition)
#endif
{
	struct line *victim_line = NULL;
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	struct nand_lun *lunp;
	struct ppa ppa;
	int ch, lun, flashpg;

	victim_line = select_victim_line(conv_ftl, force);
	if (!victim_line) {
		NVMEV_ASSERT(0);
	    return -1;
	}

	ppa.g.blk = victim_line->id;
		//printk("GC-ing curline: line:%d,ipc=%d(%d),victim=%d,full=%d,free=%d total_vblks: %u", 
		//	ppa.g.blk,\
	    //     victim_line->ipc,victim_line->vpc, conv_ftl->lm.victim_line_cnt, conv_ftl->lm.full_line_cnt, \
	    //      conv_ftl->lm.free_line_cnt, total_valid_blks); 
		//printk("GC-ing curline: line:%d,ipc=%d(%d),victim=%d,full=%d,free=%d total_vblks: %u slpn: 0x%lx line: 0x%lx", 
		//	ppa.g.blk,\
	    //     victim_line->ipc,victim_line->vpc, conv_ftl->lm.victim_line_cnt, conv_ftl->lm.full_line_cnt, \
	    //      conv_ftl->lm.free_line_cnt, total_valid_blks, 
		//	  LPN_FROM_LOCAL_LPN(victim_line->start_local_lpn, conv_ftl->no_part, conv_ftl->ns->nr_parts), 
		//	  victim_line);
	//static int cnt = 0;
	//cnt ++;
	//if (cnt % 5000 == 0) {
	//	printk("GC-ing curline: line:%d,ipc=%d(%d),victim=%d,full=%d,free=%d total_vblks: %u n_gclog: %u slpn: 0x%lx line: 0x%lx", 
	//		ppa.g.blk,\
	//         victim_line->ipc,victim_line->vpc, conv_ftl->lm.victim_line_cnt, conv_ftl->lm.full_line_cnt, \
	//          conv_ftl->lm.free_line_cnt, total_valid_blks, 
	//		  conv_ftl->ns->gclm->n_buffered, 
	//		  LPN_FROM_LOCAL_LPN(victim_line->start_local_lpn, conv_ftl->no_part, conv_ftl->ns->nr_parts), 
	//		  victim_line);
	//	int i__;
	//	for (i__ = 0; i__ < NO_USER_PARTITION; i__ ++) {
	//		printk("GC part: %d partition: %u vzone: %d / %lu gc free zone: %lu", 
	//				conv_ftl->no_part, i__, conv_ftl->valid_zone_cnt[i__],
	//				conv_ftl->nzones_per_partition, 
	//				conv_ftl->gc_free_zone_cnt[i__]);
	//	}
	//	printk("%s: total valid zone cnt: %u", __func__, conv_ftl->total_valid_zone_cnt);
	//}

	NVMEV_DEBUG("GC-ing line:%d,ipc=%d(%d),victim=%d,full=%d,free=%d\n", ppa.g.blk,\
	          victim_line->ipc,victim_line->vpc, conv_ftl->lm.victim_line_cnt, conv_ftl->lm.full_line_cnt,\
	          conv_ftl->lm.free_line_cnt);

#ifdef WAF
	conv_ftl->ns->write_volume_gc += victim_line->vpc;
	conv_ftl->ns->total_write_volume_gc += victim_line->vpc;
#endif

#ifndef MULTI_PARTITION_FTL
	conv_ftl->wfc.credits_to_refill = victim_line->ipc;
#else
	conv_ftl->wfc[no_partition].credits_to_refill = victim_line->ipc;
#endif

	uint16_t merge_cnt = 0;
	uint16_t v_cnt = 0;
	/* copy back valid data */
	for (flashpg = 0; flashpg < spp->flashpgs_per_blk; flashpg++) {
	    ppa.g.pg = flashpg * spp->pgs_per_flashpg;
	    for (ch = 0; ch < spp->nchs; ch++) {
	        for (lun = 0; lun < spp->luns_per_ch; lun++) {
	            ppa.g.ch = ch;
	            ppa.g.lun = lun;
	            ppa.g.pl = 0;
	            lunp = get_lun(conv_ftl->ssd, &ppa);
#ifndef COUPLED_GC_MTL
	            clean_one_flashpg(conv_ftl, &ppa);
#else
				clean_one_flashpg(conv_ftl, &ppa, ret, &merge_cnt, &v_cnt);
#endif
	            if (flashpg == (spp->flashpgs_per_blk - 1)) {
	                mark_block_free(conv_ftl, &ppa);

	                if (cpp->enable_gc_delay) {
	                    struct nand_cmd gce;
	                    gce.type = GC_IO;
	                    gce.cmd = NAND_ERASE;
	                    gce.stime = 0;
	                    gce.interleave_pci_dma = false;
	                    gce.ppa = &ppa;
#ifdef CHIP_UTIL
					   	ssd_advance_nand(conv_ftl->ssd, &gce, 
								&(conv_ftl->ns->nand_idle_t_sum), 
								&(conv_ftl->ns->nand_active_t_sum));
#else
	                    ssd_advance_nand(conv_ftl->ssd, &gce);
#endif
	                }

	                lunp->gc_endtime = lunp->next_lun_avail_time;
	            }
	        }
	    }
	}
#ifdef COUPLED_GC
	/* main & gc partition case (NULL start_local_lpn means metadata partition) */
	if (victim_line->start_local_lpn != INVALID_LPN){
		unsigned int victim_no_part = NO_LOCAL_PARTITION(victim_line->start_local_lpn);
		/* if victim is on gc partition, set remain cnt. */
		if (IS_GC_PARTITION(victim_no_part)) {
			NVMEV_ASSERT(victim_line->wpc == victim_line->ipc + victim_line->vpc);
			NVMEV_ASSERT(victim_line->vpc == v_cnt);
			uint16_t remain_cnt = victim_line->vpc - merge_cnt;
			NVMEV_ASSERT(victim_line->vpc >= merge_cnt);
			set_zone_remain_cnt(conv_ftl, victim_line->start_local_lpn, remain_cnt);
		}

		/* invalidate zone maptbl if valid blocks exist */
		if (victim_line->ipc < spp->pgs_per_line) {
			conv_ftl->gc_free_zone_cnt[victim_no_part] ++;
			invalidate_zone_maptbl_ent_from_gc(conv_ftl, victim_line->start_local_lpn, victim_line);
		}
		
		update_window_mgmt_for_discard(conv_ftl, victim_line->start_local_lpn);
	}
	//else if (! IS_META_PARTITION(no_partition)) {
	//	//printk("%s: unexpected!!!!!!!! no_part: %lu", __func__, no_partition);
	//}


	//if (victim_line->ipc == spp->pgs_per_line && victim_line->start_local_lpn != INVALID_LPN) {
	//	struct ppa *tmp_zone_map_ent;
	//	tmp_zone_map_ent = get_zone_maptbl_ent(conv_ftl, victim_line->start_local_lpn);
	//	if (mapped_ppa(tmp_zone_map_ent)) {
	//		printk("%s: start lpn: 0x%lx segno: %lu ppa: 0x%llx", 
	//				__func__, victim_line->start_local_lpn * 4, 
	//				victim_line->start_local_lpn * 4 / 512, 
	//			 tmp_zone_map_ent->ppa );
	//	}
	//}


	////if (victim_line->vpc && victim_line->start_local_lpn != INVALID_LPN){
	//if (victim_line->ipc < spp->pgs_per_line && victim_line->start_local_lpn != INVALID_LPN){
	////if (victim_line->start_local_lpn != INVALID_LPN){
	//	invalidate_zone_maptbl_ent_from_gc(conv_ftl, victim_line->start_local_lpn);
	//	update_window_mgmt_for_discard(conv_ftl, victim_line->start_local_lpn);
	//}
#endif
	/* update line status */
	mark_line_free(conv_ftl, &ppa);
	/*printk("GC done curline: %p line:%d,ipc=%d(%d),victim=%d,full=%d,free=%d\n", 
			victim_line, ppa.g.blk, victim_line->ipc, victim_line->vpc, 
			conv_ftl->lm.victim_line_cnt, conv_ftl->lm.full_line_cnt,
	          conv_ftl->lm.free_line_cnt);
	*/

	return 0;
}

#ifndef MULTI_PARTITION_FTL
static void forground_gc(struct conv_ftl *conv_ftl)
#elif defined COUPLED_GC_MTL
static void forground_gc(struct conv_ftl *conv_ftl, int no_partition, struct nvmev_result *ret) 
#else
static void forground_gc(struct conv_ftl *conv_ftl, int no_partition) 
#endif
{
	if (should_gc_high(conv_ftl)) {
	    NVMEV_DEBUG("should_gc_high passed");
#ifndef MULTI_PARTITION_FTL
	    do_gc(conv_ftl, true);
#elif defined COUPLED_GC_MTL
	    do_gc(conv_ftl, true, no_partition, ret);
#else
		do_gc(conv_ftl, true, no_partition);
#endif
	}
}

static bool is_same_flash_page(struct conv_ftl *conv_ftl, struct ppa ppa1, struct ppa ppa2)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	uint32_t ppa1_page = ppa1.g.pg / spp->pgs_per_flashpg;
	uint32_t ppa2_page = ppa2.g.pg / spp->pgs_per_flashpg;

	return (ppa1.h.blk_in_ssd == ppa2.h.blk_in_ssd) &&
	       (ppa1_page == ppa2_page);
}

bool start_done = false;
bool conv_read(struct nvmev_ns *ns, struct nvmev_request * req, struct nvmev_result * ret)
{
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];
	/* spp are shared by all instances*/
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	struct nvme_command * cmd = req->cmd;
	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;
	uint64_t lpn, local_lpn;
	uint64_t nsecs_start = req->nsecs_start;
	uint64_t nsecs_completed, nsecs_latest = nsecs_start;
	uint32_t xfer_size, i;
	uint32_t nr_parts = ns->nr_parts;

	struct ppa cur_ppa, prev_ppa;
	struct nand_cmd srd;
	srd.type = USER_IO;
	srd.cmd = NAND_READ;
	srd.stime = nsecs_start;
	srd.interleave_pci_dma = true;

	static int pcnt = 0;

#ifdef HOST_GC_OVERHEAD_ANALYSIS
    ns->req_cnt ++;          
#endif                          

#ifdef WAF	
	try_print_WAF(ns);
#endif
	
	/*SADDR_ZNS is start of main partitoin. but we subtract START_OFS_IN_MAIN_PART behind so just deduct SADDR_ZNS-START_OFS_IN_MAIN_PART*/
	if (start_lpn >= SADDR_ZNS) {
		start_lpn  = start_lpn - (SADDR_ZNS - START_OFS_IN_MAIN_PART) + 0x20000000;
		end_lpn  = end_lpn - (SADDR_ZNS - START_OFS_IN_MAIN_PART) + 0x20000000;
	}

#if (defined ZONE_MAPPING || defined MULTI_PARTITION_FTL)
	int no_partition = NO_LOCAL_PARTITION(start_lpn / nr_parts);
	NVMEV_ASSERT(no_partition == NO_LOCAL_PARTITION(end_lpn / nr_parts));
	struct ppa (*read_handler) (struct conv_ftl *conv_ftl, uint64_t local_lpn);

#ifdef ZONE_MAPPING
	if (IS_META_PARTITION(no_partition)){
		read_handler = read_meta_page_mapping_handler;
	} else if (IS_MAIN_PARTITION(no_partition)){
		read_handler = read_zone_mapping_handler;
		/* TODO FTL range check for Data Partition */
		/* comment here due to the coupled gc. */
		/*if (out_of_partition(conv_ftl, end_lpn/nr_parts)){
			if (pcnt < 5)
		    	NVMEV_ERROR("conv_read: lpn passed FTL range(start_lpn=0x%llx,tt_pgs=%ld)\n", start_lpn, spp->tt_pgs);
			pcnt += 1;
			NVMEV_ASSERT(0);
		    return false;
		}*/
	} else {
		NVMEV_ERROR("%s: partition %d error\n", __func__, no_partition);
	}
#else
	read_handler = get_maptbl_ent;
#endif
#endif

	NVMEV_ASSERT(conv_ftls);
	NVMEV_DEBUG("conv_read: start_lpn=%lld, len=%lld, end_lpn=%lld", start_lpn, nr_lba, end_lpn);
#ifdef JWDBG_CONV_FTL
//	static int print_ = 0;
//	int print_interval = 1000;
//	if (print_ % print_interval == 0){
//		NVMEV_INFO("conv_read: start_lpn=%lld, len=%lld, end_lpn=%lld", start_lpn, nr_lba, end_lpn);
//	}
//	print_ ++;
#endif

#ifdef CMD_CNT
	if (start_done) {
		ns->total_read_blks_host += (end_lpn - start_lpn + 1);
	}
#endif

#if !(defined ZONE_MAPPING || defined MULTI_PARTITION_FTL)
	if ((end_lpn/nr_parts) >= spp->tt_pgs) {
	    NVMEV_ERROR("conv_read: lpn passed FTL range(start_lpn=%lld,tt_pgs=%ld)\n", start_lpn, spp->tt_pgs);
	    return false;
	}
#endif

	if (LBA_TO_BYTE(nr_lba) <= (KB(4) * nr_parts)) {
	    srd.stime += spp->fw_4kb_rd_lat;
	} else {
	    srd.stime += spp->fw_rd_lat;
	}
	
	if (IS_MAIN_PARTITION(no_partition)){
		start_lpn -= START_OFS_IN_MAIN_PART;
		end_lpn -= START_OFS_IN_MAIN_PART;
	}

	for (i = 0; (i < nr_parts) && (start_lpn <= end_lpn); i++, start_lpn++) {
	    conv_ftl = &conv_ftls[start_lpn % nr_parts];
	    xfer_size = 0;
#if (defined ZONE_MAPPING || defined MULTI_PARTITION_FTL)
	    prev_ppa = read_handler(conv_ftl, start_lpn/nr_parts);
#else
	    prev_ppa = get_maptbl_ent(conv_ftl, start_lpn/nr_parts);
#endif

	    NVMEV_DEBUG("[%s] conv_ftl=%p, ftl_ins=%lld, local_lpn=%lld",__FUNCTION__, conv_ftl, lpn%nr_parts, lpn/nr_parts);

	    /* normal IO read path */
	    for (lpn = start_lpn; lpn <= end_lpn; lpn+=nr_parts) {
	        local_lpn = lpn / nr_parts;
#if (defined ZONE_MAPPING || defined MULTI_PARTITION_FTL)
			cur_ppa = read_handler(conv_ftl, local_lpn);
#else
	        cur_ppa = get_maptbl_ent(conv_ftl, local_lpn);
#endif
    		//printk("[JWDBG] %s lpn 0x%llx ppa 0x%llx, ", __func__, lpn, cur_ppa.ppa );
			if (!mapped_ppa(&cur_ppa) || !valid_ppa(conv_ftl, &cur_ppa)) {
#ifdef COUPLED_GC
				/* check gc log */
				uint64_t trash_lpn;
				struct gc_log gc_log_ret = {
					.old_lpn = INVALID_LPN,
					.new_lpn = INVALID_LPN,
				};

				cur_ppa = read_from_aimless_translator(conv_ftl, local_lpn, &gc_log_ret);
				if (!mapped_ppa(&cur_ppa) || !valid_ppa(conv_ftl, &cur_ppa)) {
					NVMEV_DEBUG("lpn 0x%llx not mapped to valid ppa\n", local_lpn);
	            	NVMEV_DEBUG("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d\n",
	            	cur_ppa.g.ch, cur_ppa.g.lun, cur_ppa.g.blk, cur_ppa.g.pl, cur_ppa.g.pg);
	            	continue;
				}
				NVMEV_ASSERT(gc_log_ret.old_lpn != INVALID_LPN && 
						gc_log_ret.new_lpn != INVALID_LPN);

				NVMEV_ASSERT(gc_log_ret.old_lpn == LPN_FROM_LOCAL_LPN(local_lpn, conv_ftl->no_part, conv_ftl->ns->nr_parts));
				//printk("%s: get helped from aimless translator", __func__);
				/* add translation log for mtl */
				add_mtl_translation_log_for_read(ns, ret, gc_log_ret.old_lpn, gc_log_ret.new_lpn); 
#else
	            NVMEV_DEBUG("lpn 0x%llx not mapped to valid ppa\n", local_lpn);
	            NVMEV_DEBUG("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d\n",
	            cur_ppa.g.ch, cur_ppa.g.lun, cur_ppa.g.blk, cur_ppa.g.pl, cur_ppa.g.pg);
	            continue;
#endif
			}

	        // aggregate read io in same flash page
	        if (mapped_ppa(&prev_ppa) && is_same_flash_page(conv_ftl, cur_ppa, prev_ppa)) {
	            xfer_size += spp->pgsz;
	            continue;
	        }

	        if (xfer_size > 0) {
	            srd.xfer_size = xfer_size;
	            srd.ppa = &prev_ppa;
#ifdef CHIP_UTIL
				nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &srd, 
						&(ns->nand_idle_t_sum), 
						&(ns->nand_active_t_sum));
#else
	            nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &srd);
#endif
	            nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;
	        }

	        xfer_size = spp->pgsz;
	        prev_ppa = cur_ppa;
	    }

	    // issue remaining io
	    if (xfer_size > 0) {
	        srd.xfer_size = xfer_size;
	        srd.ppa = &prev_ppa;
#ifdef CHIP_UTIL
			nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &srd, 
						&(ns->nand_idle_t_sum), 
						&(ns->nand_active_t_sum));
#else
	        nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &srd);
#endif
	        nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;
	    }
	}

	ret->nsecs_target = nsecs_latest;
	ret->status = NVME_SC_SUCCESS;
	return true;
}

#ifndef ZONE_MAPPING
bool conv_write(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];

	/* wbuf and spp are shared by all instances */
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct buffer * wbuf = conv_ftl->ssd->write_buffer;

	struct nvme_command *cmd = req->cmd;
	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;

	uint64_t lpn, local_lpn;
	uint32_t nr_parts = ns->nr_parts;

	uint64_t nsecs_start = req->nsecs_start;
	uint64_t nsecs_completed = 0, nsecs_latest;
	uint64_t nsecs_xfer_completed;
	uint32_t allocated_buf_size;
	struct ppa ppa;
	struct nand_cmd swr;
#ifdef MULTI_PARTITION_FTL
	int no_partition = NO_LOCAL_PARTITION(start_lpn / nr_parts);
	NVMEV_ASSERT(no_partition == NO_LOCAL_PARTITION(end_lpn / nr_parts));
#endif

	NVMEV_ASSERT(conv_ftls);
	NVMEV_DEBUG("conv_write: start_lpn=%lld, len=%d, end_lpn=%lld", start_lpn, nr_lba, end_lpn);

#ifndef MULTI_PARTITION_FTL
	if ((end_lpn/nr_parts) >= spp->tt_pgs) {
	    NVMEV_ERROR("conv_write: lpn passed FTL range(start_lpn=%lld,tt_pgs=%ld)\n", start_lpn, spp->tt_pgs);
	    return false;
	}
#else
	/* TODO: range check for multi page mapping */
#endif

	allocated_buf_size = buffer_allocate(wbuf, LBA_TO_BYTE(nr_lba));

	if (allocated_buf_size < LBA_TO_BYTE(nr_lba))
		return false;

	nsecs_latest = nsecs_start;
	nsecs_latest = ssd_advance_write_buffer(
			conv_ftl->ssd, nsecs_latest, LBA_TO_BYTE(nr_lba));
	nsecs_xfer_completed = nsecs_latest;

	swr.type = USER_IO;
	swr.cmd = NAND_WRITE;
	swr.stime = nsecs_latest;
	swr.interleave_pci_dma = false;

	for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
	    conv_ftl = &conv_ftls[lpn % nr_parts];
	    local_lpn = lpn / nr_parts;
	    ppa = get_maptbl_ent(conv_ftl, local_lpn);
	    if (mapped_ppa(&ppa)) {
	        /* update old page information first */
	        mark_page_invalid(conv_ftl, &ppa);
	        set_rmap_ent(conv_ftl, INVALID_LPN, &ppa);
	        NVMEV_DEBUG("conv_write: %lld is invalid, ", ppa2pgidx(conv_ftl, &ppa));
	    }

	    /* new write */
#ifndef MULTI_PARTITION_FTL
	    ppa = get_new_page(conv_ftl, USER_IO);
#else
	    ppa = get_new_page(conv_ftl, USER_IO, no_partition);
#endif
	    /* update maptbl */
	    set_maptbl_ent(conv_ftl, local_lpn, &ppa);
	    NVMEV_DEBUG("conv_write: got new ppa %lld, ", ppa2pgidx(conv_ftl, &ppa));
	    /* update rmap */
	    set_rmap_ent(conv_ftl, local_lpn, &ppa);

	    mark_page_valid(conv_ftl, &ppa);

	    /* need to advance the write pointer here */
#ifndef MULTI_PARTITION_FTL
	    advance_write_pointer(conv_ftl, USER_IO);
#else
	    advance_write_pointer(conv_ftl, USER_IO, no_partition);
#endif
	    /* Aggregate write io in flash page */
	    if (last_pg_in_wordline(conv_ftl, &ppa)) {
	        swr.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg;
	        swr.ppa = &ppa;
#ifdef CHIP_UTIL
			nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &swr, 
						&(ns->nand_idle_t_sum), 
						&(ns->nand_active_t_sum));
#else
	        nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &swr);
#endif
			nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;

	        enqueue_writeback_io_req(req->sq_id, nsecs_completed, wbuf, spp->pgs_per_oneshotpg * spp->pgsz);
	    }

#ifndef MULTI_PARTITION_FTL
	    consume_write_credit(conv_ftl);
	    check_and_refill_write_credit(conv_ftl);
#else
	    consume_write_credit(conv_ftl, no_partition);
	    check_and_refill_write_credit(conv_ftl, no_partition);
#endif
	}

	if ((cmd->rw.control & NVME_RW_FUA) || (spp->write_early_completion == 0)) {
		/* Wait all flash operations */
		ret->nsecs_target = nsecs_latest;
	} else {
		/* Early completion */
		ret->nsecs_target = nsecs_xfer_completed;
	}
	ret->status = NVME_SC_SUCCESS;

	return true;
}
#else 
/* zone mapping */
#ifndef COUPLED_GC_MTL
struct ppa write_meta_page_mapping_handler(struct conv_ftl *conv_ftl, uint64_t local_lpn, int no_partition)
#else
struct ppa write_meta_page_mapping_handler(struct conv_ftl *conv_ftl, uint64_t local_lpn, int no_partition, struct nvmev_result *ret)
#endif
{
	NVMEV_ASSERT(no_partition == META_PARTITION);

	if (out_of_meta_partition(conv_ftl, local_lpn)){
		NVMEV_INFO("[JWDBG] %s: out of meta partition. local_lpn: 0x%llx noPartition: %d\n",
						__func__, local_lpn, no_partition);
		NVMEV_ASSERT(0);
	}

	//NVMEV_ASSERT(!out_of_meta_partition(conv_ftl, lpn));
    struct ppa ppa = read_meta_page_mapping_handler(conv_ftl, local_lpn);
    if (mapped_ppa(&ppa)) {
        /* update old page information first */
        mark_page_invalid(conv_ftl, &ppa);
        set_rmap_ent(conv_ftl, INVALID_LPN, &ppa);
        NVMEV_DEBUG("conv_write: %lld is invalid, ", ppa2pgidx(conv_ftl, &ppa));
    }

    /* new write */
    ppa = get_new_page(conv_ftl, USER_IO, no_partition);
    /* update maptbl */
    set_maptbl_ent(conv_ftl, local_lpn, &ppa);
    NVMEV_DEBUG("conv_write: got new ppa %lld, ", ppa2pgidx(conv_ftl, &ppa));
    //printk("[JWDBG] lpn 0x%llx got new ppa %lld, ", local_lpn * SSD_PARTITIONS, ppa.ppa );
	return ppa;
}

#ifdef COUPLED_GC

struct ppa append_zone_mapping_handler(struct conv_ftl *conv_ftl, uint64_t local_lpn, int no_partition)
{
	struct ppa ppa, calc_ppa, *zone_map_ent;
	zone_map_ent = get_zone_maptbl_ent(conv_ftl, local_lpn);

	if (!mapped_ppa(zone_map_ent)) {
		/* new zone allocation */
		NVMEV_ASSERT(is_first_lpn_in_zone(conv_ftl, local_lpn));
		ppa = get_new_zone_for_append(conv_ftl, USER_IO, no_partition);
#ifdef GC_LOG_PRINT2
		printk("%s: new zone: lpn: 0x%lx local_lpn: 0x%lx, ppa: 0x%llx", __func__, 
				LPN_FROM_LOCAL_LPN(local_lpn, conv_ftl->no_part, conv_ftl->ns->nr_parts),
				local_lpn, ppa.ppa);
#endif
		set_zone_maptbl_ent(conv_ftl, zone_map_ent, &ppa);

		update_window_mgmt_for_write(conv_ftl, local_lpn);
		NVMEV_ASSERT(get_zone_offset(conv_ftl, local_lpn) == 0);
		set_rmap_zone_ent(conv_ftl, local_lpn, USER_IO, no_partition);
		
		conv_ftl->valid_zone_cnt[no_partition] ++;
		conv_ftl->total_valid_zone_cnt ++;
	} else {
		/* To check ppa calculation in zone. */
		/* TODO: to reduce overhead, use calc_ppa_in_zones in conv_read only. */
		ppa = calc_ppa_in_zone(conv_ftl, local_lpn, zone_map_ent);
		if (is_first_lpn_in_zone(conv_ftl, local_lpn)){
			printk("%s: local_lpn: %lld, zonesz: %ld zoneofs: %lld", __func__, local_lpn,
				 conv_ftl->ssd->sp.pgs_per_line, get_zone_offset(conv_ftl, local_lpn));
		}
		NVMEV_ASSERT(!is_first_lpn_in_zone(conv_ftl, local_lpn));
		struct ppa _ppa = get_new_page(conv_ftl, USER_IO, no_partition);
		NVMEV_ASSERT(ppa.ppa == _ppa.ppa);
	}
	return ppa;
}
#endif

#ifndef COUPLED_GC_MTL
struct ppa write_zone_mapping_handler(struct conv_ftl *conv_ftl, uint64_t local_lpn, int no_partition)
#else
struct ppa write_zone_mapping_handler(struct conv_ftl *conv_ftl, uint64_t local_lpn, int no_partition, 
		struct nvmev_result *ret)
#endif
{
	struct ppa ppa, calc_ppa, *zone_map_ent;
	//static int wcnt = 0;
	//wcnt ++;
	//if (wcnt % PCNT == 0)
	//	printk("%s: dcnt: %d", __func__, wcnt);
	zone_map_ent = get_zone_maptbl_ent(conv_ftl, local_lpn);
	if (!mapped_ppa(zone_map_ent)) {
		/* new zone allocation */
#ifdef GURANTEE_SEQ_WRITE
		NVMEV_ASSERT(is_first_lpn_in_zone(conv_ftl, local_lpn));
#endif
		//print_dbg_get_zone_maptbl_ent(conv_ftl, local_lpn, __func__);
#ifndef COUPLED_GC_MTL
		ppa = get_new_zone(conv_ftl, USER_IO, no_partition);
#else
		ppa = get_new_zone(conv_ftl, USER_IO, no_partition, ret);
#endif
#ifdef JWDBG_CONV_FTL
		//printk("[JWDBG] %s: new zone! ppa: %llx %lld\n", __func__, ppa.ppa, ppa.ppa);	
#endif
		set_zone_maptbl_ent(conv_ftl, zone_map_ent, &ppa);
#ifdef COUPLED_GC
		update_window_mgmt_for_write(conv_ftl, local_lpn);
#endif
#ifndef GURANTEE_SEQ_WRITE
		if (!is_first_lpn_in_zone(conv_ftl, local_lpn)){
			ppa = calc_ppa_in_zone(conv_ftl, local_lpn, &ppa);
		}
#endif
		set_rmap_zone_ent(conv_ftl, local_lpn - get_zone_offset(conv_ftl, local_lpn), USER_IO, no_partition);
		conv_ftl->valid_zone_cnt[no_partition] ++;
		conv_ftl->total_valid_zone_cnt ++;

		//printk("%s: part: %d start lpn: 0x%lx line: 0x%lx", __func__, 
		//		conv_ftl->no_part, 
		//		LPN_FROM_LOCAL_LPN(get_line(conv_ftl, &ppa)->start_local_lpn,
		//			conv_ftl->no_part, conv_ftl->ns->nr_parts), 
		//		get_line(conv_ftl, &ppa)
		//		);
		//set_rmap_zone_ent(conv_ftl, local_lpn, USER_IO, no_partition);
	} else {
		/* To check ppa calculation in zone. */
		/* TODO: to reduce overhead, use calc_ppa_in_zones in conv_read only. */
		ppa = calc_ppa_in_zone(conv_ftl, local_lpn, zone_map_ent);
#ifdef GURANTEE_SEQ_WRITE
		NVMEV_ASSERT(!is_first_lpn_in_zone(conv_ftl, local_lpn));
		_ppa = get_new_page(conv_ftl, USER_IO, no_partition);
		NVMEV_ASSERT(ppa.ppa == _ppa.ppa);
#else
#endif
		//NVMEV_DEBUG("conv_write: got new ppa %lld, ", ppa2pgidx(conv_ftl, &ppa));
	}
	return ppa;
}

#ifndef GURANTEE_SEQ_WRITE
static void classify_line(struct conv_ftl *conv_ftl, uint32_t io_type, int no_partition, 
							struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *line;
	line = get_line(conv_ftl, ppa);

	if (line->wpc <= spp->pgs_per_line){

	}
	NVMEV_ASSERT(line->wpc <= spp->pgs_per_line);
	if (line->wpc == spp->pgs_per_line) {
		if (line->vpc == spp->pgs_per_line) {
			/* all pgs are still valid, move to full line list */
			NVMEV_ASSERT(line->ipc == 0);
			list_add_tail(&line->entry, &lm->full_line_list);
			lm->full_line_cnt++;
			NVMEV_DEBUG("%s: wpp: move line to full_line_list\n", __func__);
		} else {
			NVMEV_DEBUG("%s: wpp: line is moved to victim list\n", __func__);
			NVMEV_ASSERT(line->vpc >= 0 && line->vpc < spp->pgs_per_line);
			/* there must be some invalid pages in this line */
			NVMEV_ASSERT(line->ipc > 0);
			pqueue_insert(lm->victim_line_pq, line);
			lm->victim_line_cnt++;
		}
	}
}
#endif

/* write for zone mapping */
bool lm_write(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];

	/* wbuf and spp are shared by all instances */
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct buffer * wbuf = conv_ftl->ssd->write_buffer;

	struct nvme_command *cmd = req->cmd;
	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;

	uint64_t lpn, local_lpn, logical_zoneno;
	uint32_t nr_parts = ns->nr_parts;

	uint64_t nsecs_start = req->nsecs_start;
	uint64_t nsecs_completed = 0, nsecs_latest;
	uint64_t nsecs_xfer_completed;
	uint32_t allocated_buf_size;
	struct ppa ppa, *zone_map_ent, calc_ppa;
	struct nand_cmd swr;
	
#ifdef HOST_GC_OVERHEAD_ANALYSIS
    ns->req_cnt ++;          
#endif                          
#ifdef WAF	
	try_print_WAF(ns);
#endif
	
	//printk("lm_write: start_lpn=0x%llx, len=%d, end_lpn=0x%llx", start_lpn, nr_lba, end_lpn);
	uint64_t start_lpn_ = start_lpn;
	uint64_t end_lpn_ = end_lpn;

	/*SADDR_ZNS is start of main partitoin. but we subtract START_OFS_IN_MAIN_PART behind so just deduct SADDR_ZNS-START_OFS_IN_MAIN_PART*/
	if (start_lpn == SADDR_ZNS)
		start_done = true;
	if (start_lpn >= SADDR_ZNS) {
		start_lpn  = start_lpn - (SADDR_ZNS - START_OFS_IN_MAIN_PART) + 0x20000000;
		end_lpn  = end_lpn - (SADDR_ZNS - START_OFS_IN_MAIN_PART) + 0x20000000;
	}
	int no_partition = NO_LOCAL_PARTITION(start_lpn / nr_parts);
	if (no_partition == COLD_DATA_PARTITION || no_partition == COLD_NODE_PARTITION) {
		//printk("%s: host write on cold partition!! type: %d lpn: %lld", __func__, no_partition, start_lpn);
	}

	//if (no_partition != NO_LOCAL_PARTITION(end_lpn / nr_parts)) {
	//	printk("lm_write: start_lpn=0x%llx, len=%d, end_lpn=0x%llx", start_lpn_, nr_lba, end_lpn_);
	//}
	NVMEV_ASSERT(no_partition == NO_LOCAL_PARTITION(end_lpn / nr_parts));
	ret->cid = cmd->common.command_id;

#ifndef COUPLED_GC_MTL
	struct ppa (*write_handler) (struct conv_ftl *conv_ftl, uint64_t local_lpn, int no_partition);
#else
	struct ppa (*write_handler) (struct conv_ftl *conv_ftl, uint64_t local_lpn, int no_partition, struct nvmev_result *ret);
#endif
#ifndef GURANTEE_SEQ_WRITE
	void (*line_handler) (struct conv_ftl *conv_ftl, uint32_t io_type, int no_partition,
								struct ppa *ppa);
#endif
	//static int print = 1;
	NVMEV_ASSERT(conv_ftls);
	NVMEV_DEBUG("conv_write: start_lpn=%lld, len=%d, end_lpn=%lld", start_lpn, nr_lba, end_lpn);
#ifdef JWDBG_CONV_FTL
	/*static int print_ = 0;
	int print_interval = 1000;
	if (print_ % print_interval == 0){
		//printk("%s: slpn=%lld, len=%lld, elpn=%lld lpn: 0x%llx ~ 0x%llx", 
		//	__func__, start_lpn, nr_lba, end_lpn, 
		//	start_lpn, end_lpn);
	}
	print_ ++ ;*/
#endif


	if (IS_META_PARTITION(no_partition)){
		/* TODO: ftl range check for meta partition */
		write_handler = write_meta_page_mapping_handler;
#ifndef GURANTEE_SEQ_WRITE
		line_handler  = advance_write_pointer;
#endif
	} else if (IS_MAIN_PARTITION(no_partition)){
		write_handler = write_zone_mapping_handler;
#ifndef GURANTEE_SEQ_WRITE
		line_handler  = classify_line;
#endif
		//if (out_of_partition(conv_ftl, end_lpn/nr_parts)){
		//	if (print){
	    //		printk("conv_write: lpn passed FTL range(start_lpn=0x%llx,tt_pgs=%ld)\n", start_lpn, spp->tt_pgs);
		//		print = 0;
		//	}
		//	NVMEV_ASSERT(0);
	    //	//NVMEV_ERROR("conv_write: lpn passed FTL range(start_lpn=0x%llx,tt_pgs=%ld)\n", start_lpn, spp->tt_pgs);
	    //	return false;
		//}
	} else {
		NVMEV_ERROR("%s: partition %d error\n", __func__, no_partition);
		printk("%s: partition %d error\n", __func__, no_partition);
		return false;
	}

	allocated_buf_size = buffer_allocate(wbuf, LBA_TO_BYTE(nr_lba));

	if (allocated_buf_size < LBA_TO_BYTE(nr_lba)){
		//printk("%s: wrong 1!!!!!!!", __func__);
		return false;
	}

	nsecs_latest = nsecs_start;
	nsecs_latest = ssd_advance_write_buffer(
			conv_ftl->ssd, nsecs_latest, LBA_TO_BYTE(nr_lba));
	nsecs_xfer_completed = nsecs_latest;

	swr.type = USER_IO;
	swr.cmd = NAND_WRITE;
	swr.stime = nsecs_latest;
	swr.interleave_pci_dma = false;

	struct line *tmp_line;
	static uint64_t min_slpn[NO_TYPE] = {0xffffffff, 0xffffffff, \
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
	if (IS_MAIN_PARTITION(no_partition)){
		start_lpn -= START_OFS_IN_MAIN_PART;
		end_lpn -= START_OFS_IN_MAIN_PART;
		if (min_slpn[NO_PARTITION(start_lpn)] > start_lpn){
			min_slpn[NO_PARTITION(start_lpn)] = start_lpn;
			//printk("%s: type: %lld start_lpn: 0x%llx", __func__, NO_PARTITION(start_lpn), start_lpn);
		}
	}
	
	//printk("lm_write: start_lpn= 0x%lx, end_lpn= 0x%lx", start_lpn, end_lpn);
	
#ifdef WAF
	ns->write_volume_host += (end_lpn - start_lpn + 1);
	ns->total_write_volume_host += (end_lpn - start_lpn + 1);
#endif

#ifdef CMD_CNT
	if (start_done) {
		if ((end_lpn - start_lpn + 1) < 0) {
			printk("%s: !!!!!!!!!!!!!!!!!!!!!!: slpn: 0x%llx elpn: 0x%llx s: 0x%llx e: 0x%llx", 
					__func__, start_lpn, end_lpn, 
					start_lpn_, end_lpn_);

		}	
		ns->total_write_blks_host += (end_lpn - start_lpn + 1);
	}
#endif
	/* meta partition. page mapping */
	for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
	    conv_ftl = &conv_ftls[lpn % nr_parts];
	    local_lpn = lpn / nr_parts;
#ifndef COUPLED_GC_MTL
		ppa = write_handler(conv_ftl, local_lpn, no_partition);
#else
		ppa = write_handler(conv_ftl, local_lpn, no_partition, ret);
#endif
#ifdef JWDBG_CONV_FTL
		//printk("[JWDBG] %s: lpn: 0x%llx local lpn: 0x%llx ppa: 0x%llx %lld\n", 
		//		__func__, lpn, local_lpn, ppa.ppa, ppa.ppa);
#endif
		//if (lpn > 0x20000000) {
		//	printk("%s: lpn: 0x%lx local_lpn: 0x%lx ftl no: %u ppa: 0x%lx", 
		//			__func__, lpn, local_lpn, lpn % nr_parts, ppa.ppa);
		//}
	    /* update rmap */
	    set_rmap_ent(conv_ftl, local_lpn, &ppa);
		
	    check_mark_page_valid(conv_ftl, &ppa, local_lpn);
	    mark_page_valid(conv_ftl, &ppa);

	    /* need to advance the write pointer here */
#ifdef GURANTEE_SEQ_WRITE
	    advance_write_pointer(conv_ftl, USER_IO, no_partition);
#else
	    line_handler(conv_ftl, USER_IO, no_partition, &ppa);
#endif
	    /* Aggregate write io in flash page */
	    if (last_pg_in_wordline(conv_ftl, &ppa)) {
	        swr.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg;
	        swr.ppa = &ppa;
#ifdef CHIP_UTIL
			nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &swr, 
						&(ns->nand_idle_t_sum), 
						&(ns->nand_active_t_sum));
#else
	        nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &swr);
#endif
	        nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;

	        enqueue_writeback_io_req(req->sq_id, nsecs_completed, wbuf, spp->pgs_per_oneshotpg * spp->pgsz);
	    }

#ifndef MULTI_PARTITION_FTL
	    consume_write_credit(conv_ftl);
	    check_and_refill_write_credit(conv_ftl);
#else
	    consume_write_credit(conv_ftl, no_partition);
#ifndef  COUPLED_GC_MTL
	    check_and_refill_write_credit(conv_ftl, no_partition);
#else
	    check_and_refill_write_credit(conv_ftl, no_partition, ret);
#endif

#endif
	}

	if ((cmd->rw.control & NVME_RW_FUA) || (spp->write_early_completion == 0)) {
		/* Wait all flash operations */
		ret->nsecs_target = nsecs_latest;
	} else {
		/* Early completion */
		ret->nsecs_target = nsecs_xfer_completed;
	}
	ret->status = NVME_SC_SUCCESS;

	return true;
}
#endif

#ifdef DISCARD_ENABLED

static inline void discard_zone_mapping_handler(struct conv_ftl *conv_ftl, uint64_t local_lpn,
													struct ppa *ppa)
{
//	static int dcnt = 0;
//	dcnt ++;
//	if (dcnt % PCNT == 0)
//		printk("%s: dcnt: %d", __func__, dcnt);
	if (invalidate_zone_maptbl_ent(conv_ftl, local_lpn, ppa)){
#ifdef COUPLED_GC
		update_window_mgmt_for_discard(conv_ftl, local_lpn);
#endif
	}
}


#define BUF_CNT_ 10
static int glb_cnt = 0;
		
static uint64_t glb_old_stack[BUF_CNT_], glb_new_stack[BUF_CNT_], glb_cid_stack[BUF_CNT_];

bool conv_discard(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];

	/* wbuf and spp are shared by all instances */
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	struct nvme_command *cmd = req->cmd;
	uint64_t nranges = cmd->dsm.nr + 1; /* zero-based */
	u64 paddr = cmd->dsm.prp1;
	void *vaddr = kmap_atomic_pfn(PRP_PFN(paddr));
	struct nvme_dsm_range *dsm_range = (struct nvme_dsm_range *) vaddr;
	int i;
	//memcpy(vdev->ns[nsid].mapped + offset, vaddr + mem_offs, io_size);

	NVMEV_ASSERT(conv_ftls);
	uint64_t lpn, local_lpn, start_lpn, end_lpn, lba, nr_lba;
	uint32_t nr_parts = ns->nr_parts;
	struct ppa ppa;
	//uint64_t nr_lpn_total = 0;

#ifdef HOST_GC_OVERHEAD_ANALYSIS
    ns->req_cnt ++;          
#endif                          
#ifdef WAF	
	try_print_WAF(ns);
#endif

#if (defined ZONE_MAPPING || defined MULTI_PARTITION_FTL)
	int no_partition;
	struct ppa (*read_handler) (struct conv_ftl *conv_ftl, uint64_t local_lpn);
	void (*discard_handler) (struct conv_ftl *conv_ftl, uint64_t local_lpn, struct ppa *ppa);
#endif

	static int getit = 0;
	uint64_t total_dblk = 0;
	
	ret->cid = cmd->common.command_id;
	if (!start_done)
		goto goto_end;

#ifdef CMD_CNT
	if (start_done) {
		ns->total_discard_cmds_host ++;
	}
#endif

	for (i = 0; i < nranges; i ++){
		lba = dsm_range[i].slba;
		//nr_lba = dsm_range[i].nlb + 1; /* zero-based */
		nr_lba = dsm_range[i].nlb; /* zero-based */
		
		start_lpn = lba / spp->secs_per_pg;
		end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;
		NVMEV_DEBUG("conv_discard: start_lpn=%lld, len=%d, end_lpn=%lld", start_lpn, nr_lba, end_lpn);
		
		/*SADDR_ZNS is start of main partitoin. but we subtract START_OFS_IN_MAIN_PART behind so just deduct SADDR_ZNS-START_OFS_IN_MAIN_PART*/
		if (start_lpn >= SADDR_ZNS) {
			start_lpn  = start_lpn - (SADDR_ZNS - START_OFS_IN_MAIN_PART) + 0x20000000;
			end_lpn  = end_lpn - (SADDR_ZNS - START_OFS_IN_MAIN_PART) + 0x20000000;
		}
		
		//printk("conv_discard: start_lpn: 0x%llx, len=%d, end_lpn: 0x%llx", start_lpn, nr_lba, end_lpn);
#ifdef JWDBG_CONV_FTL
		//NVMEV_INFO("conv_discard: start_lpn=%lld, len=%lld, end_lpn=%lld nranges: %lld", start_lpn, nr_lba, end_lpn, nranges);
#endif

#if !(defined ZONE_MAPPING || defined MULTI_PARTITION_FTL)
		if ((end_lpn/nr_parts) >= spp->tt_pgs) {
		    NVMEV_ERROR("conv_discard: lpn passed FTL range(start_lpn=%lld,tt_pgs=%ld)\n", start_lpn, spp->tt_pgs);
		    return false;
		}
#endif
	

#if !(defined ZONE_MAPPING || defined MULTI_PARTITION_FTL)
		/* TODO: Alignment Check for conventional SSD */
		/* TODO: Sector level bitmap? */
#else
		no_partition = NO_LOCAL_PARTITION(start_lpn / nr_parts);
		//if (no_partition != NO_LOCAL_PARTITION(end_lpn / nr_parts)) {
		//	goto goto_end;
		//}
		NVMEV_ASSERT(no_partition == NO_LOCAL_PARTITION(end_lpn / nr_parts));
		
		if (IS_MAIN_PARTITION(no_partition)){
			start_lpn -= START_OFS_IN_MAIN_PART;
			end_lpn -= START_OFS_IN_MAIN_PART;
			total_dblk += (end_lpn - start_lpn + 1);
			getit = 1;
		}
		static uint64_t expected_future_lba = 0xffffffff;
		static bool was_long_discard = false;

		/* Alignment Check */
		if (IS_MAIN_PARTITION(no_partition)){
			if (nr_lba % spp->secs_per_pg != 0 || lba % spp->secs_per_pg != 0){
				/* Corner case for cassandra DB YCSB A workload. */
				/* In the future, we need to modify host side to align discard sector size to 4 KByte block size. */
				if (was_long_discard){
					NVMEV_ASSERT(expected_future_lba != 0xffffffff);
					NVMEV_ASSERT(expected_future_lba == lba);
					lba ++;
					nr_lba --;
					NVMEV_ASSERT(!(nr_lba % spp->secs_per_pg != 0 || lba % spp->secs_per_pg != 0));
					start_lpn = lba / spp->secs_per_pg;
					end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;
					was_long_discard = false;
					expected_future_lba = 0xffffffff;

				} else if (nr_lba == 0x7fffff) {
					was_long_discard = true;
					expected_future_lba = nr_lba + lba;
					nr_lba ++;
					NVMEV_ASSERT(!(nr_lba % spp->secs_per_pg != 0 || lba % spp->secs_per_pg != 0));
					start_lpn = lba / spp->secs_per_pg;
					end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;

				} else {
					printk("[JWDBG] %s: lba: 0x%llx nr lba: 0x%llx not aligned!! slpn: 0x%llx 0x%llx", 
						__func__, 
						lba, nr_lba, lba / spp->secs_per_pg, start_lpn);
				}

				//int jw_i;
				//uint64_t jw_lba, jw_nr_lba;

				//for (jw_i = 0; jw_i < nranges; jw_i ++){
				//	jw_lba = dsm_range[jw_i].slba;
				//	//nr_lba = dsm_range[i].nlb + 1; /* zero-based */
				//	jw_nr_lba = dsm_range[jw_i].nlb; /* zero-based */
				//	printk("%s: %d th: lba: 0x%llx nr_lba: 0x%llx", 
				//			__func__, jw_i, jw_lba, jw_nr_lba);
				//}

			}
			//NVMEV_ASSERT(lba % spp->secs_per_pg == 0);
			//NVMEV_ASSERT(nr_lba % spp->secs_per_pg == 0);
		}

#ifdef ZONE_MAPPING
		if (IS_META_PARTITION(no_partition)){
			read_handler = read_meta_page_mapping_handler;
			NVMEV_ERROR("[JWDBG] %s: discard on meta partition. slpn: 0x%llx elpn; 0x%llx\n", 
							__func__, start_lpn, end_lpn);
			discard_handler = invalidate_maptbl_ent;
		} else if (IS_MAIN_PARTITION(no_partition)){
			read_handler = read_zone_mapping_handler;
			/* comment here due to the coupled gc. */
			/*if (out_of_partition(conv_ftl, end_lpn/nr_parts)){
	    		NVMEV_ERROR("conv_discard: lpn passed FTL range(start_lpn=0x%llx,tt_pgs=%ld)\n", start_lpn, spp->tt_pgs);
				NVMEV_ASSERT(0);
				return false;
			}*/
			discard_handler = discard_zone_mapping_handler;
		} else {
			NVMEV_ERROR("%s: partition %d error\n", __func__, no_partition);
		}
#else
		read_handler = get_maptbl_ent;
		discard_handler = invalidate_maptbl_ent;
#endif
#endif
		//printk("%s: start lpn: 0x%llx end lpn: 0x%llx len: %llu",
		//		__func__, start_lpn, end_lpn, end_lpn - start_lpn + 1);
//		static int cnt = 0;
//		
//		static uint64_t old_stack[BUF_CNT_], new_stack[BUF_CNT_], cid_stack[BUF_CNT_];
//		
//		//if ((start_lpn & 0xe0000000) == 0xe0000000 ) {
//		if (1) {
//			old_stack[cnt] = start_lpn;
//			new_stack[cnt] = end_lpn;
//			cid_stack[cnt] = ret->cid;
//			cnt ++;
//			if (cnt == BUF_CNT_) {
//#ifdef PLEASE
//				printk("%s \n \
//						cid: %u start_lpn: 0x%llx end_lpn: 0x%llx \n \
//						cid: %u start_lpn: 0x%llx end_lpn: 0x%llx \n \
//						cid: %u start_lpn: 0x%llx end_lpn: 0x%llx \n \
//						cid: %u start_lpn: 0x%llx end_lpn: 0x%llx \n \
//						cid: %u start_lpn: 0x%llx end_lpn: 0x%llx \n \
//						cid: %u start_lpn: 0x%llx end_lpn: 0x%llx \n \
//						cid: %u start_lpn: 0x%llx end_lpn: 0x%llx \n \
//						cid: %u start_lpn: 0x%llx end_lpn: 0x%llx \n \
//						cid: %u start_lpn: 0x%llx end_lpn: 0x%llx \n \
//						cid: %u start_lpn: 0x%llx end_lpn: 0x%llx \
//						", __func__,
//						cid_stack[0], old_stack[0], new_stack[0],
//						cid_stack[1], old_stack[1], new_stack[1],
//						cid_stack[2], old_stack[2], new_stack[2],
//						cid_stack[3], old_stack[3], new_stack[3],
//						cid_stack[4], old_stack[4], new_stack[4],
//						cid_stack[5], old_stack[5], new_stack[5],
//						cid_stack[6], old_stack[6], new_stack[6],
//						cid_stack[7], old_stack[7], new_stack[7],
//						cid_stack[8], old_stack[8], new_stack[8],
//						cid_stack[9], old_stack[9], new_stack[9]
//						//old_stack[10], new_stack[10],
//						//old_stack[11], new_stack[11],
//						//old_stack[12], new_stack[12],
//						//old_stack[13], new_stack[13],
//						//old_stack[14], new_stack[14],
//						//old_stack[15], new_stack[15],
//						//old_stack[16], new_stack[16],
//						//old_stack[17], new_stack[17],
//						//old_stack[18], new_stack[18],
//						//old_stack[19], new_stack[19]
//				);
//#endif
//				cnt = 0;
//			}
//		}
		//printk("conv_discard: start_lpn = 0x%lx end_lpn = 0x%lx", start_lpn, end_lpn);
		for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
		    conv_ftl = &conv_ftls[lpn % nr_parts];
		    local_lpn = lpn / nr_parts;
			uint64_t ori_lpn = lpn;
			
			int translated = 0;

#if (defined ZONE_MAPPING || defined MULTI_PARTITION_FTL)
		    ppa = read_handler(conv_ftl, local_lpn);
#else
		    ppa = get_maptbl_ent(conv_ftl, local_lpn);
#endif
			int trial_cnt = 0;
invalidate_page:
		    if (mapped_ppa(&ppa)) {
				
				if ((get_pg(conv_ftl->ssd, &ppa))->status != PG_VALID){
					printk("HALOOOO 1");
					printk("[JWDBG] %s: ori lpn: 0x%lx lpn: 0x%lx line: 0x%p ppa: 0x%llx pg status: %d translated: %d", 
							__func__, ori_lpn, local_lpn*nr_parts, 
							get_line(conv_ftl, &ppa),
							ppa.ppa, get_pg(conv_ftl->ssd, &ppa)->status, translated);
					printk("[JWDBG] %s: ori lpn: 0x%lx lpn: 0x%lx start lpn: 0x%lx ppa: 0x%llx pg status: %d translated: %d line: 0x%lx line wpc: %d", 
							__func__, ori_lpn, local_lpn*nr_parts, 
							LPN_FROM_LOCAL_LPN(get_line(conv_ftl, &ppa)->start_local_lpn,
								conv_ftl->no_part, conv_ftl->ns->nr_parts), 
							ppa.ppa, get_pg(conv_ftl->ssd, &ppa)->status, translated, 
							get_line(conv_ftl, &ppa), get_line(conv_ftl, &ppa)->wpc);
					printk("HALOOOO 2");
				}
		        
				mark_page_invalid(conv_ftl, &ppa);

		        set_rmap_ent(conv_ftl, INVALID_LPN, &ppa);
		        NVMEV_DEBUG("conv_write: %lld is invalid, ", ppa2pgidx(conv_ftl, &ppa));
#if (defined ZONE_MAPPING || defined MULTI_PARTITION_FTL)
    			discard_handler(conv_ftl, local_lpn, &ppa);
#else
    			invalidate_maptbl_ent(conv_ftl, local_lpn);
#endif
		    }
#if (defined ZONE_MAPPING || defined MULTI_PARTITION_FTL)
		    else if (IS_MAIN_PARTITION(no_partition)){
#ifdef COUPLED_GC
				if (IS_GC_PARTITION(no_partition)) {
					if (get_zone_remain_cnt(conv_ftl, local_lpn) <= 0) {
						printk("%s: lpn: 0x%lx remain_cnt: %u", __func__, 
								LPN_FROM_LOCAL_LPN(local_lpn, conv_ftl->no_part, conv_ftl->ns->nr_parts), 
								get_zone_remain_cnt(conv_ftl, local_lpn));

					}

					NVMEV_ASSERT(get_zone_remain_cnt(conv_ftl, local_lpn) > 0);
				}

				uint64_t trans_local_lpn;
				struct ppa trans_ppa;
				struct gc_log gc_log_ret;
				if (trial_cnt > 0)
					printk("%s: WHAT the SHIVAL? trial_cnt: %d", __func__, trial_cnt);
				trans_ppa = pop_from_aimless_translator(conv_ftl, local_lpn, &trans_local_lpn, &gc_log_ret);
				//if (gc_log_ret.old_lpn != 
				//		LPN_FROM_LOCAL_LPN(local_lpn, conv_ftl->no_part, conv_ftl->ns->nr_parts)) {
				//	int iiii = 0;
				//	for (iiii = 0; iiii < cnt; iiii ++) {
				//		printk("%s: start_lpn: 0x%lx end_lpn: 0x%lx", 
				//				__func__, old_stack[iiii], new_stack[iiii]);
				//	}
				//	for (iiii = 0; iiii < glb_cnt; iiii ++) {
				//		printk("%s: cid %u comp: old_lpn: 0x%lx new_lpn: 0x%lx", 
				//				__func__, glb_cid_stack[iiii], glb_old_stack[iiii], glb_new_stack[iiii]);
				//	}
				//	printk("%s: lpn: 0x%lx", __func__,  
				//			LPN_FROM_LOCAL_LPN(local_lpn, conv_ftl->no_part, conv_ftl->ns->nr_parts));

				//}
				//NVMEV_ASSERT(gc_log_ret.old_lpn == LPN_FROM_LOCAL_LPN(local_lpn, conv_ftl->no_part, conv_ftl->ns->nr_parts));
				if (mapped_ppa(&trans_ppa)){
					local_lpn = trans_local_lpn;
					ppa = trans_ppa;

					/* add translation log for mtl */
					add_mtl_translation_log(ns, ret, gc_log_ret.old_lpn, gc_log_ret.new_lpn); 
					//printk("%s: get helped from aimless translator", __func__);
					trial_cnt ++;

					translated = 1;

					goto invalidate_page;
					/*NVMEV_ASSERT(mapped_ppa(&ppa));
		        	mark_page_invalid(conv_ftl, &ppa);
		        	set_rmap_ent(conv_ftl, INVALID_LPN, &ppa);
		        	NVMEV_DEBUG("conv_write: %lld is invalid, ", ppa2pgidx(conv_ftl, &ppa));
    				discard_handler(conv_ftl, local_lpn, &ppa);*/
				}
#endif
				NVMEV_ERROR("conv_discard: discard target lpn 0x%llx must be valid!! slpn: 0x%lx elpn: 0x%lx", lpn, start_lpn, end_lpn);
		    } else {
				if (getit) {
						printk("%s: something can be wrong lpn: 0x%lx ~ 0x%lx !!!!!!!!!!!!!!!!!!!!!!!!!",
							   	__func__, lpn, end_lpn);
				}
				static int cnt_ = 0;
				if (!IS_META_PARTITION(no_partition)){
					cnt_ ++;
					if (cnt_ > 20) {
						printk("%s: something can be wrong lpn: 0x%lx ~ 0x%lx !!!!!!!!!!!!!!!!!!!!!!!!!",
							   	__func__, lpn, end_lpn);
					}
				}

			}
#endif
		}
	}
	//printk("%s: total_dblk: %llu nranges: %lld avg len: %llu", 
	//		__func__, total_dblk, nranges, total_dblk / nranges);
goto_end:
	kunmap_atomic(vaddr);	
	ret->status = NVME_SC_SUCCESS;

	return true;
}
#endif

void conv_flush(struct nvmev_ns *ns, struct nvmev_request * req, struct nvmev_result * ret)
{
	uint64_t start, latest;
	uint32_t i;
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;

	start = local_clock();
	latest = start;
	for (i = 0; i < ns->nr_parts; i++) {
	    latest = max(latest, ssd_next_idle_time(conv_ftls[i].ssd));
	}

	NVMEV_DEBUG("%s latency=%llu\n",__FUNCTION__, latest - start);

	ret->status = NVME_SC_SUCCESS;
	ret->nsecs_target = latest;
	return;
}

#ifdef MIGRATION_IO
void conv_proc_nvme_rev_io_cmd(struct nvmev_ns * ns, struct nvme_rev_completion *cmd)
{
	struct gc_log_mgmt *gclm = ns->gclm;
	struct inflight_set_entry *ise;
	unsigned int command_id = (unsigned int) cmd->command_id;

	static unsigned int last_secno[SSD_PARTITIONS] = 
	{NULL_SECNO, NULL_SECNO, NULL_SECNO, NULL_SECNO};
	unsigned int cur_secno[SSD_PARTITIONS], no_ftl;
	struct conv_ftl *conv_ftl_tmp;
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;

	/* remove gc_log */
	ise = &gclm->ise_array[command_id % NR_INFLIGHT_SET];
	
	//if (command_id % 1000 == 0) {
	//	printk("%s: nxt_cid: %u completed_cid: %u", __func__, gclm->next_command_id, command_id);

	//}
	//if (ise->command_id != cmd->command_id) 
	//	printk("%s: ise cid: %u completed cid: %u", __func__, ise->command_id, cmd->command_id);

	gclm->completed_command_id ++;

	NVMEV_ASSERT(ise->command_id != INVALID_COMMAND_ID);
	NVMEV_ASSERT(ise->command_id == cmd->command_id);
	
	//printk("%s: free! command id: %u idx: %u", __func__, cmd->command_id, 
	//		cmd->command_id % NR_INFLIGHT_SET);	

	
	struct list *gc_log_list = &ise->gc_log_list;

	while(!list_empty_(gc_log_list)){
		struct list_elem * le = list_front(gc_log_list);
		struct gc_log * gcle = list_entry(le, struct gc_log, list_elem);
		NVMEV_ASSERT(gcle->status == GC_LOG_INFLIGHT);
		
//#ifdef SHIVAL3
//		
//		if ((gcle->old_lpn & 0xe0000000) == 0xe0000000 ||
//			(gcle->new_lpn & 0xe0000000) == 0xe0000000 ) {
//			glb_cid_stack[glb_cnt] = cmd->command_id;
//			glb_old_stack[glb_cnt] = gcle->old_lpn;
//			glb_new_stack[glb_cnt] = gcle->new_lpn;
//			glb_cnt ++;
//			if (glb_cnt == BUF_CNT_) {
//				printk("%s \n \
//						cid: %u old_lpn: 0x%llx new_lpn: 0x%llx \n \
//						cid: %u old_lpn: 0x%llx new_lpn: 0x%llx \n \
//						cid: %u old_lpn: 0x%llx new_lpn: 0x%llx \n \
//						cid: %u old_lpn: 0x%llx new_lpn: 0x%llx \n \
//						cid: %u old_lpn: 0x%llx new_lpn: 0x%llx \n \
//						cid: %u old_lpn: 0x%llx new_lpn: 0x%llx \n \
//						cid: %u old_lpn: 0x%llx new_lpn: 0x%llx \n \
//						cid: %u old_lpn: 0x%llx new_lpn: 0x%llx \n \
//						cid: %u old_lpn: 0x%llx new_lpn: 0x%llx \n \
//						cid: %u old_lpn: 0x%llx new_lpn: 0x%llx \
//						", __func__,
//						glb_cid_stack[0], glb_old_stack[0], glb_new_stack[0],
//						glb_cid_stack[1], glb_old_stack[1], glb_new_stack[1],
//						glb_cid_stack[2], glb_old_stack[2], glb_new_stack[2],
//						glb_cid_stack[3], glb_old_stack[3], glb_new_stack[3],
//						glb_cid_stack[4], glb_old_stack[4], glb_new_stack[4],
//						glb_cid_stack[5], glb_old_stack[5], glb_new_stack[5],
//						glb_cid_stack[6], glb_old_stack[6], glb_new_stack[6],
//						glb_cid_stack[7], glb_old_stack[7], glb_new_stack[7],
//						glb_cid_stack[8], glb_old_stack[8], glb_new_stack[8],
//						glb_cid_stack[9], glb_old_stack[9], glb_new_stack[9]
//						//old_stack[10], new_stack[10],
//						//old_stack[11], new_stack[11],
//						//old_stack[12], new_stack[12],
//						//old_stack[13], new_stack[13],
//						//old_stack[14], new_stack[14],
//						//old_stack[15], new_stack[15],
//						//old_stack[16], new_stack[16],
//						//old_stack[17], new_stack[17],
//						//old_stack[18], new_stack[18],
//						//old_stack[19], new_stack[19]
//				);
//				glb_cnt = 0;
//			}
//		}
//#endif
		//if ((gcle->old_lpn & 0xe0000000)== 0x60000000 || 
		//		(gcle->new_lpn & 0xe0000000) == 0x60000000) {
		//	printk("%s: free gc log: old lpn: 0x%llx new lpn: 0x%llx cid: %u",
		//		__func__, gcle->old_lpn, gcle->new_lpn, ise->command_id);
		//}
		if (IS_GC_PARTITION(NO_PARTITION(gcle->old_lpn))) {
			struct conv_ftl *conv_ftl = &conv_ftls[gcle->old_lpn % ns->nr_parts];
			dec_zone_remain_cnt(conv_ftl, LOCAL_LPN_FROM_LPN(gcle->old_lpn, ns->nr_parts));
		}
		
		/* TODO: This is heuristic code. Need to be polish in the future */	
		no_ftl = gcle->old_lpn % ns->nr_parts;
		conv_ftl_tmp = &conv_ftls[no_ftl];
		cur_secno[no_ftl] = get_zone_idx(conv_ftl_tmp, (gcle->old_lpn / ns->nr_parts));
		if (cur_secno[no_ftl] != last_secno[no_ftl]) {
			unsigned int no_part = NO_PARTITION(gcle->old_lpn);
			if (no_part == HOT_DATA_PARTITION || no_part == HOT_NODE_PARTITION) {
				if (conv_ftl_tmp->valid_zone_cnt[no_part] > 0)
					conv_ftl_tmp->valid_zone_cnt[no_part] --;
				if (conv_ftl_tmp->total_valid_zone_cnt > 0)
					conv_ftl_tmp->total_valid_zone_cnt --;
				//conv_ftl_tmp->valid_zone_cnt[no_part] --;
				if (conv_ftl_tmp->gc_free_zone_cnt[no_part] > 0)
					conv_ftl_tmp->gc_free_zone_cnt[no_part] --;
			} 
			last_secno[no_ftl] = cur_secno[no_ftl];
		}

#ifdef MG_HANDLER_DISABLED
		list_remove(&gcle->list_elem);
		list_push_back(&gclm->unhandled_gc_log_list, &gcle->list_elem);
#else
		free_gc_log(gclm, gcle);
#endif
	}

#ifdef SHIVAL
	//printk("%s: complete rev cmd. cid: %u", __func__, ise->command_id);
#endif
	//printk("%s: complete rev cmd. cid: %u", __func__, ise->command_id);

	ise->command_id = INVALID_COMMAND_ID;
	gclm->n_ise --;

}
#endif

bool conv_proc_nvme_io_cmd(struct nvmev_ns * ns, struct nvmev_request * req, struct nvmev_result * ret)
{
	struct nvme_command *cmd = req->cmd;

	NVMEV_ASSERT(ns->csi == NVME_CSI_NVM);

	switch(cmd->common.opcode) {
	case nvme_cmd_write:
#ifdef ZONE_MAPPING
		if (!lm_write(ns, req, ret))
#else
		if (!conv_write(ns, req, ret))
#endif
			return false;
		break;
	case nvme_cmd_read:
		if (!conv_read(ns, req, ret))
			return false;
		break;
	case nvme_cmd_flush:
		conv_flush(ns, req, ret);
		break;
	case nvme_cmd_write_uncor:
	case nvme_cmd_compare:
	case nvme_cmd_write_zeroes:
	case nvme_cmd_dsm:
#ifdef DISCARD_ENABLED
		if (!conv_discard(ns, req, ret))
			return false;
		break;
#endif
	case nvme_cmd_resv_register:
	case nvme_cmd_resv_report:
	case nvme_cmd_resv_acquire:
	case nvme_cmd_resv_release:
		break;
	default:
		break;
	}

	return true;
}
