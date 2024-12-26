/*
 * wchunk.c
 *
 *  Created on: 2021. 7. 16.
 *      Author: Minsu Jang (nobleminsu@gmail.com)
 */

#include "map_segment.h"

#include <assert.h>
#include <string.h>

#include "../../alex/openssd_allocator.h"
#include "../../ftl_config.h"
#include "../../memory_map.h"
#include "../functional/functional_mapping.h"
#include "xil_printf.h"
#include "xparameters.h"
#include "xtime_l.h"

char *ftableMemPool = (char *)RESERVED0_START_ADDR;

FunctionalMappingTree fmTrees[FUNCTIONAL_MAPPING_TREE_COUNT];
OpenSSDAllocator<RootNode> rootNodeAllocator;
OpenSSDAllocator<ZoneNode> tAllocator;
OpenSSDAllocator<MapSegment> mapSegmentAllocator;
OpenSSDAllocator<ZoneNode *> rootNodeChildrenAllocator;
OpenSSDAllocator<uint16_t> rangeDirAllocator;
OpenSSDAllocator<LOGICAL_SLICE_ENTRY> rangeBufferAllocator;
OpenSSDAllocator<RangeBuffer> mapSegmentBufferAllocator;

int isFmTreesInitialized[FUNCTIONAL_MAPPING_TREE_COUNT];

MapSegment_p target_saved  = NULL;



MapSegment *mapseg_select_map_segment(unsigned int logicalSliceAddr,
                                      int isAllocate, uint32_t *parent_addr_ret);
void wchunk_print_alex_stats();

void mapseg_init() {
	bool trash;
    for (int i = 0; i < FUNCTIONAL_MAPPING_TREE_COUNT; i++) {
        initMappingTree(&fmTrees[i]);

        initRootNode((fmTrees[i].rootNode), TREE_NUM_TO_FIRST_LSA(i));
        fetchMapSegmentFromFmTree(&fmTrees[i], TREE_NUM_TO_FIRST_LSA(i), 1, NULL, &trash);
        xil_printf("FmTree %d init: %p rootNode %p df %d\n", i, TREE_NUM_TO_FIRST_LSA(i), fmTrees[i].rootNode
        	,
		sizeof(*fmTrees[i].rootNode));

        isFmTreesInitialized[i] = 0;
    }
}

void mapseg_init_map_segment(MapSegment *pMapSegment, unsigned int startLsa) {
    pMapSegment->startLsa = startLsa;
    pMapSegment->mappingSize = MAPSEG_MAP_SEGMENT_SIZE;
    pMapSegment->unitRangeSize = MAPSEG_MAP_SEGMENT_SIZE;
    pMapSegment->numOfValidMaps = 0;
    pMapSegment->numOfWrittenMaps = 0;
    pMapSegment->compact_cnt = 0;
    memset(
        pMapSegment->validBits, 0,
        sizeof(unsigned int) * MAPSEG_VALID_BIT_INDEX(MAPSEG_MAP_SEGMENT_SIZE));

    unsigned int dirSize =
        pMapSegment->mappingSize / pMapSegment->unitRangeSize;
    // init directory
    mapseg_init_range_directory(&pMapSegment->rangeDir, dirSize);
    /* init directory contents */
    pMapSegment->rangeDir.bufferIdxs[0]  = 0;
    // init buffer
    pMapSegment->rangeBuffer =
        mapSegmentBufferAllocator.allocate(MAPSEG_INITIAL_BUFFER_SIZE);
    /*mapseg_init_range_buffer(pMapSegment->rangeBuffer, 0,
                             pMapSegment->mappingSize);
    */
     mapseg_init_range_buffer(pMapSegment->rangeBuffer, startLsa,
                             pMapSegment->mappingSize + 1);
    /* size of bitmap + region mapping + region directory */
    pMapSegment->sz = MAPSEG_VALID_BIT_INDEX(MAPSEG_MAP_SEGMENT_SIZE)
			+ (pMapSegment->mappingSize + 1) * sizeof(LOGICAL_SLICE_ENTRY)
 			+ dirSize * sizeof(uint16_t);
}





MapSegment *mapseg_select_map_segment(unsigned int logicalSliceAddr,
                                      int isAllocate, uint32_t *parent_addr_ret,
									  bool* last_in_mapseg) {
    FunctionalMappingTree *fmTree;
    MapSegment_p selectedMapSegment = NULL;
    uint32_t parent_addr;
    fmTree = &fmTrees[LSA_TO_TREE_NUM(logicalSliceAddr)];
    if (isFmTreesInitialized[LSA_TO_TREE_NUM(logicalSliceAddr)] == 0) {
        isFmTreesInitialized[LSA_TO_TREE_NUM(logicalSliceAddr)] = 1;
        xil_printf("FmTree %d first access: %p\n",
                   LSA_TO_TREE_NUM(logicalSliceAddr), logicalSliceAddr);
    }
    selectedMapSegment =
        fetchMapSegmentFromFmTree(fmTree, logicalSliceAddr, isAllocate, &parent_addr, last_in_mapseg);
    if (parent_addr_ret != NULL)
    	*parent_addr_ret = parent_addr;
    return selectedMapSegment;
}

