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

extern struct nvmev_dev *vdev;

inline unsigned long long __get_wallclock(void)
{
	return cpu_clock(vdev->config.cpu_nr_dispatcher);
}

unsigned int total_valid_blks = 0;

#define BUF_CNT_ 10

#ifdef WAF
unsigned long long OS_TimeGetUS( void )
{
    struct timespec64 lTime;
    ktime_get_coarse_real_ts64(&lTime);
    return (lTime.tv_sec * 1000000 + div_u64(lTime.tv_nsec, 1000) );

}
#endif

#ifdef MEM_CALC

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

inline uint64_t find_next_set_bit_(char *bitmap, uint64_t nbits, uint64_t sidx)
{
                uint64_t idx;
                for (idx = sidx; idx < nbits; idx ++) {
                        if (wftl_test_bit(idx, bitmap) == 1) {
                              return idx;
                        }
                }
                return idx;
}



#ifdef COMPACTION_OVERHEAD

#define MAPSEG_LENGTH_DIGIT (20)
#define MAPSEG_BUCKET_DIGIT 8
#define MAPSEG_MAP_SEGMENT_SIZE (1 << MAPSEG_LENGTH_DIGIT)
#define MAPSEG_BUCKET_SIZE (1 << MAPSEG_BUCKET_DIGIT)

#define MAPSEG_VALID_BIT_INDEX(index) (index >> 3)
#define DEADBEEF 0xdeadbeef
#define MAPSEG_INITIAL_BUFFER_SIZE 1

typedef struct _LOGICAL_SLICE_ENTRY {               
#ifdef MEM_CALC_32BIT
    uint32_t virtualSliceAddr;
#else
    uint64_t virtualSliceAddr;
#endif
} LOGICAL_SLICE_ENTRY, *P_LOGICAL_SLICE_ENTRY;      

#ifdef MEM_CALC_32BIT
#define MAP_SZ	4
#else
#define MAP_SZ	8
#endif

typedef struct range_directory {
    uint16_t *bufferIdxs;  // size = mappingSize/unitRangeSize,
                               // use this to find mapping in range buffer.
} RangeDirectory;

typedef struct range_buffer {
    /*struct header {
        unsigned int startLsa : 16;
    };
    struct header header;*/
    LOGICAL_SLICE_ENTRY *entries;  // size = variable
} RangeBuffer;

typedef struct map_segment {
    // metadata
    unsigned int startLsa;
    unsigned int mappingSize : 16;
    unsigned int unitRangeSize : 16;
    int numOfValidMaps : 16;  // number of valid bits, used for efficient
                              // decision of erase
    int numOfWrittenMaps : 16;  // number of valid written Mappings, used for checking
			      // map segment had been fully written. 
    unsigned int validBits[MAPSEG_VALID_BIT_INDEX(MAPSEG_MAP_SEGMENT_SIZE)];
    uint32_t sz;  /* size of map_segment*/
    RangeDirectory rangeDir;
    RangeBuffer *rangeBuffer;

    uint8_t compact_cnt;

    struct zone_node *parent;
} MapSegment, *MapSegment_p;


void free_map_node(MapSegment_p mnode)
{
	vfree(mnode->rangeBuffer->entries);
	kfree(mnode->rangeBuffer);
	kfree(mnode->rangeDir.bufferIdxs);
	vfree(mnode);
}

MapSegment_p compact_map_node(struct ms *ms, int new_mlen, 
								int region_sz, int range_directory_cnt, 
								int range_buf_sz, int new_sofs, 
								int valid_range_cnt, int valid_map_cnt)
//	MapSegment_p old_mnode,
//                                uint32_t first_valid_idx,
//                                uint32_t last_valid_idx,
//                                uint32_t nmaps_in_region,
//                                uint32_t range_directory_sz,
//                                uint32_t bitmap_sz,
//                                uint32_t mapping_table_sz,
//								uint32_t n_active_regions)
{
    int i;
	MapSegment_p mnode;

    mnode = (MapSegment_p) vmalloc(sizeof(MapSegment));
	NVMEV_ASSERT(mnode != NULL);
	mnode->startLsa = DEADBEEF;
	mnode->mappingSize = new_mlen;
    mnode->unitRangeSize = region_sz;
	mnode->numOfValidMaps = DEADBEEF;
	mnode->numOfWrittenMaps = DEADBEEF;
	mnode->sz = DEADBEEF;
	//////////////////////////
	mnode->compact_cnt = DEADBEEF;
	mnode->parent = NULL;
	
	/*This sould be modified when bitmap size becomes variable */
    memset(
    	mnode->validBits, 0,
    	sizeof(unsigned int) * MAPSEG_VALID_BIT_INDEX(MAPSEG_MAP_SEGMENT_SIZE));

    /* copy bitmap  */
	int tmptmp = (new_mlen % 8 > 0) ? new_mlen / 8 + 1 : new_mlen / 8;
	NVMEV_ASSERT(tmptmp <= 
			sizeof(unsigned int) * MAPSEG_VALID_BIT_INDEX(MAPSEG_MAP_SEGMENT_SIZE));
    for(i = 0; i < new_mlen; i += 1){
		wftl_set_bit(i, (char *) mnode->validBits);
    }


	/* initialize rangeDirectory */
    //mapseg_init_range_directory(&mnode->rangeDir, range_directory_sz / sizeof(uint16_t));
	mnode->rangeDir.bufferIdxs = kmalloc(range_directory_cnt * sizeof(uint16_t), GFP_KERNEL);
	NVMEV_ASSERT(mnode->rangeDir.bufferIdxs != NULL);
	/* initialize rangeBuffer */
    //mnode->rangeBuffer =
    //    	mapSegmentBufferAllocator.allocate(MAPSEG_INITIAL_BUFFER_SIZE);
	mnode->rangeBuffer = kmalloc(sizeof(RangeBuffer) * MAPSEG_INITIAL_BUFFER_SIZE, 
									GFP_KERNEL);
	NVMEV_ASSERT(mnode->rangeBuffer != NULL);
	mnode->rangeBuffer->entries = vmalloc(range_buf_sz*2);
	NVMEV_ASSERT(mnode->rangeBuffer->entries != NULL);

    /* Fill range directory and fixed-region mappings */
	
    int prev_dir_idx = -1, cur_dir_idx, cur_dir_eidx, cur_header_idx;
    uint32_t idx = 0, ii;
    bool scan_finished = false;
    uint32_t sidx, eidx,
            n_invalid_mappings, n_total_invalid_mappings,
            sub_region_idx, range_idx, delta_idx, sidx_range_directory;
    sidx = 0;
    LOGICAL_SLICE_ENTRY *range_mappings = mnode->rangeBuffer->entries,
		    //*old_range_mappings = old_mnode->rangeBuffer->entries,
		    *mapping_p;
    uint16_t *range_directory = mnode->rangeDir.bufferIdxs;
		//*old_range_directory = old_mnode->rangeDir.bufferIdxs;

    unsigned int region_cnt = 0;
	unsigned int tmptmptmp = 0;

    /* scan bitmap and fill mappings */
	
    //while((eidx = GET_NEXT_VALID_BIT(old_mnode->validBits, sidx, last_valid_idx, false)) != -1){
    while((eidx = find_next_set_bit_(ms->bitmap, new_sofs + new_mlen, sidx)) < new_sofs + new_mlen){
    		cur_dir_idx = (eidx - new_sofs) / mnode->unitRangeSize;
            if (cur_dir_idx == prev_dir_idx){
                    /* Multiple active region in a single region.
                       This case, invalid mapping should be included in
                       new map node. */
					tmptmptmp = idx;
                    for (ii = 0; ii < eidx - sidx; ii += 1){
						if (idx >= range_buf_sz/MAP_SZ*2) {
							continue;
							//printk("idx: %d sidx: %u eidx: %u ii: %u range_buf_sz: %u MAP_SZ: %u", 
							//		idx, sidx, eidx, ii, range_buf_sz, MAP_SZ);
							//printk("scan finished: %d new_mlen: %d new_sofs: %d region_cnt: %u valid_range_cnt: %d valid_map_cnt: %d", 
							//		scan_finished, new_mlen, new_sofs, region_cnt, 
							//		valid_range_cnt, valid_map_cnt);
						}
						//NVMEV_ASSERT(idx < range_buf_sz/MAP_SZ*2);
						//NVMEV_ASSERT(idx < range_buf_sz/sizeof(LOGICAL_SLICE_ENTRY));
                        (range_mappings[idx++]).virtualSliceAddr = 0xffffffff;
                    }
					//printk("fill rbuf idx:%d ~ %d mapidx:: %d ~ %d fill NULL", tmptmptmp, idx-1, 
					//		sidx, eidx-1);
            } else {
                    cur_header_idx = idx;
                    /* Create new region mapping */
                    /* Set header of new region mapping */
			//NVMEV_ASSERT(idx < range_buf_sz/MAP_SZ*2);
					//NVMEV_ASSERT(idx < range_buf_sz/sizeof(LOGICAL_SLICE_ENTRY));
                    (range_mappings[idx++]).virtualSliceAddr
                            = DEADBEEF + eidx;
                    region_cnt += 1;
					//printk("fill header rbuf idx: %d", idx-1);
            }


            //if ((sidx = GET_NEXT_INVALID_BIT(old_mnode->validBits,
            //                eidx + 1, last_valid_idx, false)) == -1){
            if ((sidx = find_next_zero_bit_(ms->bitmap,
                            new_sofs + new_mlen, eidx + 1)) == new_sofs + new_mlen){
                    /* fill mappings of new region mapping */
                    scan_finished = true;
                    sidx = new_sofs + new_mlen + 1;
            }

            cur_dir_eidx =
                            (sidx-1-new_sofs)/mnode->unitRangeSize;

            /* Fill Range Directory */
            for (ii = cur_dir_idx; ii <= cur_dir_eidx; ii += 1){
				//NVMEV_ASSERT(ii < range_directory_cnt );
                range_directory[ii] = cur_header_idx;

            }
            prev_dir_idx = cur_dir_eidx;

			/* fill mappings of new region mapping */
            //sub_region_idx = eidx / old_mnode->unitRangeSize;
            //range_idx = old_range_directory[sub_region_idx];

            /* access to header of range mapping*/
			/* read start LBA in old noode's header*/
            //mapping_p = &(old_range_mappings[range_idx]);

            //if ((delta_idx = ((old_mnode->startLsa & 0xffffffff) + eidx)
            //			- mapping_p->virtualSliceAddr + 1) <= 0){
            //	return NULL;
            //}

            //for (ii = 0; ii < sidx- eidx; ii += 1){
            //        mapping_p = &(old_range_mappings[range_idx + delta_idx + ii]);
            //        (range_mappings[idx++]).virtualSliceAddr = mapping_p->virtualSliceAddr;
            //}
			int tmp_idx = idx;
			tmptmptmp = idx;
            for (ii = 0; ii < sidx- eidx; ii += 1){
                    //mapping_p = &(old_range_mappings[range_idx + delta_idx + ii]);
					//NVMEV_ASSERT(idx < range_buf_sz/MAP_SZ*3/2);
					if (idx >= range_buf_sz/MAP_SZ*2)
						continue;
					//NVMEV_ASSERT(idx < range_buf_sz/sizeof(LOGICAL_SLICE_ENTRY));
					//if (tmp_idx + sidx - eidx - ii - 1 >= range_buf_sz/MAP_SZ) {
					//	printk("tmp_idx: %d sidx: %u eidx: %u ii: %u range_buf_sz: %u MAP_SZ: %u", 
					//			tmp_idx, sidx, eidx, ii, range_buf_sz, MAP_SZ);
					//	printk("scan finished: %d new_mlen: %d new_sofs: %d region_cnt: %d valid_range_cnt: %d valid_map_cnt: %d", 
					//			scan_finished, new_mlen, new_sofs, 
					//				region_cnt, valid_range_cnt, valid_map_cnt);
					//}
					//NVMEV_ASSERT(tmp_idx + sidx - eidx - ii - 1 - 1 < range_buf_sz/MAP_SZ*3/2);
					if (tmp_idx + sidx - eidx - ii - 1 - 1 >= range_buf_sz/MAP_SZ*2)
						continue;
					//NVMEV_ASSERT(tmp_idx + sidx - eidx - ii - 1 - 1
					//	   	< range_buf_sz/sizeof(LOGICAL_SLICE_ENTRY));
                    (range_mappings[idx++]).virtualSliceAddr
						 = (range_mappings[tmp_idx + sidx - eidx - ii - 1]).virtualSliceAddr;
            }
			//printk("fill 2 rbuf idx: %d ~ %d mapidx:: %d ~ %d", tmptmptmp, idx-1, 
			//				eidx, sidx-1);

            if (scan_finished){
				break;
            }
    }
    mnode->compact_cnt += 1;

	return mnode;
}

