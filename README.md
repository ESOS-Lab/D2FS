# D2FS

This manual is organized as follows:  
1. **Getting Started Instructions**
2. **Detailed Instructions**
3. **Source Code Overview**  

---

## 1. Getting Started Instructions  

### 1-1. How to Start `D2FS`  

To facilitate a smooth artifact evaluation, we have pre-installed the `D2FS` source code from GitHub on the experimental server.  
The `D2FS` source code is located in the `~/D2FS_Artifact` directory in the experimental server.  

You will find that the current branch is set to `D2FS`. Please use the following commands to run the SSD emulator:  
```bash
$ cd ~/D2FS_Artifact/ssd_emulator  
$ sudo ./start_nvmevirt.sh  
```

Once the command completes, use the `lsblk` command to verify that the emulated SSD, `/dev/nvme3n1`, has been successfully created.  

If the device is not created, please reboot the server using the Remote Access Controller.  
`NVMeVirt`, the SSD emulator that we are using may occasionally fail to initialize on our server. We carefully suspect that this might be due to the very early version of `NVMeVirt` we are currently using.  We plan to port `D2FS` to the latest version of `NVMeVirt` in the future to address potential initialization issues and further stabilize its operation.  
If the same error occurs more than three times, we recommend you to recompile the kernel. To do so, run the following commands and then reboot the server. We sincerely apologize for any inconvenience. 

```bash
$ cd ~/D2FS_Artifact/kernel
$ sudo ./build_kernel.sh
```

Now, let's run `D2FS` on the emulated SSD. Please execute the following commands:  

```bash
$ cd ~/D2FS_Artifact/experiment_script/quick_start/
$ sudo ./run_fio_d2fs_short_time.sh
```

The `run_fio_d2fs_short_time.sh` script sets up `D2FS` on the emulated SSD by making the filesystem and mounting it. It then uses the FIO benchmark to perform a 10-second 4 KB random write workload on `D2FS`. Once the benchmark completes, a directory containing the experimental results will be created in `d2fs_data`. You can find the result files in that directory.  



## 2. Detailed Instructions

### 2-1. Artifact Evaluation Procedure

In this Artifact Evaluation, our primary goal is to demonstrate that `D2FS` achieves superior performance compared to other filesystems. The evaluation process for `D2FS` is time-consuming, particularly because the kernel must be recompiled whenever switching filesystems, which takes significant time. To minimize this overhead, we recommend completing all benchmarks for one filesystem before moving on to the next filesystem. The recommended evaluation order is as follows:  

(1) Evaluate `D2FS` across all benchmarks.  
(2) Evaluate `zoned F2FS` across all benchmarks.  
(3) Evaluate `IPLFS` across all benchmarks.  
(4) Evaluate `F2FS` across all benchmarks.  
(5) Compare the performance of device GC and filesystem GC (refer to Fig. 1 in the paper). 

Please note that running a single benchmark takes approximately 20–30 minutes. 
Evaluating all benchmarks for a single file system takes a fair amount of time. If you do not have enough time to test all file systems, you can test only the desired file system and proceed directly to ***2-6. Making Experiment Graph***. Since we already provide data for all file systems, you can still generate the experiment graphs even if you evaluate only a specific file system.


### 2-2. Artifact Repository Structure

The `D2FS` repository includes five branches: `D2FS`, `IPLFS`, `zoned_F2FS`, `F2FS`, and `F2FS-vanilla`.  
The branches `D2FS`, `IPLFS`, `zoned_F2FS`, and `F2FS` correspond to the filesystems evaluated in the paper. The `F2FS-vanilla` branch is used in the experiments presented in the Fig.1 in the paper.   

The difference between `F2FS` and `F2FS-vanilla` lies in their discard command submission policies.  
`F2FS-vanilla` follows the original F2FS behavior, where discard commands are submitted only when the system is idle. In contrast, the `F2FS` branch aggressively submits discard commands.  