unsigned int mapseg_get_mapping(unsigned int logicalSliceAddr) {
    unsigned int virtualSliceAddr, selectedMapSegmentStartLsa,
        indexInMapSegment;
    MapSegment_p selectedMapSegment;
    bool trash;
//    static int pc = 0;
//    int print_ = 50000;
//    pc += 1;
//    if (pc % print_ == 0)
//    	xil_printf("%s: HIII\n", __func__);
    selectedMapSegment = mapseg_select_map_segment(logicalSliceAddr, 0, NULL, &trash);
//    if (selectedMapSegment != NULL){
////    	if (target_saved == selectedMapSegment)
//    	if (selectedMapSegment->unitRangeSize < MAPSEG_MAP_SEGMENT_SIZE)
//    		xil_printf("%s: access to compacted addr: %x\n", __func__, selectedMapSegment);
//    }
    if (selectedMapSegment == NULL) return VSA_FAIL;
    selectedMapSegmentStartLsa = selectedMapSegment->startLsa;
    indexInMapSegment = logicalSliceAddr - selectedMapSegmentStartLsa;
//    if (selectedMapSegment != NULL){
////    	if (target_saved == selectedMapSegment)
//    	if (selectedMapSegment->unitRangeSize < MAPSEG_MAP_SEGMENT_SIZE){
//    		xil_printf("%s: access to compacted addr: %x lsa: %x indexinMS: %u, inputLBA: %u sLBA: %u sz: %u\n", __func__,
//    				selectedMapSegment,
//    				selectedMapSegment->startLsa, indexInMapSegment,
//					logicalSliceAddr, selectedMapSegmentStartLsa,
//					selectedMapSegment->mappingSize);
//    		if (selectedMapSegment->compact_cnt > 1)
//    			xil_printf("%s: 1 compact cnt: %u \n", __func__, selectedMapSegment->compact_cnt);
//
//    	}
//    }

    if (!mapseg_is_valid(selectedMapSegment, indexInMapSegment)) {
//    	if (selectedMapSegment != NULL){
//        	if (selectedMapSegment->unitRangeSize < MAPSEG_MAP_SEGMENT_SIZE){
////    	    	if (target_saved == selectedMapSegment)
//				xil_printf("%s: map invalid!!\n", __func__);
////	    		if (selectedMapSegment->compact_cnt > 1)
////	    			xil_printf("%s: 1 compact cnt: %u \n", __func__, selectedMapSegment->compact_cnt);
//        	}
//    	}
        return VSA_FAIL;
    }
//    if (selectedMapSegment != NULL){
////    	if (target_saved == selectedMapSegment)
//		if (selectedMapSegment->unitRangeSize < MAPSEG_MAP_SEGMENT_SIZE){
////    		xil_printf("%s: access to compacted shival nyuan 2\n", __func__);
////    		if (selectedMapSegment->compact_cnt > 1)
////    			xil_printf("%s: 1 compact cnt: %u \n", __func__, selectedMapSegment->compact_cnt);
//		}
//    }
    uint16_t diridx_;
    uint32_t bufidx_;
    if (selectedMapSegment->unitRangeSize < MAPSEG_MAP_SEGMENT_SIZE){
		virtualSliceAddr =
			mapseg_fetch_entry_in_map_segment(selectedMapSegment, logicalSliceAddr, &diridx_, &bufidx_)
				->virtualSliceAddr;
    } else {
		virtualSliceAddr =
			mapseg_fetch_entry_in_map_segment(selectedMapSegment, logicalSliceAddr, NULL, NULL)
				->virtualSliceAddr;
    }
    if (selectedMapSegment != NULL){
    	if (selectedMapSegment->unitRangeSize < MAPSEG_MAP_SEGMENT_SIZE){
////    	if (target_saved == selectedMapSegment)
//    		xil_printf("%s: nodeaddr: %x diridx: %u bufidx: %u virtualSliceAddr: %x\n", __func__,
//    				selectedMapSegment,
//    				diridx_, bufidx_,
//    				virtualSliceAddr);
//    		if (selectedMapSegment->compact_cnt > 1)
//    			xil_printf("%s: 1 compact cnt: %u \n", __func__, selectedMapSegment->compact_cnt);
    	}
    }
    return virtualSliceAddr;
}