uint32_t GET_REMAINING_INVALID_MAPPING(char * bitmap,
                        uint32_t first_valid_idx, uint32_t last_valid_idx,
                        uint32_t nmaps_in_new_region,
                        uint32_t* n_headers)
{
        int prev_region_idx = -1;
        uint32_t sidx, eidx, prev_valid_eidx,
                n_invalid_mappings, n_total_invalid_mappings;
        sidx = first_valid_idx;
        *n_headers = 0;
        /*scan bitmap*/
        //while((eidx = GET_NEXT_VALID_BIT(bitmap, sidx, last_valid_idx, false)) != -1){
        while((eidx = find_next_set_bit_(bitmap, last_valid_idx, sidx)) < last_valid_idx){
                if (eidx / nmaps_in_new_region == prev_region_idx){
                        /* Multiple active region in a single region.
                           This case, invalid mapping should be included in
                           new map node. */
                        n_invalid_mappings = eidx - prev_valid_eidx - 1;
                        n_total_invalid_mappings += n_invalid_mappings;

                } else {
                        *n_headers += 1;
                }

                //if ((sidx = GET_NEXT_INVALID_BIT(bitmap, eidx + 1, last_valid_idx, false))
                //                == -1){
                if ((sidx = find_next_zero_bit_(bitmap, last_valid_idx, eidx + 1))
                                >= last_valid_idx){
                        break;
                }
                prev_region_idx = (sidx-1) / nmaps_in_new_region;
                prev_valid_eidx = sidx-1;
        }
        return n_total_invalid_mappings;
}
#endif


#ifdef COMPACTION_DIRTY_ONLY
void compaction1_integrity(struct nvmev_ns *ns, int i)
{
        struct ms_info *ms_infos = ns->ms_infos;
        int total_sz = 0;
        int total_pt_sz = 0;
        int total_total_MScnt = 0;
        int total_dealloc_ms = 0;
        int total_znode_sz = 0;
        int MSidx, LBA;
	unsigned int total_created_zone_sz = 0;
	unsigned int total_dealloc_zone_sz = 0;
	static int compaction_cnt = 0;

	int sidx, length, total_length, i_;

        if(!ms_infos)
                return;

                struct ms_info *ms_info = &(ms_infos[i]);

                if(ms_info->trimmed_start_MSidx == -1)
			return;

                //printk("%s: [MEM_CALC] partition.%d sMSidx : %d, eMSidx : %d\n", __func__, i, ms_info->trimmed_start_MSidx, ms_info->global_eMSidx);
#ifdef PARTIAL_COMPACTION
		total_length = ms_info->global_eMSidx - ms_info->trimmed_start_MSidx + 1;
		sidx = (ms_info->last_compaction_eidx > ms_info->trimmed_start_MSidx)?
			ms_info->last_compaction_eidx : ms_info->trimmed_start_MSidx;
		//length = min(total_length, 4096*4 * 3/2);
		length = min(total_length, 4096*4 * 5/4);
#else
		length = ms_info->global_eMSidx - ms_info->trimmed_start_MSidx;
		//sidx = ms_info->trimmed_start_MSidx;
		//eidx = ms_info->global_eMSidx;
#endif
                //for(MSidx = sidx; MSidx < eidx; MSidx++){
                for(i_ = 0; i_ < length; i_++){
#ifdef PARTIAL_COMPACTION
			MSidx = i_ + sidx;
			MSidx = ms_info->trimmed_start_MSidx +
				(MSidx - ms_info->trimmed_start_MSidx) % total_length;
			if (MSidx == ms_info->global_eMSidx)
				continue;
#else
			MSidx = i_ + ms_info->trimmed_start_MSidx;
#endif
                        struct ms *ms = &(ms_info->ms[MSidx]);
			if (ms == NULL)
                                continue;
                        if(!ms->is_dirty)
                                continue;
//			printk("%s: partno: %u dirty MSidx: %u", __func__, i, MSidx);
			
                }
}


