#!/bin/bash

USER=juwon
HOME=/home/${USER}
YCSB=${HOME}/lee/YCSB
MNT=/mnt
DATA=/exp_mysql_data
DEV_whole=/dev/nvme3n1
CUR_DIR=$(pwd)
FILESYSTEM=(f2fs)
OUTPUTDIR="f2fs_data/ycsbf_F2FS_exp_output_${FILESYSTEM}_`date "+%Y%m%d"`_`date "+%H%M"`"
RECORDSIZE=(8000)
RECOUNT=(16000000)
OPCOUNT=(10000000)
THREAD=15

main()
{
	# Create Disk Partition
	echo -e "d\nw" | fdisk ${DEV_whole}
	echo -e "n\n\n\n\n\nw" | fdisk ${DEV_whole}
	
	# Create result root directory
	mkdir -p ${OUTPUTDIR}
	
	# Disable ASLR
	echo 0 > /proc/sys/kernel/randomize_va_space
	
	rs=${RECORDSIZE[i]}
	rs_kb=$(expr ${rs} / 1000)
	fieldlength=$(expr ${rs} / 10)

       	echo $'\n'
       	echo "==== Start experiment of mysql  ===="
       	
	# Format and Mount
    	echo "==== Format $dev on $MNT ===="
	systemctl stop mysql
	
	cd ../general_resource
	./f2fs.sh ${FILESYSTEM}
	cd "$CUR_DIR"

        echo "==== Format complete ===="
	cp -rp ${DATA}/cold_20G ${MNT}/
	cp -rp ${DATA}/cold_20G ${MNT}/cold_20G_another
	cp -rp ${DATA}/cold_10G ${MNT}/
	cp -rp ${DATA}/mysql_174G ${MNT}/mysql
	chown -R mysql ${MNT}
	chmod -R 777 ${MNT}
	systemctl start mysql

        echo "==== Run YCSB-F workload ===="
        sync
        echo 3 > /proc/sys/vm/drop_caches
	sudo sysctl kernel.randomize_va_space=0;
	
	cd ${YCSB}
	su root -c 'echo STARTTTTTT > /dev/kmsg' 
	sudo bin/ycsb run jdbc -P ${CUR_DIR}/workloadf -P db.properties \
		       	-p recordcount=${RECOUNT[i]} \
	       		-p fieldlength=${fieldlength} \
			-p operationcount=${OPCOUNT} \
			-p writeallfields=false \
			-threads ${THREAD} \
			-cp mysql-connector-j-8.0.33.jar \
			> ${CUR_DIR}/${OUTPUTDIR}/result.txt

	echo "==== Workload complete ===="
	
	echo "==== Process Result Data ===="
	cd "$CUR_DIR"
	chown -R ${USER} ${OUTPUTDIR}
	echo $'\n'
	rm new
	ln -s ${OUTPUTDIR} new

	dmesg > ${OUTPUTDIR}/dmesg
	number=$(cat ${OUTPUTDIR}/dmesg | grep STARTT | sed 's/\]//g' |  awk '{print $2}') 
				
	cat ${OUTPUTDIR}/dmesg | sed 's/\]//g' | sed 's/\[//g' | awk -v num="$number" '{$1 = $1 - num; print $0}' >${OUTPUTDIR}/dmesg_parsed


	echo "==== End the experiment ===="
}
main