int mapseg_set_mapping(unsigned int logicalSliceAddr,
                       unsigned int virtualSliceAddr, bool last_discard) {
    unsigned int selectedMapSegmentStartAddr, indexInMapSegment;
    MapSegment_p selectedMapSegment;
    uint32_t parent_addr;
    bool trash;

    selectedMapSegment = mapseg_select_map_segment(logicalSliceAddr, 1, &parent_addr, &trash);

//    if (selectedMapSegment->unitRangeSize < MAPSEG_MAP_SEGMENT_SIZE){
//        		xil_printf("%s: nodeaddr: %x 1\n", __func__,
//        				selectedMapSegment);
//	}

//    if (selectedMapSegment != NULL){
//    	if (target_saved == selectedMapSegment)
//    		xil_printf("%s: access to compacted shival nyuan\n", __func__);
//    }

    if (selectedMapSegment == NULL) return -1;
//    if (selectedMapSegment->unitRangeSize < MAPSEG_MAP_SEGMENT_SIZE){
//        		xil_printf("%s: nodeaddr: %x 2\n", __func__,
//        				selectedMapSegment);
//	}
    selectedMapSegmentStartAddr = selectedMapSegment->startLsa;
    indexInMapSegment = logicalSliceAddr - selectedMapSegmentStartAddr;
//    if (selectedMapSegment->unitRangeSize < MAPSEG_MAP_SEGMENT_SIZE){
//        		xil_printf("%s: nodeaddr: %x 3\n", __func__,
//        				selectedMapSegment);
//	}

//   if (target_saved != NULL)
//		xil_printf("%s: access to compacted shival nyuan 1\n", __func__);
    mapseg_fetch_entry_in_map_segment(selectedMapSegment, logicalSliceAddr, NULL, NULL)
        ->virtualSliceAddr = virtualSliceAddr;
//    if (selectedMapSegment->unitRangeSize < MAPSEG_MAP_SEGMENT_SIZE){
//        		xil_printf("%s: nodeaddr: %x 4\n", __func__,
//        				selectedMapSegment);
//	}
//    if (target_saved != NULL)
// 		xil_printf("%s: access to compacted shival nyuan 2\n", __func__);

    if (virtualSliceAddr != VSA_NONE)
        mapseg_mark_valid(selectedMapSegment, indexInMapSegment, 1,
                          selectedMapSegmentStartAddr, 1,
                          MAPSEG_FULL_BITS_IN_SLICE, parent_addr,
						  false);
    else
        mapseg_mark_valid(selectedMapSegment, indexInMapSegment, 1,
                          selectedMapSegmentStartAddr, 0,
                          MAPSEG_FULL_BITS_IN_SLICE, parent_addr,
						  last_discard);

//    if (selectedMapSegment->unitRangeSize < MAPSEG_MAP_SEGMENT_SIZE){
//        		xil_printf("%s: nodeaddr: %x 5\n", __func__,
//        				selectedMapSegment);
//	}

//    if (target_saved != NULL)
// 		xil_printf("%s: access to compacted shival nyuan 3\n", __func__);
    return 0;
}


int mapseg_remove(unsigned int logicalSliceAddr, bool last_discard) {
    unsigned int virtualSliceAddr, selectedMapSegmentStartLsa,
        indexInMapSegment;
    MapSegment_p selectedMapSegment;
    bool last_in_mapseg = false;

    selectedMapSegment = mapseg_select_map_segment(logicalSliceAddr, 0, NULL, &last_in_mapseg);
//    if (selectedMapSegment->unitRangeSize < MAPSEG_MAP_SEGMENT_SIZE){
//        		xil_printf("%s: nodeaddr: %x input_lba: %x 1\n", __func__,
//        				selectedMapSegment, logicalSliceAddr);
//	}
    if (selectedMapSegment == NULL) return VSA_FAIL;
    selectedMapSegmentStartLsa = selectedMapSegment->startLsa;
    indexInMapSegment = logicalSliceAddr - selectedMapSegmentStartLsa;

//    if (selectedMapSegment->unitRangeSize < MAPSEG_MAP_SEGMENT_SIZE){
//        		xil_printf("%s: nodeaddr: %x input_lba: %x 2\n", __func__,
//        				selectedMapSegment, logicalSliceAddr);
//	}

    if (mapseg_is_empty(selectedMapSegment, indexInMapSegment)) {
        incrementZoneNodeInvCount(selectedMapSegment->parent);
    }
    if (selectedMapSegment->unitRangeSize < MAPSEG_MAP_SEGMENT_SIZE){
//        		xil_printf("%s: nodeaddr: %x input_lba: %x 3\n", __func__,
//        				selectedMapSegment, logicalSliceAddr);
	}


    return mapseg_set_mapping(logicalSliceAddr, VSA_NONE, (last_in_mapseg || last_discard) );

}

void mapseg_deallocate(MapSegment_p wchunk_p, unsigned int chunkStartAddr) {
    // TODO: implement fm deallocate
}

int mapseg_is_valid(MapSegment_p wchunk_p, unsigned int indexInChunk) {
    int validBitIndex, validBitSelector;
    validBitIndex = MAPSEG_VALID_BIT_INDEX(indexInChunk);
    validBitSelector =
        MAPSEG_VALID_BIT_SELECTOR(indexInChunk, MAPSEG_FULL_BITS_IN_SLICE);

    return wchunk_p->validBits[validBitIndex] & validBitSelector;
}

