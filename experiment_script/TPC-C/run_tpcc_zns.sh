#!/bin/bash

USER=juwon
HOME=/home/${USER}
TPCC=${HOME}/lee/tpcc-mysql
MNT=/mnt
DATA=/exp_mysql_data
DEV_whole=/dev/nvme3n1
CUR_DIR=$(pwd)
FILESYSTEM=(zns)
OUTPUTDIR="zns_data/tpcc_ZNS_exp_output_${FILESYSTEM}_`date "+%Y%m%d"`_`date "+%H%M"`"

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

       	echo $'\n'
       	echo "==== Start experiment of mysql  ===="
       	
	# Format and Mount
    	echo "==== Format $dev on $MNT ===="
	systemctl stop mysql
	
	cd ../general_resource
	./f2fs_zns.sh ${FILESYSTEM}
	cd "$CUR_DIR"

        echo "==== Format complete ===="
	cp -rp ${DATA}/cold_40G ${MNT}/
	cp -rp ${DATA}/cold_20G ${MNT}/
	cp -rp ${DATA}/mysql-tpcc-155G ${MNT}/mysql
		
	echo "==== Cold File Created ===="
	chown -R mysql ${MNT}
	chmod -R 777 ${MNT}
	systemctl start mysql

	echo "==== Run TPC-C workload ===="
	sync
	echo 3 > /proc/sys/vm/drop_caches
	sudo sysctl kernel.randomize_va_space=0;
	
	cd ${TPCC}	
	su root -c 'echo STARTTTTTT > /dev/kmsg' 
	./tpcc_start -h127.0.0.1 -P3306 -dtpcc1000 -ujw -w1700 -c15 -r10 -l1800 -i1800 -poslab0810 > ${CUR_DIR}/${OUTPUTDIR}/result.txt

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
	
	cat ${OUTPUTDIR}/dmesg_parsed | grep 'GC_LATENCY: latency:' > ${OUTPUTDIR}/gc_latency_breakdown
	echo "# timestamp	GC Total Latency (msec)		CP	Meta	Filemap	Cache	Read	Write\n" > ${OUTPUTDIR}/GC_latency_breakdown
	cat ${OUTPUTDIR}/dmesg_parsed | grep 'gc_latency_breakdown total' | awk '{print $1, $5, $7, $9, $11, $13, $15, $17}' >>  ${OUTPUTDIR}/GC_latency_breakdown
	rm ${OUTPUTDIR}/gc_latency_breakdown
	
	chown -R ${USER} ${OUTPUTDIR}

	echo "==== End the experiment ===="
}
main