The source code is organized into four directories: `kernel`, `mkfs-tools`, `ssd_emulator`, and `experiment_script`.  
The `kernel` directory contains the kernel source code and the kernel compilation script, `build_kernel.sh`.  

The `mkfs-tools` directory includes the source code that makes the filesystem on the disk (`mkfs`) and the compilation script for `mkfs` tools, `build_tools.sh`.  

The `ssd_emulator` directory consists of the SSD emulator source code and the script, `start_nvmevirt.sh`, for compiling and executing the SSD emulator.  


The `experiment_script` directory contains experiment scripts, graph-generating scripts, and experimental data. Below is the information about the directories within `experiment_script`:  

- `FIO`, `TPC-C`, `YCSB-A`, `YCSB-F`, `Fileserver`: These directories contain experiment scripts and data for each benchmark. The contents of each benchmark directory are as follows:  
  - `run_{Benchmark_Name}_{Filesystem_Name}.sh`: This is the script to run the benchmark. By executing this script, you can complete all setup and data extraction necessary for running the benchmark.  
  - `{Filesystem_Name}_data`: This directory stores the experimental data. After completing the experiment, a directory named `{Benchmark_Name}_{Filesystem_Name}_exp_output_{Filesystem_Module_Name}_{Date}_{Time}` will be created within this directory to store the experiment data.  

- `general_resource`: This directory contains scripts (such as filesystem mount scripts and experiment data processing Python code) and filesystem modules used across all benchmarks.  
- `graph`: This directory contains the scripts to generate experimental graphs shown in the paper.  


### 2-3. Performance Evaluation of D2FS across All Benchmarks

We assume that the current branch is `D2FS`. If it is not, please refer to the “2-4. How to switch to the other filesystem” section.

Currently, our artifact requires a system reboot when you finishes running a single benchmark. We deeply apologize for the inconvenience. Please reboot the computer using the Remote Access Controller. Click the Power button at the top of the Virtual Console window and select the “Reset System (warm boot)” option. It will take approximately 2 minutes and 30 seconds for the system to reboot.

Once the system has rebooted, please connect to the experimental server. To run the SSD emulator, execute the following command. :
```bash
$ cd ~/D2FS_Artifact/ssd_emulator
$ sudo ./start_nvmevirt.sh
```

If `/dev/nvme3n1` is created, you can proceed. If it is not created, please reboot the system via the Remote Access Controller and try running the command again. (If the same error occurs more than three times, you will need to recompile the kernel. Please run `build_kernel.sh` with sudo privileges in the `~/D2FS_Artifact/kernel` directory and reboot the system. We deeply apologize for any further inconvenience.)


If the SSD emulator starts successfully, you can proceed to run the FIO benchmark by executing the following command:

```bash
$ cd ~/D2FS_Artifact/experiment_script/FIO/
$ sudo ./run_fio_d2fs.sh
```

To check if FIO is running correctly, use the `dmesg` command. If you see the message "`print_GC_LOG_MEM: mem: ~~~~~~~`" being printed every second, it indicates that the benchmark is functioning correctly.
The FIO benchmark takes 1200 seconds to complete. Once the benchmark is finished, use `dmesg` to check for any error messages. If an error occurs, we would appreciate it if you could try running the experiment again.


Once the experiment is completed, a directory will be created within the `d2fs_data` directory inside the `FIO` directory. The directory will be named in the following format: `{Benchmark_Name}_{Filesystem_Name}_exp_output_{Filesystem_Module_Name}_{Date}_{Time}`. This directory will contain various experimental log files (e.g., `kiops_sum`, `results_4k.txt`, `migration_record_memory_footprint`, etc.).

There is a script named `copy_to_graph_directory.sh` inside the `d2fs_data` directory to transfer the experimental data to the graph-generating directory. Please navigate to the `d2fs_data` directory and execute the following command:

```bash
$ ./copy_to_graph_directory.sh {Experiment_Data_Directory}
# example for {Experiment_Data_Directory}: `fio_D2FS_exp_output_d2fs_20241226_2220/`
```