int mapseg_is_empty(MapSegment_p wchunk_p, unsigned int indexInChunk) {
    int validBitIndex, validBitSelector;
    validBitIndex = MAPSEG_VALID_BIT_INDEX(indexInChunk);
    validBitSelector =
        MAPSEG_VALID_BIT_SELECTOR(indexInChunk, MAPSEG_FULL_BITS_IN_SLICE);

    if (wchunk_p->validBits[validBitIndex] & validBitSelector){
    	if (wchunk_p->numOfValidMaps == 1)
    		return true;
    } else {
    	xil_printf("%s: bitmap consistency failed\n", __func__);
    }
    return false;
}



bool no_more_write(MapSegment_p wchunk_p)
{
    return wchunk_p->numOfWrittenMaps >= MAPSEG_MAP_SEGMENT_SIZE;
}

#define COMPACTION_CONDITION_RATIO	0.5
//#define COMPACTION_CONDITION_RATIO	0.8


bool is_compaction_candidate(MapSegment_p wchunk_p)
{
    return wchunk_p->numOfValidMaps <=
		wchunk_p->mappingSize * COMPACTION_CONDITION_RATIO;
}

int TEST_BITMAP(unsigned int* bitmap, int i)
{
    int validBitIndex, validBitSelector;
    validBitIndex = MAPSEG_VALID_BIT_INDEX(i);
    validBitSelector =
        MAPSEG_VALID_BIT_SELECTOR(i, MAPSEG_FULL_BITS_IN_SLICE);

    return bitmap[validBitIndex] & validBitSelector;
}



void GET_RANGE_SIZE(unsigned int* bitmap,  uint32_t n_maps,
                        uint32_t* first_valid_idx, uint32_t* last_valid_idx, bool print)
{
        int i;
        for (i = 0; i < n_maps; i += 1){
                if (TEST_BITMAP(bitmap, i)){
                	//if (print)
//                		xil_printf("%s: first valid idx: %d\n",__func__, i);
					*first_valid_idx = i;
					break;
                }
        }

        for (i = 0; i < n_maps; i += 1){
                if (TEST_BITMAP(bitmap, n_maps-i-1)){
					//if (print)
//						xil_printf("%s: last valid idx: %d\n",__func__, n_maps-i-1);
					*last_valid_idx = n_maps-i-1;
					break;
                }

        }
}


int GET_NEXT_VALID_BIT(unsigned int* bitmap, uint32_t sidx,
                                uint32_t eidx, bool print)
{
        int i;
//        if (print)
//        	xil_printf("%s: sidx: %d eidx: %d\n", __func__, sidx, eidx);
        for (i = sidx; i <= eidx; i += 1){
//            if (print)
//            	xil_printf("%s: in loop i: %d eidx: %d\n", __func__, i, eidx);
                if (TEST_BITMAP(bitmap, i)){
//                    if (print)
//                    	xil_printf("%s: loop out i: %d eidx: %d\n", __func__, i, eidx);
                        return i;
                }
        }
        return -1;
}

int GET_NEXT_INVALID_BIT(unsigned int* bitmap, uint32_t sidx,
                                uint32_t eidx, bool print)
{
        int i;
//        if (print)
//        	xil_printf("%s: sidx: %d eidx: %d\n",__func__, sidx, eidx);
        for (i = sidx; i <= eidx; i += 1){
                if (!TEST_BITMAP(bitmap, i)){
                        return i;
                }
        }
        return -1;
}

uint32_t GET_REMAINING_INVALID_MAPPING(unsigned int* bitmap,
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
        while((eidx = GET_NEXT_VALID_BIT(bitmap, sidx, last_valid_idx, false)) != -1){
                if (eidx / nmaps_in_new_region == prev_region_idx){
                        /* Multiple active region in a single region.
                           This case, invalid mapping should be included in
                           new map node. */
                        n_invalid_mappings = eidx - prev_valid_eidx - 1;
                        n_total_invalid_mappings += n_invalid_mappings;

                } else {
                        *n_headers += 1;
                }

                if ((sidx = GET_NEXT_INVALID_BIT(bitmap, eidx + 1, last_valid_idx, false))
                                == -1){
                        break;
                }
                prev_region_idx = (sidx-1) / nmaps_in_new_region;
                prev_valid_eidx = sidx-1;
        }
        return n_total_invalid_mappings;
}


uint32_t GET_MIN_HOLE_SIZE(unsigned int* bitmap,
                        uint32_t first_valid_idx, uint32_t last_valid_idx,
                        uint32_t* n_active_regions, bool print)
{
        uint32_t i, min_hole = last_valid_idx - first_valid_idx;
        uint32_t sidx, eidx, hole_sz;
        sidx = first_valid_idx;
        *n_active_regions = 0;
        /*scan bitmap*/
//		if (print)
//    		xil_printf("%s: first: %d, last: %d\n", __func__, first_valid_idx,
//    				last_valid_idx);
        while((eidx = GET_NEXT_VALID_BIT(bitmap, sidx, last_valid_idx, print)) != -1){
//        		if (print)
//                		xil_printf("%s: 1 eidx: %d last_valid_idx: %d\n", __func__, eidx,
//                				last_valid_idx);
                hole_sz = eidx - sidx;
                if (hole_sz > 0)
                        min_hole = (hole_sz < min_hole)? hole_sz : min_hole;
                if ((sidx = GET_NEXT_INVALID_BIT(bitmap, eidx + 1, last_valid_idx, print))
                                == -1){
//                	if (print)
//                        	xil_printf("%s: 3\n", __func__);
					*n_active_regions += 1;
					break;
                }
                *n_active_regions += 1;
//            	if (print)
//                    	xil_printf("%s: 2\n", __func__);

        }
        return min_hole;
}