void compaction1(struct nvmev_ns *ns)
{
//	printk("======================start==========================");
        struct ms_info *ms_infos = ns->ms_infos;
        int total_sz = 0;
        int total_pt_sz = 0;
        int total_total_MScnt = 0;
        int total_dealloc_ms = 0;
        int total_znode_sz = 0;
        int i, MSidx, LBA;
	unsigned int total_created_zone_sz = 0;
	unsigned int total_dealloc_zone_sz = 0;
	int compaction_cnt = 0;
	struct dirty_ms_entry *dme;
	struct list_elem *tmp_elem;
	int sidx, length, total_length, i_;
	int dirty_ms_cnt_total = 0, compaction_ms_cnt_total = 0;

        if(!ms_infos)
                return;

        for(i = 1; i < NO_TYPE; i++){
                struct ms_info *ms_info = &(ms_infos[i]);

                if(ms_info->trimmed_start_MSidx == -1)
                        continue;
//		printk("-----------------partno: %u start------------", i);
//		compaction1_integrity(ns, i);
                //printk("%s: [MEM_CALC] partition.%d sMSidx : %d, eMSidx : %d\n", __func__, i, ms_info->trimmed_start_MSidx, ms_info->global_eMSidx);
		length = ms_info->global_eMSidx - ms_info->trimmed_start_MSidx;
		//sidx = ms_info->trimmed_start_MSidx;
		//eidx = ms_info->global_eMSidx;
                //for(MSidx = sidx; MSidx < eidx; MSidx++){
		while(!list_empty_(&(ms_info->dirty_ms_list))) {
			tmp_elem = list_pop_front(&(ms_info->dirty_ms_list));
			dme = (struct dirty_ms_entry *) list_entry(tmp_elem, struct dirty_ms_entry, list_elem);
			MSidx = dme->ms_idx;
			kfree(dme);
                        struct ms *ms = &(ms_info->ms[MSidx]);
//			printk("%s: partno: %u dirty candi MSidx: %u", __func__, i, MSidx);
			if (ms == NULL){
//				printk("not good.");
                                continue;
			}
                        if(!ms->is_dirty && ms->is_discard){
				//NVMEV_ASSERT(0);
//				printk("SKIP vacantone! MSidx: %u sidx: %u eidx: %u", MSidx, ms_info->trimmed_start_MSidx, ms_info->global_eMSidx);
                                continue;
			}
//			printk("%s: partno: %u dirty MSidx: %u", __func__, i, MSidx);
			dirty_ms_cnt_total ++;

                        int valid_range_cnt = 0;
                        int valid_map_cnt = 0;
                        int new_sofs = MSblks;
                        int new_eofs = 0;
                        int drange_size = MSblks;
                        int cur_drange_size = 0;
                        int new_mlen, usz, newMS_sz, truncatedMS_sz;
                        bool is_past_valid = true;
                        bool is_first_valid = true;

						/* scan and get minimum hole size */
                        for(LBA = 0; LBA < MSblks; LBA++){
                                if(wftl_test_bit(LBA, ms->bitmap)){
                                        valid_map_cnt++;
                                        new_sofs = (new_sofs > LBA) ? LBA : new_sofs;
                                        new_eofs = (new_eofs < LBA) ? LBA : new_eofs;

                                        if(!is_past_valid){
                                                valid_range_cnt++;
                                                drange_size = (!is_first_valid && cur_drange_size >= 64 && cur_drange_size < drange_size) ? cur_drange_size : drange_size;
                                                cur_drange_size = 0;
                                        }
										

                                        is_past_valid = true;
                                        is_first_valid = false;
                                }
                                else{
                                        cur_drange_size++;
                                        is_past_valid = false;
                                }
                        }

                        usz = drange_size; /* region size determined */

                        new_mlen = new_eofs - new_sofs + 1;


#ifdef COMPACTION_OVERHEAD
			uint32_t n_headers = 0;
			//int n_invalid_mappings = 0;
			int n_invalid_mappings = GET_REMAINING_INVALID_MAPPING(ms->bitmap,
								new_sofs, new_eofs, usz, &n_headers);
#endif

			int dir_cnt = 4 * ((new_mlen + usz - 1) / usz); /* range directory cnt*/
			//int range_buf_sz = MAP_SZ * (valid_range_cnt + valid_map_cnt); /* range buf cnt*/
#ifdef COMPACTION_OVERHEAD
			int range_buf_sz = MAP_SZ * (n_headers + valid_map_cnt + n_invalid_mappings); /* range buf cnt*/
#else
			int range_buf_sz = MAP_SZ * (valid_range_cnt + valid_map_cnt); /* range buf cnt*/
#endif
			newMS_sz = range_buf_sz + RANGE_DIR_SZ * dir_cnt + 10;

                        if(ms->size < newMS_sz){
                        	ms->is_dirty = 0;
                                continue;
			}

                        //if(100 - (100 * newMS_sz) / ms->size < 10)
                        //if(100 - (100 * newMS_sz) / ms->size < 80)
                        if(100 - (100 * newMS_sz) / ms->size < 10) {
                        	ms->is_dirty = 0;
                                continue;
			}
			
//			/* calculate correct mem footprint */	
//			n_invalid_mappings = GET_REMAINING_INVALID_MAPPING(ms->bitmap,
//  				new_sofs, new_eofs, usz, &n_headers);
//			range_buf_sz = MAP_SZ * (n_headers + valid_map_cnt + n_invalid_mappings); /* range buf cnt*/
//			newMS_sz = range_buf_sz + RANGE_DIR_SZ * dir_cnt + 10;
//
//                        if(100 - (100 * newMS_sz) / ms->size < 10)
//                                continue;

#ifdef COMPACTION_OVERHEAD
			/* start compaction */
			MapSegment_p mnode = 
				compact_map_node(ms, new_mlen, usz, dir_cnt, range_buf_sz, new_sofs, 
						valid_range_cnt, valid_map_cnt);
			compaction_cnt ++;
			free_map_node(mnode);
#endif

                        ms_info->compacted_size += (ms->size - newMS_sz);
                        ms->size = newMS_sz;

                        truncatedMS_sz = MAP_SZ * (new_mlen + 1) + 4 + 10;
                        ms_info->truncated_memory += (ms->trunc_size - truncatedMS_sz);
                        ms->trunc_size = truncatedMS_sz;

                        ms->is_dirty = 0;
                }

                int total_MScnt = ms_info->global_eMSidx - ms_info->trimmed_start_MSidx + 1;
		unsigned int created_znodes = ms_info->global_eMSidx / MNODES_PER_ZNODE;
                int dealloc_ms = ms_info->dealloc_ms - ms_info->trimmed_start_MSidx;
		unsigned int trimmed_zcnt = ms_info->trimmed_start_MSidx / MNODES_PER_ZNODE;
		unsigned int dealloc_znodes = ms_info->dealloc_znode - trimmed_zcnt;
		unsigned valid_znode = (total_MScnt / MNODES_PER_ZNODE - dealloc_znodes);
		unsigned int znode_sz =  valid_znode * ZNODE_SIZE;
		unsigned int root_sz = total_MScnt / MNODES_PER_ZNODE * MAP_SZ;
                int sz = (znode_sz + root_sz + (total_MScnt - dealloc_ms) * default_MS_sz - ms_info->compacted_size) /1024/1024;
                int pt_sz = (znode_sz + root_sz + (total_MScnt - dealloc_ms) * default_MS_sz) /1024/1024;

                total_sz += sz;
                total_pt_sz += pt_sz;
                total_total_MScnt += total_MScnt;
                total_dealloc_ms += dealloc_ms;
				total_znode_sz += znode_sz/1024;
				total_created_zone_sz += created_znodes * ZNODE_SIZE / 1024;
				total_dealloc_zone_sz += ms_info->dealloc_znode * ZNODE_SIZE / 1024;

                //printk("%s: [MEM_CALC] part%d IM: %d PT: %d interval: %d MB znode: %u %u KB v/i %u / %u KBi written: %u MB dealloc_ms: %d MB compaction !!\n", 
				//		__func__, i, sz, pt_sz, total_MScnt*16, valid_znode, znode_sz/1024, 
				//		created_znodes * ZNODE_SIZE / 1024, 
				//		ms_info->dealloc_znode * ZNODE_SIZE / 1024, ms_info->global_eMSidx * 16, 
				//	   	dealloc_ms);

//		printk("-----------------partno: %u end------------", i);
        }
        printk("Memory Footprint: Interval_Mapping: %d PT: %d interval: %d MB znode: %u KB v/i: %u / %u KB dealloc_ms: %d MB dirty_ms: %u (%u MB) compact_ms: %u (%u MB) \n", 
				 total_sz, total_pt_sz, total_total_MScnt*16, 
				total_znode_sz, 
				total_created_zone_sz, total_dealloc_zone_sz,
				total_dealloc_ms, dirty_ms_cnt_total, 
				dirty_ms_cnt_total*MSblks*4/1024, compaction_cnt, compaction_cnt*MSblks*4/1024 );
//	printk("======================end==========================");
}
#else
void compaction1(struct nvmev_ns *ns)
{
        struct ms_info *ms_infos = ns->ms_infos;
        int total_sz = 0;
        int total_pt_sz = 0;
        int total_total_MScnt = 0;
        int total_dealloc_ms = 0;
        int total_znode_sz = 0;
        int i, MSidx, LBA;
	unsigned int total_created_zone_sz = 0;
	unsigned int total_dealloc_zone_sz = 0;
	static int compaction_cnt = 0;

	int sidx, length, total_length, i_;

        if(!ms_infos)
                return;

        for(i = 1; i < NO_TYPE; i++){
                struct ms_info *ms_info = &(ms_infos[i]);

                if(ms_info->trimmed_start_MSidx == -1)
                        continue;

                //printk("%s: [MEM_CALC] partition.%d sMSidx : %d, eMSidx : %d\n", __func__, i, ms_info->trimmed_start_MSidx, ms_info->global_eMSidx);
#ifdef PARTIAL_COMPACTION
		total_length = ms_info->global_eMSidx - ms_info->trimmed_start_MSidx + 1;
		sidx = (ms_info->last_compaction_eidx > ms_info->trimmed_start_MSidx)?
			ms_info->last_compaction_eidx : ms_info->trimmed_start_MSidx;
		//length = min(total_length, 4096*4 * 3/2);
		length = min(total_length, 4096*4 * 5/4);
#else
		length = ms_info->global_eMSidx - ms_info->trimmed_start_MSidx;
		//sidx = ms_info->trimmed_start_MSidx;
		//eidx = ms_info->global_eMSidx;
#endif
                //for(MSidx = sidx; MSidx < eidx; MSidx++){
                for(i_ = 0; i_ < length; i_++){
#ifdef PARTIAL_COMPACTION
			MSidx = i_ + sidx;
			MSidx = ms_info->trimmed_start_MSidx +
				(MSidx - ms_info->trimmed_start_MSidx) % total_length;
			if (MSidx == ms_info->global_eMSidx)
				continue;
#else
			MSidx = i_ + ms_info->trimmed_start_MSidx;
#endif
                        struct ms *ms = &(ms_info->ms[MSidx]);
			if (ms == NULL)
                                continue;
                        if(!ms->is_dirty)
                                continue;

                        int valid_range_cnt = 0;
                        int valid_map_cnt = 0;
                        int new_sofs = MSblks;
                        int new_eofs = 0;
                        int drange_size = MSblks;
                        int cur_drange_size = 0;
                        int new_mlen, usz, newMS_sz, truncatedMS_sz;
                        bool is_past_valid = true;
                        bool is_first_valid = true;

						/* scan and get minimum hole size */
                        for(LBA = 0; LBA < MSblks; LBA++){
                                if(wftl_test_bit(LBA, ms->bitmap)){
                                        valid_map_cnt++;
                                        new_sofs = (new_sofs > LBA) ? LBA : new_sofs;
                                        new_eofs = (new_eofs < LBA) ? LBA : new_eofs;

                                        if(!is_past_valid){
                                                valid_range_cnt++;
                                                drange_size = (!is_first_valid && cur_drange_size >= 64 && cur_drange_size < drange_size) ? cur_drange_size : drange_size;
                                                cur_drange_size = 0;
                                        }
										

                                        is_past_valid = true;
                                        is_first_valid = false;
                                }
                                else{
                                        cur_drange_size++;
                                        is_past_valid = false;
                                }
                        }

                        usz = drange_size; /* region size determined */

                        new_mlen = new_eofs - new_sofs + 1;


#ifdef COMPACTION_OVERHEAD
			uint32_t n_headers = 0;
			//int n_invalid_mappings = 0;
			//int n_invalid_mappings = GET_REMAINING_INVALID_MAPPING(ms->bitmap,
			//					new_sofs, new_eofs, usz, &n_headers);
			int n_invalid_mappings = GET_REMAINING_INVALID_MAPPING(ms->bitmap,
								0, MSblks, usz, &n_headers);
#endif

			int dir_cnt = 4 * ((new_mlen + usz - 1) / usz); /* range directory cnt*/
			//int range_buf_sz = MAP_SZ * (valid_range_cnt + valid_map_cnt); /* range buf cnt*/
#ifdef COMPACTION_OVERHEAD
			int range_buf_sz = MAP_SZ * (n_headers + valid_map_cnt + n_invalid_mappings); /* range buf cnt*/
#else
			int range_buf_sz = MAP_SZ * (valid_range_cnt + valid_map_cnt); /* range buf cnt*/
#endif
			newMS_sz = range_buf_sz + RANGE_DIR_SZ * dir_cnt + 10;

                        if(ms->size < newMS_sz)
                                continue;

                        //if(100 - (100 * newMS_sz) / ms->size < 10)
                        //if(100 - (100 * newMS_sz) / ms->size < 80)
                        if(100 - (100 * newMS_sz) / ms->size < 10)
                                continue;
			
//			/* calculate correct mem footprint */	
//			n_invalid_mappings = GET_REMAINING_INVALID_MAPPING(ms->bitmap,
//  				new_sofs, new_eofs, usz, &n_headers);
//			range_buf_sz = MAP_SZ * (n_headers + valid_map_cnt + n_invalid_mappings); /* range buf cnt*/
//			newMS_sz = range_buf_sz + RANGE_DIR_SZ * dir_cnt + 10;
//
//                        if(100 - (100 * newMS_sz) / ms->size < 10)
//                                continue;

#ifdef COMPACTION_OVERHEAD
			/* start compaction */
			MapSegment_p mnode = 
				compact_map_node(ms, new_mlen, usz, dir_cnt, range_buf_sz, new_sofs, 
						valid_range_cnt, valid_map_cnt);
			compaction_cnt ++;
			free_map_node(mnode);
#endif

                        ms_info->compacted_size += (ms->size - newMS_sz);
                        ms->size = newMS_sz;

                        truncatedMS_sz = MAP_SZ * (new_mlen + 1) + 4 + 10;
                        ms_info->truncated_memory += (ms->trunc_size - truncatedMS_sz);
                        ms->trunc_size = truncatedMS_sz;

                        ms->is_dirty = 0;
                }
#ifdef PARTIAL_COMPACTION
		MSidx = i_ + sidx;
		MSidx = ms_info->trimmed_start_MSidx +
			(MSidx - ms_info->trimmed_start_MSidx) % total_length;
		ms_info->last_compaction_eidx = MSidx;
#endif

                int total_MScnt = ms_info->global_eMSidx - ms_info->trimmed_start_MSidx + 1;
		unsigned int created_znodes = ms_info->global_eMSidx / MNODES_PER_ZNODE;
                int dealloc_ms = ms_info->dealloc_ms - ms_info->trimmed_start_MSidx;
		unsigned int trimmed_zcnt = ms_info->trimmed_start_MSidx / MNODES_PER_ZNODE;
		unsigned int dealloc_znodes = ms_info->dealloc_znode - trimmed_zcnt;
		unsigned valid_znode = (total_MScnt / MNODES_PER_ZNODE - dealloc_znodes);
		unsigned int znode_sz =  valid_znode * ZNODE_SIZE;
		unsigned int root_sz = total_MScnt / MNODES_PER_ZNODE * MAP_SZ;
                int sz = (znode_sz + root_sz + (total_MScnt - dealloc_ms) * default_MS_sz - ms_info->compacted_size) /1024/1024;
                int pt_sz = (znode_sz + root_sz + (total_MScnt - dealloc_ms) * default_MS_sz) /1024/1024;

                total_sz += sz;
                total_pt_sz += pt_sz;
                total_total_MScnt += total_MScnt;
                total_dealloc_ms += dealloc_ms;
				total_znode_sz += znode_sz/1024;
				total_created_zone_sz += created_znodes * ZNODE_SIZE / 1024;
				total_dealloc_zone_sz += ms_info->dealloc_znode * ZNODE_SIZE / 1024;

                //printk("%s: [MEM_CALC] part%d IM: %d PT: %d interval: %d MB znode: %u %u KB v/i %u / %u KBi written: %u MB dealloc_ms: %d MB compaction !!\n", 
				//		__func__, i, sz, pt_sz, total_MScnt*16, valid_znode, znode_sz/1024, 
				//		created_znodes * ZNODE_SIZE / 1024, 
				//		ms_info->dealloc_znode * ZNODE_SIZE / 1024, ms_info->global_eMSidx * 16, 
				//	   	dealloc_ms);
        }
        printk("Memory Footprint: Interval_Mapping: %d PT: %d interval: %d MB znode: %u KB v/i: %u / %u KB dealloc_ms: %d MB!!\n", 
				__func__, total_sz, total_pt_sz, total_total_MScnt*16, 
				total_znode_sz, 
				total_created_zone_sz, total_dealloc_zone_sz,
				total_dealloc_ms);
}
#endif

