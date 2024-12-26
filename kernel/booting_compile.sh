#!/bin/bash

sudo make -j40 && sudo make modules_install -j40 && sudo find /lib/modules/5.11.0.vanilla-f2fs/ -name *.ko -exec strip --strip-unneeded {} + && sudo make install -j40
cp fs/f2fs/f2fs.ko /home/oslab/juwon/IFLBA_test/mod/f2fs_vanilla_dsm_waf.ko
#sudo rm -rf /boot/*5.11.0*/ && rm -rf /lib/modules/*5.11.0* 