The FIO experiment has been completed. The remaining benchmark experiments will proceed in a similar manner.

Before starting each benchmark, please reboot the experimental server using the Remote Access Controller and run the SSD emulator. The process for running each benchmark experiment is as follows:


#### TPC-C
```bash
$ cd ~/D2FS_Artifact/experiment_script/TPCC/
$ sudo ./run_tpcc_d2fs.sh
```

#### YCSB-A
```bash
$ cd ~/D2FS_Artifact/experiment_script/YCSB-A/
$ sudo ./run_ycsba_d2fs.sh
```

#### YCSB-F
```bash
$ cd ~/D2FS_Artifact/experiment_script/YCSB-F/
$ sudo ./run_ycsbf_d2fs.sh
```

#### Fileserver
```bash
$ cd ~/D2FS_Artifact/experiment_script/Fileserver/
$ sudo ./run_fileserver_d2fs.sh
```

Sometimes, errors may occur during the benchmark execution. In such cases, please reboot and try running the benchmark again. If the same issue persists more than twice, please recompile the kernel.

Once the experiment for each benchmark is successfully completed, an experiment data directory will be created under `~/D2FS_Artifact/experiment_script/{Benchmark_Name}/d2fs_data/`. 
Please navigate to the `d2fs_data` and run the following command to copy the the created directory to the graph-generating directory:

```bash
$ ./copy_to_graph_directory.sh {Experiment_Data_Directory}
```

Now that all experiments for D2FS are complete.


### 2-4. How to switch to the other filesystem.

To switch to a different filesystem, follow these steps:
(1) Change the git branch
(2) Compile the kernel
(3) Compile mkfs-tools
(4) Update grub
(5) Reboot


(1) Switch to the desired filesystem branch using `sudo` privileges. 

(2) Use the following command to compile the kernel:

```bash
$ cd ~/D2FS_Artifact/kernel
$ sudo ./build_kernel.sh
```

(3) Use the following command to compile the `mkfs` tools:

```bash
$ cd ~/D2FS_Artifact/mkfs-tools/
$ sudo ./build_tools.sh
```

Don't forget to compile `mkfs-tools` after switching the filesystem. Otherwise, you will encounter the following error:
```
mount: /mnt: mount(2) system call failed: Structure needs cleaning.
```

(4) Update GRUB. Open the GRUB configuration file using the following command:
```bash
$ sudo su
$ vi /etc/default/grub 
```

In the GRUB configuration file, you will find the `GRUB_DEFAULT` value for each filesystem in the following format:

```Makefile
# {Filesystem_Name}
GRUB_DEFAULT=”~~~”
```

Uncomment the `GRUB_DEFAULT` value of the target fileesystem that you compiled, and comment out the `GRUB_DEFAULT` values for the other filesytems. After that, run the following command to update GRUB:

```bash
$ sudo update-grub
```

(5) Please reboot the server. 
Now, you will be able to evaluate the new filesystem. Please perform these steps every time you switch to the other filesystem branch.

### 2-5. Performance Evaluation of Other Filesystems across All Benchmarks

Once all benchmarks for D2FS are completed, you can proceed to evaluate other filesystems. The `zoned_F2FS`, `IPLFS`, and `F2FS` branches correspond to the respective filesystems. 

The evaluation process of each benchmark for other filesystems is the same as for D2FS and should be conducted in the following sequence:  
(1) Reboot the server  
(2) Run the SSD emulator  
(3) Execute the experiment script  
(4) Move the experimental result data to the graph-generating directory

Please refer to steps (1) and (2) in the **"2-3. Performance Evaluation of D2FS across All Benchmarks"** section, as they are the same.

(3) The steps to run the experiment script for each benchmark are as follows:

#### FIO
```bash
$ cd ~/D2FS_Artifact/experiment_script/FIO/
$ sudo ./run_fio_{Filesystem_Name}.sh
```