void vacancy_check(struct ms_info *ms_info, struct ms *ms, int MSidx, bool is_last)
{
        int i;

        if(ms->valid_cnt == 0){
                ms_info->dealloc_ms++;
                ms_info->compacted_size -= (default_MS_sz - ms->size);
                ms_info->truncated_memory -= (default_MS_sz - ms->trunc_size);

                vfree(ms->bitmap);
                ms->is_alloc = 0;
                ms->is_discard = 1;
                ms->is_dirty = 0;
		//printk("%s: MSidx: %lu becomes vacant.  ms_info: %p", __func__, MSidx, ms_info);

                if(ms_info->trimmed_start_MSidx == MSidx){
                        ms_info->trimmed_start_MSidx++;

                        if(is_last){
                                for(i = ms_info->trimmed_start_MSidx; i < ms_info->global_eMSidx; i++){
                                        if(!ms_info->ms[i].is_discard)
                                                break;
                                        ms_info->trimmed_start_MSidx++;
                                }
                        }
                }
				ms_info->znode_cnt[MSidx/MNODES_PER_ZNODE] ++;
				if (ms_info->znode_cnt[MSidx/MNODES_PER_ZNODE] == MNODES_PER_ZNODE)
					ms_info->dealloc_znode ++;
				else if (ms_info->znode_cnt[MSidx/MNODES_PER_ZNODE] > MNODES_PER_ZNODE) {
					printk("%s: zcnt: %u zidx: %u", __func__, 
							ms_info->znode_cnt[MSidx/MNODES_PER_ZNODE], 
							MSidx/MNODES_PER_ZNODE);
					NVMEV_ASSERT(0);
				}

        }
}

