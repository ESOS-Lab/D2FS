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
 * *********************************************************************/

#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/highmem.h>
#include <linux/sched/clock.h>

#include "nvmev.h"
#include "dma.h"

#ifdef MULTI_PARTITION_MTL
#include "list.h"
#endif

#if ((BASE_SSD == SAMSUNG_970PRO) || (BASE_SSD) == ZNS_PROTOTYPE)
#include "ssd.h"
#else
struct buffer;
#endif

#undef PERF_DEBUG

#define PRP_PFN(x)	((unsigned long)((x) >> PAGE_SHIFT))

#define sq_entry(entry_id) \
	sq->sq[SQ_ENTRY_TO_PAGE_NUM(entry_id)][SQ_ENTRY_TO_PAGE_OFFSET(entry_id)]
#define cq_entry(entry_id) \
	cq->cq[CQ_ENTRY_TO_PAGE_NUM(entry_id)][CQ_ENTRY_TO_PAGE_OFFSET(entry_id)]

#ifdef MIGRATION_IO

#define rev_sq_entry(entry_id) \
	rsq->sq[REV_SQ_ENTRY_TO_PAGE_NUM(entry_id)][REV_SQ_ENTRY_TO_PAGE_OFFSET(entry_id)]
#define rev_cq_entry(entry_id) \
	rcq->cq[REV_CQ_ENTRY_TO_PAGE_NUM(entry_id)][REV_CQ_ENTRY_TO_PAGE_OFFSET(entry_id)]

#define rev_mgb_entry(entry_id) \
	rsq->mpb[REV_MGB_ENTRY_TO_PAGE_NUM(entry_id)][REV_MGB_ENTRY_TO_PAGE_OFFSET(entry_id)]

#endif

#define BUF_CNT_ 10
int glb_cnt = 0;

uint64_t glb_old_stack[BUF_CNT_], glb_new_stack[BUF_CNT_];

static int mg_cnt = 0;

static uint64_t mg_old_stack[BUF_CNT_], mg_new_stack[BUF_CNT_], mg_order_stack[BUF_CNT_];


static int ent_cnt = 0;

#define ENT_CNT_ 30
static uint64_t ent_stack[ENT_CNT_], ent_stack2[ENT_CNT_], order_stack[ENT_CNT_], way_stack[ENT_CNT_];


extern struct nvmev_dev *vdev;

static int io_using_dma = false;

static inline unsigned long long __get_wallclock(void)
{
	return cpu_clock(vdev->config.cpu_nr_dispatcher);
}

#ifdef MULTI_PARTITION_MTL

static inline bool out_of_mtl_window(struct nvmev_ns *ns, uint64_t lba)
{
	unsigned int partno = NO_PARTITION(lba);
	uint64_t start_zoneno = ns->start_zoneno[partno];
	uint64_t mtl_zoneno = NO_MTL_ZONE(lba);
	uint64_t n_mtl_zones = (IS_GC_PARTITION(partno))?
		ns->n_mtl_gc_zones : ns->n_mtl_zones;
	bool ret = !(start_zoneno <= mtl_zoneno && mtl_zoneno < start_zoneno + n_mtl_zones);

//	if (ret) 
//		printk("%s: lba: 0x%lx start_zoneno: %lu mtl_zoneno: %lu n_mtl_zones: %lu PGS_PER_MTL_ZONE: %u",
//				__func__, lba, start_zoneno, mtl_zoneno, ns->n_mtl_zones, PGS_PER_MTL_ZONE);
	return ret;
	//return !(start_zoneno <= mtl_zoneno && mtl_zoneno < start_zoneno + ns->n_mtl_zones);
}

static inline MTL_ENTRY read_mtl_entry(struct nvmev_ns *ns, uint64_t lba)
{
	struct mtl_zone_entry *_mtl;
	int i;
	/* TODO: Sliding Window */
	if (NO_PARTITION(lba) > NO_TYPE-1 || out_of_mtl_window(ns, lba))
		return INVALID_MAPPING;

	_mtl = (struct mtl_zone_entry *) ns->mtls[NO_PARTITION(lba)][NO_MTL_ZONE_IDX(ns, lba)];
	return _mtl->map_table[OFFSET_MTL(lba)];
}

static inline bool is_fully_invalidated_mtl_zone(struct mtl_zone_entry *mtl_ze)
{
	return mtl_ze->zone_info.nr_inv_pgs == PGS_PER_MTL_ZONE;
}


static inline bool corner_case(struct nvmev_ns *ns, uint64_t lba, struct mtl_zone_entry *mtl_ze)
{
	return (ns->start_zoneno[NO_PARTITION(lba)] == 0) 
		&& (mtl_ze->zone_info.nr_inv_pgs == mtl_ze->zone_info.nr_v_pgs);
}

static inline bool is_head_mtl_zone(struct nvmev_ns *ns, uint64_t lba)
{
	//NVMEV_ASSERT(ns->start_zoneno[NO_PARTITION(lba)] <= NO_MTL_ZONE(lba));
	return ns->start_zoneno[NO_PARTITION(lba)] == NO_MTL_ZONE(lba);
}

/* invalidate mtl entry and return previous mtl entry */
static MTL_ENTRY invalidate_mtl_entry(struct nvmev_ns *ns, uint64_t lba)
{
	MTL_ENTRY mtle;
	struct mtl_zone_entry *_mtl;
	int i, zidx;
	uint64_t part_no = NO_PARTITION(lba);

	if (part_no > NO_TYPE-1 || out_of_mtl_window(ns, lba)){
		//if ((lba & 0xe0000000) == 0xe0000000)
		//	printk("%s: out of mtl: %d lba: 0x%lx", __func__, 
		//			out_of_mtl_window(ns, lba), lba);
		return INVALID_MAPPING;
	}
	
	_mtl = (struct mtl_zone_entry *) ns->mtls[part_no][NO_MTL_ZONE_IDX(ns, lba)];
	mtle = _mtl->map_table[OFFSET_MTL(lba)];

	if (IS_MAIN_PARTITION(part_no) && mtle != INVALID_MAPPING){
		_mtl->zone_info.nr_inv_pgs ++ ;
		
		if (!(_mtl->zone_info.nr_inv_pgs >= 0 && _mtl->zone_info.nr_inv_pgs <= PGS_PER_MTL_ZONE)){
			printk("%s: nr_inv_pgs: %u, PGS_PER_MTL_ZONE: %lu partno: %llu NO_MTL_ZONE_IDX %llu n_mtl_zones %llu mtl: %p", 
					__func__, _mtl->zone_info.nr_inv_pgs, PGS_PER_MTL_ZONE, part_no, NO_MTL_ZONE_IDX(ns, lba), 
					ns->n_mtl_zones, _mtl);
		}

		NVMEV_ASSERT(_mtl->zone_info.nr_inv_pgs >= 0 && _mtl->zone_info.nr_inv_pgs <= PGS_PER_MTL_ZONE);
	
#ifndef DEACTIVATE_SLIDING_WINDOW 	
		/* slide window */
		if (is_head_mtl_zone(ns, lba) && (is_fully_invalidated_mtl_zone(_mtl) 
					|| corner_case(ns, lba, _mtl))) {	
			/* reset mtl zone */
			_mtl->zone_info.nr_inv_pgs = 0;
			_mtl->zone_info.nr_v_pgs = 0;
			for (i = 1; i < ns->n_mtl_zones; i++){
				zidx = (ns->start_zoneno[part_no] + i) % ns->n_mtl_zones;
				if (!is_fully_invalidated_mtl_zone(ns->mtls[part_no][zidx]))
					break;
				/* reset mtl zone */
				ns->mtls[part_no][zidx]->zone_info.nr_inv_pgs = 0;
			}

			ns->start_zoneno[part_no] += i;
		}
#else
		if (is_fully_invalidated_mtl_zone(_mtl)) {
			_mtl->zone_info.nr_inv_pgs = 0;
			_mtl->zone_info.nr_v_pgs = 0;
		}
#endif
	}

	_mtl->map_table[OFFSET_MTL(lba)] = INVALID_MAPPING;
	
	return mtle;
}

static inline bool allocate_mem_page(struct nvmev_ns *ns, uint64_t laddr, uint64_t *mem_addr)
{
	struct mtl_zone_entry * _mtl;
	MTL_ENTRY mtl_entry;
	uint64_t lba = laddr / PAGE_SIZE;
	static bool dbg = true;
	//static int alloc_cnt = 0;
	if (NO_PARTITION(lba) > NO_TYPE-1 || out_of_mtl_window(ns, lba)){
		if (dbg){
			printk("%s: lba: 0x%llx zoneno: %llu start zoneno: %llu n_mtl_zones: %llu, PGS_PER_MTl_ZONE: %d", 
					__func__, lba, NO_MTL_ZONE(lba), 
					ns->start_zoneno[NO_PARTITION(lba)], ns->n_mtl_zones, PGS_PER_MTL_ZONE);
			dump_stack();
			NVMEV_ASSERT(0);
			dbg = false;
		}
		return false;
	}
	/* find free mem page */
	if (list_empty_(&ns->free_mem_page_list)){
		if (dbg){
			printk("%s: lba: 0x%llx", __func__, lba);
			NVMEV_ASSERT(0);
			dbg = false;
		}
		return false;
	}
	//alloc_cnt ++;
	//if (alloc_cnt % PCNT == 0)
	//	printk("%s: alloc_cnt: %d", __func__, alloc_cnt);
	mtl_entry = (MTL_ENTRY) list_entry(list_pop_front(&ns->free_mem_page_list), 
											MEM_PAGE_ENTRY, list_elem);
	/* map mtl to new mem page */
	_mtl = ns->mtls[NO_PARTITION(lba)][NO_MTL_ZONE_IDX(ns, lba)];
	_mtl->map_table[OFFSET_MTL(lba)] = mtl_entry; 
	_mtl->zone_info.nr_v_pgs ++;
	*mem_addr = mtl_entry->mem_addr + laddr % PAGE_SIZE;
	return true;
}

