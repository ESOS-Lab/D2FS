
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
#define PCNT 10000
#define MULTI_PARTITION_MTL
#define MULTI_PARTITION_FTL
#define JWDBG_CONV_FTL
#undef JWDBG_IO
#undef GURANTEE_SEQ_WRITE
#define DISCARD_ENABLED

#define GC_LATENCY

#define EQUAL_IM_MEM
#define CHIP_UTIL
#undef ZERO_OP_AREA

/* multi partition */
#ifdef MULTI_PARTITION_FTL  

/* zone mapping */
#define ZONE_MAPPING
#ifdef ZONE_MAPPING

/* coupled gc */
#define COUPLED_GC
#define COUPLED_GC_MTL

#define WAF
#define MG_CMD_CNT
#define CMD_CNT
#define GC_LOG_MEM

#define PRINT_PART_UTIL

#define GC_LOG_MERGE
#define SEPARATE_GC_LOG

#define TWO_GC_PARTITION

#define DEACTIVATE_SLIDING_WINDOW

#ifdef COUPLED_GC_MTL
//#define INIT_NR_MIGRATION_LOG_ENT 64
#define NR_MAX_MIGRATION_LOG 256
#define MIGRATION_LOGS_PER_CMD 256
#define NR_MAX_TRANSLATION_LOG 256
#endif

#undef COUPLED_GC_DEBUG
#undef COUPLED_GC_PRINT
#undef GC_LOG_PRINT
#undef GC_LOG_PRINT2
#undef LINE_PRINT
#undef GC_PRINT

/* migration io */
#define MIGRATION_IO

#undef MG_HANDLER_DISABLED

#endif
/* ------ zone mapping end -------*/
#endif
/* ------ multi partition end -------*/
