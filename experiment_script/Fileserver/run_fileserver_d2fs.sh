#!/bin/bash


WORKLOAD=fileserver
FILEBENCH_PATH=/home/juwon/filebench
MNT=/mnt

DEV_whole=/dev/nvme3n1
CUR_DIR=$(pwd)
FILESYSTEM=(d2fs)
OUTPUTDIR="d2fs_data/filebench_D2FS_exp_output_"${WORKLOAD}_${FILESYSTEM}"_`date "+%Y%m%d"`_`date "+%H%M"`"
#FILESYSTEM=(d2fs)
IO_TYPE=(randwrite)
NUM_JOBS=(4)

RANDOM_BLOCK_SIZE=(4k) # 4k 8k 16k 32k)
SEQ_BLOCK_SIZE=(4k)   # 64k 256k)

main()
{
	# Create Disk Partition
	echo -e "d\nw" | fdisk ${DEV_whole}
	echo -e "n\n\n\n\n\nw" | fdisk ${DEV_whole}
	
	# Create result root directory
	mkdir -p ${OUTPUTDIR}

	# Disable ASLR
	echo 0 > /proc/sys/kernel/randomize_va_space

	# Create directory for device
	for fs in ${IO_TYPE[@]}
	do
		# Set a filesystem result name
		OUTPUTDIR_FS=${OUTPUTDIR}/${fs}

		# Craete directory for filesystem
		mkdir ${OUTPUTDIR_FS}

 	    case $fs in
		    "write")
				BLOCK_SIZE=${SEQ_BLOCK_SIZE[@]}
			    ;;
			"randwrite")
				BLOCK_SIZE=${RANDOM_BLOCK_SIZE[@]}
				;;
			"trimwrite")
			    BLOCK_SIZE=${SEQ_BLOCK_SIZE[@]}
				;;
			"read")
			    BLOCK_SIZE=${SEQ_BLOCK_SIZE[@]}
				;;
			"randread")
			    BLOCK_SIZE=${RANDOM_BLOCK_SIZE[@]}
				;;
	    esac
	    for filesys in ${FILESYSTEM[@]}
	    	do
	    for numjob in ${NUM_JOBS[@]}
		do
		   # Set a number of jobs result name
		   OUTPUTDIR_FS_JOB=${OUTPUTDIR_FS}/${filesys}/${numjob}

		   # Create dirctory for numjob
		   mkdir -p ${OUTPUTDIR_FS_JOB}

		   for block_size in ${BLOCK_SIZE}
		   do
		 	   echo $'\n'
			   echo "==== Start experiment of ${block_size} fio ===="

			   # Format and Mount
			   echo "==== Format $dev on $MNT ===="
				cd ../general_resource
				./d2fs.sh ${filesys}
				cd "$CUR_DIR"
			   echo "==== Format complete ===="


			   # Run
			   echo "==== Run workload ===="
			   sudo echo 0 > /proc/sys/kernel/randomize_va_space
			   sync
			   echo 3 > /proc/sys/vm/drop_caches 
			   sudo sysctl kernel.randomize_va_space=0;
			   su root -c 'echo STARTTTTTT > /dev/kmsg'
			   sudo ${FILEBENCH_PATH}/filebench -f ${WORKLOAD}.f 1> ${OUTPUTDIR_FS_JOB}/result.txt;
			   echo "==== Workload complete ===="
			   echo "==== Process Result Data ===="
			   echo $'\n'
			   rm new
			   ln -s ${OUTPUTDIR_FS_JOB} new
			   dmesg > ${OUTPUTDIR_FS_JOB}/dmesg
			   number=$(cat ${OUTPUTDIR_FS_JOB}/dmesg | grep STARTT | sed 's/\]//g' |  awk '{print $2}') 

		           cat ${OUTPUTDIR_FS_JOB}/dmesg |  sed 's/\]//g' | sed 's/\[//g' | awk -v num="$number" '{$1 = $1 - num; print $0}' > ${OUTPUTDIR_FS_JOB}/dmesg_parsed

		           cat ${OUTPUTDIR_FS_JOB}/dmesg_parsed | grep MG_CMD_CNT > ${OUTPUTDIR_FS_JOB}/mgcmd_only
			   echo "# timestamp	command count (MB)\n" > ${OUTPUTDIR_FS_JOB}/migration_command_count
		           cat ${OUTPUTDIR_FS_JOB}/mgcmd_only | awk '{print $1, $6}'  >> ${OUTPUTDIR_FS_JOB}/migration_command_count
		           rm ${OUTPUTDIR_FS_JOB}/mgcmd_only
		
		           cat ${OUTPUTDIR_FS_JOB}/dmesg_parsed | grep GC_LOG_MEM > ${OUTPUTDIR_FS_JOB}/gclog_mem_only
			   echo "# timestamp	memory footprint (MB)\n" > ${OUTPUTDIR_FS_JOB}/migration_record_memory_footprint 
		           cat ${OUTPUTDIR_FS_JOB}/gclog_mem_only | awk '{print $1, $4}'  >> ${OUTPUTDIR_FS_JOB}/migration_record_memory_footprint
		           rm ${OUTPUTDIR_FS_JOB}/gclog_mem_only
		           
			   cat ${OUTPUTDIR_FS_JOB}/dmesg_parsed | grep 'GC_LATENCY: latency:' > ${OUTPUTDIR_FS_JOB}/gc_latency_breakdown
			   echo "# timestamp	GC Total Latency (msec)		Read Latency (msec)	Write Latency (msec)\n" > ${OUTPUTDIR_FS_JOB}/GC_latency_breakdown
		           cat ${OUTPUTDIR_FS_JOB}/gc_latency_breakdown | sed 's/)//g' | sed 's/(//g' | awk '{print $1, $4, $6, $8}'  >> ${OUTPUTDIR_FS_JOB}/GC_latency_breakdown
		           rm ${OUTPUTDIR_FS_JOB}/gc_latency_breakdown
			   
			   cat ${OUTPUTDIR_FS_JOB}/dmesg_parsed | grep 'section utilization of regular region' > ${OUTPUTDIR_FS_JOB}/sec_util_regular_region
			   echo "# timestamp	utilization (%) \n" > ${OUTPUTDIR_FS_JOB}/section_utilization_regular_region
		           cat ${OUTPUTDIR_FS_JOB}/sec_util_regular_region | awk '{print $1, $7}'  >> ${OUTPUTDIR_FS_JOB}/section_utilization_regular_region
			   rm ${OUTPUTDIR_FS_JOB}/sec_util_regular_region
			   
			   cat ${OUTPUTDIR_FS_JOB}/dmesg_parsed | grep 'section utilization of gc region' > ${OUTPUTDIR_FS_JOB}/sec_util_gc_region
			   echo "# timestamp	utilization (%) \n" > ${OUTPUTDIR_FS_JOB}/section_utilization_gc_region
		           cat ${OUTPUTDIR_FS_JOB}/sec_util_gc_region | awk '{print $1, $7}'  >> ${OUTPUTDIR_FS_JOB}/section_utilization_gc_region
			   rm ${OUTPUTDIR_FS_JOB}/sec_util_gc_region
	

			   cat ${OUTPUTDIR_FS_JOB}/result.txt | grep -e Summary > ${OUTPUTDIR_FS_JOB}/result_time
			   cat ${OUTPUTDIR_FS_JOB}/result_time | awk 'BEGIN {t=5} {print t, $6/1000} {t+=5}' > ${OUTPUTDIR_FS_JOB}/kiops_sum
			   rm ${OUTPUTDIR_FS_JOB}/result_time

			   chown -R juwon ${OUTPUTDIR}

			   echo "==== End the experiment ===="
		   done
	   	done
		done
	done
	
}

main               





