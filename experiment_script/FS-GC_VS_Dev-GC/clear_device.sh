#!/bin/bash

rm -rf /tmp_mnt/*
rm -rf /mnt/*
umount /mnt
umount /tmp_mnt
echo -e "d\n\n\nd\nw\n" | fdisk /dev/nvme2n1