static inline bool get_mem_addr(struct nvmev_ns *ns, uint64_t laddr, uint64_t *mem_addr)
{
	MTL_ENTRY mtl_entry;
	//NVMEV_INFO("[JWDBG] %s lba: 0x%llx\n", __func__, laddr);
	uint64_t lba = laddr / PAGE_SIZE;
	bool ret;
	if ((mtl_entry = read_mtl_entry(ns, lba)) != INVALID_MAPPING){
		/* allow mem page overwrite */
		*mem_addr = mtl_entry->mem_addr + laddr % PAGE_SIZE;
		ret = !is_interior(&mtl_entry->list_elem);
		if (!ret){
			printk("%s: not interior!! lba 0x%llx", __func__, lba);
			NVMEV_ASSERT(0);
		}
		return ret;
		//return !is_interior(&mtl_entry->list_elem);
	}
	/* allocate new mem addr */
	return allocate_mem_page(ns, laddr, mem_addr);
}

static inline uint64_t read_translation_log(struct nvmev_ns *ns, struct list *tlist, uint64_t lba, 
		char * func, int goback_cnt)
{
	struct trans_entry *te;
	struct list_elem *le;
	uint64_t ret;
	int i;
	le = list_begin(tlist);
	te = list_entry(le, struct trans_entry, list_elem);
	if (!(te->log_buf[te->cur_idx].old_lpn == lba)){
		printk("%s: goback cnt: %d", __func__, goback_cnt);	
		for (i = 0; i < mg_cnt; i ++) {
			printk("%s: migration old_lpn: 0x%lx new_lpn: 0x%lx", __func__, 
					mg_old_stack[i], mg_new_stack[i]);
		}
		
		for (i = 0; i < glb_cnt; i ++) {
			printk("%s: discard slba: 0x%lx elba: 0x%lx", __func__, glb_old_stack[i], glb_new_stack[i]);
		}

		printk("%s: cur_idx: %llu nr_log: %llu old_lpn: 0x%llx lba: 0x%llx func: %s", \
				__func__, te->cur_idx, te->nr_log, te->log_buf[te->cur_idx].old_lpn, lba, func);
		for(i = 0; i < te->nr_log; i ++){
			printk("%s: idx: %d old_lpn: 0x%llx new_lpn: 0x%llx",\
					__func__, i, te->log_buf[i].old_lpn, te->log_buf[i].new_lpn);
		}
	}
	NVMEV_ASSERT(te->log_buf[te->cur_idx].old_lpn == lba);
	ret = te->log_buf[te->cur_idx].new_lpn;

	te->cur_idx ++;
	NVMEV_ASSERT(te->cur_idx <= te->nr_log);

	/* remove translation entry */
	if (te->cur_idx == te->nr_log){
		list_remove(le);
		kmem_cache_free(ns->mtl_translation_entry_slab, te);
	}

	return ret;
}

static inline bool access_mem_addr(struct nvmev_ns *ns, uint64_t laddr, uint64_t *mem_addr, 
		struct nvmev_proc_table *pe)
{
	MTL_ENTRY mtl_entry;
	uint64_t lba = laddr / PAGE_SIZE;
	struct list *tlist = &pe->mtl_read_translation_list[lba % SSD_PARTITIONS];
	int goback_cnt = 0;

read_mtl:
	if ((mtl_entry = read_mtl_entry(ns, lba))!= INVALID_MAPPING){
		
		*mem_addr = mtl_entry->mem_addr + laddr % PAGE_SIZE;
		
		return !is_interior(&mtl_entry->list_elem);
	} else {
		/* try refering to translation log */
		if (!list_empty_(tlist)){
			
			lba = read_translation_log(ns, tlist, lba, __func__, goback_cnt);
			goback_cnt ++;
			
			goto read_mtl;
		}

		/* corner case: for read request during booting and mounting */
		/* f2fs forward recovery reads some pages when mounted */
		*mem_addr = 0;
#ifdef JWDBG_IO
		//NVMEV_INFO("[JWDBG] %s corner case lba: 0x%llx memaddr: 0x%llx\n", 
		//			__func__, lba, *mem_addr/PAGE_SIZE);
#endif
		return true;
	}
}

#ifdef DISCARD_ENABLED
static bool free_mem_addr(struct nvmev_ns *ns, uint64_t laddr, struct list *tlist)
{
	MTL_ENTRY mtl_entry;
	uint64_t lba = laddr / PAGE_SIZE;
	//static int freed_cnt = 0;
	int goback_cnt = 0;

invalidate_mtl:
	if ((mtl_entry = invalidate_mtl_entry(ns, lba)) != INVALID_MAPPING){
		
		list_push_back(&ns->free_mem_page_list, &mtl_entry->list_elem);
		//freed_cnt ++;
		//if (freed_cnt % PCNT == 0)
		//	printk("%s: freed_cnt: %d", __func__, freed_cnt);
		
		return true;
	} else {
		/* try refering to translation log */
		if (!list_empty_(tlist)){
			
			lba = read_translation_log(ns, tlist, lba, __func__, goback_cnt);

			goback_cnt ++;
			
			goto invalidate_mtl;
		}

		/* corner case: discard f2fs namespace during mounting */
#ifdef JWDBG_IO
		//NVMEV_INFO("[JWDBG] %s corner case lba: 0x%llx\n", 
		//			__func__, lba);
#endif
		return false;
	}
}
#endif

#endif /* endif limited interval */

#ifdef COUPLED_GC_MTL

static void migrate_mem_addr(struct nvmev_ns *ns, uint64_t old_lba, uint64_t new_lba, 
		unsigned long long order)
{
	MTL_ENTRY mtl_entry;
	struct mtl_zone_entry *_mtl;

//#ifdef SHIVAL3
	
	//if ((old_lba & 0x20000000) == 0x20000000
	//		|| (new_lba & 0x60000000) == 0x60000000) {
//	if ((new_lba >> 29) == 3) {
//	//if (1) {
//		//	(old_lba & 0x20000000) == 0x20000000 ||
//		//		(new_lba & 0xe0000000) == 0xe0000000) {
//		mg_old_stack[mg_cnt] = old_lba;
//		mg_new_stack[mg_cnt] = new_lba;
//		mg_order_stack[mg_cnt] = order;
//		mg_cnt ++;
//		if (mg_cnt == BUF_CNT_) {
//			printk("%s \n \
//					order: %lu old_lpn: 0x%llx new_lpn: 0x%llx \n \
//					order: %lu old_lpn: 0x%llx new_lpn: 0x%llx \n \
//					order: %lu old_lpn: 0x%llx new_lpn: 0x%llx \n \
//					order: %lu old_lpn: 0x%llx new_lpn: 0x%llx \n \
//					order: %lu old_lpn: 0x%llx new_lpn: 0x%llx \n \
//					order: %lu old_lpn: 0x%llx new_lpn: 0x%llx \n \
//					order: %lu old_lpn: 0x%llx new_lpn: 0x%llx \n \
//					order: %lu old_lpn: 0x%llx new_lpn: 0x%llx \n \
//					order: %lu old_lpn: 0x%llx new_lpn: 0x%llx \n \
//					order: %lu old_lpn: 0x%llx new_lpn: 0x%llx \n \
//					", __func__,
//					mg_order_stack[0], mg_old_stack[0], mg_new_stack[0],
//					mg_order_stack[1], mg_old_stack[1], mg_new_stack[1],
//					mg_order_stack[2], mg_old_stack[2], mg_new_stack[2],
//					mg_order_stack[3], mg_old_stack[3], mg_new_stack[3],
//					mg_order_stack[4], mg_old_stack[4], mg_new_stack[4],
//					mg_order_stack[5], mg_old_stack[5], mg_new_stack[5],
//					mg_order_stack[6], mg_old_stack[6], mg_new_stack[6],
//					mg_order_stack[7], mg_old_stack[7], mg_new_stack[7],
//					mg_order_stack[8], mg_old_stack[8], mg_new_stack[8],
//					mg_order_stack[9], mg_old_stack[9], mg_new_stack[9]
//				//	mg_order_stack[10], mg_old_stack[10], mg_new_stack[10],
//				//	mg_order_stack[11], mg_old_stack[11], mg_new_stack[11],
//				//	mg_order_stack[12], mg_old_stack[12], mg_new_stack[12],
//				//	mg_order_stack[13], mg_old_stack[13], mg_new_stack[13],
//				//	mg_order_stack[14], mg_old_stack[14], mg_new_stack[14],
//				//	mg_order_stack[15], mg_old_stack[15], mg_new_stack[15],
//				//	mg_order_stack[16], mg_old_stack[16], mg_new_stack[16],
//				//	mg_order_stack[17], mg_old_stack[17], mg_new_stack[17],
//				//	mg_order_stack[18], mg_old_stack[18], mg_new_stack[18],
//				//	mg_order_stack[19], mg_old_stack[19], mg_new_stack[19]
//				//	old_stack[20], new_stack[20],
//				//	old_stack[21], new_stack[21],
//				//	old_stack[22], new_stack[22],
//				//	old_stack[23], new_stack[23],
//				//	old_stack[24], new_stack[24],
//				//	old_stack[25], new_stack[25],
//				//	old_stack[26], new_stack[26],
//				//	old_stack[27], new_stack[27],
//				//	old_stack[28], new_stack[28],
//				//	old_stack[29], new_stack[29],
//				//	old_stack[30], new_stack[30],
//				//	old_stack[31], new_stack[31],
//				//	old_stack[32], new_stack[32],
//				//	old_stack[33], new_stack[33],
//				//	old_stack[34], new_stack[34],
//				//	old_stack[35], new_stack[35],
//				//	old_stack[36], new_stack[36],
//				//	old_stack[37], new_stack[37],
//				//	old_stack[38], new_stack[38],
//				//	old_stack[39], new_stack[39],
//				//	old_stack[40], new_stack[40],
//				//	old_stack[41], new_stack[41],
//				//	old_stack[42], new_stack[42],
//				//	old_stack[43], new_stack[43],
//				//	old_stack[44], new_stack[44],
//				//	old_stack[45], new_stack[45],
//				//	old_stack[46], new_stack[46],
//				//	old_stack[47], new_stack[47],
//				//	old_stack[48], new_stack[48],
//				//	old_stack[49], new_stack[49]
//			);
//			mg_cnt = 0;
//		}
//	}
//#endif
	/* invalidate old mapping */
	mtl_entry = invalidate_mtl_entry(ns, old_lba);

	if (mtl_entry == INVALID_MAPPING){

		int i = 0;
		for (i = 0; i < glb_cnt; i ++) {
			printk("%s: slba: 0x%lx elba: 0x%lx", __func__, glb_old_stack[i], glb_new_stack[i]);
		}

//#ifdef SHIVAL3
		for (i = 0; i < mg_cnt; i ++) {
			printk("%s: order: %lu old_lpn: 0x%lx new_lpn: 0x%lx", __func__, 
					mg_order_stack[i], mg_old_stack[i], mg_new_stack[i]);
		}
//#endif
		printk("%s: prob info: old_lba: 0x%llx new_lba: 0x%llx", __func__, old_lba, new_lba);
	}
	NVMEV_ASSERT(mtl_entry != INVALID_MAPPING);
	if (out_of_mtl_window(ns, new_lba)){
		printk("%s: out of mtl window!!!! new_lba: 0x%lx ", __func__, new_lba);
		NVMEV_ASSERT(0);
	}

	/* migrate to new lba */
	_mtl = ns->mtls[NO_PARTITION(new_lba)][NO_MTL_ZONE_IDX(ns, new_lba)];
	_mtl->map_table[OFFSET_MTL(new_lba)] = mtl_entry; 
}

