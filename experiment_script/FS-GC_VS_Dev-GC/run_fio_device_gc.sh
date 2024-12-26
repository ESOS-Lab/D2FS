#!/bin/bash

MNT=/mnt
DATA=/exp_mysql_data

DEV=(/dev/nvme2n1p1)
DEV_sub=(/dev/nvme2n1p2)
DEV_whole=/dev/nvme2n1
CUR_DIR=$(pwd)
FIO_PATH=(/home/juwon/fio_src)
FILESYSTEM=(raw_device)
OUTPUTDIR="device_gc/fio_exp_output_${FILESYSTEM}_`date "+%Y%m%d"`_`date "+%H%M"`"
IO_TYPE=(randwrite)

NUM_JOBS=(1)
RANDOM_BLOCK_SIZE=(4k) # 4k 8k 16k 32k)
SEQ_BLOCK_SIZE=(4k)   # 64k 256k)

main()
{
	# Create Disk Partition
	echo -e "d\nw" | fdisk ${DEV_whole}
	echo -e "d\nw" | fdisk ${DEV_whole}
	echo -e "n\n\n\n\n+256G\nw" | fdisk ${DEV_whole}
	echo -e "n\n\n\n\n\nw" | fdisk ${DEV_whole}

	# Fill the Empty Partition
	echo -e "y\n" | mkfs.ext4 ${DEV_sub}
	mount ${DEV_sub} /tmp_mnt
	cp -rp ${DATA}/cold_40G /tmp_mnt/cold_40G_1
	cp -rp ${DATA}/cold_40G /tmp_mnt/cold_40G_2
	cp -rp ${DATA}/cold_40G /tmp_mnt/cold_40G_3
	cp -rp ${DATA}/cold_40G /tmp_mnt/cold_40G_4
	cp -rp ${DATA}/cold_40G /tmp_mnt/cold_40G_5
	cp -rp ${DATA}/cold_20G /tmp_mnt/cold_20G
	
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



			   # Run
			   echo "==== Run workload ===="
			   sync
			   echo 3 > /proc/sys/vm/drop_caches 
			   sudo sysctl kernel.randomize_va_space=0;
			  su root -c 'echo STARTTTTTT > /dev/kmsg' 
			   ${FIO_PATH}/fio \
				   	    --filename=${DEV}  \
				   	    --name test \
						--rw=${fs}  \
					    --bs=${block_size} \
					    --filesize=180GB \
					    --numjobs=${numjob} \
						--norandommap \
						--allow_mounted_write=1	\
						--random_generator=tausworthe \
						--randseed=1 \
					    --group_reporting=1 \
					    --log_avg_msec=2000\
					    --write_iops_log=${block_size}\
					    --write_lat_log=${block_size} \
					    --fadvise_hint=0 \
					    --time_based --runtime=1200s \
					    > ${OUTPUTDIR_FS_JOB}/result_${block_size}.txt;

			   
			   echo $'\n'
			   rm new
			   ln -s ${OUTPUTDIR_FS_JOB} new
			   cat ${block_size}_lat.*.log | \
			   awk -F ',' '{print $2}' | sort -n -k 1 > tmp_lat.txt
			   mv tmp_lat.txt ${OUTPUTDIR_FS_JOB}/lat_sum_sorted;
			   mv *.log ${OUTPUTDIR_FS_JOB}/;
			   python ../general_resource/sum.py ${OUTPUTDIR_FS_JOB}/4k_iops. ${numjob} > ${OUTPUTDIR_FS_JOB}/kiops_sum
			   dmesg > ${OUTPUTDIR_FS_JOB}/dmesg

   	  		   echo "fb end";
			   
			   chown -R juwon ${OUTPUTDIR}

			   echo "==== Workload complete ===="

			   echo "==== End the experiment ===="
		   done
	   	done
		done
	done
	
}

main               





