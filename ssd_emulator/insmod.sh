#!/bin/bash

#sudo insmod ./nvmev.ko memmap_start=32 memmap_size=8192 cpus=14,15
#sudo insmod ./nvmev.ko memmap_start=32 memmap_size=8192 cpus=14,15
#sudo insmod ./nvmev.ko memmap_start=30 memmap_size=32768 cpus=14,15
#sudo insmod ./nvmev.ko memmap_start=32 memmap_size=98304 cpus=14,15
#sudo insmod ./nvmev.ko memmap_start=64 memmap_size=65536 cpus=14,15

#server config
sudo insmod ./nvmev.ko memmap_start=256 memmap_size=262144 cpus=37,39 #256 GB
#sudo insmod ./nvmev.ko memmap_start=256 memmap_size=131072 cpus=37,39  #128 GB
#sudo insmod ./nvmev.ko memmap_start=256 memmap_size=32818 cpus=37,39  #64 GB
