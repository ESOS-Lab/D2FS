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

#include "nvmev.h"
#include "conv_ftl.h"

#ifdef DISCARD_ENABLED
#include <linux/highmem.h>
#endif
#ifdef WAF
unsigned long long OS_TimeGetUS( void )
{
    struct timespec64 lTime;
    ktime_get_coarse_real_ts64(&lTime);
    return (lTime.tv_sec * 1000000 + div_u64(lTime.tv_nsec, 1000) );

}

#define SEC_IN_USEC 1000000
#define MSEC_IN_USEC 1000
#define PRINT_TIME_SEC	1
#define WAF_TIME_INTERVAL	(PRINT_TIME_SEC * SEC_IN_USEC)
#define HOST_GC_OVERHEAD_ANALYSIS_TIME_INTERVAL	(PRINT_TIME_SEC * SEC_IN_USEC)

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
		//printk("%s: WAF: %u percent gc: %llu KB write_req: %llu KB total: %llu KB total_gcwrite: %u KB total WAF: %u percent", 
		//	__func__, waf, ns->write_volume_gc*4, ns->write_volume_host*4, 
		//	ns->total_write_volume_host*4, ns->total_write_volume_gc*4, total_waf);
	}
}

#ifdef HOST_GC_OVERHEAD_ANALYSIS
static inline void print_req_cnt(struct nvmev_ns *ns, unsigned long long t_intval)
{

	printk("%s: user_req_cnt: %llu time_interval: %llu", 
		__func__, ns->req_cnt, t_intval);
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

	if (cur_t - ns->last_t > WAF_TIME_INTERVAL) {
		print_WAF(ns);
#ifdef MG_CMD_CNT
		print_MG_CMD_CNT(ns);
		ns->mg_cmd_cnt = 0;
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

static inline struct ppa get_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t lpn)
{
	return conv_ftl->maptbl[lpn];
}

static inline void set_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *ppa)
{
	NVMEV_ASSERT(lpn < conv_ftl->ssd->sp.tt_pgs);
	conv_ftl->maptbl[lpn] = *ppa;
}


static inline void invalidate_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t local_lpn)
{
    NVMEV_ASSERT(local_lpn < conv_ftl->ssd->sp.tt_pgs);
    conv_ftl->maptbl[local_lpn].ppa = INVALID_PPA;
}






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

static inline void consume_write_credit(struct conv_ftl *conv_ftl)
{
	conv_ftl->wfc.write_credits--;
}

static void forground_gc(struct conv_ftl *conv_ftl);

static inline void check_and_refill_write_credit(struct conv_ftl *conv_ftl)
{
	struct write_flow_control * wfc = &(conv_ftl->wfc);
	if (wfc->write_credits <= 0) {
	    forground_gc(conv_ftl);

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
	    /* initialize all the lines as free lines */
		list_add_tail(&line->entry, &lm->free_line_list);
	    lm->free_line_cnt++;
	}

	NVMEV_ASSERT(lm->free_line_cnt == lm->tt_lines);
	lm->victim_line_cnt = 0;
	lm->full_line_cnt = 0;
}

static void init_write_pointer(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	struct write_pointer *wpp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *curline = NULL;

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
}

static void init_write_flow_control(struct conv_ftl *conv_ftl) {
	struct write_flow_control * wfc = &(conv_ftl->wfc);
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	wfc->write_credits = spp->pgs_per_line;
	wfc->credits_to_refill = spp->pgs_per_line;
}