void reflect_mtl_migration_log(struct list *mglist, int sqid, int sq_entry, unsigned long long order, 
		unsigned int migration_cnt)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	size_t nsid = sq_entry(sq_entry).rw.nsid - 1; // 0-based
	struct list_elem *le;
	struct mg_entry *mge;
	struct nvmev_ns *ns = &vdev->ns[nsid];
	int i;
	unsigned int real_migration_cnt = 0;

	while(!list_empty_(mglist)){
		le = list_pop_front(mglist);
		
		mge = list_entry(le, struct mg_entry, list_elem);
		
		NVMEV_ASSERT(mge->nr_log > 0 && mge->nr_log <= NR_MAX_MIGRATION_LOG);
		
		for (i = 0; i < mge->nr_log; i ++){
			migrate_mem_addr(ns, mge->log_buf[i].old_lpn, mge->log_buf[i].new_lpn, order);
			real_migration_cnt ++;
		}
		
		kmem_cache_free(ns->mtl_migration_entry_slab, mge);
	}
	if (migration_cnt != real_migration_cnt)
		printk("%s: something wrong!!!!!!!!!!!! mgcnt: %u real mgcnt: %u", 
				__func__, migration_cnt, real_migration_cnt);
	NVMEV_ASSERT(migration_cnt == real_migration_cnt);
}
#endif

#ifdef MIGRATION_IO

static void __nvmev_proc_rev_io(int cq_entry)
{
	struct nvmev_rev_completion_queue *rcq = vdev->rev_cqe;
	struct nvme_rev_completion *cmd = &rev_cq_entry(cq_entry);
	
	unsigned int nsid = le32_to_cpu(cmd->nsid);
	//printk("%s: command_id: %u nsid: %u not converted: %u", __func__, cmd->command_id, nsid, cmd->nsid);
	struct nvmev_ns *ns = &vdev->ns[nsid];

	ns->proc_rev_io_cmd(ns, cmd);
}

int nvmev_proc_io_rev_cq(int cqid, int new_db, int old_db)
{
	struct nvmev_rev_completion_queue *cq = vdev->rev_cqe;
	int num_proc = new_db - old_db;
	int seq;
	int cq_entry = old_db;
	int latest_db;

	if (unlikely(num_proc < 0)) num_proc += cq->queue_size;

	for (seq = 0; seq < num_proc; seq++) {
		__nvmev_proc_rev_io(cq_entry);

		if (++cq_entry == cq->queue_size) {
			cq_entry = 0;
		}
	}

	latest_db = (old_db + seq) % cq->queue_size;
	
	return latest_db;
}

static inline int __fill_rev_sq_cmd(struct inflight_set_entry *ise, int sqid, int sq_entry)
{
	/* To get nvme_ns */
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	size_t nsid = sq_entry(sq_entry).rw.nsid - 1; // 0-based
	struct nvmev_ns *ns = &vdev->ns[nsid];

	struct list_elem *le, *le_tail;
	int i = 0, rsq_head, mgb_idx;

	struct nvmev_rev_submission_queue *rsq = vdev->rev_sqe;

	int mg_cmd_submitted = 0;
	static bool first = true;
	static unsigned int exp_cid = 0;

	if (ise == NULL)
		return mg_cmd_submitted;
	

	if (list_empty_(&ise->gc_log_list)){
		printk("%s: ise gc log list empty. cid: %u", __func__, ise->command_id);
		return mg_cmd_submitted;
	}

	le = list_front(&ise->gc_log_list);
	le_tail = list_end(&ise->gc_log_list);

	spin_lock(&rsq->entry_lock); /* no meaning with single io thread */
	
	/* fill reverse submission command */
	rsq_head = rsq->sq_head;
	mgb_idx = ise->command_id % rsq->nr_mg_batch;
	//mgb_head = rsq->mgb_head;
	//mgb_head = ise->command_id % rsq->nr_mg_batch;
	//if ((!(mgb_head == ise->command_id % rsq->nr_mg_batch)) || 
	//		(exp_cid != ise->command_id)
	//		) 
	//	printk("%s: problem!! mgb_head: %u ise cid: %u , nr_mg_batch: %u exp_cid: %u", 
	//			__func__, mgb_head, ise->command_id, rsq->nr_mg_batch, exp_cid);
	//NVMEV_ASSERT(mgb_head == ise->command_id % rsq->nr_mg_batch);
	//printk("%s: ise->cid: %u mgb_head: %u", __func__, ise->command_id, mgb_head);
#ifdef SHIVAL
	static int cnt = 0;
	static unsigned int cid_stack[10], old_stack[10], new_stack[10];
#endif

	while(le != le_tail){
		struct gc_log *gc_log = list_entry(le, struct gc_log, list_elem);
		
		/* fill mg pair batch */
		rev_mgb_entry(mgb_idx).mg_pairs[i].old_lba 
			= cpu_to_le64(gc_log->old_lpn);
		rev_mgb_entry(mgb_idx).mg_pairs[i].new_lba 
			= cpu_to_le64(gc_log->new_lpn);
#ifdef SHIVAL
//		if ((cpu_to_le64(gc_log->old_lpn) & 0xe0000000) == 0xe0000000 ||
//					(cpu_to_le64(gc_log->new_lpn) & 0xe0000000) == 0xe0000000) {
//			cid_stack[cnt] = ise->command_id;
//			old_stack[cnt] = cpu_to_le64(gc_log->old_lpn);
//			new_stack[cnt] = cpu_to_le64(gc_log->new_lpn);
//			cnt ++;
//			if (cnt == 10) {
//				printk("%s \n \
//						cid: %u old_lpn: 0x%lx new_lpn: 0x%lx \n \
//						cid: %u old_lpn: 0x%lx new_lpn: 0x%lx \n \
//						cid: %u old_lpn: 0x%lx new_lpn: 0x%lx \n \
//						cid: %u old_lpn: 0x%lx new_lpn: 0x%lx \n \
//						cid: %u old_lpn: 0x%lx new_lpn: 0x%lx \n \
//						cid: %u old_lpn: 0x%lx new_lpn: 0x%lx \n \
//						cid: %u old_lpn: 0x%lx new_lpn: 0x%lx \n \
//						cid: %u old_lpn: 0x%lx new_lpn: 0x%lx \n \
//						cid: %u old_lpn: 0x%lx new_lpn: 0x%lx \n \
//						cid: %u old_lpn: 0x%lx new_lpn: 0x%lx \n \
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
//				);
//				cnt = 0;
//			}
//		}
#endif
		//printk("%s: cid: %u old_addr: 0x%llx new_addr: 0x%llx", __func__,
		//		ise->command_id, 
		//		cpu_to_le64(gc_log->old_lpn),
        //        cpu_to_le64(gc_log->new_lpn));
		
		le = le->next;
		i ++;
	}

	NVMEV_ASSERT(i == NR_MG_PAIR);

	rev_sq_entry(rsq_head).command_id = ise->command_id;
	rev_sq_entry(rsq_head).opcode = nvme_cmd_rev_mg;
	rev_sq_entry(rsq_head).nsid = cpu_to_le32(nsid);
	rev_sq_entry(rsq_head).nr = cpu_to_le32(i - 1);
	rev_sq_entry(rsq_head).phase = cpu_to_le16(rsq->phase);

#ifdef SHIVAL	
	//printk("%s: cid: %u mgb_head: %u cidx: %u", __func__, ise->command_id, mgb_head, 
	//		ise->command_id % rsq->nr_mg_batch);
#endif
	//printk("%s: cid: %u mgb_head: %u cidx: %u", __func__, ise->command_id, mgb_head, 
	//		ise->command_id % rsq->nr_mg_batch);
	/* host can access mg pair batch based on the mgb_head index. */
	rev_sq_entry(rsq_head).prp1 = cpu_to_le64(mgb_idx);
	
	if (++rsq_head == rsq->queue_size) {
		rsq_head = 0;
		rsq->phase = !rsq->phase;
	}
	//exp_cid += 1;
	//exp_cid %= MAX_CID;

	//++ mgb_head;
	//mgb_head = mgb_head % rsq->nr_mg_batch;	
	//if (++mgb_head == rsq->nr_mg_batch) {
	//	mgb_head = 0; 
	//	/* TODO: need mgb phase ? */
	//}

	rsq->sq_head = rsq_head;
	//rsq->mgb_head = mgb_head;
	rsq->interrupt_ready = true;

	spin_unlock(&rsq->entry_lock);

	mg_cmd_submitted = 1;
	if (first) {
		printk("%s: first submitted!!", __func__);
		first = false;
	}
	return mg_cmd_submitted;

	//while(!list_empty_(mg_batch_list)){
	//	le = list_pop_front(mg_batch_list);
	//	mgbe = list_entry(le, struct mg_batch_entry, list_elem);
	//	
	//	NVMEV_ASSERT(mgbe->nr > 0 && mgbe->nr <= NR_MG_PAIR);
	//	
	//	/* fill reverse submission command */
	//	rsq_head = rsq->sq_head;
	//	mgb_head = rsq->mgb_head;

	//	spin_lock(&rsq->entry_lock);

	//	rev_sq_entry(rsq_head).command_id = mgbe->command_id;
	//	rev_sq_entry(rsq_head).opcode = nvme_cmd_rev_mg;
	//	rev_sq_entry(rsq_head).nsid = cpu_to_le32(nsid);
	//	rev_sq_entry(rsq_head).nr = cpu_to_le32(mgbe->nr - 1);
	//	rev_sq_entry(rsq_head).phase = cpu_to_le16(rsq->phase);

	//	/* host can access mg pair batch based on the mgb_head index. */
	//	rev_sq_entry(rsq_head).prp1 = cpu_to_le64(mgb_head);
	//	
	//	NVMEV_ASSERT(mgbe->nr == NR_MG_PAIR);

	//	/* fill mg pair batch */
	//	for (i = 0; i < mgbe->nr; i ++){
	//		rev_mgb_entry(mgb_head).mg_pairs[i].old_lba = cpu_to_le64(mgbe->mg_pairs[i].old_lba);
	//		rev_mgb_entry(mgb_head).mg_pairs[i].new_lba = cpu_to_le64(mgbe->mg_pairs[i].new_lba);
	//	}
	//	
	//	if (++rsq_head == rsq->queue_size) {
	//		rsq_head = 0;
	//		rsq->phase = !rsq->phase;
	//	}
	//	
	//	if (++mgb_head == rsq->nr_mg_batch) {
	//		mgb_head = 0; 
	//		/* TODO: need mgb phase ? */
	//	}

	//	rsq->sq_head = rsq_head;
	//	rsq->mgb_head = mgb_head;
	//	rsq->interrupt_ready = true;
	//	spin_unlock(&rsq->entry_lock);
	//	
	//	kmem_cache_free(ns->gclm->mg_batch_slab, mgbe);

	//	ret = 1;
	//}
}

