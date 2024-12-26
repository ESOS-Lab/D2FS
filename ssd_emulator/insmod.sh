#!/bin/bash

#sudo insmod ./nvmev.ko memmap_start=32 memmap_size=8192 cpus=14,15
#sudo insmod ./nvmev.ko memmap_start=32 memmap_size=8192 cpus=14,15
#sudo insmod ./nvmev.ko memmap_start=30 memmap_size=32768 cpus=14,15
#sudo insmod ./nvmev.ko memmap_start=32 memmap_size=98304 cpus=14,15
sudo insmod ./nvmev.ko memmap_start=256 memmap_size=262144 cpus=37,39