void SET_BITMAP(unsigned int* bitmap, int i)
{
    int validBitIndex, validBitSelector, origBits, newBits = 0;
    validBitIndex = MAPSEG_VALID_BIT_INDEX((i));
    validBitSelector =
        MAPSEG_VALID_BIT_SELECTOR((i), MAPSEG_FULL_BITS_IN_SLICE);

    origBits = bitmap[validBitIndex];

	newBits = origBits | validBitSelector;

	bitmap[validBitIndex] = newBits;
//
//	validBitIndex = MAPSEG_VALID_BIT_INDEX((indexInChunk + i));
//	validBitSelector =
//		MAPSEG_VALID_BIT_SELECTOR((indexInChunk + i), bitsInSlice);
//
//	origBits = wchunk_p->validBits[validBitIndex];
//
//	if (isValid) {
//		newBits = origBits | validBitSelector;
//	wchunk_p->numOfWrittenMaps += 1;
//	} else {
//		newBits = origBits & (~validBitSelector);
//	}
//	wchunk_p->numOfValidMaps +=
//		__builtin_popcountl(newBits) - __builtin_popcountl(origBits);
//	wchunk_p->validBits[validBitIndex] = newBits;


}

int COPY_BITMAP_MASK(unsigned int* dst_bitmap, uint32_t dst_index,
		unsigned int* src_bitmap, uint32_t src_index)
{
	int validBitIndex, validBitSelector, origBits, newBits = 0;

		if (TEST_BITMAP(src_bitmap, src_index)){
			SET_BITMAP(dst_bitmap, dst_index);
//			newBits = origBits | validBitSelector;
		} else {
//			newBits = origBits & (~validBitSelector);
		}

	return 0;
}