#endif

#ifdef DISCARD_ENABLED
//static unsigned int __do_perform_io_rw(int sqid, int sq_entry)
static unsigned int __do_perform_io_rw(struct nvmev_proc_table *pe)
#else
static unsigned int __do_perform_io(int sqid, int sq_entry)
//static unsigned int __do_perform_io(struct nvmev_proc_table *pe)
#endif
{
	int sqid = pe->sqid;
	int sq_entry = pe->sq_entry;
	//struct list *tlist = &pe->mtl_translation_list;
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	size_t offset;
	size_t length, remaining;
	int prp_offs = 0;
	int prp2_offs = 0;
	u64 paddr;
	u64 *paddr_list = NULL;
	size_t nsid = sq_entry(sq_entry).rw.nsid - 1; // 0-based
#ifdef MULTI_PARTITION_MTL
	uint64_t trans_offset;
#endif
	offset = sq_entry(sq_entry).rw.slba << 9;
	length = (sq_entry(sq_entry).rw.length + 1) << 9;
	remaining = length;

	//if (sq_entry(sq_entry).rw.opcode == nvme_cmd_write)
	//	printk("%s: pe cid: %u", __func__, pe->command_id);

	//if (IS_MAIN_PARTITION(NO_PARTITION(offset / PAGE_SIZE)))
	//	offset -= (START_OFS_IN_MAIN_PART * PAGE_SIZE);
	//if (sq_entry(sq_entry).rw.opcode == nvme_cmd_write) {
	//	printk("[JWDBG] %s write: ofs: %ld len: %ld, lpn: 0x%lx ~ 0x%lx\n", __func__, offset, length, offset/PAGE_SIZE, (offset+length-1)/PAGE_SIZE);
	//}
	while (remaining) {
		size_t io_size;
		void *vaddr;
		size_t mem_offs = 0;

		prp_offs++;
		if (prp_offs == 1) {
			paddr = sq_entry(sq_entry).rw.prp1;
		} else if (prp_offs == 2) {
			paddr = sq_entry(sq_entry).rw.prp2;
			if (remaining > PAGE_SIZE) {
				paddr_list = kmap_atomic_pfn(PRP_PFN(paddr)) + (paddr & PAGE_OFFSET_MASK);
				paddr = paddr_list[prp2_offs++];
			}
		} else {
			paddr = paddr_list[prp2_offs++];
		}

		vaddr = kmap_atomic_pfn(PRP_PFN(paddr));

		io_size = min_t(size_t, remaining, PAGE_SIZE);

		if (paddr & PAGE_OFFSET_MASK) {
			mem_offs = paddr & PAGE_OFFSET_MASK;
			if (io_size + mem_offs > PAGE_SIZE)
				io_size = PAGE_SIZE - mem_offs;
		}

		if (sq_entry(sq_entry).rw.opcode == nvme_cmd_write) {
#ifdef MULTI_PARTITION_MTL
			if (!get_mem_addr(&vdev->ns[nsid], offset, &trans_offset)){
				NVMEV_ERROR("[JWDBG] get_mem_addr failed\n");
				NVMEV_ASSERT(0);
				//memcpy(vdev->ns[nsid].mapped + offset, vaddr + mem_offs, io_size);
			} else {
#ifdef JWDBG_IO
				NVMEV_INFO("[JWDBG] %s: writes lba: 0x%lx tlba: 0x%llx ofs: 0x%lx tofs: 0x%llx\n", 
						__func__, offset/PAGE_SIZE, trans_offset/PAGE_SIZE, offset, trans_offset);
#endif

				memcpy(vdev->ns[nsid].mapped + trans_offset, vaddr + mem_offs, io_size);
			}
#else
			memcpy(vdev->ns[nsid].mapped + offset, vaddr + mem_offs, io_size);
#endif
		} else if (sq_entry(sq_entry).rw.opcode == nvme_cmd_read) {
#ifdef MULTI_PARTITION_MTL
			if (!access_mem_addr(&vdev->ns[nsid], offset, &trans_offset, pe))
				NVMEV_ERROR("[JWDBG] access_mem_addr failed\n");
#ifdef JWDBG_IO
			NVMEV_INFO("[JWDBG] %s: read lba: 0x%lx tlba: 0x%llx ofs: 0x%lx tofs: 0x%llx\n", 
						__func__, offset/PAGE_SIZE, trans_offset/PAGE_SIZE, offset, trans_offset);
#endif
			memcpy(vaddr + mem_offs, vdev->ns[nsid].mapped + trans_offset, io_size);
#else
			memcpy(vaddr + mem_offs, vdev->ns[nsid].mapped + offset, io_size);
#endif
		} 
		kunmap_atomic(vaddr);

		remaining -= io_size;
		offset += io_size;
	}

	if (paddr_list != NULL)
		kunmap_atomic(paddr_list);

	return length;
}