void punch_bitmap(struct ms_info *ms_info, uint64_t sLBA, uint64_t eLBA, int no_partition)
{
        uint64_t global_sLBA = ms_info->global_sLBA;
        uint64_t local_sLBA = sLBA - global_sLBA;
        uint64_t local_eLBA = eLBA - global_sLBA;
        int sMSidx = local_sLBA / MSblks;
        int eMSidx = local_eLBA / MSblks;
        int cur_MSidx, cur_local_LBA;

        //printk("%s: MSidx: %lu ~ %lu local_LBA: 0x%llx ~ 0x%llx global_LBA: 0x%llx ~ 0x%llx interval_sLBA: 0x%llx\n",
	//__func__, sMSidx, eMSidx, local_sLBA, local_eLBA, sLBA, eLBA, global_sLBA);

        for(cur_MSidx = sMSidx; cur_MSidx < eMSidx + 1; cur_MSidx++){
                struct ms *ms = &(ms_info->ms[cur_MSidx]);

                if(!ms->is_alloc && !ms->is_discard){
                        ms->size = default_MS_sz;
                        ms->trunc_size = default_MS_sz;
                        ms->valid_cnt = MSblks;

                        ms->bitmap = (char *) vmalloc(MSblks/8);
                        memset(ms->bitmap, 0xff, MSblks/8);

                        ms->is_alloc = 1;
                }

                int sofs = (cur_MSidx == sMSidx) ? local_sLBA % MSblks : 0;
                int eofs = (cur_MSidx == eMSidx) ? local_eLBA % MSblks + 1 : MSblks;
                int len = eofs - sofs;

                for(cur_local_LBA = sofs; cur_local_LBA < eofs; cur_local_LBA++)
                        wftl_clear_bit(cur_local_LBA, ms->bitmap);

                ms->valid_cnt -= len;

                if(ms->valid_cnt < 0)
                        printk("%s: [MEM_CALC] error\n", __func__);
#ifdef COMPACTION_DIRTY_ONLY
		if (!ms->is_dirty) {
			ms_info->n_dirty_ms ++;
			struct dirty_ms_entry *dme;
			if ((dme = (struct dirty_ms_entry *) 
				kmalloc(sizeof(struct dirty_ms_entry), GFP_KERNEL)) == NULL)
				NVMEV_ASSERT(0);
			dme->ms_idx = cur_MSidx;
			list_push_back(&ms_info->dirty_ms_list, &dme->list_elem);
			//printk("%s: MSidx: %lu becomes dirty partno: %d ms_info: %p", __func__, cur_MSidx, no_partition, ms_info);
		}
#endif
                ms->is_dirty = 1;
                vacancy_check(ms_info, ms, cur_MSidx, (cur_MSidx == eMSidx));
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

#ifndef MULTI_PARTITION_FTL
static void forground_gc(struct conv_ftl *conv_ftl);
#elif defined COUPLED_GC_MTL
static void forground_gc(struct conv_ftl *conv_ftl, int no_partition, struct nvmev_result *ret);
#else
static void forground_gc(struct conv_ftl *conv_ftl, int no_partition);
#endif

//static inline bool out_of_partition(struct conv_ftl *conv_ftl, uint64_t local_lpn)
//{
//	static int print = 1;
//	//static int cnt = 0;
//	int no_partition = NO_LOCAL_PARTITION(local_lpn);
//	struct window_mgmt *wm = &conv_ftl->wm[no_partition];
//
//	/* due to coupled gc */
//	if (behind_active_interval(conv_ftl, local_lpn))
//		return true;
//#ifdef COUPLED_GC
//	uint64_t logical_zoneno = get_relational_zoneno(conv_ftl, local_lpn);
//#else
//	uint64_t logical_zoneno = get_logical_zoneno(conv_ftl, local_lpn);
//#endif
//
//	//if (cnt % 100000 == 0) {
//	////if (conv_ftl->no_part != 0) {
//	//	printk("%s: n_ftl: %u head zoneno: %llu zone no: %llu nzones_per_partitoin: %llu pgs_per_zone: %llu", 
//	//		__func__, conv_ftl->no_part, conv_ftl->wm[no_partition].head_zoneno, 
//	//		NO_ZONE(conv_ftl, local_lpn), conv_ftl->nzones_per_partition,
//	//		conv_ftl->ssd->sp.pgs_per_line);
//	//}
//	//cnt ++;
//
//	if (!(logical_zoneno < wm->nzones_per_partition)){
//		if (print){
//			NVMEV_INFO("[JWDBG] %s: local_lpn: 0x%llx logical_zoneno: %llu nzones_per_partition: %lu pgsPline: %lu PSA: 0x%llx\n",
//					__func__, local_lpn, logical_zoneno, wm->nzones_per_partition,
//					conv_ftl->ssd->sp.pgs_per_line, PARTITION_START_ADDR(local_lpn));
//		}
//		print = 0;
//		return true;
//	}
//
//	return false;
//}

static inline bool out_of_meta_partition(struct conv_ftl *conv_ftl, uint64_t local_lpn)
{
    return (OFFSET_LOCAL_META_PARTITION(local_lpn) >= conv_ftl->npages_meta);
}

static inline struct ppa fake_ppa(void)
{
    struct ppa fake_ppa;
    fake_ppa.ppa = UNMAPPED_PPA;
    return fake_ppa;
}

static inline struct ppa get_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t local_lpn)
{
	unsigned int no_partition = NO_LOCAL_PARTITION(local_lpn);
	if (IS_META_PARTITION(no_partition)){
    	if (out_of_meta_partition(conv_ftl, local_lpn)){
    	    /* to handle with device mount and file system mount. */
    	    return fake_ppa();
    	}
	}

	return conv_ftl->maptbl[NO_LOCAL_PARTITION(local_lpn)][OFFSET_LOCAL_META_PARTITION(local_lpn)];
}

static inline void set_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t local_lpn, struct ppa *ppa)
{
#ifndef MULTI_PARTITION_FTL
	NVMEV_ASSERT(local_lpn < conv_ftl->ssd->sp.tt_pgs);
	conv_ftl->maptbl[local_lpn] = *ppa;
#else
	conv_ftl->maptbl[NO_LOCAL_PARTITION(local_lpn)][OFFSET_LOCAL_META_PARTITION(local_lpn)] = *ppa;
#endif
}

