#!/bin/bash


dmesg -C
make clean
make
./insmod.sh