#ifdef DISCARD_ENABLED
//static unsigned int __do_perform_io_dsm(struct nvmev_proc_table *pe)
//{
//	int sqid = pe->sqid;
//	int sq_entry = pe->sq_entry;
//	struct list *tlist = &pe->mtl_translation_list;
//
//	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
//	size_t offset;
//	size_t length, remaining;
//	int prp_offs = 0;
//	int prp2_offs = 0;
//	u64 paddr;
//	size_t nsid = sq_entry(sq_entry).dsm.nsid - 1; // 0-based
//	int nranges = sq_entry(sq_entry).dsm.nr + 1; /* zero-based */
//	int i;
//	paddr = sq_entry(sq_entry).dsm.prp1;
//	void *vaddr = kmap_atomic_pfn(PRP_PFN(paddr));
//	struct nvme_dsm_range *dsm_range = (struct nvme_dsm_range *) vaddr;
//	
//	//static int dsm_cnt = 0, dsm_cid_stack[BUF_CNT_];	
//	//dsm_cid_stack[dsm_cnt] = pe->command_id;
//	//dsm_cnt ++;
//	//if (dsm_cnt == BUF_CNT_) {
//	//	printk("%s \n \
//	//			pe cid: 0x%llx \n \
//	//			pe cid: 0x%llx \n \
//	//			pe cid: 0x%llx \n \
//	//			pe cid: 0x%llx \n \
//	//			pe cid: 0x%llx \n \
//	//			pe cid: 0x%llx \n \
//	//			pe cid: 0x%llx \n \
//	//			pe cid: 0x%llx \n \
//	//			pe cid: 0x%llx \n \
//	//			pe cid: 0x%llx \n \
//	//			pe cid: 0x%llx \n \
//	//			pe cid: 0x%llx \n \
//	//			pe cid: 0x%llx \n \
//	//			pe cid: 0x%llx \n \
//	//			pe cid: 0x%llx \n \
//	//			pe cid: 0x%llx \n \
//	//			pe cid: 0x%llx \n \
//	//			pe cid: 0x%llx \n \
//	//			pe cid: 0x%llx \n \
//	//			pe cid: 0x%llx \n \
//	//			", __func__,
//	//			dsm_cid_stack[0], 
//	//			dsm_cid_stack[1], 
//	//			dsm_cid_stack[2], 
//	//			dsm_cid_stack[3], 
//	//			dsm_cid_stack[4], 
//	//			dsm_cid_stack[5], 
//	//			dsm_cid_stack[6], 
//	//			dsm_cid_stack[7], 
//	//			dsm_cid_stack[8], 
//	//			dsm_cid_stack[9], 
//	//			dsm_cid_stack[10],
//	//			dsm_cid_stack[11],
//	//			dsm_cid_stack[12],
//	//			dsm_cid_stack[13],
//	//			dsm_cid_stack[14],
//	//			dsm_cid_stack[15],
//	//			dsm_cid_stack[16],
//	//			dsm_cid_stack[17],
//	//			dsm_cid_stack[18],
//	//			dsm_cid_stack[19]);
//	//	dsm_cnt == 0;
//	//}
//
//	/* for each discard range */
//	for (i = 0; i < nranges; i ++){
//		offset = dsm_range[i].slba << 9;
//		if (IS_MAIN_PARTITION(NO_PARTITION(offset / PAGE_SIZE)))
//			offset -= (START_OFS_IN_MAIN_PART * PAGE_SIZE);
//		//length = (dsm_range[i].nlb + 1) << 9; /* zero-based */
//		length = (dsm_range[i].nlb) << 9; /* zero-based */
//		remaining = length;
//		//NVMEV_INFO("[JWDBG] %s: ofs: %ld len: %ld, lpn: 0x%lx ~ 0x%lx\n", __func__, offset, length, offset/PAGE_SIZE, (offset+length-1)/PAGE_SIZE);
//
//
//		//printk("%s: slpn: 0x%lx len: %lu", 
//		//		__func__, offset/PAGE_SIZE, length/PAGE_SIZE);
//
////#define BUF_CNT_ 50
//		
////		if (1) {
////		if	(((offset/PAGE_SIZE) >> 29) == 3) {
////			glb_old_stack[glb_cnt] = offset/PAGE_SIZE;
////			glb_new_stack[glb_cnt] = offset/PAGE_SIZE + length/PAGE_SIZE - 1;
////			glb_cnt ++;
////			if (glb_cnt == BUF_CNT_) {
//////#ifdef PLEASE
////				printk("%s \n \
////						slba: 0x%llx elba: 0x%llx \n \
////						slba: 0x%llx elba: 0x%llx \n \
////						slba: 0x%llx elba: 0x%llx \n \
////						slba: 0x%llx elba: 0x%llx \n \
////						slba: 0x%llx elba: 0x%llx \n \
////						slba: 0x%llx elba: 0x%llx \n \
////						slba: 0x%llx elba: 0x%llx \n \
////						slba: 0x%llx elba: 0x%llx \n \
////						slba: 0x%llx elba: 0x%llx \n \
////						slba: 0x%llx elba: 0x%llx \n \
////						", __func__,
////						glb_old_stack[0], glb_new_stack[0],
////						glb_old_stack[1], glb_new_stack[1],
////						glb_old_stack[2], glb_new_stack[2],
////						glb_old_stack[3], glb_new_stack[3],
////						glb_old_stack[4], glb_new_stack[4],
////						glb_old_stack[5], glb_new_stack[5],
////						glb_old_stack[6], glb_new_stack[6],
////						glb_old_stack[7], glb_new_stack[7],
////						glb_old_stack[8], glb_new_stack[8],
////						glb_old_stack[9], glb_new_stack[9]
////				//		glb_old_stack[10], glb_new_stack[10],
////				//		glb_old_stack[11], glb_new_stack[11],
////				//		glb_old_stack[12], glb_new_stack[12],
////				//		glb_old_stack[13], glb_new_stack[13],
////				//		glb_old_stack[14], glb_new_stack[14],
////				//		glb_old_stack[15], glb_new_stack[15],
////				//		glb_old_stack[16], glb_new_stack[16],
////				//		glb_old_stack[17], glb_new_stack[17],
////				//		glb_old_stack[18], glb_new_stack[18],
////				//		glb_old_stack[19], glb_new_stack[19]
////					//	old_stack[20], new_stack[20],
////					//	old_stack[21], new_stack[21],
////					//	old_stack[22], new_stack[22],
////					//	old_stack[23], new_stack[23],
////					//	old_stack[24], new_stack[24],
////					//	old_stack[25], new_stack[25],
////					//	old_stack[26], new_stack[26],
////					//	old_stack[27], new_stack[27],
////					//	old_stack[28], new_stack[28],
////					//	old_stack[29], new_stack[29],
////					//	old_stack[30], new_stack[30],
////					//	old_stack[31], new_stack[31],
////					//	old_stack[32], new_stack[32],
////					//	old_stack[33], new_stack[33],
////					//	old_stack[34], new_stack[34],
////					//	old_stack[35], new_stack[35],
////					//	old_stack[36], new_stack[36],
////					//	old_stack[37], new_stack[37],
////					//	old_stack[38], new_stack[38],
////					//	old_stack[39], new_stack[39],
////					//	old_stack[40], new_stack[40],
////					//	old_stack[41], new_stack[41],
////					//	old_stack[42], new_stack[42],
////					//	old_stack[43], new_stack[43],
////					//	old_stack[44], new_stack[44],
////					//	old_stack[45], new_stack[45],
////					//	old_stack[46], new_stack[46],
////					//	old_stack[47], new_stack[47],
////					//	old_stack[48], new_stack[48],
////					//	old_stack[49], new_stack[49]
////				);
//////#endif
////				glb_cnt = 0;
////			}
////		}
//
//		while (remaining) {
//			size_t io_size;
//			size_t mem_offs = 0;
//
//			io_size = min_t(size_t, remaining, PAGE_SIZE);
//
//			/* JW: change paddr to offset. need to verify. */
//			if (offset & PAGE_OFFSET_MASK) { 
//				NVMEV_ERROR("[JWDBG] %s: offset not aligned to PG. ofs: %lx len: %lx\n", __func__, 
//						offset, length);
//				mem_offs = offset & PAGE_OFFSET_MASK;
//				if (io_size + mem_offs > PAGE_SIZE)
//					io_size = PAGE_SIZE - mem_offs;
//			}
//			if (io_size != PAGE_SIZE)
//				NVMEV_ERROR("[JWDBG] %s: iosize is not PGSIZE. iosize: %ld\n", __func__, 
//						io_size);
//
//#ifdef MULTI_PARTITION_MTL
//			free_mem_addr(&vdev->ns[nsid], offset, tlist);
//#endif
//			remaining -= io_size;
//			offset += io_size;
//		}
//	}
//
//	kunmap_atomic(vaddr);
//
//
//	return length;
//}
#endif

#ifdef DISCARD_ENABLED
//static unsigned int __do_perform_io(int sqid, int sq_entry)
static unsigned int __do_perform_io(struct nvmev_proc_table *pe)
{
	int sqid = pe->sqid;
	int sq_entry = pe->sq_entry;
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];

	//if (sq_entry(sq_entry).common.opcode == nvme_cmd_write || 
	//    sq_entry(sq_entry).common.opcode == nvme_cmd_read) 	{
		return __do_perform_io_rw(pe);
	//} else if (sq_entry(sq_entry).common.opcode == nvme_cmd_dsm) {
	//	return __do_perform_io_dsm(pe);
	//}
	//return 0;
}
#endif

static u64 paddr_list[513] = {0,}; // Not using index 0 to make max index == num_prp
static unsigned int __do_perform_io_using_dma(int sqid, int sq_entry)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	size_t offset;
	size_t length, remaining;
	int prp_offs = 0;
	int prp2_offs = 0;
	int num_prps = 0;
	u64 paddr;
	u64 *tmp_paddr_list = NULL;
	size_t io_size;
	size_t mem_offs = 0;

	offset = sq_entry(sq_entry).rw.slba << 9;
	length = (sq_entry(sq_entry).rw.length + 1) << 9;
	remaining = length;

	memset(paddr_list, 0, sizeof(paddr_list));
	/* Loop to get the PRP list */
	while (remaining) {
		io_size = 0;

		prp_offs++;
		if (prp_offs == 1) {
			paddr_list[prp_offs] = sq_entry(sq_entry).rw.prp1;
		} else if (prp_offs == 2) {
			paddr_list[prp_offs] = sq_entry(sq_entry).rw.prp2;
			if (remaining > PAGE_SIZE) {
				tmp_paddr_list = kmap_atomic_pfn(PRP_PFN(paddr_list[prp_offs])) + (paddr_list[prp_offs] & PAGE_OFFSET_MASK);
				paddr_list[prp_offs] = tmp_paddr_list[prp2_offs++];
			}
		} else {
			paddr_list[prp_offs] = tmp_paddr_list[prp2_offs++];
		}

		io_size = min_t(size_t, remaining, PAGE_SIZE);

		if (paddr_list[prp_offs] & PAGE_OFFSET_MASK) {
			mem_offs = paddr_list[prp_offs] & PAGE_OFFSET_MASK;
			if (io_size + mem_offs > PAGE_SIZE)
				io_size = PAGE_SIZE - mem_offs;
		}

		remaining -= io_size;
	}
	num_prps = prp_offs;

	if (tmp_paddr_list != NULL)
		kunmap_atomic(tmp_paddr_list);

	remaining = length;
	prp_offs = 1;

	/* Loop for data transfer */
	while (remaining) {
		size_t page_size;
		mem_offs = 0;
		io_size = 0;
		page_size = 0;

		paddr = paddr_list[prp_offs];
		page_size = min_t(size_t, remaining, PAGE_SIZE);

		/* For non-page aligned paddr, it will never be between continuous PRP list (Always first paddr)  */
		if (paddr & PAGE_OFFSET_MASK) {
			mem_offs = paddr & PAGE_OFFSET_MASK;
			if (page_size + mem_offs > PAGE_SIZE) {
				page_size = PAGE_SIZE - mem_offs;
			}
		}

		for (prp_offs++; prp_offs <= num_prps; prp_offs++) {
			if (paddr_list[prp_offs] == paddr_list[prp_offs - 1] + PAGE_SIZE)
				page_size += PAGE_SIZE;
			else
				break;
		}

		io_size = min_t(size_t, remaining, page_size);

		if (sq_entry(sq_entry).rw.opcode == nvme_cmd_write) {
			dmatest_submit(paddr, vdev->config.storage_start + offset, io_size);
		} else if (sq_entry(sq_entry).rw.opcode == nvme_cmd_read) {
			dmatest_submit(vdev->config.storage_start + offset, paddr, io_size);
		}

		remaining -= io_size;
		offset += io_size;
	}

	return length;
}