MapSegment_p CONSTRUCT_COMPACTED_MAP_NODE(MapSegment_p old_mnode,
                                uint32_t first_valid_idx,
                                uint32_t last_valid_idx,
                                uint32_t nmaps_in_region,
                                uint32_t range_directory_sz,
                                uint32_t bitmap_sz,
                                uint32_t mapping_table_sz,
								uint32_t n_active_regions)
{
        int i;
	MapSegment_p mnode;

        mnode = mapSegmentAllocator.allocate(1);
        unsigned int mapSegmentStartAddr =
            old_mnode->startLsa + first_valid_idx;
	mnode->startLsa = mapSegmentStartAddr;
	mnode->mappingSize = last_valid_idx - first_valid_idx + 1;
    mnode->unitRangeSize = nmaps_in_region;
	mnode->numOfValidMaps = old_mnode->numOfValidMaps;
	mnode->numOfWrittenMaps = old_mnode->numOfWrittenMaps;
	mnode->sz = range_directory_sz + bitmap_sz + mapping_table_sz;
	mnode->compact_cnt = old_mnode->compact_cnt;
	mnode->parent = old_mnode->parent;
	uint32_t new_mnode_sz = range_directory_sz + bitmap_sz + mapping_table_sz;
//	xil_printf("%s: old slba: %x new slba: %x first vidx: %u last vidx: %u dirsz: %u bmapsz: %u bufsz: %u\n",
//			__func__, old_mnode->startLsa, mnode->startLsa,
//			first_valid_idx, last_valid_idx, range_directory_sz, bitmap_sz, mapping_table_sz);
	/*This sould be modified when bitmap size becomes variable */
    	memset(
        	mnode->validBits, 0,
        	sizeof(unsigned int) * MAPSEG_VALID_BIT_INDEX(MAPSEG_MAP_SEGMENT_SIZE));

        /* copy bitmap  */
        for(i = 0; i < last_valid_idx-first_valid_idx+1; i += 1){
                COPY_BITMAP_MASK(mnode->validBits, i,
                                old_mnode->validBits, i+first_valid_idx);
        }


	/* initialize rangeDirectory */
    	mapseg_init_range_directory(&mnode->rangeDir, range_directory_sz / sizeof(uint16_t));

	/* initialize rangeBuffer */
    	mnode->rangeBuffer =
        	mapSegmentBufferAllocator.allocate(MAPSEG_INITIAL_BUFFER_SIZE);
	mnode->rangeBuffer->entries = rangeBufferAllocator.allocate(
			mapping_table_sz/sizeof(LOGICAL_SLICE_ENTRY));

        /* Fill range directory and fixed-region mappings */

        int prev_dir_idx = -1, cur_dir_idx, cur_dir_eidx, cur_header_idx;
        uint32_t idx = 0, ii;
        bool scan_finished = false;
        uint32_t sidx, eidx,
                n_invalid_mappings, n_total_invalid_mappings,
                sub_region_idx, range_idx, delta_idx, sidx_range_directory;
//        sidx = first_valid_idx;
        sidx = 0;
        LOGICAL_SLICE_ENTRY *range_mappings = mnode->rangeBuffer->entries,
			    *old_range_mappings = old_mnode->rangeBuffer->entries,
			    *mapping_p;
        uint16_t *range_directory = mnode->rangeDir.bufferIdxs,
			*old_range_directory = old_mnode->rangeDir.bufferIdxs;

        unsigned int region_cnt = 0;

        /* scan bitmap and fill mappings */
        while((eidx = GET_NEXT_VALID_BIT(old_mnode->validBits, sidx, last_valid_idx, false)) != -1){
//            	xil_printf("%s: 6 sidx: %d, last valid idx: %d nxt valid idx: %d\n", __func__,
//            			sidx, last_valid_idx, eidx);
        		cur_dir_idx = (eidx - first_valid_idx) / mnode->unitRangeSize;
                if (cur_dir_idx == prev_dir_idx){
                        /* Multiple active region in a single region.
                           This case, invalid mapping should be included in
                           new map node. */
                        for (ii = 0; ii < eidx - sidx; ii += 1){
                                (range_mappings[idx++]).virtualSliceAddr
                                		= VSA_NONE;

                        }
                } else {
                        cur_header_idx = idx;
                        /* Create new region mapping */
                        /* Set header of new region mapping */
                        (range_mappings[idx++]).virtualSliceAddr
                                = old_mnode->startLsa + eidx;
//                        if (n_active_regions > 1){
//                    	if (new_mnode_sz == 11)
//							xil_printf("%s: header set: mnodeaddr: %x rangeidx: %u: virtualSliceAddr: %x\n",
//									__func__, mnode,
//									idx-1, old_mnode->startLsa + eidx);
//						}

                        region_cnt += 1;
                }


                if ((sidx = GET_NEXT_INVALID_BIT(old_mnode->validBits,
                                eidx + 1, last_valid_idx, false)) == -1){
                        /* fill mappings of new region mapping */
                        scan_finished = true;
                        sidx = last_valid_idx + 1;
                }
//            	xil_printf("%s: 8 nxt invalid idx: %d, last valid idx: %d eidx: %d\n", __func__,
//            			sidx, last_valid_idx, eidx);

                cur_dir_eidx =
                                (sidx-1-first_valid_idx)/mnode->unitRangeSize;

                /* Fill Range Directory */
                for (ii = cur_dir_idx; ii <= cur_dir_eidx; ii += 1){
                        range_directory[ii] = cur_header_idx;
//                        if (n_active_regions > 1)
//                    	if (new_mnode_sz == 11)
//                        	xil_printf("%s: mnodeaddr: %x directory set: dir idx: %u header_idx: %u\n", __func__,
//								mnode,
//                        		ii, cur_header_idx);

                }
//                prev_dir_idx = (sidx-1) / mnode->unitRangeSize;
                prev_dir_idx = cur_dir_eidx;
//            	xil_printf("%s: 8-1\n", __func__);


		/* fill mappings of new region mapping */
                sub_region_idx = eidx / old_mnode->unitRangeSize;
                range_idx = old_range_directory[sub_region_idx];

                /* access to header of range mapping*/
		/* read start LBA in old noode's header*/
                mapping_p = &(old_range_mappings[range_idx]);

//            	delta_idx = ((old_mnode->startLsa & 0xffffffff) + eidx);
//            	xil_printf("%s: delta_idx 1: %u %x\n", __func__, delta_idx, delta_idx);
//            	delta_idx += 1 - mapping_p->virtualSliceAddr;
//            	xil_printf("%s: delta_idx 2: %u %x\n", __func__, delta_idx, delta_idx);
//            	delta_idx = ((old_mnode->startLsa & 0xffffffff) + eidx)
//            	                			- mapping_p->virtualSliceAddr + 1;
//            	xil_printf("%s: delta_idx 3: %u %x\n", __func__, delta_idx, delta_idx);


                if ((delta_idx = ((old_mnode->startLsa & 0xffffffff) + eidx)
                			- mapping_p->virtualSliceAddr + 1) <= 0){
                        //printf("[JWDBG] %s: wrong header in mapnode", __func__);
                        //exit(-1);
//                	xil_printf("%s: NO!!!!!!!!\n", __func__);

                	return NULL;
                }
//            	xil_printf("%s: 8-4 oldmnode slba: %u %x eidx: %u slba_in_map: %u %x\n", __func__,
//            			old_mnode->startLsa, old_mnode->startLsa,
//						eidx,
//						mapping_p->virtualSliceAddr,  mapping_p->virtualSliceAddr);

//            	xil_printf("%s: delta_idx: %u range_idx: %u\n", __func__, delta_idx, range_idx);




                for (ii = 0; ii < sidx- eidx; ii += 1){
//                		xil_printf("%s: 8 in loop oldmapping idx %u new mapping idx: %u\n", __func__,
//                				range_idx + delta_idx + ii,
//								idx);
                        mapping_p = &(old_range_mappings[range_idx + delta_idx + ii]);
//                        xil_printf("%s: 8 loop 1 access: %x\n", __func__, mapping_p->virtualSliceAddr);
                        (range_mappings[idx++]).virtualSliceAddr = mapping_p->virtualSliceAddr;
//                        if (n_active_regions > 1){
//                    	if (new_mnode_sz == 11)
//                        	xil_printf("%s: mnode: %x slsa: %x map set: rangeidx: %u realinputlba: %x inputlba: %x virtualSliceAddr: %x\n",
//                        			__func__, mnode, mnode->startLsa,
//									idx-1, mapseg_get_mapping(old_mnode->startLsa + ii + eidx),
//									old_mnode->startLsa + ii + eidx, mapping_p->virtualSliceAddr);

//                        }

//                        xil_printf("%s: 8 loop 2\n", __func__);
                }

                if (scan_finished){
//					xil_printf("%s: loop out\n", __func__);
					break;
                }
        }
//        xil_printf("%s: fin\n", __func__);
        mnode->compact_cnt += 1;
//
//        if (n_active_regions > 1)
//    	if (new_mnode_sz == 11)
//        	xil_printf("%s: addr: %x region_cnt: %u region_cnt_arg: %u firstidx: %u lastidx: %u bufsz: %u idx: %u\n", __func__,
//        			mnode, region_cnt, n_active_regions,
//					first_valid_idx, last_valid_idx, mapping_table_sz/sizeof(LOGICAL_SLICE_ENTRY),
//					idx);

	return mnode;
}



