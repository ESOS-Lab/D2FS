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

#ifndef _NVMEVIRT_CONV_FTL_H
#define _NVMEVIRT_CONV_FTL_H

#include <linux/types.h>
#include "pqueue.h"
#include "ssd_config.h"
#include "ssd.h"

inline int wftl_test_bit(unsigned int nr, char *addr);
inline void wftl_set_bit(unsigned int nr, char *addr);
inline void wftl_clear_bit(unsigned int nr, char *addr);
inline int wftl_test_and_set_bit(unsigned int nr, char *addr);
inline int wftl_test_and_clear_bit(unsigned int nr, char *addr);
inline void wftl_change_bit(unsigned int nr, char *addr);
//inline uint64_t find_next_zero_bit(char *bitmap, uint64_t nbits, uint64_t sidx);


struct convparams {
    uint32_t gc_thres_lines;
    uint32_t gc_thres_lines_high;
    bool enable_gc_delay;

    double op_area_pcent;
    int pba_pcent;    /* (physical space / logical space) * 100*/

#ifdef ZONE_MAPPING
	/* for calculating ppa in zone */
	int bitmask_pg_per_oneshotpg;
	int bitmask_channel;
	int bitmask_lun;
	int bitmask_oneshotpg;
	int remainder_oneshotpg;

	int divider_channel;
	int divider_lun;
	int divider_oneshotpg;
#endif
};

typedef struct line {
    int id;  /* line id, the same as corresponding block id */
    int ipc; /* invalid page count in this line */
    int vpc; /* valid page count in this line */
#ifndef GURANTEE_SEQ_WRITE
    int wpc; /* written page count in this line */
#endif
#ifdef ZONE_MAPPING
	uint64_t start_local_lpn;		/* for zone mapping invalidation during gc */
#endif
    //QTAILQ_ENTRY(line) _entry; /* in either {free,victim,full} list */
	struct list_head entry;
    /* position in the priority queue for victim lines */
    size_t                  pos;
} line;

/* wp: record next write addr */
struct write_pointer {
    struct line *curline;
    uint32_t ch;
    uint32_t lun;
    uint32_t pg;
    uint32_t blk;
    uint32_t pl;
};

struct line_mgmt {
    struct line *lines;

    /* free line list, we only need to maintain a list of blk numbers */
    //QTAILQ_HEAD(free_line_list, line) _free_line_list;
	struct list_head free_line_list;
    pqueue_t *victim_line_pq;
    // //QTAILQ_HEAD(victim_line_list, line) victim_line_list;
	struct list_head full_line_list;
    //QTAILQ_HEAD(full_line_list, line) _full_line_list;

    uint32_t tt_lines;
    uint32_t free_line_cnt;
    uint32_t victim_line_cnt;
    uint32_t full_line_cnt;
};

struct write_flow_control {
    uint32_t write_credits;
    uint32_t credits_to_refill;
};

#ifdef COUPLED_GC
struct window_mgmt {
    /* only for  partitions. for coupled gc and nameless write */
    uint64_t next_local_lpn; /* next local lpn to allocate */
	int	head_idx;			/* head of both active interval and mapping window */
	uint64_t head_zoneno;
	int	tail_idx;			/* tail of active interval */
	uint64_t tail_zoneno;
	uint64_t nzones_per_partition;
	unsigned long * zone_bitmap; /* zone validity bitmap. only used for GC partition */
	uint64_t free_zone;
	uint16_t * remain_cnt_array;
};
#endif

struct conv_ftl {
    struct ssd *ssd;

    struct convparams cp;

#ifdef ZONE_MAPPING
	unsigned long npages_meta;	/* # of lpns in metadata partition */
#ifdef EQUAL_IM_MEM
	unsigned long npages_main;	/* # of lpns in metadata partition */
#endif
	unsigned long nzones_per_partition;	/* # of logical zones in each partition */
    unsigned long nzones_per_gc_partition;	/* # of logical zones in each partition */
#ifdef COUPLED_GC
	unsigned int no_part;				/* ftl number to recover lpn from local lpn */
	struct nvmev_ns *ns;
	struct window_mgmt wm[NO_USER_PARTITION];
#endif
#endif

#ifdef MULTI_PARTITION_FTL
    struct ppa *maptbl[NO_TYPE]; /* page level mapping table */
    struct write_pointer wp[NO_USER_PARTITION];
    struct write_flow_control wfc[NO_USER_PARTITION];
#else
    struct ppa *maptbl; /* page level mapping table */
    struct write_pointer wp;
    struct write_flow_control wfc;
#endif
    struct write_pointer gc_wp;
    uint64_t *rmap;     /* reverse mapptbl, assume it's stored in OOB */
    struct line_mgmt lm;

	int64_t valid_zone_cnt[NO_USER_PARTITION];
	int64_t gc_free_zone_cnt[NO_USER_PARTITION];
#ifdef EQUAL_IM_MEM
	struct ppa *redundant;
#endif
	uint64_t total_valid_zone_cnt;
};


void conv_init_namespace(struct nvmev_ns * ns, uint32_t id, uint64_t size, void * mapped_addr, uint32_t cpu_nr_dispatcher);

bool conv_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret);
bool conv_read(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret);
bool conv_write(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret);
void conv_flush(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret);
#ifdef MIGRATION_IO
void conv_proc_nvme_rev_io_cmd(struct nvmev_ns * ns, struct nvme_rev_completion *cmd);
#endif

#endif