#ifdef COUPLED_GC_MTL
static void __enqueue_io_req(int sqid, int cqid, int sq_entry, unsigned long long nsecs_start, struct nvmev_result *ret)
#else
static void __enqueue_io_req(struct nvmev_ns *ns, int sqid, int cqid, int sq_entry, unsigned long long nsecs_start, struct nvmev_result *ret)
#endif
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];

#if SUPPORT_MULTI_IO_WORKER_BY_SQ
	unsigned int proc_turn = (sqid - 1) % (vdev->config.nr_io_cpu);
#else
	unsigned int proc_turn = vdev->proc_turn;
#endif
	struct nvmev_proc_info *pi = &vdev->proc_info[proc_turn];
	unsigned int entry = pi->free_seq;

	if (pi->proc_table[entry].next >= NR_MAX_PARALLEL_IO) {
		WARN_ON_ONCE("IO queue is almost full");
		pi->free_seq = entry;
		return;
	}

	if (++proc_turn == vdev->config.nr_io_cpu) proc_turn = 0;
	vdev->proc_turn = proc_turn;
	pi->free_seq = pi->proc_table[entry].next;
	BUG_ON(pi->free_seq >= NR_MAX_PARALLEL_IO);

	NVMEV_DEBUG("%s/%u[%d], sq %d cq %d, entry %d %llu + %llu\n",
			pi->thread_name, entry, sq_entry(sq_entry).rw.opcode,
			sqid, cqid, sq_entry, nsecs_start, ret->nsecs_target - nsecs_start);

	/////////////////////////////////
	pi->proc_table[entry].sqid = sqid;
	pi->proc_table[entry].cqid = cqid;
	pi->proc_table[entry].sq_entry = sq_entry;
	pi->proc_table[entry].command_id = sq_entry(sq_entry).common.command_id;
	pi->proc_table[entry].nsecs_start = nsecs_start;
	pi->proc_table[entry].nsecs_enqueue = local_clock();
	pi->proc_table[entry].nsecs_target =  ret->nsecs_target;
	pi->proc_table[entry].status = ret->status;
	pi->proc_table[entry].is_completed = false;
	pi->proc_table[entry].is_copied = false;
	pi->proc_table[entry].prev = -1;
	pi->proc_table[entry].next = -1;

	pi->proc_table[entry].writeback_cmd = false;

	//static unsigned long long order = 0;
	pi->proc_table[entry].order = ret->order;
	//order ++;

	//if (order % 200 == 0)
	//	printk("%s: order: %llu", __func__, order);

#ifdef COUPLED_GC_MTL
	/* convey mtl migration list to io kthread */
	//struct list_elem *le;

	/* TODO: optimize by just modifying and tail */	
	//while(!list_empty_(&ret->mtl_migration_list)){
	//	le = list_pop_front(&ret->mtl_migration_list);
	//	list_push_back(&pi->proc_table[entry].mtl_migration_list, le);
	//}
	
	list_init(&pi->proc_table[entry].mtl_migration_list);

	if (!list_empty_(&ret->mtl_migration_list)) 
		change_list(&ret->mtl_migration_list, &pi->proc_table[entry].mtl_migration_list);
	
	list_init(&ret->mtl_migration_list);
	
	pi->proc_table[entry].migration_cnt = ret->migration_cnt;
	
	/* convey mtl translation list to io kthread */
	
	//while(!list_empty_(&ret->mtl_translation_list)){
	//	le = list_pop_front(&ret->mtl_translation_list);
	//	list_push_back(&pi->proc_table[entry].mtl_translation_list, le);
	//}
	
	list_init(&pi->proc_table[entry].mtl_translation_list);
	
	if (!list_empty_(&ret->mtl_translation_list)) 
		change_list(&ret->mtl_translation_list, &pi->proc_table[entry].mtl_translation_list);
	
	list_init(&ret->mtl_translation_list);

	int i;
	for (i = 0; i < SSD_PARTITIONS; i ++) {
		list_init(&pi->proc_table[entry].mtl_read_translation_list[i]);
	
		if (!list_empty_(&ret->mtl_read_translation_list[i])) 
			change_list(&ret->mtl_read_translation_list[i], 
					&pi->proc_table[entry].mtl_read_translation_list[i]);
		
		list_init(&ret->mtl_read_translation_list[i]);
	}

	

#endif

#ifdef MIGRATION_IO
	/* convey mtl migration list to io kthread */
	//list_init(&pi->proc_table[entry].mg_batch_list);
	//pi->proc_table[entry].ise = NULL;

	/* TODO: optimize by just modifying and tail */	
	/*while(!list_empty_(&ret->mg_batch_list)){
		le = list_pop_front(&ret->mg_batch_list);
		list_push_back(&pi->proc_table[entry].mg_batch_list, le);
	}*/
	//pi->proc_table[entry].ise = ret->ise;

	
	//while(!list_empty_(&ret->ise_list)){
	//	le = list_pop_front(&ret->ise_list);
	//	list_push_back(&pi->proc_table[entry].ise_list, le);
	//}

	list_init(&pi->proc_table[entry].ise_list);
	
	if (!list_empty_(&ret->ise_list)) 
		change_list(&ret->ise_list, &pi->proc_table[entry].ise_list);

	list_init(&ret->ise_list);
#endif

	mb();	/* IO kthread shall see the updated pe at once */

	// (END) -> (START) order, nsecs target ascending order
	if (pi->io_seq == -1) {
		pi->io_seq = entry;
		pi->io_seq_end = entry;
		//printk("%s: mola!!! order: %llu", __func__, order-1);
	} else {
		unsigned int curr = pi->io_seq_end;

		//while (curr != -1) {
		//	if (pi->proc_table[curr].nsecs_target <= pi->proc_io_nsecs)
		//		break;

		//	if (pi->proc_table[curr].nsecs_target <= ret->nsecs_target)
		//		break;

		//	curr = pi->proc_table[curr].prev;
		//}

		if (curr == -1) { /* Head inserted */
			pi->proc_table[pi->io_seq].prev = entry;
			pi->proc_table[entry].next = pi->io_seq;
			pi->io_seq = entry;
			way_stack[ent_cnt] = 1;
			printk("%s: NOWAY!!! 1", __func__);
		} else if (pi->proc_table[curr].next == -1) { /* Tail */
			pi->proc_table[entry].prev = curr;
			pi->io_seq_end = entry;
			pi->proc_table[curr].next = entry;
			way_stack[ent_cnt] = 2;
		} else { /* In between */
			pi->proc_table[entry].prev = curr;
			pi->proc_table[entry].next = pi->proc_table[curr].next;

			pi->proc_table[pi->proc_table[entry].next].prev = entry;
			pi->proc_table[curr].next = entry;
			way_stack[ent_cnt] = 3;
			printk("%s: NOWAY!!! 3", __func__);
		}

		ent_stack[ent_cnt] = entry;
		ent_stack2[ent_cnt] = pi->proc_table[entry].next;
		//order_stack[ent_cnt] = order-1;
		order_stack[ent_cnt] = ret->order;
		ent_cnt ++;
		if (ent_cnt == ENT_CNT_) {
			ent_cnt = 0;
		}

	}

}