#define MIN_REGION	16
#define N_BITS		8

MapSegment_p try_compaction(MapSegment_p wchunk_p, bool print)
{
        MapSegment_p new_mnode;
        uint32_t nmaps_in_new_region, nmaps_in_new_range,
                min_hole, new_bitmap_sz,
                n_entries_range_directory, first_valid_idx, last_valid_idx,
                new_range_directory_sz, n_active_regions, fixed_region_mapping_sz,
				n_entries_fixed_region_mappings, n_entries_new_range_directory,
				new_mnode_sz;
        /* get new valid range size */
//        if (print)
//        	xil_printf("%s: 1\n", __func__);
        GET_RANGE_SIZE(wchunk_p->validBits, wchunk_p->mappingSize,
                        &first_valid_idx, &last_valid_idx, print);
        nmaps_in_new_range = last_valid_idx - first_valid_idx + 1;
//        if (print)
//                	xil_printf("%s: 2\n", __func__);
        /* get minimum hole size */
        min_hole = GET_MIN_HOLE_SIZE(wchunk_p->validBits,
                                        first_valid_idx, last_valid_idx,
                                        &n_active_regions, print);
//        if (print)
//                	xil_printf("%s: 3\n", __func__);
//        if (first_valid_idx == last_valid_idx)
//        	xil_printf("%s: min_hole: %u\n", __func__, min_hole);

        nmaps_in_new_region = (min_hole > MIN_REGION)? min_hole : MIN_REGION;

//        if (first_valid_idx == last_valid_idx)
//        	xil_printf("%s: nmaps_in_new_region: %u nmaps_in_new_range: %u\n", __func__,
//        			nmaps_in_new_region, nmaps_in_new_range);

        /* calculate new bitmap size*/
        new_bitmap_sz = (nmaps_in_new_range % N_BITS)?
                        nmaps_in_new_range / N_BITS + 1:
                        nmaps_in_new_range / N_BITS ;

        /* calculate new range directory size*/
        n_entries_new_range_directory =
                        (nmaps_in_new_range % nmaps_in_new_region)?
                        nmaps_in_new_range / nmaps_in_new_region + 1:
                        nmaps_in_new_range / nmaps_in_new_region ;
//        if (first_valid_idx == last_valid_idx)
//        	xil_printf("%s: n_entries_new_range_directory: %u\n", __func__, n_entries_new_range_directory);

        new_range_directory_sz = n_entries_new_range_directory * sizeof(uint16_t);

//        if (first_valid_idx == last_valid_idx)
//        	xil_printf("%s: new_range_directory_sz: %u\n", __func__, new_range_directory_sz);
//        if (print)
//                	xil_printf("%s: 4\n", __func__);
        /* calculate new fixed-region mapping size*/
        if (min_hole > MIN_REGION){
//        	if (print)
//        	        	xil_printf("%s: 5\n", __func__);
                /* # of headers + # of mappings*/
                n_entries_fixed_region_mappings =
                        wchunk_p->numOfValidMaps + n_active_regions;
        } else {
//				if (print)
//							xil_printf("%s: 6\n", __func__);
                uint32_t n_invalid_mappings, n_headers = 0;
                n_invalid_mappings =
                        GET_REMAINING_INVALID_MAPPING(
                                        wchunk_p->validBits,
                                        first_valid_idx, last_valid_idx,
                                        nmaps_in_new_region, &n_headers);
//                if (print)
//                        	xil_printf("%s: 7\n", __func__);
                n_entries_fixed_region_mappings =
                        wchunk_p->numOfValidMaps
                        + n_invalid_mappings + n_headers;
        }

        fixed_region_mapping_sz =
                n_entries_fixed_region_mappings * sizeof(LOGICAL_SLICE_ENTRY);
//        if (print)
//                	xil_printf("%s: 8\n", __func__);
	new_mnode_sz = new_bitmap_sz + new_range_directory_sz
                + fixed_region_mapping_sz;
//


	if (wchunk_p->sz * 0.7 > new_mnode_sz){
//		if (print)
//		        	xil_printf("%s: 9\n", __func__);
		return CONSTRUCT_COMPACTED_MAP_NODE(wchunk_p,
						first_valid_idx, last_valid_idx,
						nmaps_in_new_region,
						new_range_directory_sz,
						new_bitmap_sz, fixed_region_mapping_sz, n_active_regions);
	} else {
//		if (print)
//		        	xil_printf("%s: 10\n", __func__);
	        return NULL;
	}

}