static inline void invalidate_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t local_lpn)
{
#ifndef MULTI_PARTITION_FTL
	NVMEV_ASSERT(local_lpn < conv_ftl->ssd->sp.tt_pgs);
	conv_ftl->maptbl[local_lpn].ppa = INVALID_PPA;
#else
	conv_ftl->maptbl[NO_LOCAL_PARTITION(local_lpn)][OFFSET_LOCAL_META_PARTITION(local_lpn)].ppa 
		= INVALID_PPA;
#endif
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

#ifndef MULTI_PARTITION_FTL
static inline void consume_write_credit(struct conv_ftl *conv_ftl)
#else
static inline void consume_write_credit(struct conv_ftl *conv_ftl, int no_partition)
#endif
{
#ifndef MULTI_WP
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
#ifndef MULTI_WP
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

#ifndef MULTI_WP
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
#ifdef MULTI_GC_WP
	    wpp = conv_ftl->gc_wp;
	    n_partitions = NO_GC_WP; /* gc partition */
#else
	    wpp = &conv_ftl->gc_wp;
	    n_partitions = 1; /* gc partition */
#endif
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
#ifndef MULTI_WP
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
	if (lm->free_line_cnt - 1 < 1) {
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
static void advance_write_pointer(struct conv_ftl *conv_ftl, uint32_t io_type, int no_partition)
#endif
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct write_pointer *wpp;
#ifndef MULTI_WP
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
#ifdef MULTI_GC_WP
		if (IS_NODE_PARTITION(no_partition))
			wpp = &conv_ftl->gc_wp[NODE_GC_WP];
		else if (IS_DATA_PARTITION(no_partition))
			wpp = &conv_ftl->gc_wp[DATA_GC_WP];
		else {
			NVMEV_ASSERT(IS_META_PARTITION(no_partition));
			wpp = &conv_ftl->gc_wp[META_GC_WP];
		}
#else
	    wpp = &conv_ftl->gc_wp;
#endif
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

#ifndef MULTI_PARTITION_FTL
static struct ppa get_new_page(struct conv_ftl *conv_ftl, uint32_t io_type)
#else
static struct ppa get_new_page(struct conv_ftl *conv_ftl, uint32_t io_type, int no_partition)
#endif
{
	struct write_pointer *wpp;
	struct ppa ppa;

#ifndef MULTI_WP
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
#ifdef MULTI_GC_WP
		if (IS_NODE_PARTITION(no_partition))
			wpp = &conv_ftl->gc_wp[NODE_GC_WP];
		else if (IS_DATA_PARTITION(no_partition))
			wpp = &conv_ftl->gc_wp[DATA_GC_WP];
		else {
			NVMEV_ASSERT(IS_META_PARTITION(no_partition));
			wpp = &conv_ftl->gc_wp[META_GC_WP];
		}
#else
	    wpp = &conv_ftl->gc_wp;
#endif
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
	/* metadata partition (page mapping) */
	unsigned long meta_pgs = conv_ftl->npages_meta;
	unsigned long main_pgs = conv_ftl->npages_main;
	conv_ftl->maptbl[META_PARTITION] = vmalloc(sizeof(struct ppa) * meta_pgs);
	for (i = 0; i < meta_pgs; i++) {
	    conv_ftl->maptbl[META_PARTITION][i].ppa = UNMAPPED_PPA;
	}
	for (no_type = META_PARTITION + 1; no_type < NO_TYPE; no_type ++){
		
		unsigned int main_pgs_tmp = main_pgs;
		if (no_type == COLD_DATA_PARTITION){
			conv_ftl->maptbl[no_type] = NULL;
			continue;
		} else if (no_type == COLD_NODE_PARTITION) {
			main_pgs_tmp = main_pgs_tmp / 3 - main_pgs_tmp / 18;
		} else if (no_type == HOT_NODE_PARTITION) { 
				//|| no_type == HOT_DATA_PARTITION) {
			main_pgs_tmp = main_pgs_tmp / 3 - main_pgs_tmp / 18;
		} else if (no_type == WARM_NODE_PARTITION) { 
			main_pgs_tmp = main_pgs_tmp / 3 - main_pgs_tmp / 18;
		} else if (no_type == WARM_NODE_PARTITION) { 
			main_pgs_tmp = main_pgs_tmp *7/6;
		}

		conv_ftl->maptbl[no_type] = vmalloc(sizeof(struct ppa) * main_pgs_tmp);
		for (i = 0; i < main_pgs_tmp; i++) {
		    conv_ftl->maptbl[no_type][i].ppa = UNMAPPED_PPA;
		}
	}
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

#ifdef COUPLED_GC
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
		printk("%s type: %d head_zoneno: %llu tail_zoneno: %llu next_local_lpn: %llu head_idx: %d tail_idx: %d nzones_per_partition: %lu", 
				__func__, i, conv_ftl->wm[i].head_zoneno, conv_ftl->wm[i].tail_zoneno, 
		  		local_start_addr, conv_ftl->wm[i].head_idx, conv_ftl->wm[i].tail_idx, 
				conv_ftl->wm[i].nzones_per_partition);
#ifdef COUPLED_GC_PRINT
		printk("%s type: %d head_zoneno: %llu tail_zoneno: %llu next_local_lpn: %llu head_idx: %d tail_idx: %d", 
				__func__, i, conv_ftl->wm[i].head_zoneno, conv_ftl->wm[i].tail_zoneno, 
		  		local_start_addr, conv_ftl->wm[i].head_idx, conv_ftl->wm[i].tail_idx);
#endif
		if (IS_GC_PARTITION(i)) {
			wftl_set_bit(get_zone_idx(conv_ftl, conv_ftl->wm[i].next_local_lpn), 
					(char *) conv_ftl->wm[i].zone_bitmap);
			conv_ftl->wm[i].free_zone --;
		}
	}
}
#endif

static void conv_init_ftl(struct conv_ftl *conv_ftl, struct convparams *cpp, struct ssd *ssd, uint32_t no_part, struct nvmev_ns *ns)
{
	/*copy convparams*/
	conv_ftl->cp = *cpp;

	conv_ftl->ssd = ssd;

	conv_ftl->ns = ns;
#ifdef COUPLED_GC
	conv_ftl->no_part = no_part;
	printk("%s: conv ftl: no_part: %u", __func__, conv_ftl->no_part);
#endif

	conv_ftl->npages_meta = NPAGES_META(ssd->sp);
	conv_ftl->npages_main = NPAGES_MAIN(ssd->sp);
#ifdef ZONE_MAPPING
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

#ifdef COUPLED_GC
	init_window_mgmt(conv_ftl);
#endif
	//int i;
	//for (i = 0; i < NO_USER_PARTITION; i ++) {
	//	conv_ftl->valid_zone_cnt[i] = 0;
	//	conv_ftl->gc_free_zone_cnt[i] = 0;
	//}
	
	NVMEV_INFO("Init FTL Instance with %d channels(%ld pages)\n",  conv_ftl->ssd->sp.nchs, conv_ftl->ssd->sp.tt_pgs);

	return;
}

#ifndef ZONE_MAPPING
static void conv_init_params(struct convparams *cpp)
#else
static void conv_init_params(struct convparams *cpp, struct ssdparams *spp)
#endif
{
	cpp->op_area_pcent = OP_AREA_PERCENT;
//#ifdef COUPLED_GC
//	cpp->gc_thres_lines = 5; /* Need only two lines.(host write, gc)*/
//	cpp->gc_thres_lines_high = 5; /* Need only two lines.(host write, gc)*/
//#else
#ifdef MULTI_WP
	cpp->gc_thres_lines = 10; /* Need only two lines.(host write, gc)*/
	cpp->gc_thres_lines_high = 10; /* Need only two lines.(host write, gc)*/
#else
	cpp->gc_thres_lines = 2; /* Need only two lines.(host write, gc)*/
	cpp->gc_thres_lines_high = 2; /* Need only two lines.(host write, gc)*/
#endif
//#endif
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
	uint64_t ii, n_mtl_zones, n_mtl_meta_zones, 
			 meta_window_size;
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


    for (i = 0; i < NO_TYPE; i++){
		/* JW: init ith mtls */
		/* TODO: need to redesign for meta mtl */
		if (IS_META_PARTITION(i)) {
			if ((ns->mtls[i] = kmalloc(sizeof(struct mtl_zone_entry *) * n_mtl_meta_zones , GFP_KERNEL)) == NULL)
				NVMEV_ERROR("[JWDBG] kmalloc return NULL. %lld window chunk set sz: %lldKB\n", 
								i, n_mtl_zones * sizeof(void *)/1024 );

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

		} else if (IS_MAIN_PARTITION(i)) {
			unsigned int n_mtl_zones_tmp = n_mtl_zones;
			if (i == COLD_DATA_PARTITION){
				ns->mtls[i] = NULL;
				continue;
			} else if (i == COLD_NODE_PARTITION) {
				n_mtl_zones_tmp = n_mtl_zones / 3- n_mtl_zones / 18 ;
			} else if (i == HOT_NODE_PARTITION) {
				n_mtl_zones_tmp = n_mtl_zones / 3- n_mtl_zones / 18 ;
			} else if (i == WARM_NODE_PARTITION) {
				n_mtl_zones_tmp = n_mtl_zones / 3- n_mtl_zones / 18 ;
			} else if (i == HOT_DATA_PARTITION) {
				n_mtl_zones_tmp = n_mtl_zones *7/6;
			}
			
			if ((ns->mtls[i] = kmalloc(sizeof(struct mtl_zone_entry *) * n_mtl_zones_tmp, GFP_KERNEL)) == NULL)
				NVMEV_ERROR("[JWDBG] kmalloc return NULL. %lld window chunk set sz: %lldKB\n", 
								i, n_mtl_zones_tmp * sizeof(void *)/1024 );

			/* JW: init mtls in unit of MTL_ZONE_SIZE */
			for (ii = 0; ii < n_mtl_zones_tmp; ii++){
    			if ((ns->mtls[i][ii] = kmalloc(sizeof(struct mtl_zone_entry), GFP_KERNEL)) == NULL)
					NVMEV_ERROR("[JWDBG] kmalloc return NULL. %lld sz: %ldKB\n", i, sizeof(struct mtl_zone_entry)/1024);
				struct mtl_zone_entry *__mtl;
				__mtl =  ns->mtls[i][ii];
				init_mtl(ns->mtls[i][ii]);
				//printk("%s: init_mtl type: %llu mtl_zoneno: %llu mtl: %p nr_inv_pgs: %u", 
				//		__func__, i, ii, ns->mtls[i][ii], __mtl->zone_info.nr_inv_pgs);
			}

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
	ns->last_compaction_t = 0;
	ns->write_volume_host = 0;
	ns->write_volume_gc = 0;
	ns->total_write_volume_host = 0;
	ns->total_write_volume_gc = 0;
#endif
#ifdef MG_CMD_CNT
	ns->mg_cmd_cnt = 0;
	ns->total_mg_cmd_cnt = 0;
#endif
#ifdef MEM_CALC
        if((ns->ms_infos = kzalloc(sizeof(struct ms_info) * NO_TYPE, GFP_KERNEL)) == NULL)
                NVMEV_ERROR("[SJDBG] kmalloc return NULL.");

        struct ms_info *ms_infos = ns->ms_infos;
        unsigned int ms_num = (NPAGES_MAIN(ssd->sp) + spp.pgs_per_blk - 1) / spp.pgs_per_blk / 8 * 2;
	printk("%s: compaction trace range: %u GB", __func__, ms_num*16/1024);
	unsigned int zone_node_cnt = ms_num / MNODES_PER_ZNODE;
        int no_type;
        for(no_type = 1; no_type < NO_TYPE; no_type ++){
                ms_infos[no_type].global_sLBA = 0xffffffffffffffff;
                ms_infos[no_type].global_sMSidx = 0;
                ms_infos[no_type].trimmed_start_MSidx = -1;
#ifdef PARTIAL_COMPACTION
		ms_infos[no_type].last_compaction_sidx = 0;
		ms_infos[no_type].last_compaction_eidx = 0;
#endif
                if( (ms_infos[no_type].ms = vzalloc(sizeof(struct ms) * ms_num)) == NULL)
                        NVMEV_ERROR("[SJDBG] vzalloc returun NULL");
                if( (ms_infos[no_type].znode_cnt = vzalloc(sizeof(uint16_t) * zone_node_cnt)) == NULL)
                        NVMEV_ERROR("[SJDBG] vzalloc returun NULL");

#ifdef COMPACTION_DIRTY_ONLY
		list_init(&ms_infos[no_type].dirty_ms_list);
		ms_infos[no_type].n_dirty_ms = 0;
#endif
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

	total_valid_blks -= 1;

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
	total_valid_blks += 1;
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

static uint64_t __gc_write_meta_page(struct conv_ftl *conv_ftl, struct ppa *old_ppa)
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

	total_valid_blks -= 1;

	/* need to advance the write pointer here */
	advance_write_pointer(conv_ftl, GC_IO, no_partition);

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

	__gc_write_meta_page(conv_ftl, old_ppa);
	
	return 0;
}

//#else
///* move valid page data (already in DRAM) from victim line to a new page */
//static uint64_t gc_write_page(struct conv_ftl *conv_ftl, struct ppa *old_ppa)
//{
//	struct ssdparams *spp = &conv_ftl->ssd->sp;
//	struct convparams *cpp = &conv_ftl->cp;
//	struct ppa new_ppa;
//	uint64_t lpn = get_rmap_ent(conv_ftl, old_ppa);
//#ifdef MULTI_PARTITION_FTL
//	int no_partition = NO_LOCAL_PARTITION(lpn);
//#endif
//
//	NVMEV_ASSERT(valid_lpn(conv_ftl, lpn));
//#ifndef MULTI_PARTITION_FTL
//	new_ppa = get_new_page(conv_ftl, GC_IO);
//#else
//	new_ppa = get_new_page(conv_ftl, GC_IO, no_partition);
//#endif
//	/* update maptbl */
//	set_maptbl_ent(conv_ftl, lpn, &new_ppa);
//
//	/* update rmap */
//	set_rmap_ent(conv_ftl, lpn, &new_ppa);
//
//	mark_page_valid(conv_ftl, &new_ppa);
//
//#ifdef SHIVAL2
//	total_valid_blks -= 1;
//#endif
//
//	/* need to advance the write pointer here */
//#ifndef MULTI_PARTITION_FTL
//	advance_write_pointer(conv_ftl, GC_IO);
//#else
//#ifdef GURANTEE_SEQ_WRITE
//	advance_write_pointer(conv_ftl, GC_IO, no_partition);
//#else
//	struct ppa *trash_ppa;
//	advance_write_pointer(conv_ftl, GC_IO, no_partition, trash_ppa);
//#endif
//#endif
//
//	if (cpp->enable_gc_delay) {
//	    struct nand_cmd gcw;
//	    gcw.type = GC_IO;
//	    gcw.cmd = NAND_NOP;
//	    gcw.stime = 0;
//	    gcw.interleave_pci_dma = false;
//	    gcw.ppa = &new_ppa;
//	    if (last_pg_in_wordline(conv_ftl, &new_ppa)) {
//	        gcw.cmd = NAND_WRITE;
//	        gcw.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg;
//	    }
//
//	    ssd_advance_nand(conv_ftl->ssd, &gcw);
//	}
//
//	/* advance per-ch gc_endtime as well */
//#if 0
//	new_ch = get_ch(conv_ftl, &new_ppa);
//	new_ch->gc_endtime = new_ch->next_ch_avail_time;
//
//	new_lun = get_lun(conv_ftl, &new_ppa);
//	new_lun->gc_endtime = new_lun->next_lun_avail_time;
//#endif
//
//	return 0;
//}
//#endif

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
		completed_time = ssd_advance_nand(conv_ftl->ssd, &gcr);
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
	line->start_local_lpn = INVALID_LPN;
#endif
	/* move this line to free line list */
	list_add_tail(&line->entry, &lm->free_line_list);
	lm->free_line_cnt++;
#ifdef LINE_PRINT
	//printk("[JWDBG] %s: freed line: %p free_line_cnt: %d", __func__, line, lm->free_line_cnt);
#endif
}

#ifdef WAF
static inline void print_WAF(struct nvmev_ns *ns)
{
	if (ns->write_volume_host) {
		//float waf = 
		//	(float) ((float) (ns->write_volume_gc + ns->write_volume_host)) / ns->write_volume_host;
		//float total_waf = 
		//	(float) ((float) (ns->total_write_volume_gc + ns->total_write_volume_host)) 
		//	/ ns->total_write_volume_host;
	//	unsigned int waf = 
	//		(100* (ns->write_volume_gc + ns->write_volume_host)) / ns->write_volume_host;
	//	unsigned int total_waf = 
	//		(100* (ns->total_write_volume_gc + ns->total_write_volume_host)) 
	//		/ ns->total_write_volume_host;
	//	printk("%s: WAF: %u percent gc: %llu KB write_req: %llu KB total: %llu KB total WAF: %u percent", 
	//		__func__, waf, ns->write_volume_gc*4, ns->write_volume_host*4, 
	//		ns->total_write_volume_host*4, total_waf);
	}
}

#ifdef MG_CMD_CNT
static inline void print_MG_CMD_CNT(struct nvmev_ns *ns)
{
	if (ns->total_mg_cmd_cnt) {
		printk("%s: mg cmd submitted: %llu  total mg cmd: %llu", 
			__func__, ns->mg_cmd_cnt, ns->total_mg_cmd_cnt);
	}
}
#endif

#ifdef GC_LOG_MEM
static inline void print_GC_LOG_MEM(struct nvmev_ns *ns)
{
	unsigned int n_valid_gc_log = ns->gclm->n_buffered + ns->gclm->n_inflight;
	unsigned int gc_log_mem_MB = n_valid_gc_log * sizeof(struct gc_log) / 1024 / 1024;

	if (ns->gclm->buffering_trial_cnt) {
		printk("%s: mem: %u MB gc log cnt (buffer/inflight): %u ( %u / %u ) merge ratio: %u / %u %u percent", 
			__func__, gc_log_mem_MB, 
			n_valid_gc_log, ns->gclm->n_buffered, ns->gclm->n_inflight, 
			ns->gclm->buffering_trial_cnt - ns->gclm->buffering_cnt, 
			ns->gclm->buffering_trial_cnt, 
			(ns->gclm->buffering_trial_cnt - ns->gclm->buffering_cnt) * 100 
			/ ns->gclm->buffering_trial_cnt );
	}
}
#endif

#ifdef MEASURE_TAIL
static inline void print_tail_lba(struct nvmev_ns *ns)
{
	int i;
	for (i = 1; i < NO_TYPE; i ++ ) {
		printk("%s: partno: %u tail lba: 0x%llx", 
			__func__, i, ns->tail_lba[i]);
	}
}
#endif

#define SEC_IN_USEC 1000000
#define MSEC_IN_USEC 1000
#define WAF_TIME_INTERVAL	(1 * SEC_IN_USEC)
#define COMPACTION_TIME_INTERVAL	(1 * SEC_IN_USEC)
//#define WAF_TIME_INTERVAL	(500 * MSEC_IN_USEC)
static inline uint64_t try_print_WAF(struct nvmev_ns *ns) {
	unsigned long long cur_t = OS_TimeGetUS();
	uint64_t nsecs_ret = 0, nsecs_start, nsecs_end;
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
#ifdef MEASURE_TAIL
		print_tail_lba(ns);
#endif
	}

	if (cur_t - ns->last_compaction_t > COMPACTION_TIME_INTERVAL) {
#ifdef REFLECT_COMP_OVERHEAD
		nsecs_start = __get_wallclock();
#endif
#ifdef MEM_CALC
                compaction1(ns);
#endif
#ifdef REFLECT_COMP_OVERHEAD
		nsecs_end = __get_wallclock();
		nsecs_ret = nsecs_end - nsecs_start;
#endif
		ns->last_compaction_t = cur_t;
	}
	return nsecs_ret;
}

#endif

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
	//static int cnt = 0;
	//cnt ++;
	//if (cnt % 5000 == 0) {
	//	printk("GC-ing curline: line:%d,ipc=%d(%d),victim=%d,full=%d,free=%d total_vblks: %u line: 0x%lx", 
	//		ppa.g.blk,\
	//         victim_line->ipc,victim_line->vpc, conv_ftl->lm.victim_line_cnt, conv_ftl->lm.full_line_cnt, \
	//          conv_ftl->lm.free_line_cnt, total_valid_blks, 
	//		  victim_line);
	//}

	NVMEV_DEBUG("GC-ing line:%d,ipc=%d(%d),victim=%d,full=%d,free=%d\n", ppa.g.blk,\
	          victim_line->ipc,victim_line->vpc, conv_ftl->lm.victim_line_cnt, conv_ftl->lm.full_line_cnt,\
	          conv_ftl->lm.free_line_cnt);

#ifdef WAF
	conv_ftl->ns->write_volume_gc += victim_line->vpc;
	conv_ftl->ns->total_write_volume_gc += victim_line->vpc;
#endif

#ifndef MULTI_WP
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
	                    ssd_advance_nand(conv_ftl->ssd, &gce);
	                }

	                lunp->gc_endtime = lunp->next_lun_avail_time;
	            }
	        }
	    }
	}
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

