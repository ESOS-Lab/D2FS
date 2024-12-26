#!/bin/bash
BENCHMARK_DIR=/mnt
DEV_partition=/dev/nvme3n1p1
DEV_whole=/dev/nvme3n1

sudo rm $BENCHMARK_DIR/* -rf
#sudo umount $DEV_partition\

#sudo rmmod f2fs
sudo insmod ./filesystem_module/$1.ko;

#section size: 1 GByte
#sudo mkfs.f2fs -s 512 -f $DEV_partition;\

#section size: 32 MByte
sudo mkfs.f2fs -s 16 -f $DEV_partition;\

sudo mount -t f2fs -o mode=lfs -o discard $DEV_partition $BENCHMARK_DIR;\

