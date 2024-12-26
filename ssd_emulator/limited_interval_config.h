
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
//#define MULTI_PARTITION_MTL
#define MULTI_PARTITION_FTL
#define JWDBG_CONV_FTL
#undef JWDBG_IO
#undef GURANTEE_SEQ_WRITE
#define DISCARD_ENABLED
#undef GC_TEST

#define CMD_CNT

#define EQUAL_IM_MEM

#define ZERO_OP_AREA

/* multi partition */
#ifdef MULTI_PARTITION_FTL  

/* zone mapping */
#define ZONE_MAPPING
#ifdef ZONE_MAPPING

/* coupled gc */
//#define COUPLED_GC
//#define COUPLED_GC_MTL

#define WAF
#define CHIP_UTIL
#undef HOST_GC_OVERHEAD_ANALYSIS 
#undef MG_CMD_CNT
#undef GC_LOG_MEM

#undef GC_LOG_MERGE
#undef SEPARATE_GC_LOG
#undef SHIVAL
#define SHIVAL2
#define SHIVAL3

//#define TWO_GC_PARTITION

#define DEACTIVATE_SLIDING_WINDOW

#ifdef COUPLED_GC_MTL
//#define INIT_NR_MIGRATION_LOG_ENT 64
#define NR_MAX_MIGRATION_LOG 256
#define MIGRATION_LOGS_PER_CMD 256
#define NR_MAX_TRANSLATION_LOG 256
#endif

//#define COUPLED_GC_DEBUG
#undef COUPLED_GC_PRINT
#undef GC_LOG_PRINT
#undef GC_LOG_PRINT2
#define LINE_PRINT

/* migration io */
//#define MIGRATION_IO

#undef MG_HANDLER_DISABLED

#endif
/* ------ zone mapping end -------*/
#endif
/* ------ multi partition end -------*/