#### TPC-C
```bash
$ cd ~/D2FS_Artifact/experiment_script/TPCC/
$ sudo ./run_tpcc_{Filesystem_Name}.sh
```

#### YCSB-A
```bash
$ cd ~/D2FS_Artifact/experiment_script/YCSB-A/
$ sudo ./run_ycsba_{Filesystem_Name}.sh
```

#### YCSB-F
```bash
$ cd ~/D2FS_Artifact/experiment_script/YCSB-F/
$ sudo ./run_ycsbf_{Filesystem_Name}.sh
```

#### Fileserver
```bash
$ cd ~/D2FS_Artifact/experiment_script/Fileserver/
$ sudo ./run_fileserver_{Filesystem_Name}.sh
```


(4) Once the experiment for each benchmark has been successfully completed, navigate to `~/D2FS_Artifact/experiment_script/{Benchmark_Name}/{filesystem_name}_data/`. Then, execute the following command:

```bash
$ ./copy_to_graph_directory.sh {Experiment_Data_Directory}
```


### 2-6. Making Experiment Graph

In the `~/D2FS_Artifact/experiment_script/graph` directory, there are subdirectories related to each graph from the paper. The naming format for each graph's directory is as follows:

```
Fig-{Figure_Number_in_the_Paper}.{Contents}
```

Once the evaluation for all file systems is completed, you can obtain the graphs from the following directory:
```
Fig-13_and_15.FIO_throughput_and_latency
Fig-14_and_16.Macrobenchmark_throughput_and_latency
Fig-17.GC_latency_breakdown
Fig-12_and_18.FIO_region_util_and_migration_record_size
```

Navigate to the desired directory and run the following command:
```bash
$ ./make_graph.sh
```
This will automatically generate the graph file in the format `Fig-{Figure_Number}.{Contents}.eps`.
To view the graph, use the Virtual Console. Open a terminal in the Virtual Console, navigate to the directory containing the graph, and run the following command:

```bash
$ gv {Graph_File}
```

### 2-7. Performance Comparison between device GC and filesystem GC 

This experiment corresponds to Figure 1 in the paper. Please switch to the `F2FS-vanilla` branch, then compile the kernel, compile mkfs-tools, and update grub. 
If there exists any partition (e.g. `/dev/nvme2n1p1`) in the device `/dev/nvme2n1`, please execute the following command:
```bash
$ cd ~/D2FS_Artifact/experiment_script/FS-GC_VS_Dev-GC/
$ sudo ./clear_device.sh
```
Then please reboot the server. Executing `clear_deivce.sh` and rebooting are important since it ensures the flash pages in the SSD are completely erased. 

To observe the effect of device GC, please execute the following command:
```bash
$ cd ~/D2FS_Artifact/experiment_script/FS-GC_VS_Dev-GC/
$ sudo ./run_fio_device_gc.sh
$ sudo ./clear_device.sh
```
When the experiment is complete, a new experiment data directory will be created in the `device_gc` directory of the current directory. Please navigate to the `device_gc` directory and run `./copy_to_graph_directory.sh` to move the experiment data to the graph-generating directory.

Then, please reboot the server again. 
To observe the effect of Filesystem GC, execute the following command:
```bash
$ cd ~/D2FS_Artifact/experiment_script/FS-GC_VS_Dev-GC/
$ sudo ./run_fio_filesystem_gc.sh
$ sudo ./clear_device.sh
```
When the experiment is complete, a new experiment data directory will be created in the `filesystem_gc` directory of the current directory. Please navigate to the `filesystem_gc` directory and run `copy_to_graph_directory.sh` to move the experiment data to the graph-generating directory.

To view the experiment graph, please run the following command in the Virtual Console:

```bash
$ cd ~/D2FS_Artifact/experiment_script/graph/Fig-1.FS-GC_VS_Dev-GC/
$ ./make_graph.sh
$ gv Fig1-Throughput_of_FS-GC_and_Dev-GC.eps 
```


## 3. Source Code Overview