void enqueue_writeback_io_req(int sqid, unsigned long long nsecs_target, struct buffer *write_buffer, unsigned int buffs_to_release)
{
#if SUPPORT_MULTI_IO_WORKER_BY_SQ
	unsigned int proc_turn = (sqid - 1) % (vdev->config.nr_io_cpu);
#else
	unsigned int proc_turn = vdev->proc_turn;
#endif
	struct nvmev_proc_info *pi = &vdev->proc_info[proc_turn];
	unsigned int entry = pi->free_seq;

	if (pi->proc_table[entry].next >= NR_MAX_PARALLEL_IO) {
		WARN_ON_ONCE("IO queue is almost full");
		pi->free_seq = entry;
		return;
	}

	if (++proc_turn == vdev->config.nr_io_cpu) proc_turn = 0;
	vdev->proc_turn = proc_turn;
	pi->free_seq = pi->proc_table[entry].next;
	BUG_ON(pi->free_seq >= NR_MAX_PARALLEL_IO);

	NVMEV_DEBUG("%s/%u[%d], sq %d cq %d, entry %d %llu + %llu\n",
			pi->thread_name, entry, sq_entry(sq_entry).rw.opcode,
			sqid, cqid, sq_entry, nsecs_start, ret->nsecs_target - nsecs_start);

	/////////////////////////////////
	pi->proc_table[entry].sqid = sqid;
	pi->proc_table[entry].nsecs_start = local_clock();
	pi->proc_table[entry].nsecs_enqueue = local_clock();
	pi->proc_table[entry].nsecs_target =  nsecs_target;
	pi->proc_table[entry].is_completed = false;
	pi->proc_table[entry].is_copied = true;
	pi->proc_table[entry].prev = -1;
	pi->proc_table[entry].next = -1;

	pi->proc_table[entry].writeback_cmd = true;
	pi->proc_table[entry].buffs_to_release = buffs_to_release;
	pi->proc_table[entry].write_buffer = (void*)write_buffer;
	mb();	/* IO kthread shall see the updated pe at once */

	// (END) -> (START) order, nsecs target ascending order
	if (pi->io_seq == -1) {
		pi->io_seq = entry;
		pi->io_seq_end = entry;
	} else {
		unsigned int curr = pi->io_seq_end;

		while (curr != -1) {
			if (pi->proc_table[curr].nsecs_target <= pi->proc_io_nsecs)
				break;

			if (pi->proc_table[curr].nsecs_target <= nsecs_target)
				break;

			curr = pi->proc_table[curr].prev;
		}

		if (curr == -1) { /* Head inserted */
			pi->proc_table[pi->io_seq].prev = entry;
			pi->proc_table[entry].next = pi->io_seq;
			pi->io_seq = entry;
		} else if (pi->proc_table[curr].next == -1) { /* Tail */
			pi->proc_table[entry].prev = curr;
			pi->io_seq_end = entry;
			pi->proc_table[curr].next = entry;
		} else { /* In between */
			pi->proc_table[entry].prev = curr;
			pi->proc_table[entry].next = pi->proc_table[curr].next;

			pi->proc_table[pi->proc_table[entry].next].prev = entry;
			pi->proc_table[curr].next = entry;
		}
	}
}

static void __reclaim_completed_reqs(void)
{
	unsigned int turn;

	for (turn = 0; turn < vdev->config.nr_io_cpu; turn++) {
		struct nvmev_proc_info *pi;
		struct nvmev_proc_table *pe;

		unsigned int first_entry = -1;
		unsigned int last_entry = -1;
		unsigned int curr;
		int nr_reclaimed = 0;

		pi = &vdev->proc_info[turn];

		first_entry = pi->io_seq;
		curr = first_entry;

		while (curr != -1) {
			pe = &pi->proc_table[curr];
			if (pe->is_completed == true && pe->is_copied == true
					&& pe->nsecs_target <= pi->proc_io_nsecs) {
				last_entry = curr;
				curr = pe->next;
				nr_reclaimed++;
			} else {
				break;
			}
		}

		if (last_entry != -1) {
			pe = &pi->proc_table[last_entry];
			pi->io_seq = pe->next;
			if (pe->next != -1) {
				pi->proc_table[pe->next].prev = -1;
			}
			pe->next = -1;

			pe = &pi->proc_table[first_entry];
			pe->prev = pi->free_seq_end;

			pe = &pi->proc_table[pi->free_seq_end];
			pe->next = first_entry;

			pi->free_seq_end = last_entry;
			NVMEV_DEBUG("Reclaimed %u -- %u, %d\n", first_entry, last_entry, nr_reclaimed);
		}
	}
}

static size_t __nvmev_proc_io(int sqid, int sq_entry)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	unsigned long long nsecs_start = __get_wallclock();
	struct nvme_command *cmd = &sq_entry(sq_entry);
	
	static unsigned long long order = 0;

#if (BASE_SSD == KV_PROTOTYPE)
	uint32_t nsid = 0; // Some KVSSD programs give 0 as nsid for KV IO
#else
	uint32_t nsid = cmd->common.nsid - 1;
#endif

	struct nvmev_ns *ns = &vdev->ns[nsid];
	
	struct nvmev_request req = {
		.cmd = cmd,
		.sq_id = sqid,
		.nsecs_start = nsecs_start,
	};
	struct nvmev_result ret = {
		.nsecs_target = nsecs_start,
		.status = NVME_SC_SUCCESS,
		.order = order,
	};
#ifdef COUPLED_GC_MTL
	list_init(&ret.mtl_migration_list);
	list_init(&ret.mtl_translation_list);
	ret.migration_cnt = 0;
	
	int i;
	for (i = 0; i < SSD_PARTITIONS; i ++) {
		list_init(&ret.mtl_read_translation_list[i]);
	}
#endif
#ifdef MIGRATION_IO
	//list_init(&ret.mg_batch_list);
	//ret.ise = NULL;
	list_init(&ret.ise_list);
#endif

#ifdef PERF_DEBUG
	unsigned long long prev_clock = local_clock();
	unsigned long long prev_clock2 = 0;
	unsigned long long prev_clock3 = 0;
	unsigned long long prev_clock4 = 0;
	static unsigned long long clock1 = 0;
	static unsigned long long clock2 = 0;
	static unsigned long long clock3 = 0;
	static unsigned long long counter = 0;
#endif

	if (!ns->proc_io_cmd(ns, &req, &ret))
			return false;
	
	order ++;

#ifdef PERF_DEBUG
	prev_clock2 = local_clock();
#endif

#ifdef COUPLED_GC_MTL
	__enqueue_io_req(sqid, sq->cqid, sq_entry, nsecs_start, &ret);
#else
	__enqueue_io_req(ns, sqid, sq->cqid, sq_entry, nsecs_start, &ret);
#endif

#ifdef PERF_DEBUG
	prev_clock3 = local_clock();
#endif

	__reclaim_completed_reqs();

#ifdef PERF_DEBUG
	prev_clock4 = local_clock();

	clock1 += (prev_clock2 - prev_clock);
	clock2 += (prev_clock3 - prev_clock2);
	clock3 += (prev_clock4 - prev_clock3);
	counter++;

	if (counter > 1000) {
		NVMEV_DEBUG("LAT: %llu, ENQ: %llu, CLN: %llu\n",
				clock1 / counter, clock2 / counter, clock3 / counter);
		clock1 = 0;
		clock2 = 0;
		clock3 = 0;
		counter = 0;
	}
#endif
	return true;
}


int nvmev_proc_io_sq(int sqid, int new_db, int old_db)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	int num_proc = new_db - old_db;
	int seq;
	int sq_entry = old_db;
	int latest_db;

	if (unlikely(!sq)) return old_db;
	if (unlikely(num_proc < 0)) num_proc += sq->queue_size;

	for (seq = 0; seq < num_proc; seq++) {
		if (!__nvmev_proc_io(sqid, sq_entry))
			break;

		if (++sq_entry == sq->queue_size) {
			sq_entry = 0;
		}
		sq->stat.nr_dispatched++;
		sq->stat.nr_in_flight++;
		//sq->stat.total_io += io_size;
	}
	sq->stat.nr_dispatch++;
	sq->stat.max_nr_in_flight =
		max_t(int, sq->stat.max_nr_in_flight, sq->stat.nr_in_flight);

	latest_db = (old_db + seq) % sq->queue_size;
	//latest_db = new_db;
	return latest_db;
}

void nvmev_proc_io_cq(int cqid, int new_db, int old_db)
{
	struct nvmev_completion_queue *cq = vdev->cqes[cqid];
	int i;
	for (i = old_db; i != new_db; i++) {
		if (i >= cq->queue_size) {
			i = -1;
			continue;
		}
		vdev->sqes[cq_entry(i).sq_id]->stat.nr_in_flight--;
	}

	cq->cq_tail = new_db - 1;
	if (new_db == -1) cq->cq_tail = cq->queue_size - 1;
}

#ifdef MIGRATION_IO
static void __fill_cq_result(struct nvmev_proc_table * proc_entry, int mg_cmd_submitted)
#else
static void __fill_cq_result(struct nvmev_proc_table * proc_entry)
#endif
{
	int sqid = proc_entry -> sqid;
	int cqid = proc_entry -> cqid;
	int sq_entry = proc_entry -> sq_entry;
	unsigned int command_id = proc_entry -> command_id;
	unsigned int status = proc_entry -> status;
	unsigned int result0 = proc_entry -> result0;
	unsigned int result1 = proc_entry -> result1;


	struct nvmev_completion_queue *cq = vdev->cqes[cqid];
	int cq_head = cq->cq_head;

	spin_lock(&cq->entry_lock);
	cq_entry(cq_head).command_id = command_id;
	cq_entry(cq_head).sq_id = sqid;
	cq_entry(cq_head).sq_head = sq_entry;
	cq_entry(cq_head).status = cq->phase | status << 1;
	cq_entry(cq_head).result0 = result0;
#ifdef MIGRATION_IO
	/* To let blk mq completion thread know migration entry is loaded */
	cq_entry(cq_head).result1 = mg_cmd_submitted;
#else
	cq_entry(cq_head).result1 = result1;
#endif

	if (++cq_head == cq->queue_size) {
		cq_head = 0;
		cq->phase = !cq->phase;
	}

	cq->cq_head = cq_head;
	cq->interrupt_ready = true;
	spin_unlock(&cq->entry_lock);
}