static inline void check_addr(int a, int max)
{
	NVMEV_ASSERT(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct conv_ftl *conv_ftl)
{
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *curline = NULL;

	curline = list_first_entry(&lm->free_line_list, struct line, entry);
	if (!curline) {
	    NVMEV_ERROR("No free lines left in VIRT !!!!\n");
	    return NULL;
	}

	list_del_init(&curline->entry);
	lm->free_line_cnt--;
	NVMEV_DEBUG("[%s] free_line_cnt %d\n",__FUNCTION__, lm->free_line_cnt);
	return curline;
}

static void advance_write_pointer(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct write_pointer *wpp;
	if (io_type == USER_IO) {
	    wpp = &conv_ftl->wp;
	} else if (io_type == GC_IO) {
	    wpp = &conv_ftl->gc_wp;
	} else {
	    NVMEV_ASSERT(0);
	}

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
	} else {
		NVMEV_DEBUG("wpp: line is moved to victim list\n");
		NVMEV_ASSERT(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
		/* there must be some invalid pages in this line */
		NVMEV_ASSERT(wpp->curline->ipc > 0);
		pqueue_insert(lm->victim_line_pq, wpp->curline);
		lm->victim_line_cnt++;
	}
	/* current line is used up, pick another empty line */
	check_addr(wpp->blk, spp->blks_per_pl);
	wpp->curline = NULL;
	wpp->curline = get_next_free_line(conv_ftl);
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

static struct ppa get_new_page(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	struct write_pointer *wpp;
	struct ppa ppa;

	if (io_type == USER_IO) {
	    wpp = &conv_ftl->wp;
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

	return ppa;
}

static void init_maptbl(struct conv_ftl *conv_ftl)
{
	int i;
	struct ssdparams *spp = &conv_ftl->ssd->sp;
#ifdef EQUAL_IM_MEM
	unsigned long meta_pgs_IM = conv_ftl->npages_im_meta;
	unsigned long main_pgs_IM = conv_ftl->npages_im_main;
	uint64_t mem_consumption = 0;
#endif
	conv_ftl->maptbl = vmalloc(sizeof(struct ppa) * spp->tt_pgs);
	for (i = 0; i < spp->tt_pgs; i++) {
	    conv_ftl->maptbl[i].ppa = UNMAPPED_PPA;
	}
#ifdef EQUAL_IM_MEM
	mem_consumption += (sizeof(struct ppa) * spp->tt_pgs);
    uint64_t IM_mem = 0;
    //for (no_type = META_PARTITION + 1; no_type < NO_TYPE_IM; no_type ++){
    //  IM_mem += (sizeof(struct ppa) * main_pgs);
    //}
    IM_mem += (sizeof(struct ppa) * main_pgs_IM * (NO_TYPE_IM-1-3));
    NVMEV_ASSERT(IM_mem > mem_consumption);
    printk("%s: redundant: %u MB", __func__, (IM_mem-mem_consumption)/1024/1024);
    conv_ftl->redundant = vmalloc(IM_mem - mem_consumption);
    NVMEV_ASSERT(conv_ftl->redundant != NULL);
	
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

static void conv_init_ftl(struct conv_ftl *conv_ftl, struct convparams *cpp, struct ssd *ssd)
{
	/*copy convparams*/
	conv_ftl->cp = *cpp;

	conv_ftl->ssd = ssd;

#ifdef EQUAL_IM_MEM
	conv_ftl->npages_im_meta = NPAGES_IM_META(ssd->sp);
	conv_ftl->npages_im_main = NPAGES_IM_MAIN(ssd->sp);
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

	NVMEV_INFO("Init FTL Instance with %d channels(%ld pages)\n",  conv_ftl->ssd->sp.nchs, conv_ftl->ssd->sp.tt_pgs);

	return;
}

static void conv_init_params(struct convparams *cpp)
{
	cpp->op_area_pcent = OP_AREA_PERCENT;
	cpp->gc_thres_lines = 2; /* Need only two lines.(host write, gc)*/
	cpp->gc_thres_lines_high = 2; /* Need only two lines.(host write, gc)*/
	cpp->enable_gc_delay = 1;
	cpp->pba_pcent = (int)((1 + cpp->op_area_pcent) * 100);
}

void conv_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr, uint32_t cpu_nr_dispatcher)
{
	struct ssdparams spp;
	struct convparams cpp;
	struct conv_ftl *conv_ftls;
	struct ssd *ssd;
	uint32_t i;
	const uint32_t nr_parts = SSD_PARTITIONS;

	ssd_init_params(&spp, size, nr_parts);
	conv_init_params(&cpp);

	conv_ftls = kmalloc(sizeof(struct conv_ftl) * nr_parts, GFP_KERNEL);

	for (i = 0; i < nr_parts; i++) {
	    ssd = kmalloc(sizeof(struct ssd), GFP_KERNEL);
	    ssd_init(ssd, &spp, cpu_nr_dispatcher);
	    conv_init_ftl(&conv_ftls[i], &cpp, ssd);
	}

	/* PCIe, Write buffer are shared by all instances*/
	for (i = 1; i < nr_parts; i++) {
	    kfree(conv_ftls[i].ssd->pcie);
	    kfree(conv_ftls[i].ssd->write_buffer);

	    conv_ftls[i].ssd->pcie = conv_ftls[0].ssd->pcie;
	    conv_ftls[i].ssd->write_buffer = conv_ftls[0].ssd->write_buffer;
	}

	for (i = 0; i < nr_parts; i++) {
		conv_ftls[i].ns = ns;
	}

	ns->id = id;
	ns->csi = NVME_CSI_NVM;
	ns->nr_parts = nr_parts;
	ns->ftls = (void *)conv_ftls;
	ns->size = (uint64_t)((size * 100) / cpp.pba_pcent);
	ns->mapped = mapped_addr;
	/*register io command handler*/
	ns->proc_io_cmd = conv_proc_nvme_io_cmd;

	NVMEV_INFO("FTL physical space: %lld, logical space: %lld (physical/logical * 100 = %d)\n", size, ns->size, cpp.pba_pcent);

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


#ifdef EQUAL_IM_MEM
    uint64_t window_size_im, n_mtl_zones_im, meta_window_size, n_mtl_meta_zones;
    window_size_im = ns->size * IM_WINDOW_EXT_RATE / PAGE_SIZE * sizeof(MTL_ENTRY);
    n_mtl_zones_im = (window_size_im % MTL_ZONE_SIZE)?
                        window_size_im / MTL_ZONE_SIZE + 1:
                        window_size_im / MTL_ZONE_SIZE;

    n_mtl_zones_im = n_mtl_zones_im + n_mtl_zones_im / 5;
    uint64_t mem_consump = 0, IM_mem = 0;
    IM_mem += (sizeof(struct mtl_zone_entry *) * n_mtl_zones_im * (NO_TYPE_IM-1 - 3)); // -3: -1 for cold data partition, -2 for 1/3 cold node, 1/3 hot node and 1/3 hot data
    IM_mem += (sizeof(struct mtl_zone_entry) * n_mtl_zones_im * (NO_TYPE_IM-1 - 3));

    meta_window_size = ns->size / PAGE_SIZE * sizeof(MTL_ENTRY);
    n_mtl_meta_zones = (meta_window_size % MTL_ZONE_SIZE)?
                        meta_window_size / MTL_ZONE_SIZE + 1:
                        meta_window_size / MTL_ZONE_SIZE;

	IM_mem += (sizeof(struct mtl_zone_entry *) * n_mtl_meta_zones);
	IM_mem += (sizeof(struct mtl_zone_entry) * n_mtl_meta_zones);
	NVMEV_ASSERT(IM_mem > 0);
	printk("%s: mtl redundant: %u MB", __func__, IM_mem / 1024 / 1024);
	ns->mtl_redundant = vmalloc(IM_mem);
	NVMEV_ASSERT(ns->mtl_redundant != NULL);
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
	return (lpn < conv_ftl->ssd->sp.tt_pgs);
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
	NVMEV_ASSERT(pg->status == PG_VALID);
	pg->status = PG_INVALID;

	/* update corresponding block status */
	blk = get_blk(conv_ftl->ssd, ppa);
	NVMEV_ASSERT(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
	blk->ipc++;
	NVMEV_ASSERT(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
	blk->vpc--;

	/* update corresponding line status */
	line = get_line(conv_ftl, ppa);
	NVMEV_ASSERT(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
	if (line->vpc == spp->pgs_per_line) {
	    NVMEV_ASSERT(line->ipc == 0);
	    was_full_line = true;
	}
	line->ipc++;
	NVMEV_ASSERT(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
	/* Adjust the position of the victime line in the pq under over-writes */
	if (line->pos) {
	    /* Note that line->vpc will be updated by this call */
	    pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
	} else {
	    line->vpc--;
	}

	if (was_full_line) {
	    /* move line: "full" -> "victim" */
	    list_del_init(&line->entry);
	    lm->full_line_cnt--;
	    pqueue_insert(lm->victim_line_pq, line);
	    lm->victim_line_cnt++;
	}
}

static void mark_page_valid(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	struct line *line;

	/* update page status */
	pg = get_pg(conv_ftl->ssd, ppa);
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

static void gc_read_page(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	/* advance conv_ftl status, we don't care about how long it takes */
	if (cpp->enable_gc_delay) {
	    struct nand_cmd gcr;
	    gcr.type = GC_IO;
	    gcr.cmd = NAND_READ;
	    gcr.stime = 0;
	    gcr.xfer_size = spp->pgsz;
	    gcr.interleave_pci_dma = false;
	    gcr.ppa = ppa;
	    ssd_advance_nand(conv_ftl->ssd, &gcr);
	}
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct conv_ftl *conv_ftl, struct ppa *old_ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	struct ppa new_ppa;
	uint64_t lpn = get_rmap_ent(conv_ftl, old_ppa);

	NVMEV_ASSERT(valid_lpn(conv_ftl, lpn));
	new_ppa = get_new_page(conv_ftl, GC_IO);
	/* update maptbl */
	set_maptbl_ent(conv_ftl, lpn, &new_ppa);
	/* update rmap */
	set_rmap_ent(conv_ftl, lpn, &new_ppa);

	mark_page_valid(conv_ftl, &new_ppa);

	/* need to advance the write pointer here */
	advance_write_pointer(conv_ftl, GC_IO);

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

	    ssd_advance_nand(conv_ftl->ssd, &gcw);
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
static void clean_one_block(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_page *pg_iter = NULL;
	int cnt = 0;
	int pg;

	for (pg = 0; pg < spp->pgs_per_blk; pg++) {
	    ppa->g.pg = pg;
	    pg_iter = get_pg(conv_ftl->ssd, ppa);
	    /* there shouldn't be any free page in victim blocks */
	    NVMEV_ASSERT(pg_iter->status != PG_FREE);
	    if (pg_iter->status == PG_VALID) {
	        gc_read_page(conv_ftl, ppa);
	        /* delay the maptbl update until "write" happens */
	        gc_write_page(conv_ftl, ppa);
	        cnt++;
	    }
	}

	NVMEV_ASSERT(get_blk(conv_ftl->ssd, ppa)->vpc == cnt);
}

/* here ppa identifies the block we want to clean */
static void clean_one_flashpg(struct conv_ftl *conv_ftl, struct ppa *ppa)
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

	if (cpp->enable_gc_delay) {
		struct nand_cmd gcr;
		gcr.type = GC_IO;
		gcr.cmd = NAND_READ;
		gcr.stime = 0;
		gcr.xfer_size = spp->pgsz * cnt;
		gcr.interleave_pci_dma = false;
		gcr.ppa = &ppa_copy;
		completed_time = ssd_advance_nand(conv_ftl->ssd, &gcr);
	}

	for (i = 0; i < spp->pgs_per_flashpg; i++) {
		pg_iter = get_pg(conv_ftl->ssd, &ppa_copy);

		/* there shouldn't be any free page in victim blocks */
		if (pg_iter->status == PG_VALID) {
			/* delay the maptbl update until "write" happens */
			gc_write_page(conv_ftl, &ppa_copy);
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
	/* move this line to free line list */
	list_add_tail(&line->entry, &lm->free_line_list);
	lm->free_line_cnt++;
}

static int do_gc(struct conv_ftl *conv_ftl, bool force)
{
	struct line *victim_line = NULL;
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	struct nand_lun *lunp;
	struct ppa ppa;
	int ch, lun, flashpg;

	victim_line = select_victim_line(conv_ftl, force);
	if (!victim_line) {
	    return -1;
	}

	ppa.g.blk = victim_line->id;
	NVMEV_DEBUG("GC-ing line:%d,ipc=%d(%d),victim=%d,full=%d,free=%d\n", ppa.g.blk,
	          victim_line->ipc,victim_line->vpc, conv_ftl->lm.victim_line_cnt, conv_ftl->lm.full_line_cnt,
	          conv_ftl->lm.free_line_cnt);

#ifdef WAF
	conv_ftl->ns->write_volume_gc += victim_line->vpc;
	conv_ftl->ns->total_write_volume_gc += victim_line->vpc;
#endif

	conv_ftl->wfc.credits_to_refill = victim_line->ipc;

	/* copy back valid data */
	for (flashpg = 0; flashpg < spp->flashpgs_per_blk; flashpg++) {
	    ppa.g.pg = flashpg * spp->pgs_per_flashpg;
	    for (ch = 0; ch < spp->nchs; ch++) {
	        for (lun = 0; lun < spp->luns_per_ch; lun++) {
	            ppa.g.ch = ch;
	            ppa.g.lun = lun;
	            ppa.g.pl = 0;
	            lunp = get_lun(conv_ftl->ssd, &ppa);
	            clean_one_flashpg(conv_ftl, &ppa);

	            if (flashpg == (spp->flashpgs_per_blk - 1)) {
	                mark_block_free(conv_ftl, &ppa);

	                if (cpp->enable_gc_delay) {
	                    struct nand_cmd gce;
	                    gce.type = GC_IO;
	                    gce.cmd = NAND_ERASE;
	                    gce.stime = 0;
	                    gce.interleave_pci_dma = false;
	                    gce.ppa = &ppa;
	                    ssd_advance_nand(conv_ftl->ssd, &gce);
	                }

	                lunp->gc_endtime = lunp->next_lun_avail_time;
	            }
	        }
	    }
	}

	/* update line status */
	mark_line_free(conv_ftl, &ppa);

	return 0;
}

static void forground_gc(struct conv_ftl *conv_ftl) {
	if (should_gc_high(conv_ftl)) {
	    NVMEV_DEBUG("should_gc_high passed");
	    /* perform GC here until !should_gc(conv_ftl) */
	    do_gc(conv_ftl, true);
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

#ifdef HOST_GC_OVERHEAD_ANALYSIS
	ns->req_cnt ++;
#endif

#ifdef WAF	
	try_print_WAF(ns);
#endif

	NVMEV_ASSERT(conv_ftls);
	NVMEV_DEBUG("conv_read: start_lpn=%lld, len=%d, end_lpn=%ld", start_lpn, nr_lba, end_lpn);
	if ((end_lpn/nr_parts) >= spp->tt_pgs) {
	    NVMEV_ERROR("conv_read: lpn passed FTL range(start_lpn=%lld,tt_pgs=%ld)\n", start_lpn, spp->tt_pgs);
	    return false;
	}

	if (LBA_TO_BYTE(nr_lba) <= (KB(4) * nr_parts)) {
	    srd.stime += spp->fw_4kb_rd_lat;
	} else {
	    srd.stime += spp->fw_rd_lat;
	}

	for (i = 0; (i < nr_parts) && (start_lpn <= end_lpn); i++, start_lpn++) {
	    conv_ftl = &conv_ftls[start_lpn % nr_parts];
	    xfer_size = 0;
	    prev_ppa = get_maptbl_ent(conv_ftl, start_lpn/nr_parts);

	    NVMEV_DEBUG("[%s] conv_ftl=%p, ftl_ins=%lld, local_lpn=%lld",__FUNCTION__, conv_ftl, lpn%nr_parts, lpn/nr_parts);

	    /* normal IO read path */
	    for (lpn = start_lpn; lpn <= end_lpn; lpn+=nr_parts) {
	        local_lpn = lpn / nr_parts;
	        cur_ppa = get_maptbl_ent(conv_ftl, local_lpn);
			if (!mapped_ppa(&cur_ppa) || !valid_ppa(conv_ftl, &cur_ppa)) {
	            NVMEV_DEBUG("lpn 0x%llx not mapped to valid ppa\n", local_lpn);
	            NVMEV_DEBUG("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d\n",
	            cur_ppa.g.ch, cur_ppa.g.lun, cur_ppa.g.blk, cur_ppa.g.pl, cur_ppa.g.pg);
	            continue;
	        }

	        // aggregate read io in same flash page
	        if (mapped_ppa(&prev_ppa) && is_same_flash_page(conv_ftl, cur_ppa, prev_ppa)) {
	            xfer_size += spp->pgsz;
	            continue;
	        }

	        if (xfer_size > 0) {
	            srd.xfer_size = xfer_size;
	            srd.ppa = &prev_ppa;
	            nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &srd);
	            nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;
	        }

	        xfer_size = spp->pgsz;
	        prev_ppa = cur_ppa;
	    }

	    // issue remaining io
	    if (xfer_size > 0) {
	        srd.xfer_size = xfer_size;
	        srd.ppa = &prev_ppa;
	        nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &srd);
	        nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;
	    }
	}

	ret->nsecs_target = nsecs_latest;
	ret->status = NVME_SC_SUCCESS;
	return true;
}

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

#ifdef HOST_GC_OVERHEAD_ANALYSIS
	ns->req_cnt ++;
#endif

#ifdef WAF	
	try_print_WAF(ns);
#endif

	NVMEV_ASSERT(conv_ftls);
	NVMEV_DEBUG("conv_write: start_lpn=%lld, len=%d, end_lpn=%lld", start_lpn, nr_lba, end_lpn);
	if ((end_lpn/nr_parts) >= spp->tt_pgs) {
	    NVMEV_ERROR("conv_write: lpn passed FTL range(start_lpn=%lld,tt_pgs=%ld)\n", start_lpn, spp->tt_pgs);
	    return false;
	}

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
#ifdef WAF
	ns->write_volume_host += (end_lpn - start_lpn + 1);
	ns->total_write_volume_host += (end_lpn - start_lpn + 1);
#endif

	for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
	    conv_ftl = &conv_ftls[lpn % nr_parts];
	    local_lpn = lpn / nr_parts;
	    ppa = get_maptbl_ent(conv_ftl, local_lpn); // 현재 LPN에 대해 전에 이미 쓰인 PPA가 있는지 확인
	    if (mapped_ppa(&ppa)) {
	        /* update old page information first */
	        mark_page_invalid(conv_ftl, &ppa);
	        set_rmap_ent(conv_ftl, INVALID_LPN, &ppa);
	        NVMEV_DEBUG("conv_write: %lld is invalid, ", ppa2pgidx(conv_ftl, &ppa));
	    }

	    /* new write */
	    ppa = get_new_page(conv_ftl, USER_IO);
	    /* update maptbl */
	    set_maptbl_ent(conv_ftl, local_lpn, &ppa);
	    NVMEV_DEBUG("conv_write: got new ppa %lld, ", ppa2pgidx(conv_ftl, &ppa));
	    /* update rmap */
	    set_rmap_ent(conv_ftl, local_lpn, &ppa);

	    mark_page_valid(conv_ftl, &ppa);

	    /* need to advance the write pointer here */
	    advance_write_pointer(conv_ftl, USER_IO);

	    /* Aggregate write io in flash page */
	    if (last_pg_in_wordline(conv_ftl, &ppa)) {
	        swr.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg;
	        swr.ppa = &ppa;
	        nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &swr);
	        nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;

	        enqueue_writeback_io_req(req->sq_id, nsecs_completed, wbuf, spp->pgs_per_oneshotpg * spp->pgsz);
	    }

	    consume_write_credit(conv_ftl);
	    check_and_refill_write_credit(conv_ftl);
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

#ifdef DISCARD_ENABLED

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

//#if (defined ZONE_MAPPING || defined MULTI_PARTITION_FTL)
//	int no_partition;
//#endif

	static int getit = 0;
	uint64_t total_dblk = 0;
	
	//ret->cid = cmd->common.command_id;

	for (i = 0; i < nranges; i ++){
		lba = dsm_range[i].slba;
		//nr_lba = dsm_range[i].nlb + 1; /* zero-based */
		nr_lba = dsm_range[i].nlb; /* zero-based */
		
		start_lpn = lba / spp->secs_per_pg;
		end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;
		NVMEV_DEBUG("conv_discard: start_lpn=%lld, len=%d, end_lpn=%lld", start_lpn, nr_lba, end_lpn);
#ifdef JWDBG_CONV_FTL
		//NVMEV_INFO("conv_discard: start_lpn=%lld, len=%lld, end_lpn=%lld nranges: %lld", start_lpn, nr_lba, end_lpn, nranges);
#endif

#if !(defined ZONE_MAPPING || defined MULTI_PARTITION_FTL)
		if ((end_lpn/nr_parts) >= spp->tt_pgs) {
		    NVMEV_ERROR("conv_discard: lpn passed FTL range(start_lpn=%lld,tt_pgs=%ld)\n", start_lpn, spp->tt_pgs);
		    return false;
		}
#endif

//#if !(defined ZONE_MAPPING || defined MULTI_PARTITION_FTL)
//		/* TODO: Alignment Check for conventional SSD */
//		/* TODO: Sector level bitmap? */
//#else
//		no_partition = NO_LOCAL_PARTITION(start_lpn / nr_parts);
//		NVMEV_ASSERT(no_partition == NO_LOCAL_PARTITION(end_lpn / nr_parts));
//		
//		if (IS_MAIN_PARTITION(no_partition)){
//			start_lpn -= START_OFS_IN_MAIN_PART;
//			end_lpn -= START_OFS_IN_MAIN_PART;
//			total_dblk += (end_lpn - start_lpn + 1);
//			getit = 1;
//		}
//
//		/* Alignment Check */
//		if (IS_MAIN_PARTITION(no_partition)){
//			NVMEV_ASSERT(lba % spp->secs_per_pg == 0);
//			if (nr_lba % spp->secs_per_pg != 0 || lba % spp->secs_per_pg != 0){
//				printk("[JWDBG] %s: nr lba: 0x%llx not aligned!!", __func__, nr_lba);
//			}
//			//NVMEV_ASSERT(nr_lba % spp->secs_per_pg == 0);
//		}
//
//#endif
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
			

		    ppa = get_maptbl_ent(conv_ftl, local_lpn);
			int trial_cnt = 0;
invalidate_page:
		    if (mapped_ppa(&ppa)) {
				
				if ((get_pg(conv_ftl->ssd, &ppa))->status != PG_VALID){
					printk("[JWDBG] %s: ori lpn: 0x%lx lpn: 0x%lx ppa: 0x%llx pg status: %d line: 0x%lx", 
							__func__, ori_lpn, local_lpn*nr_parts, 
							ppa.ppa, get_pg(conv_ftl->ssd, &ppa)->status, 
							get_line(conv_ftl, &ppa));
				}
		        
				mark_page_invalid(conv_ftl, &ppa);

		        set_rmap_ent(conv_ftl, INVALID_LPN, &ppa);
		        NVMEV_DEBUG("conv_write: %lld is invalid, ", ppa2pgidx(conv_ftl, &ppa));
    			
				invalidate_maptbl_ent(conv_ftl, local_lpn);
		    }
//#if (defined ZONE_MAPPING || defined MULTI_PARTITION_FTL)
//		    else if (IS_MAIN_PARTITION(no_partition)){
//				NVMEV_ERROR("conv_discard: discard target lpn 0x%llx must be valid!! slpn: 0x%lx elpn: 0x%lx", lpn, start_lpn, end_lpn);
//		    } else {
//				if (getit) {
//						printk("%s: something can be wrong lpn: 0x%lx ~ 0x%lx !!!!!!!!!!!!!!!!!!!!!!!!!",
//							   	__func__, lpn, end_lpn);
//				}
//				static int cnt_ = 0;
//				if (!IS_META_PARTITION(no_partition)){
//					cnt_ ++;
//					if (cnt_ > 20) {
//						printk("%s: something can be wrong lpn: 0x%lx ~ 0x%lx !!!!!!!!!!!!!!!!!!!!!!!!!",
//							   	__func__, lpn, end_lpn);
//					}
//				}
//
//			}
//#endif
		}
	}
	//printk("%s: total_dblk: %llu nranges: %lld avg len: %llu", 
	//		__func__, total_dblk, nranges, total_dblk / nranges);
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

bool conv_proc_nvme_io_cmd(struct nvmev_ns * ns, struct nvmev_request * req, struct nvmev_result * ret)
{
	struct nvme_command *cmd = req->cmd;

	NVMEV_ASSERT(ns->csi == NVME_CSI_NVM);

	switch(cmd->common.opcode) {
	case nvme_cmd_write:
		if (!conv_write(ns, req, ret))
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
