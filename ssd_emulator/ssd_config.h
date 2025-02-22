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

#include "limited_interval_config.h"

#ifndef _NVMEVIRT_SSD_CONFIG_H
#define _NVMEVIRT_SSD_CONFIG_H

#if (defined MULTI_PARTITION_MTL || defined MULTI_PARTITION_FTL)
#define NO_PARTITION(lba)		(lba >> PARTITION_BITS)
#define PARTITION_START_ADDR(lba)	(NO_PARTITION(lba) * PARTITION_SIZE)
#define OFFSET_PARTITION(lba)		(lba % PARTITION_SIZE)
#define NO_MTL_ZONE(lba)			(lba % PARTITION_SIZE / PGS_PER_MTL_ZONE)
#define NO_MTL_ZONE_IDX(ns, lba)			(lba % PARTITION_SIZE / PGS_PER_MTL_ZONE % ns->n_mtl_zones)
#define OFFSET_MTL(lba)		(lba % PARTITION_SIZE % PGS_PER_MTL_ZONE)

#define NO_ZONE(conv_ftl, lba)	(lba / conv_ftl->ssd->sp.pgs_per_line)

/* For Data Partition */
/*#define DATA_PARTITION_START_OFFSET	 0x100
*/
#define DATA_PARTITION_START_OFFSET	 0x0
#define LOCAL_DATA_PARTITION_START_OFFSET	(DATA_PARTITION_START_OFFSET / SSD_PARTITIONS)
#define NO_LOCAL_PARTITION(local_addr)	\
		((((local_addr) >> LOCAL_PARTITION_BITS) == 0)? \
			0 : ((local_addr - LOCAL_DATA_PARTITION_START_OFFSET) >> LOCAL_PARTITION_BITS))

#define LOCAL_PARTITION_START_ADDR(local_addr)	\
		(NO_LOCAL_PARTITION(local_addr) * LOCAL_PARTITION_SIZE + LOCAL_DATA_PARTITION_START_OFFSET)
#define OFFSET_LOCAL_DATA_PARTITION(local_addr)	\
		((local_addr - LOCAL_DATA_PARTITION_START_OFFSET) % LOCAL_PARTITION_SIZE)
#define LOCAL_PARTITION_START_ADDR_FROM_PARTITION_NO(no_local_partition)	\
		(no_local_partition * LOCAL_PARTITION_SIZE + LOCAL_DATA_PARTITION_START_OFFSET)

#define LPN_FROM_LOCAL_LPN(local_lpn, no_part, nr_parts)	(local_lpn * nr_parts + no_part)
#define LOCAL_LPN_FROM_LPN(local_lpn, nr_parts)	(local_lpn / nr_parts)


/* For Meta Partition */
#define OFFSET_LOCAL_META_PARTITION(local_addr)	(local_addr % LOCAL_PARTITION_SIZE)

#endif

/* SSD Model */
#define INTEL_OPTANE 0
#define SAMSUNG_970PRO 1
#define ZNS_PROTOTYPE 2
#define KV_PROTOTYPE 3

/* SSD Type */
enum {
	SSD_TYPE_NVM,
	SSD_TYPE_CONV,
	SSD_TYPE_ZNS,
	SSD_TYPE_KV,
};

/* Cell Mode */
enum {
	CELL_MODE_SLC,
	CELL_MODE_MLC,
	CELL_MODE_TLC,
};

/* Must select one of INTEL_OPTANE, SAMSUNG_970PRO, or ZNS_PROTOTYPE
 * in Makefile */

#if (BASE_SSD == INTEL_OPTANE)
#define NR_NAMESPACES	1

#define NS_SSD_TYPE_0 SSD_TYPE_NVM
#define NS_CAPACITY_0 (0)
#define NS_SSD_TYPE_1 NS_SSD_TYPE_0
#define NS_CAPACITY_1 (0)
#define MDTS (5)
#elif (BASE_SSD == KV_PROTOTYPE)
#define NR_NAMESPACES	1

#define NS_SSD_TYPE_0 SSD_TYPE_KV
#define NS_CAPACITY_0 (0)
#define NS_SSD_TYPE_1 NS_SSD_TYPE_0
#define NS_CAPACITY_1 (0)
#define MDTS (5)

enum {
	ALLOCATOR_TYPE_BITMAP,
	ALLOCATOR_TYPE_APPEND_ONLY,
};

#define KV_MAPPING_TABLE_SIZE (1024)	// in MiB
#define ALLOCATOR_TYPE ALLOCATOR_TYPE_APPEND_ONLY

#elif  (BASE_SSD == SAMSUNG_970PRO)
#define NR_NAMESPACES	1

#define NS_SSD_TYPE_0 SSD_TYPE_CONV
#define NS_CAPACITY_0 (0)
#define NS_SSD_TYPE_1 NS_SSD_TYPE_0
#define NS_CAPACITY_1 (0)
#define MDTS (6)
#define CELL_MODE (CELL_MODE_MLC)

#define SSD_PARTITIONS       (4)
#define SSD_PARTITION_BITS   (2)
#define NAND_CHANNELS        (8)
#define LUNS_PER_NAND_CH     (2)
#define PLNS_PER_LUN         (1)
#define FLASH_PAGE_SIZE      (32*1024)
#define ONESHOT_PAGE_SIZE    (FLASH_PAGE_SIZE * 1)
#define BLKS_PER_PLN         (8192)
#define BLK_SIZE             (0) /*BLKS_PER_PLN should not be 0 */

#define MAX_CH_XFER_SIZE   (16*1024) /* to overlap with pcie transfer */
#define WRITE_UNIT_SIZE    (512)