static int nvmev_kthread_io(void *data)
{
	struct nvmev_proc_info *pi = (struct nvmev_proc_info *)data;
	struct nvmev_ns *ns;

#ifdef PERF_DEBUG
	static unsigned long long intr_clock[NR_MAX_IO_QUEUE + 1];
	static unsigned long long intr_counter[NR_MAX_IO_QUEUE + 1];

	unsigned long long prev_clock;
#endif
	static unsigned long long exp_order = 0;
	static unsigned long long last_order = 0;

	NVMEV_INFO("%s started on cpu %d (node %d)\n",
			pi->thread_name, smp_processor_id(), cpu_to_node(smp_processor_id()));

	while (!kthread_should_stop()) {
		unsigned long long curr_nsecs_wall = __get_wallclock();
		unsigned long long curr_nsecs_local = local_clock();
		long long delta = curr_nsecs_wall - curr_nsecs_local;

		volatile unsigned int curr = pi->io_seq;
		int qidx;

		while (curr != -1) {
			struct nvmev_proc_table *pe = &pi->proc_table[curr];
			unsigned long long curr_nsecs = local_clock() + delta;
			pi->proc_io_nsecs = curr_nsecs;

			if (pe->is_completed == true) {
				curr = pe->next;
				continue;
			}

			if (pe->is_copied == false) {
#ifdef PERF_DEBUG
				unsigned long long memcpy_time;
				pe->nsecs_copy_start = local_clock() + delta;
#endif
				if (pe->writeback_cmd) {
					;
				} else if (io_using_dma) {
					__do_perform_io_using_dma(pe->sqid, pe->sq_entry);
				} else {
#if (BASE_SSD == KV_PROTOTYPE)
					struct nvmev_submission_queue *sq = vdev->sqes[pe->sqid];
					ns = &vdev->ns[0];
					if (ns->identify_io_cmd(ns, sq_entry(pe->sq_entry))) {
						pe->result0 = ns->perform_io_cmd(ns, &sq_entry(pe->sq_entry), &(pe->status));
					} else {
						__do_perform_io(pe->sqid, pe->sq_entry);
					}
#endif
					//__do_perform_io(pe->sqid, pe->sq_entry);
					
					//if (last_order > pe->order) {
					//	printk("%s: last order: %llu cur order: %llu", __func__, 
					//			last_order, pe->order);
					//	int ii, idx;
					//	for( ii = 0; ii < ENT_CNT_; ii ++) {
					//		//idx = (ii + ent_cnt + 1) % ENT_CNT_;
					//		idx = ii;
					//		printk("%s: L order: %llu entry: %llu next entry: %llu way: %llu", 
					//				__func__, order_stack[idx], 
					//				ent_stack[idx], ent_stack2[idx], way_stack[idx]);
					//	}
					//	NVMEV_ASSERT(0);
					//}
					last_order = pe->order;
					
					if (exp_order != pe->order) {
						printk("%s: expected order: %llu cur order: %llu", __func__, 
								exp_order, pe->order, ent_cnt);
						int ii, idx;
						for( ii = 0; ii < ENT_CNT_; ii ++) {
							//idx = (ii + ent_cnt + 1) % ENT_CNT_;
							idx = ii;
							printk("%s: order: %llu entry: %llu next entry: %llu way: %llu", 
									__func__, order_stack[idx], 
									ent_stack[idx], ent_stack2[idx], way_stack[idx]);
						}
						NVMEV_ASSERT(0);

					}
					exp_order ++;
					//if (exp_order % 200 == 0)
					//	printk("%s: exp_order: %llu", __func__, exp_order);
					__do_perform_io(pe);


#ifdef COUPLED_GC_MTL
					reflect_mtl_migration_log(&pe->mtl_migration_list, pe->sqid, pe->sq_entry, 
							pe->order, pe->migration_cnt);
#endif
				}

#ifdef PERF_DEBUG
				pe->nsecs_copy_done = local_clock() + delta;
				memcpy_time = pe->nsecs_copy_done - pe->nsecs_copy_start;
#endif
				pe->is_copied = true;

				NVMEV_DEBUG("%s: copied %u, %d %d %d\n",
						pi->thread_name, curr,
						pe->sqid, pe->cqid, pe->sq_entry);
			}

			if (pe->nsecs_target <= curr_nsecs) {
				if (pe->writeback_cmd) {
#if (BASE_SSD == SAMSUNG_970PRO || BASE_SSD == ZNS_PROTOTYPE)
					buffer_release((struct buffer *)pe->write_buffer, pe->buffs_to_release);
#endif
				} else  {
#ifdef MIGRATION_IO
					int mg_cmd_submitted = 0;
					struct inflight_set_entry *ise;
					struct list_elem *le;

					while(!list_empty_(&pe->ise_list)) {
						le = list_pop_front(&pe->ise_list);
						ise = list_entry(le, struct inflight_set_entry, list_elem);
						
						mg_cmd_submitted |= __fill_rev_sq_cmd(ise, pe->sqid, pe->sq_entry);
					}
					__fill_cq_result(pe, mg_cmd_submitted);
#else
					__fill_cq_result(pe);
#endif
				}

				NVMEV_DEBUG("%s: completed %u, %d %d %d\n",
						pi->thread_name, curr,
						pe->sqid, pe->cqid, pe->sq_entry);

#ifdef PERF_DEBUG
				pe->nsecs_cq_filled = local_clock() + delta;
				trace_printk("%llu %llu %llu %llu %llu %llu\n",
						pe->nsecs_start,
						pe->nsecs_enqueue - pe->nsecs_start,
						pe->nsecs_copy_start - pe->nsecs_start,
						pe->nsecs_copy_done - pe->nsecs_start,
						pe->nsecs_cq_filled - pe->nsecs_start,
						pe->nsecs_target - pe->nsecs_start);
#endif
				mb(); /* Reclaimer shall see after here */
				pe->is_completed = true;
			}

			curr = pe->next;
		}

		for (qidx = 1; qidx <= vdev->nr_cq; qidx++) {
			struct nvmev_completion_queue *cq = vdev->cqes[qidx];
#if SUPPORT_MULTI_IO_WORKER_BY_SQ
			if ((pi->id) != ((qidx - 1) % vdev->config.nr_io_cpu)) continue;
#endif
			if (cq == NULL || !cq->irq_enabled) continue;

			if (spin_trylock(&cq->irq_lock)) {
				if (cq->interrupt_ready == true) {

#ifdef PERF_DEBUG
					prev_clock = local_clock();
#endif
					cq->interrupt_ready = false;
					nvmev_signal_irq(cq->irq_vector);

#ifdef PERF_DEBUG
					intr_clock[qidx] += (local_clock() - prev_clock);
					intr_counter[qidx]++;

					if (intr_counter[qidx] > 1000) {
						NVMEV_DEBUG("Intr %d: %llu\n", qidx,
								intr_clock[qidx] / intr_counter[qidx]);
						intr_clock[qidx] = 0;
						intr_counter[qidx] = 0;
					}
#endif
				}
				spin_unlock(&cq->irq_lock);
			}
		}
	
		/* submit mg command */
/*
#ifdef MIGRATION_IO
		struct nvmev_rev_submission_queue *rsq = vdev->rev_sqe;
		if (rsq == NULL)	goto rev_sq_done;
		qidx = rsq->qid;
#if SUPPORT_MULTI_IO_WORKER_BY_SQ
		if ((pi->id) != ((qidx - 1) % vdev->config.nr_io_cpu)) goto rev_sq_done;
#endif
		if (rsq == NULL || !rsq->irq_enabled) goto rev_sq_done;

		if (spin_trylock(&rsq->irq_lock)) {
			if (rsq->interrupt_ready == true) {

#ifdef PERF_DEBUG
				prev_clock = local_clock();
#endif
				rsq->interrupt_ready = false;
				nvmev_signal_irq(rsq->irq_vector);

#ifdef PERF_DEBUG
				intr_clock[qidx] += (local_clock() - prev_clock);
				intr_counter[qidx]++;

				if (intr_counter[qidx] > 1000) {
					NVMEV_DEBUG("Intr %d: %llu\n", qidx,
							intr_clock[qidx] / intr_counter[qidx]);
					intr_clock[qidx] = 0;
					intr_counter[qidx] = 0;
				}
#endif
			}
			spin_unlock(&rsq->irq_lock);
		}
#endif
rev_sq_done:
*/
		cond_resched();
	}

	return 0;
}

void NVMEV_IO_PROC_INIT(struct nvmev_dev *vdev)
{
	unsigned int i, proc_idx;

	vdev->proc_info = kcalloc(sizeof(struct nvmev_proc_info), vdev->config.nr_io_cpu, GFP_KERNEL);
	vdev->proc_turn = 0;

	for (proc_idx = 0; proc_idx < vdev->config.nr_io_cpu; proc_idx++) {
		struct nvmev_proc_info *pi = &vdev->proc_info[proc_idx];

		pi->proc_table = kzalloc(sizeof(struct nvmev_proc_table) * NR_MAX_PARALLEL_IO, GFP_KERNEL);
		for (i = 0; i < NR_MAX_PARALLEL_IO; i++) {
			pi->proc_table[i].next = i + 1;
			pi->proc_table[i].prev = i - 1;
		}
		pi->proc_table[NR_MAX_PARALLEL_IO - 1].next = -1;
#if SUPPORT_MULTI_IO_WORKER_BY_SQ
		pi->id = proc_idx;
#endif
		pi->free_seq = 0;
		pi->free_seq_end = NR_MAX_PARALLEL_IO - 1;
		pi->io_seq = -1;
		pi->io_seq_end = -1;

		snprintf(pi->thread_name, sizeof(pi->thread_name), "nvmev_proc_io_%d", proc_idx);

		pi->nvmev_io_worker = kthread_create(nvmev_kthread_io, pi, pi->thread_name);

		kthread_bind(pi->nvmev_io_worker, vdev->config.cpu_nr_proc_io[proc_idx]);
		wake_up_process(pi->nvmev_io_worker);
	}
}

void NVMEV_IO_PROC_FINAL(struct nvmev_dev *vdev)
{
	unsigned int i;

	for (i = 0; i < vdev->config.nr_io_cpu; i++) {
		struct nvmev_proc_info *pi = &vdev->proc_info[i];

		if (!IS_ERR_OR_NULL(pi->nvmev_io_worker)) {
			kthread_stop(pi->nvmev_io_worker);
		}

		kfree(pi->proc_table);
	}

	kfree(vdev->proc_info);
}
