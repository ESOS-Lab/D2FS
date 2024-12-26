#!/bin/bash

sudo make modules -j16 && sudo make modules_install -j16 && sudo find /lib/modules/5.11.0/ -name *.ko -exec strip --strip-unneeded {} + && sudo make install -j16
cp fs/f2fs/f2fs.ko /home/juwon/IFLBA_test/mod/f2fs_vanilla_waf.ko
#sudo rm -rf /boot/*5.11.0*/ && rm -rf /lib/modules/*5.11.0* 