//#ifdef WAF	
//	try_print_WAF(ns);
//#endif

#if (defined ZONE_MAPPING || defined MULTI_PARTITION_FTL)
	int no_partition = NO_LOCAL_PARTITION(start_lpn / nr_parts);
	NVMEV_ASSERT(no_partition == NO_LOCAL_PARTITION(end_lpn / nr_parts));
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
	    prev_ppa = get_maptbl_ent(conv_ftl, start_lpn/nr_parts);

	    NVMEV_DEBUG("[%s] conv_ftl=%p, ftl_ins=%lld, local_lpn=%lld",__FUNCTION__, conv_ftl, lpn%nr_parts, lpn/nr_parts);

	    /* normal IO read path */
	    for (lpn = start_lpn; lpn <= end_lpn; lpn+=nr_parts) {
	        local_lpn = lpn / nr_parts;
	        
			cur_ppa = get_maptbl_ent(conv_ftl, local_lpn);
    		
			//printk("[JWDBG] %s lpn 0x%llx ppa 0x%llx, ", __func__, lpn, cur_ppa.ppa );
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


#ifdef WAF	
	uint64_t nsecs_compaction = try_print_WAF(ns);
	nsecs_start += nsecs_compaction;
#endif


#ifdef MULTI_PARTITION_FTL
	int no_partition = NO_LOCAL_PARTITION(start_lpn / nr_parts);
	//if (no_partition == COLD_DATA_PARTITION || no_partition == COLD_NODE_PARTITION)
	//	printk("%s: host write on cold partition!! type: %d lpn: %lld", __func__, no_partition, start_lpn);
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
	
	struct line *tmp_line;
	static uint64_t min_slpn[NO_TYPE] = {0xffffffff, 0xffffffff, \
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
	if (IS_MAIN_PARTITION(no_partition)){
		start_lpn -= START_OFS_IN_MAIN_PART;
		end_lpn -= START_OFS_IN_MAIN_PART;
		if (min_slpn[NO_PARTITION(start_lpn)] > start_lpn){
			min_slpn[NO_PARTITION(start_lpn)] = start_lpn;
			printk("%s: type: %lld start_lpn: 0x%llx", __func__, NO_PARTITION(start_lpn), start_lpn);
		}
	}

#ifdef WAF
	ns->write_volume_host += (end_lpn - start_lpn + 1);
	ns->total_write_volume_host += (end_lpn - start_lpn + 1);
#endif
#ifdef MEM_CALC
        if(IS_MAIN_PARTITION(no_partition)){
                struct ms_info *ms_infos = ns->ms_infos;
                struct ms_info *ms_info = &ms_infos[no_partition];

                if(ms_info->global_sLBA > start_lpn){
                        ms_info->global_sLBA = start_lpn;

                        if(ms_info->trimmed_start_MSidx == -1)
                                ms_info->trimmed_start_MSidx = 0;
                }
                if(ms_info->global_eLBA < end_lpn){
                        ms_info->global_eLBA = end_lpn;
                        ms_info->global_eMSidx = (end_lpn - ms_info->global_sLBA) / MSblks;
                }

                // punch_bitmap(ms_info, lba, lba + nr_lba - 1, 1);
        }
#endif
#ifdef MEASURE_TAIL
	ns->tail_lba[no_partition] = end_lpn;
#endif
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
	        nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &swr);
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

/* write for zone mapping */
//bool lm_write(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
//{
//	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
//	struct conv_ftl *conv_ftl = &conv_ftls[0];
//
//	/* wbuf and spp are shared by all instances */
//	struct ssdparams *spp = &conv_ftl->ssd->sp;
//	struct buffer * wbuf = conv_ftl->ssd->write_buffer;
//
//	struct nvme_command *cmd = req->cmd;
//	uint64_t lba = cmd->rw.slba;
//	uint64_t nr_lba = (cmd->rw.length + 1);
//	uint64_t start_lpn = lba / spp->secs_per_pg;
//	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;
//
//	uint64_t lpn, local_lpn, logical_zoneno;
//	uint32_t nr_parts = ns->nr_parts;
//
//	uint64_t nsecs_start = req->nsecs_start;
//	uint64_t nsecs_completed = 0, nsecs_latest;
//	uint64_t nsecs_xfer_completed;
//	uint32_t allocated_buf_size;
//	struct ppa ppa, *zone_map_ent, calc_ppa;
//	struct nand_cmd swr;
//	
//#ifdef WAF	
//	try_print_WAF(ns);
//#endif
//
//	int no_partition = NO_LOCAL_PARTITION(start_lpn / nr_parts);
//	if (no_partition == COLD_DATA_PARTITION || no_partition == COLD_NODE_PARTITION)
//		printk("%s: host write on cold partition!! type: %d lpn: %lld", __func__, no_partition, start_lpn);
//
//	NVMEV_ASSERT(no_partition == NO_LOCAL_PARTITION(end_lpn / nr_parts));
//
//#ifndef COUPLED_GC_MTL
//	struct ppa (*write_handler) (struct conv_ftl *conv_ftl, uint64_t local_lpn, int no_partition);
//#else
//	struct ppa (*write_handler) (struct conv_ftl *conv_ftl, uint64_t local_lpn, int no_partition, struct nvmev_result *ret);
//#endif
//#ifndef GURANTEE_SEQ_WRITE
//	void (*line_handler) (struct conv_ftl *conv_ftl, uint32_t io_type, int no_partition,
//								struct ppa *ppa);
//#endif
//	static int print = 1;
//	NVMEV_ASSERT(conv_ftls);
//	NVMEV_DEBUG("conv_write: start_lpn=%lld, len=%d, end_lpn=%lld", start_lpn, nr_lba, end_lpn);
//#ifdef JWDBG_CONV_FTL
//	/*static int print_ = 0;
//	int print_interval = 1000;
//	if (print_ % print_interval == 0){
//		//printk("%s: slpn=%lld, len=%lld, elpn=%lld lpn: 0x%llx ~ 0x%llx", 
//		//	__func__, start_lpn, nr_lba, end_lpn, 
//		//	start_lpn, end_lpn);
//	}
//	print_ ++ ;*/
//#endif
//
//
//
//	if (IS_META_PARTITION(no_partition)){
//		/* TODO: ftl range check for meta partition */
//		write_handler = write_meta_page_mapping_handler;
//#ifndef GURANTEE_SEQ_WRITE
//		line_handler  = advance_write_pointer;
//#endif
//	} else if (IS_MAIN_PARTITION(no_partition)){
//		write_handler = write_zone_mapping_handler;
//#ifndef GURANTEE_SEQ_WRITE
//		line_handler  = classify_line;
//#endif
//		if (out_of_partition(conv_ftl, end_lpn/nr_parts)){
//			if (print){
//	    		printk("conv_write: lpn passed FTL range(start_lpn=0x%llx,tt_pgs=%ld)\n", start_lpn, spp->tt_pgs);
//				print = 0;
//			}
//			NVMEV_ASSERT(0);
//	    	//NVMEV_ERROR("conv_write: lpn passed FTL range(start_lpn=0x%llx,tt_pgs=%ld)\n", start_lpn, spp->tt_pgs);
//	    	return false;
//		}
//	} else {
//		NVMEV_ERROR("%s: partition %d error\n", __func__, no_partition);
//		return false;
//	}
//
//	allocated_buf_size = buffer_allocate(wbuf, LBA_TO_BYTE(nr_lba));
//
//	if (allocated_buf_size < LBA_TO_BYTE(nr_lba))
//		return false;
//
//	nsecs_latest = nsecs_start;
//	nsecs_latest = ssd_advance_write_buffer(
//			conv_ftl->ssd, nsecs_latest, LBA_TO_BYTE(nr_lba));
//	nsecs_xfer_completed = nsecs_latest;
//
//	swr.type = USER_IO;
//	swr.cmd = NAND_WRITE;
//	swr.stime = nsecs_latest;
//	swr.interleave_pci_dma = false;
//
//	struct line *tmp_line;
//	static uint64_t min_slpn[NO_TYPE] = {0xffffffff, 0xffffffff, \
//		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
//	if (IS_MAIN_PARTITION(no_partition)){
//		start_lpn -= START_OFS_IN_MAIN_PART;
//		end_lpn -= START_OFS_IN_MAIN_PART;
//		if (min_slpn[NO_PARTITION(start_lpn)] > start_lpn){
//			min_slpn[NO_PARTITION(start_lpn)] = start_lpn;
//			printk("%s: type: %lld start_lpn: 0x%llx", __func__, NO_PARTITION(start_lpn), start_lpn);
//		}
//	}
//	
//	//printk("lm_write: start_lpn= 0x%lx, end_lpn= 0x%lx", start_lpn, end_lpn);
//	
//#ifdef WAF
//	ns->write_volume_host += (end_lpn - start_lpn + 1);
//	ns->total_write_volume_host += (end_lpn - start_lpn + 1);
//#endif
//	/* meta partition. page mapping */
//	for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
//	    conv_ftl = &conv_ftls[lpn % nr_parts];
//	    local_lpn = lpn / nr_parts;
//#ifndef COUPLED_GC_MTL
//		ppa = write_handler(conv_ftl, local_lpn, no_partition);
//#else
//		ppa = write_handler(conv_ftl, local_lpn, no_partition, ret);
//#endif
//#ifdef JWDBG_CONV_FTL
//		//printk("[JWDBG] %s: lpn: 0x%llx local lpn: 0x%llx ppa: 0x%llx %lld\n", 
//		//		__func__, lpn, local_lpn, ppa.ppa, ppa.ppa);
//#endif
//	    /* update rmap */
//	    set_rmap_ent(conv_ftl, local_lpn, &ppa);
//
//	    check_mark_page_valid(conv_ftl, &ppa, local_lpn);
//	    mark_page_valid(conv_ftl, &ppa);
//
//	    /* need to advance the write pointer here */
//#ifdef GURANTEE_SEQ_WRITE
//	    advance_write_pointer(conv_ftl, USER_IO, no_partition);
//#else
//	    line_handler(conv_ftl, USER_IO, no_partition, &ppa);
//#endif
//	    /* Aggregate write io in flash page */
//	    if (last_pg_in_wordline(conv_ftl, &ppa)) {
//	        swr.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg;
//	        swr.ppa = &ppa;
//	        nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &swr);
//	        nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;
//
//	        enqueue_writeback_io_req(req->sq_id, nsecs_completed, wbuf, spp->pgs_per_oneshotpg * spp->pgsz);
//	    }
//
//#ifndef MULTI_PARTITION_FTL
//	    consume_write_credit(conv_ftl);
//	    check_and_refill_write_credit(conv_ftl);
//#else
//	    consume_write_credit(conv_ftl, no_partition);
//#ifndef  COUPLED_GC_MTL
//	    check_and_refill_write_credit(conv_ftl, no_partition);
//#else
//	    check_and_refill_write_credit(conv_ftl, no_partition, ret);
//#endif
//
//#endif
//	}
//
//	if ((cmd->rw.control & NVME_RW_FUA) || (spp->write_early_completion == 0)) {
//		/* Wait all flash operations */
//		ret->nsecs_target = nsecs_latest;
//	} else {
//		/* Early completion */
//		ret->nsecs_target = nsecs_xfer_completed;
//	}
//	ret->status = NVME_SC_SUCCESS;
//
//	return true;
//}
#endif

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

//#ifdef WAF	
//	try_print_WAF(ns);
//#endif

#if (defined ZONE_MAPPING || defined MULTI_PARTITION_FTL)
	int no_partition;
#endif

	static int getit = 0;
	uint64_t total_dblk = 0;
	
	ret->cid = cmd->common.command_id;

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

#if !(defined ZONE_MAPPING || defined MULTI_PARTITION_FTL)
		/* TODO: Alignment Check for conventional SSD */
		/* TODO: Sector level bitmap? */
#else
		no_partition = NO_LOCAL_PARTITION(start_lpn / nr_parts);
		NVMEV_ASSERT(no_partition == NO_LOCAL_PARTITION(end_lpn / nr_parts));
		
		if (IS_MAIN_PARTITION(no_partition)){
			start_lpn -= START_OFS_IN_MAIN_PART;
			end_lpn -= START_OFS_IN_MAIN_PART;
			total_dblk += (end_lpn - start_lpn + 1);
			getit = 1;
		}

		/* Alignment Check */
		if (IS_MAIN_PARTITION(no_partition)){
			NVMEV_ASSERT(lba % spp->secs_per_pg == 0);
			if (nr_lba % spp->secs_per_pg != 0 || lba % spp->secs_per_pg != 0){
				printk("[JWDBG] %s: nr lba: 0x%llx not aligned!!", __func__, nr_lba);
			}
			//NVMEV_ASSERT(nr_lba % spp->secs_per_pg == 0);
		}
		//printk("conv_discard: start_lpn=0x%llx,  end_lpn=0x%llx", start_lpn, end_lpn);

#endif
#ifdef MEM_CALC
		if(IS_MAIN_PARTITION(no_partition)){
                	struct ms_info *ms_infos = ns->ms_infos;
                	struct ms_info *ms_info = &ms_infos[no_partition];

                	punch_bitmap(ms_info, start_lpn, end_lpn, no_partition);
        	}
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
			

		    ppa = get_maptbl_ent(conv_ftl, local_lpn);
			int trial_cnt = 0;
invalidate_page:
		    if (mapped_ppa(&ppa)) {
				
				if ((get_pg(conv_ftl->ssd, &ppa))->status != PG_VALID){
					printk("[JWDBG] %s: ori lpn: 0x%lx lpn: 0x%lx ppa: 0x%llx pg status: %d line: 0x%lx line wpc: %d", 
							__func__, ori_lpn, local_lpn*nr_parts, 
							ppa.ppa, get_pg(conv_ftl->ssd, &ppa)->status, 
							get_line(conv_ftl, &ppa), get_line(conv_ftl, &ppa)->wpc);
				}
		        
				mark_page_invalid(conv_ftl, &ppa);

		        set_rmap_ent(conv_ftl, INVALID_LPN, &ppa);
		        NVMEV_DEBUG("conv_write: %lld is invalid, ", ppa2pgidx(conv_ftl, &ppa));
    			
				invalidate_maptbl_ent(conv_ftl, local_lpn);
		    }
#if (defined ZONE_MAPPING || defined MULTI_PARTITION_FTL)
		    else if (IS_MAIN_PARTITION(no_partition)){
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
