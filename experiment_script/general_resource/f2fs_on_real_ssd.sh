#!/bin/bash
BENCHMARK_DIR=/mnt
DEV_partition=/dev/nvme2n1p1
DEV_whole=/dev/nvme2n1

sudo rm $BENCHMARK_DIR/* -rf
sudo umount $DEV_partition\

sudo rmmod f2fs
sudo insmod ./mod/$1.ko;

sudo mkfs.f2fs -s 16 -f $DEV_partition;\

sudo mount -t f2fs -o mode=lfs $DEV_partition $BENCHMARK_DIR;\
