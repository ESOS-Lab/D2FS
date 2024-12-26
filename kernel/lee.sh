#!/bin/bash

sudo make modules -j16
cp fs/f2fs/f2fs.ko /home/juwon/IFLBA_test/mod/f2fs_lee.ko
cp fs/f2fs/f2fs.ko /home/juwon/lee/mod/f2fs_lee.ko