#define NAND_CHANNEL_BANDWIDTH	(800ull) //MB/s
#define PCIE_BANDWIDTH			   (3360ull)//MB/s

#define NAND_4KB_READ_LATENCY_LSB (35760 - 6000)  //ns
#define NAND_4KB_READ_LATENCY_MSB (35760 + 6000)  //ns
#define NAND_4KB_READ_LATENCY_CSB (0)  //not used
#define NAND_READ_LATENCY_LSB     (36013 - 6000)
#define NAND_READ_LATENCY_MSB     (36013 + 6000)
#define NAND_READ_LATENCY_CSB     (0) //not used
#define NAND_PROG_LATENCY     (185000)
#define NAND_ERASE_LATENCY    (0)

#define FW_4KB_READ_LATENCY   (21500)
#define FW_READ_LATENCY       (30490)
#define FW_WBUF_LATENCY0      (4000)
#define FW_WBUF_LATENCY1      (460)
#define FW_CH_XFER_LATENCY    (0)

#ifdef ZERO_OP_AREA
#define OP_AREA_PERCENT       (0.0)
#else
#define OP_AREA_PERCENT       (0.07)
#endif

#define WRITE_BUFFER_SIZE   (NAND_CHANNELS * LUNS_PER_NAND_CH * ONESHOT_PAGE_SIZE * 2)
#define WRITE_EARLY_COMPLETION   1

static_assert((ONESHOT_PAGE_SIZE % FLASH_PAGE_SIZE) == 0);
#elif (BASE_SSD == ZNS_PROTOTYPE)
#define NR_NAMESPACES	1

#define NS_SSD_TYPE_0 SSD_TYPE_ZNS
#define NS_CAPACITY_0 (0)
#define NS_SSD_TYPE_1 NS_SSD_TYPE_0
#define NS_CAPACITY_1 (0)
#define MDTS (6)
#define CELL_MODE (CELL_MODE_TLC)

#define SSD_PARTITIONS        (1)
#define SSD_PARTITION_BITS    (0)
#define NAND_CHANNELS         (8)
#define LUNS_PER_NAND_CH      (16)
#define FLASH_PAGE_SIZE       (64*1024)
#define PLNS_PER_LUN          (1) /* not used*/
#define DIES_PER_ZONE         (1)

#if 0 /* Real device configuration. Need to modify kernel to support zone size which is power of 2*/
#define ONESHOT_PAGE_SIZE     (FLASH_PAGE_SIZE * 3)
#define ZONE_SIZE             (96*1024*1024) //byte. kernal only support zone size which is power of 2
#else /* If kernel is not modified, use this config for just testing ZNS*/
#define ONESHOT_PAGE_SIZE     (FLASH_PAGE_SIZE * 2)
#define ZONE_SIZE             (32*1024*1024)
#endif

#define MAX_CH_XFER_SIZE    (FLASH_PAGE_SIZE) /* to overlap with pcie transfer */
#define WRITE_UNIT_SIZE     (ONESHOT_PAGE_SIZE)

#define NAND_CHANNEL_BANDWIDTH	(800ull) //MB/s
#define PCIE_BANDWIDTH			   (3200ull) //MB/s

#define NAND_4KB_READ_LATENCY_LSB (25485)
#define NAND_4KB_READ_LATENCY_MSB (25485)
#define NAND_4KB_READ_LATENCY_CSB (25485)
#define NAND_READ_LATENCY_LSB (40950)
#define NAND_READ_LATENCY_MSB (40950)
#define NAND_READ_LATENCY_CSB (40950)
#define NAND_PROG_LATENCY (1913640)
#define NAND_ERASE_LATENCY (0)

#define FW_4KB_READ_LATENCY (37540 - 7390 + 2000)
#define FW_READ_LATENCY     (37540 - 7390 + 2000)
#define FW_WBUF_LATENCY0    (0)
#define FW_WBUF_LATENCY1    (0)
#define FW_CH_XFER_LATENCY  (413)
#define OP_AREA_PERCENT     (0)

#define WRITE_BUFFER_SIZE   (NAND_CHANNELS * LUNS_PER_NAND_CH * ONESHOT_PAGE_SIZE * 2)
#define WRITE_EARLY_COMPLETION   0

/* Don't touch. BLK_SIZE is caculated by ZONE_SIZE, DIES_PER_ZONE */
#define BLKS_PER_PLN         0 /* BLK_SIZE should not be 0 */
#define BLK_SIZE             (ZONE_SIZE / DIES_PER_ZONE)

static_assert((ZONE_SIZE % DIES_PER_ZONE) == 0);

/* For ZRWA */
#define MAX_ZRWA_ZONES (0)
#define ZRWAFG_SIZE (0)
#define ZRWA_SIZE   (0)
#define ZRWA_BUFFER_SIZE   (0)

static_assert((ONESHOT_PAGE_SIZE % FLASH_PAGE_SIZE) == 0);
#endif // BASE_SSD == ZNS_PROTOTYPE
///////////////////////////////////////////////////////////////////////////

static const uint32_t ns_ssd_type[] = {NS_SSD_TYPE_0, NS_SSD_TYPE_1};
static const uint64_t ns_capacity[] = {NS_CAPACITY_0, NS_CAPACITY_1}; // MB

#define NS_SSD_TYPE(ns) (ns_ssd_type[ns])
#define NS_CAPACITY(ns) (ns_capacity[ns])

/* Still only support NR_NAMESPACES <= 2 */
static_assert(NR_NAMESPACES <= 2);

#endif