int c = 0;

void mapseg_mark_valid(MapSegment_p wchunk_p, unsigned int indexInChunk,
                       int length, unsigned int wchunkStartAddr, int isValid,
                       int bitsInSlice, uint32_t parent_addr, bool last_discard) {
    int validBitIndex, validBitSelector, origBits, newBits = 0;
    static MapSegment_p target  = NULL;
    static bool print = false;
    int cnt_ = 8000;
//    if (print && (c % cnt_ == 0)){
//    		xil_printf("shival ------\n");
//    }
    for (int i = 0; i < length; i++) {
        validBitIndex = MAPSEG_VALID_BIT_INDEX((indexInChunk + i));
        validBitSelector =
            MAPSEG_VALID_BIT_SELECTOR((indexInChunk + i), bitsInSlice);

        origBits = wchunk_p->validBits[validBitIndex];

        if (isValid) {
            newBits = origBits | validBitSelector;
	    wchunk_p->numOfWrittenMaps += 1;
        } else {
            newBits = origBits & (~validBitSelector);
        }
        wchunk_p->numOfValidMaps +=
            __builtin_popcountl(newBits) - __builtin_popcountl(origBits);
        wchunk_p->validBits[validBitIndex] = newBits;
    }
//    if (print && (c % cnt_ == 0)){
//    		xil_printf("shival ------ 1\n");
//    }
    // deallocate totally unused chunk
    if (wchunk_p->numOfValidMaps == 0){
        mapseg_deallocate(wchunk_p, wchunkStartAddr);
//        xil_printf("%s: dealloc\n", __func__);
    } else{
//    	if (c%100 == 0)
//    		xil_printf("compaction start!!\n");
//        if (print && (c % cnt_ == 0)){
//        		xil_printf("shival ------ 2\n");
//        }
    	if (last_discard){
			if (no_more_write(wchunk_p) && is_compaction_candidate(wchunk_p)) {
	//    	    if (print && (c % cnt_ == 0) ){
//	    	    		xil_printf("%s: getinto compaction\n", __func__);
	//    	    }
				MapSegment_p new_wchunk_p;
//				xil_printf("%s: last discard idx: %d\n", __func__, indexInChunk);
	//	    	if (c%100 == 0){
				if ((new_wchunk_p = try_compaction(wchunk_p, print)) != NULL){
	//			    if (print && (c % cnt_ == 0) ){
	//			    		xil_printf("shival ------ 4\n");
	//			    }
					/* replace old map segment with compacted one */
					xil_printf("oldsz: %d newsz: %d compaction ratio: %d\n", wchunk_p->sz,
							new_wchunk_p->sz,
							(int) (float(new_wchunk_p->sz) / wchunk_p->sz * 100));
					uint32_t *parent_p = (uint32_t*)parent_addr;
	//				xil_printf("shival 1\n");
					*parent_p = (uint32_t)new_wchunk_p;
	//				xil_printf("shival 2\n");
					target = new_wchunk_p;
					target_saved = new_wchunk_p;
					print = true;
				}
			}
    	}
    	c++;

    }
}

int mapseg_mark_valid_partial(unsigned int logicalSliceAddr, int isValid,
                              int start, int end, bool include_last_discard) {
    unsigned int selectedChunkStartAddr, indexInChunk;
    MapSegment_p selectedChunk;
    int bitsInSlice;
    uint32_t parent_addr;
    bool trash;
    selectedChunk = mapseg_select_map_segment(logicalSliceAddr, 1, &parent_addr, &trash);
    if (selectedChunk == NULL) return 0;
    selectedChunkStartAddr = selectedChunk->startLsa;
    indexInChunk = logicalSliceAddr - selectedChunkStartAddr;

    bitsInSlice = 0;
    for (int i = start; i < end; i++) {
        bitsInSlice += (1 << (3 - i));
    }
    mapseg_mark_valid(selectedChunk, indexInChunk, 1, selectedChunkStartAddr,
                      isValid, bitsInSlice, 0, include_last_discard); /* last argument should be fixed or 16K mapping */

    // return 1 if the entry is totally invalidated, else 0
    return !mapseg_is_valid(selectedChunk, indexInChunk);

}
