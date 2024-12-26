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

#ifndef _LIB_NVMEV_H
#define _LIB_NVMEV_H

#include <linux/pci.h>
#include <linux/msi.h>
#include <asm/apic.h>

#include "nvme.h"
#include "ssd_config.h"
#include "list.h"

#undef CONFIG_NVMEV_DEBUG_VERBOSE

#define SUPPORT_MULTI_IO_WORKER_BY_SQ	1

/*************************/
#define NVMEV_DRV_NAME "NVMeVirt"

#define NVMEV_INFO(string, args...) \
	printk(KERN_INFO "%s: " string, NVMEV_DRV_NAME, ##args)
#define NVMEV_ERROR(string, args...) \
	printk(KERN_ERR "%s: " string, NVMEV_DRV_NAME, ##args)
#define NVMEV_ASSERT(x) \
	BUG_ON((!(x)))

#ifdef CONFIG_NVMEV_DEBUG_VERBOSE
#define NVMEV_DEBUG(string, args...) \
	printk(KERN_INFO "%s: " string, NVMEV_DRV_NAME, ##args)
#else
#define NVMEV_DEBUG(string, args...)
#endif

#define NR_MAX_IO_QUEUE 72
#define NR_MAX_PARALLEL_IO 16384

#ifdef MIGRATION_IO
#define NR_REV_IO_QUEUE 1
#endif

#define MAX_CID 65536
#define MAX_CID_CNT 4294967296

#define PAGE_OFFSET_MASK (PAGE_SIZE - 1)
#define PRP_PFN(x)	((unsigned long)((x) >> PAGE_SHIFT))

#define KB(k) ((k) * 1024)
#define MB(m) (KB((m) * 1024))

#define BYTE_TO_KB(b) ((b) >> 10)
#define BYTE_TO_MB(b) ((b) >> 20)
#define BYTE_TO_GB(b) ((b) >> 30)

#define MS_PER_SEC(s)	((s) * 1000)
#define US_PER_SEC(s)	(MS_PER_SEC(s) * 1000)
#define NS_PER_SEC(s)	(US_PER_SEC(s) * 1000)

#define LBA_TO_BYTE(lba) ((lba) << 9)
#define BYTE_TO_LBA(byte) ((byte) >> 9)

#define INVALID32 (0xFFFFFFFF)
#define INVALID64 (0xFFFFFFFFFFFFFFFF)
#define ASSERT(X)

struct nvmev_sq_stat {
	unsigned int nr_dispatched;
	unsigned int nr_dispatch;
	unsigned int nr_in_flight;
	unsigned int max_nr_in_flight;
	unsigned long long total_io;
};

struct nvmev_submission_queue {
	int qid;
	int cqid;
	int sq_priority;
	bool phys_contig;

	int queue_size;

	struct nvmev_sq_stat stat;

	struct nvme_command __iomem **sq;
};

struct nvmev_completion_queue {
	int qid;
	int irq_vector;
	bool irq_enabled;
	bool interrupt_ready;
	bool phys_contig;

	spinlock_t entry_lock;
	spinlock_t irq_lock;

	int queue_size;

	int phase;
	int cq_head;
	int cq_tail;

	struct nvme_completion __iomem **cq;
};

#ifdef MIGRATION_IO
struct nvmev_rev_completion_queue {
	int qid;
	int sqid;
	int cq_priority;
	bool phys_contig;

	int queue_size;

	struct nvmev_sq_stat stat;
	
	int cq_tail;

	struct nvme_rev_completion __iomem **cq;
};

struct mg_pair_batch {
	struct mg_pair mg_pairs[NR_MG_PAIR];
};

struct nvmev_rev_submission_queue {
	int qid;
	int irq_vector;
	bool irq_enabled;
	bool interrupt_ready;
	bool phys_contig;

	spinlock_t entry_lock;
	spinlock_t irq_lock;

	int queue_size;

	int phase;
	int sq_head;
	int sq_tail;

	struct nvme_mg_command __iomem **sq;

	int mgb_head;
	int mgb_tail;

	int nr_mg_batch;
	struct mg_pair_batch __iomem **mpb;
};
#endif

struct nvmev_admin_queue {
	int phase;

	int sq_depth;
	int cq_depth;

	int cq_head;

	struct nvme_command __iomem **nvme_sq;
	struct nvme_completion __iomem **nvme_cq;
};

#define NR_SQE_PER_PAGE	(PAGE_SIZE / sizeof(struct nvme_command))
#define NR_CQE_PER_PAGE (PAGE_SIZE / sizeof(struct nvme_completion))

#define SQ_ENTRY_TO_PAGE_NUM(entry_id) (entry_id / NR_SQE_PER_PAGE)
#define CQ_ENTRY_TO_PAGE_NUM(entry_id) (entry_id / NR_CQE_PER_PAGE)

#define SQ_ENTRY_TO_PAGE_OFFSET(entry_id) (entry_id % NR_SQE_PER_PAGE)
#define CQ_ENTRY_TO_PAGE_OFFSET(entry_id) (entry_id % NR_CQE_PER_PAGE)

#ifdef MIGRATION_IO

#define NR_REV_SQE_PER_PAGE	(PAGE_SIZE / sizeof(struct nvme_mg_command))
#define NR_REV_CQE_PER_PAGE (PAGE_SIZE / sizeof(struct nvme_rev_completion))

#define REV_SQ_ENTRY_TO_PAGE_NUM(entry_id) (entry_id / NR_REV_SQE_PER_PAGE)
#define REV_CQ_ENTRY_TO_PAGE_NUM(entry_id) (entry_id / NR_REV_CQE_PER_PAGE)

#define REV_SQ_ENTRY_TO_PAGE_OFFSET(entry_id) (entry_id % NR_REV_SQE_PER_PAGE)
#define REV_CQ_ENTRY_TO_PAGE_OFFSET(entry_id) (entry_id % NR_REV_CQE_PER_PAGE)

#define NR_REV_MGB_PER_PAGE	(PAGE_SIZE / sizeof(struct mg_pair_batch))

#define REV_MGB_ENTRY_TO_PAGE_NUM(entry_id) (entry_id / NR_REV_MGB_PER_PAGE)
#define REV_MGB_ENTRY_TO_PAGE_OFFSET(entry_id) (entry_id % NR_REV_MGB_PER_PAGE)

#endif

struct nvmev_config {
	unsigned long memmap_start; // byte
	unsigned long memmap_size;	// byte

	unsigned long storage_start; //byte
	unsigned long storage_size;	// byte

	unsigned int read_delay;	// ns
	unsigned int read_time;		// ns
	unsigned int read_trailing;	// ns
	unsigned int write_delay;	// ns
	unsigned int write_time;	// ns
	unsigned int write_trailing;// ns

	unsigned int nr_io_units;
	unsigned int io_unit_shift;	// 2^

	unsigned int cpu_nr_dispatcher;
	unsigned int nr_io_cpu;
	unsigned int cpu_nr_proc_io[32];
};

struct nvmev_proc_table {
	int sqid;
	int cqid;

	int sq_entry;
	unsigned int command_id;

	unsigned long long nsecs_start;
	unsigned long long nsecs_target;

	unsigned long long nsecs_enqueue;
	unsigned long long nsecs_copy_start;
	unsigned long long nsecs_copy_done;
	unsigned long long nsecs_cq_filled;

	bool is_copied;
	bool is_completed;

	unsigned int status;
	unsigned int result0;
	unsigned int result1;

	bool writeback_cmd;
	void * write_buffer;
	unsigned int buffs_to_release;

	unsigned int next, prev;
#ifdef COUPLED_GC_MTL
	struct list mtl_migration_list;
	struct list mtl_translation_list;
	struct list mtl_read_translation_list[SSD_PARTITIONS];
#endif
#ifdef MIGRATION_IO
	//struct list mg_batch_list;		/* to convey migration i/o info to io thread */
	//struct inflight_set_entry *ise;
	struct list ise_list;
#endif
	unsigned long long order;
	unsigned int migration_cnt;
};

struct nvmev_proc_info {
	struct nvmev_proc_table *proc_table;

	unsigned int free_seq;		/* free io req head index */
	unsigned int free_seq_end;	/* free io req tail index */
	unsigned int io_seq;		/* io req head index */
	unsigned int io_seq_end;	/* io req tail index */
	unsigned int id;

	unsigned long long proc_io_nsecs;

	struct task_struct *nvmev_io_worker;
	char thread_name[32];
};

struct nvmev_dev {
	struct pci_bus *virt_bus;
	void *virtDev;
	struct pci_header *pcihdr;
	struct pci_pm_cap *pmcap;
	struct pci_msix_cap *msixcap;
	struct pcie_cap *pciecap;
	struct aer_cap *aercap;
	struct pci_exp_hdr *pcie_exp_cap;

	struct pci_dev *pdev;
	struct pci_ops pci_ops;
	struct pci_sysdata pci_sd;

	struct nvmev_config config;
	struct task_struct *nvmev_manager;

	void *storage_mapped;

	struct nvmev_proc_info *proc_info;
	unsigned int proc_turn;

	bool msix_enabled;
	void __iomem *msix_table;

	struct __nvme_bar *old_bar;
	struct nvme_ctrl_regs __iomem *bar;

	u32 *old_dbs;
	u32 __iomem *dbs;

	int nr_ns;
	int nr_sq, nr_cq;

	struct nvmev_admin_queue *admin_q;
	struct nvmev_submission_queue *sqes[NR_MAX_IO_QUEUE + 1];
	struct nvmev_completion_queue *cqes[NR_MAX_IO_QUEUE + 1];

#ifdef MIGRATION_IO
	struct nvmev_rev_submission_queue *rev_sqe;
	struct nvmev_rev_completion_queue *rev_cqe;
#endif

	struct proc_dir_entry *proc_root;
	struct proc_dir_entry *proc_read_times;
	struct proc_dir_entry *proc_write_times;
	struct proc_dir_entry *proc_io_units;
	struct proc_dir_entry *proc_stat;

	unsigned long long *io_unit_stat;

	struct nvmev_ns * ns;
	int mdts;
};

struct nvmev_request {
	struct nvme_command * cmd;
	uint32_t sq_id;
	uint64_t nsecs_start;
};

struct nvmev_result {
	uint32_t status;
	uint64_t nsecs_target;
	uint32_t early_completion;
	uint64_t wp; // only for zone append
#ifdef COUPLED_GC_MTL
	struct list mtl_migration_list;		/* to convey migrated lba to mtl */
	struct list mtl_translation_list;	/* to convey translated lba from aimless translator to mtl */

	/* since read is not translated sequentially in nvmevirt, we need devoted translatin list for read */
	//struct list mtl_read_translation_list[SSD_PARTITIONS];	/* translation list for read. */
#endif
#ifdef MIGRATION_IO
	//struct inflight_set_entry *ise;
	struct list ise_list;	/* to convey translated lba from aimless translator to mtl */
	//struct list mg_batch_list;		/* to convey migration i/o info to io thread */
#endif
	unsigned int cid;
	unsigned long long order;
	unsigned int migration_cnt;
};

#ifdef MULTI_PARTITION_MTL
#define NO_TYPE			7
//#define NO_TYPE			8

#define PARTITION_SIZE		0x20000000
#define PARTITION_BITS		29

#define LOCAL_PARTITION_BITS		(PARTITION_BITS - SSD_PARTITION_BITS)
#define LOCAL_PARTITION_SIZE		(PARTITION_SIZE / SSD_PARTITIONS)

//#define	WINDOW_EXT_RATE		25
//#define	WINDOW_EXT_RATE		18
#define	WINDOW_EXT_RATE		6
#define MEM_EXT_RATE		1
#define INVALID_MAPPING		NULL
#define MAX_KMALLOC_SIZE	MB(1)
#define MTL_ZONE_SIZE		(MAX_KMALLOC_SIZE - sizeof(struct mtl_zone_info))

#define NO_USER_PARTITION	7	/* # of meta + data partition */
#define NO_GC_WP	3	/* # of meta + data partition */

#define IS_GC_PARTITION(partno)	\
	((partno == GC_PARTITION))

#define META_PARTITION		0
#define GC_PARTITION		7
#define HOT_DATA_PARTITION		1
#define WARM_DATA_PARTITION		2
#define COLD_DATA_PARTITION		3
#define HOT_NODE_PARTITION		4
#define WARM_NODE_PARTITION		5
#define COLD_NODE_PARTITION		6

#define IS_NODE_PARTITION(n)	(COLD_DATA_PARTITION < n && n <= COLD_NODE_PARTITION)
#define IS_DATA_PARTITION(n)	(META_PARTITION < n && n <= COLD_DATA_PARTITION)

#define META_GC_WP	0
#define DATA_GC_WP	1
#define NODE_GC_WP	2

#define IS_META_PARTITION(n)	(META_PARTITION == n)

#define IS_MAIN_PARTITION(n)	(META_PARTITION < n && n < GC_PARTITION)

#define START_OFS_IN_MAIN_PART	0x100

#define META_PARTITION_RATE	32		/* for f2fs */
#define NPAGES_META(sp)		(sp.tt_pgs / META_PARTITION_RATE)
#define NPAGES_MAIN(sp)		(sp.tt_pgs * WINDOW_EXT_RATE)

/////////////////////
#ifdef ZONE_MAPPING

#ifdef TWO_GC_PARTITION
#define NO_USER_PARTITION	8	/* # of meta + data partition */
#else
#define NO_USER_PARTITION	7	/* # of meta + data partition */
#endif


#define IS_GC_PARTITION(partno)	\
	((partno == GC_PARTITION) || (partno == COLD_DATA_PARTITION))

#define IS_HOST_PARTITION(partno) \
	((partno == HOT_DATA_PARTITION) || (partno == HOT_NODE_PARTITION))

#define IS_META_PARTITION(n)	(META_PARTITION == n)

#ifdef TWO_GC_PARTITION
#define IS_MAIN_PARTITION(n)	(META_PARTITION < n)
#else
#define IS_MAIN_PARTITION(n)	(META_PARTITION < n && n < GC_PARTITION)
#endif

#define IS_DATA_PARTITION(n)	(META_PARTITION < n && n <= COLD_DATA_PARTITION)

#ifdef TWO_GC_PARTITION
#define IS_NODE_PARTITION(n)	(COLD_DATA_PARTITION < n && n <= GC_PARTITION)
#else
#define IS_NODE_PARTITION(n)	(COLD_DATA_PARTITION < n && n <= COLD_NODE_PARTITION)
#endif

#define NZONES_PER_PARTITION(sp)	(sp.tt_lines * WINDOW_EXT_RATE)
//#define NZONES_PER_GC_PARTITION(sp)	(sp.tt_lines)
#define NZONES_PER_GC_PARTITION(sp)	(sp.tt_lines * WINDOW_EXT_RATE)
#define START_OFS_IN_MAIN_PART	0x100

#endif
#ifdef MEM_CALC
#define MSblks (4096*16*16)
#ifdef MEM_CALC_32BIT
#define MAP_SZ	4
#else
#define MAP_SZ	8
#endif
#define RANGE_DIR_SZ	2
#define default_MS_sz (10 + 4 + MAP_SZ * (1 + MSblks))
#endif

typedef struct mem_page_entry {
	/* list_elem should be first member due to the container_of function.*/
	struct list_elem	list_elem;
	uint64_t	mem_addr;

} MEM_PAGE_ENTRY ;

typedef MEM_PAGE_ENTRY *	MTL_ENTRY;
#define PGS_PER_MTL_ZONE	(MTL_ZONE_SIZE/sizeof(MTL_ENTRY))

/* mtl_zone_info size should be same with MTL_ENTRY */
struct mtl_zone_info {
	uint32_t nr_inv_pgs;
	uint32_t nr_v_pgs;
	//uint32_t rsvd;
};

#define PGS_PER_MTL_ZONE	(MTL_ZONE_SIZE/sizeof(MTL_ENTRY))

struct mtl_zone_entry {
	struct mtl_zone_info zone_info;
	MTL_ENTRY map_table[PGS_PER_MTL_ZONE];
};

#endif

#ifdef COUPLED_GC
#define HBITS_AIMLESS_TRANSLATOR 18
#define PGS_PER_FS_SEGMENT		512
#define	NO_INIT_GC_LOG			(PGS_PER_FS_SEGMENT * 1024)

#ifdef MIGRATION_IO
#define HBITS_INFLIGHT_GC_LOG_HTABLE	8
//#define MIGRATION_THRESHOLD		(2 * 1024)
//#define MIGRATION_THRESHOLD		(2*1024 * 1024)
#define MIGRATION_THRESHOLD		(512 * 1024)
#define RATIO_OF_GC_LOG_TO_PAGE_MAP		10	/* Percent */
#endif

//enum gc_log_status {
//	GC_LOG_FREE,
//	GC_LOG_BUFFERED,
//	GC_LOG_INFLIGHT
//};
#define GC_LOG_FREE 0
#define GC_LOG_BUFFERED 1
#define GC_LOG_INFLIGHT	2

struct gc_log {
	uint64_t old_lpn;		/* key */
	uint64_t new_lpn;
	//enum gc_log_status status;
	char status;
	struct list_elem list_elem;	/* buffered or inflight gc log list entry */
	struct hlist_node hnode;	/* aimless entry */
#ifdef GC_LOG_MERGE
	struct hlist_node hnode_merge;
#endif
};


/* TODO: need to set NR_INFLIGHT_SET as rev completion queue size */
#ifdef MIGRATION_IO
#define NR_INFLIGHT_SET	8192
#define INVALID_COMMAND_ID     (0xFFFFFFFF)
#define NULL_SECNO     (0xFFFFFFFF)

struct inflight_set_entry {
	unsigned int command_id;		/* key */
	//unsigned int nr;
	struct list_elem list_elem;	/* buffered or inflight gc log list entry */
	struct list gc_log_list;		/* containing gc logs in a command */
	//struct hlist_node hnode;		/* to remove set after complet i/o for mg_cmd */
};

#endif
#define NR_PGS_IN_MG_POOL 2048
struct gc_log_mgmt {
	unsigned int n_free;
	unsigned int n_buffered;
	unsigned int n_inflight;
	unsigned int n_total;
	unsigned int buffering_cnt;	/* total buffering cnt. (buffered only) */
#ifdef GC_LOG_MEM
	unsigned int buffering_trial_cnt; /* total buffering trial cnt. (buffered + merged) */
#endif

	struct kmem_cache *gc_log_slab;
	
	struct list free_gc_log_list;
	struct list buffered_gc_log_list;

	unsigned int hbits;	/* aimless translator hash bits. */
#ifdef MIGRATION_IO
	struct kmem_cache *inflight_set_slab;
	unsigned int n_ise;	/* number of inflight set entry */

	//struct kmem_cache *mg_batch_slab;
	uint64_t next_command_id;	
	uint64_t completed_command_id;	
	struct inflight_set_entry ise_array[NR_INFLIGHT_SET];
#endif
};
#endif

#ifdef COUPLED_GC_MTL
/* migration log */
struct mg_log {
	uint64_t old_lpn;
	uint64_t new_lpn;
};

struct mg_entry {
	uint64_t nr_log;
	struct mg_log log_buf[NR_MAX_MIGRATION_LOG];
	struct list_elem	list_elem;
};

/* translation log */
struct trans_log {
	uint64_t old_lpn;
	uint64_t new_lpn;
};

struct trans_entry {
	uint64_t nr_log;
	uint64_t cur_idx;
	struct trans_log log_buf[NR_MAX_TRANSLATION_LOG];
	struct list_elem	list_elem;
};

#endif

#ifdef MIGRATION_IO

/*struct mg_batch_entry {
	unsigned int command_id;
	unsigned int nr;
	struct mg_pair mg_pairs[NR_MG_PAIR];
	struct list_elem list_elem;
};*/

#endif

#ifdef MEM_CALC
#define MNODES_PER_ZNODE	1024
#define	ZNODE_SIZE			(1024 * MAP_SZ)
struct ms{
        bool is_alloc;
        bool is_discard;
        bool is_dirty;
        int size;
        int trunc_size;
        int valid_cnt;

        char *bitmap;
};

struct ms_info{
        uint64_t global_sLBA;
        uint64_t global_eLBA;
        int global_sMSidx;
        int global_eMSidx;
        int trimmed_start_MSidx;
#ifdef PARTIAL_COMPACTION
        int last_compaction_sidx;
        int last_compaction_eidx;
#endif

#ifdef COMPACTION_DIRTY_ONLY
	int n_dirty_ms;
	struct list	dirty_ms_list;		/* free page list */
#endif

	int compacted_memory;
        int compacted_size;
        int truncated_memory;
        int dealloc_ms;

        struct ms *ms;

        uint16_t *znode_cnt;
	unsigned int dealloc_znode;
};


#ifdef COMPACTION_DIRTY_ONLY
struct dirty_ms_entry {
	unsigned int ms_idx;		/* key */
	struct list_elem list_elem;	/* buffered or inflight gc log list entry */
};
#endif

#endif


struct nvmev_ns {
	uint32_t id;
	uint32_t csi;
	uint64_t size;	/* logical space size. maybe ramdisk size */
	void * mapped;

	/*conv ftl or zns or kv*/
	uint32_t nr_parts; // partitions
	void * ftls; 	   // ftl instances. one ftl per partition

	/*io command handler*/
	bool (*proc_io_cmd)(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret);
#ifdef MIGRATION_IO
	void (*proc_rev_io_cmd)(struct nvmev_ns *ns, struct nvme_rev_completion *cmd);
#endif
	/*specific CSS io command identifier*/
	bool (*identify_io_cmd)(struct nvmev_ns *ns, struct nvme_command cmd);
	/*specific CSS io command processor*/
	unsigned int (*perform_io_cmd)(struct nvmev_ns *ns, struct nvme_command *cmd, uint32_t *status);

#ifdef MULTI_PARTITION_MTL
	uint64_t window_size;		/* mapping table size */
	//MTL_ENTRY **	mtls[NO_TYPE];	/* ramdisk mapping table */
	struct mtl_zone_entry ** mtls[NO_TYPE];	/* ramdisk mapping table */
	struct list	free_mem_page_list;		/* free page list */
	uint64_t		n_mtl_zones;
	uint64_t start_zoneno[NO_TYPE]; /* start zone number of each partition. support sliding window */
#endif
#ifdef COUPLED_GC
	struct gc_log_mgmt *gclm;
#ifdef COUPLED_GC_MTL
	struct kmem_cache *mtl_migration_entry_slab;
	struct kmem_cache *mtl_translation_entry_slab;
#endif
#endif
#ifdef WAF
	unsigned long long last_t;
	unsigned long long last_compaction_t;
	unsigned long long write_volume_host; /* 4KB Blocks */
	unsigned long long write_volume_gc; /* 4KB Blocks */
	unsigned long long total_write_volume_host; /* 4KB Blocks */
	unsigned long long total_write_volume_gc; /* 4KB Blocks */
#endif
#ifdef MG_CMD_CNT
	unsigned long long mg_cmd_cnt; 
	unsigned long long total_mg_cmd_cnt; 
#endif
#ifdef MEM_CALC
        struct ms_info *ms_infos;
#endif
	//unsigned int n_gc_log_max; /* migration threshold */
#ifdef MEASURE_TAIL
	unsigned int tail_lba[NO_TYPE];
#endif

};

// VDEV Init, Final Function
struct nvmev_dev *VDEV_INIT(void);
void VDEV_FINALIZE(struct nvmev_dev *vdev);

// OPS_PCI
void nvmev_proc_bars(void);
bool NVMEV_PCI_INIT(struct nvmev_dev *dev);
void nvmev_signal_irq(int msi_index);

// OPS ADMIN QUEUE
void nvmev_proc_admin_sq(int new_db, int old_db);
void nvmev_proc_admin_cq(int new_db, int old_db);

// OPS I/O QUEUE
void NVMEV_IO_PROC_INIT(struct nvmev_dev *vdev);
void NVMEV_IO_PROC_FINAL(struct nvmev_dev *vdev);
int nvmev_proc_io_sq(int qid, int new_db, int old_db);
void nvmev_proc_io_cq(int qid, int new_db, int old_db);
#ifdef MIGRATION_IO
int nvmev_proc_io_rev_cq(int cqid, int new_db, int old_db);
#endif
#endif /* _LIB_NVMEV_H */