In this section, we introduce the key parts of our source code.  
The underlying kernel filesystem is based on `F2FS`, and the SSD emulator is implemented on `NVMeVirt` (FAST '23). 
The basic structure of our artifact is similar to `F2FS` and `NVMeVirt`.  
We will focus on explaining the parts that we modified.


### 3-1. Coupled Garbage Collection

#### Call Path

Please refer to the `~/D2FS_Artifact/ssd_emulator/conv_ftl.c`

- **`do_gc()`**
  - `clean_one_flashpg()`
    - `gc_write_page()`  
      - `coupled_gc_write_page()`  
        - **`append_page()`**: Assigns a new physical page address (PPA) and logical block address (LBA) for the data page being migrated by GC.  
          - **`get_next_free_zone()`** 
        - **`buffer_gc_log()`**: Creates a migration record containing the old LBA and the newly assigned LBA.  
        - **`load_gc_log_inflight()`**: Selects migration records that will be copied to the Migration Upcall.

#### Function Descriptions

- **`do_gc()`**  
  Performs garbage collection when free superblocks are insufficient. It selects a victim superblock based on a greedy policy and processes all valid flash pages in the victim superblock using the following call path:  
  `clean_one_flashpg()` → `gc_write_page()` → `coupled_gc_write_page()` → `append_page()`.
  
- **`append_page()`**  
  Determines the new physical page address (PPA) for the data page being relocated during garbage collection and assigns a free logical block address (LBA) in the GC region for LBA remapping.  
  - Returns: The newly assigned PPA.  
  - Stores: The remapped LBA in the memory address provided as the third function argument.  
  - **`get_next_free_zone()`**: If the current logical section is full after assigning an LBA, this function locates and returns a new logical section within the GC region.

- **`buffer_gc_log()`**  
  Creates a migration record that contains the old logical address and the newly assigned logical address for the data page moved during garbage collection.

- **`load_gc_log_inflight()`**  
  When the number of migration records exceeds the threshold (default: 256), this function selects records to be included in the upcall.  
  - Inserts: Selected migration records into a linked list.  
  - Passes: The linked list to the I/O handler thread.


### 3-2. Migration Upcall Submission

Please refer to the `~/D2FS_Artifact/ssd_emulator/io.c`

#### Call Path

- `nvmev_kthread_io()`: The main function executed by the I/O handler thread. 
  - **`__fill_rev_sq_cmd()`**: Generates a migration upcall and inserts it into the Migration Queue.  
  - **`__fill_cq_result()`**: Performs Upcall Piggyback.

#### Function Descriptions

- **`__fill_rev_sq_cmd()`**: Reads the linked list of migration records to be included in the upcall and creates a migration upcall. The migration upcall is then inserted into the Submission Queue (SQ) of the Migration Queue pair.

- **`__fill_cq_result()`**: Performs Upcall Piggyback. Sets the `UPCALL` flag in the completion signal of a regular I/O command. The migration upcall notifies the host by piggybacking on the completion signal of a regular command, without using a dedicated interrupt.


### 3-3. Migration Queue Setup

Please refer to `fs/f2fs/segment.c`, `block/blk-core.c`, `block/blk-mq.c`, `drivers/nvme/host/pci.c` in the `~/D2FS_Artifact/kernel/` directory. 

- `create_migration_io_control()`: This function is called when D2FS is mounted. It creates data structures related to upcall handling. 
  - `setup_reverse_queue()`
    - `submit_bio_setup_rev_queue()`
      - `blk_mq_setup_rev_queue()`
        - `setup_rev_queue()` = `nvme_setup_rev_queue()`
          - **`init_rev_queue_handler()`**: Creates a background thread (called Migration Queue Handler) that reads upcalls from the SQ of the Migration Queue.
          - **`nvme_create_rev_queue()`**: Creates the Migration Queue and passes the location of the Migration Queue to the storage device via an admin command.


### 3-4. Processing Migration Upcall

Please refer to `drivers/nvme/host/pci.c` and `fs/f2fs/segment.c` in the `~/D2FS_Artifact/kernel/` directory. 

#### Identifiying `UPCALL` Flag

`nvme_process_cq()`: The host processes the completion signal of a regular command from the storage device through this function.
- **`should_wakeup_migration_thread()`**: Checks if the `UPCALL` flag is set within the command completion signal.
- `wake_up_interruptible_all()`: If the `UPCALL` flag is set, wakes up the Migration Queue Handler.


#### Delivering the Migration Upcall to the Filesystem

`nvme_rev_queue_handler()`: The Migration Queue Handler runs this function when it wakes up.
- `nvme_process_rev_sq()`
  - `nvme_handle_rev_sqe()`: Reads the Migration Upcall from the SQ of the Migration Queue.
    - `insert_migration_cmd()`: Delivers the contents of the Migration Upcall to the filesystem thread (called the Upcall Handler). 
    - `wakeup_migration_handler()`: Wakes up the Upcall Handler in the filesystem layer.


#### Updating Filesystem State

Upcall Handler thread updates the filesystem state according to the Migration Upcall. 

- `handle_migration_thread()`: Upcall Handler runs this function when it wakes up. 
  - `reflect_migration_entry()`: Proesses a single Migration Upcall delivered from the Migration Queue Handler. 
    - `reflect_data_segment_migration_light()`
      - `reflect_data_page_migration_light()`
        - `__reflect_data_page_migration_light()`: Updates Filesystem Metadata with respect to the migration record [old LBA, new LBA]. 
          - `set_summary()`: Updates Reverse Mapping (Segment Summary in F2FS).  
          - `update_slot_entry()`: Updates Block Bitmap (Segment Information Table in F2FS). 
          - `f2fs_update_data_blkaddr_test()`: Updates Filemapping.  


### 3-5. Migration Upcall Completion

#### Sending Completion Signal of Migration Upcall from the Host to the Storage Device
- `complete_migration_cmds()`: Completes the Migration Upcalls that have been processed when the filesystem finishes checkpoint routine. 
  - `complete_migration_cmd()`: Completes a single Migration Upcall. 
    - `blk_mq_complete_migration_cmd()`
      - `rev_queue_crq()` = nvme_rev_queue_crq()`
        - `nvme_setup_rev_cmd()`: Builds the completion signal of the Migration Upcall
        - `nvme_submit_rev_cmd()`: Inserts the completion signal of the Migration Upcall to the Completion Queue (CQ) of the Migration Queue. 
          - nvme_write_rev_cq_db(): Rings the doorbell of CQ of the Migration Queue. 



#### Processing Completion Signal of Migration Upcall from the Host

- `nvmev_proc_dbs()`: Storage device processes the doorbell. 
  - `nvmev_proc_io_rev_cq()`: This funciton is called when the doorbell of CQ of the Migration Queue rings. 
    - `__nvmev_proc_rev_io()`
      - `proc_rev_io_cmd()` = conv_proc_nvme_rev_io_cmd()`: Processes a single completion signal of the Migration Upcall. 
        - `free_gc_log()`: Free the migration records associated to the completed Migration Upcall. 


### 3-6. Read / Discard Redirection

Please refer to the `~/D2FS_Artifact/ssd_emulator/conv_ftl.c`

#### Read Redirect

- `conv_read()`: This function handles the Read command from the host.
  - **`read_from_lpn_redirector()`**: When the L2P mapping table entry for the logical address requested by the host is NULL, this function references the related migration record and redirects to the newly assigned logical address.
    - **`lookup_lpn_redirector`**: Finds and returns the migration record whose old LBA is equal to the logical address requested in the read.

#### Discard Redirect

- `conv_discard()`: This function handles the Discard command from the host.
  - **`pop_from_lpn_redirector()`**: When the L2P mapping table entry for the logical address requested by the host for discard is NULL, this function references the related migration record and redirects to the newly assigned logical address. The corresponding migration record is then deleted.







