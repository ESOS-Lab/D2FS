#!/bin/bash

sudo make -j40 && sudo make modules_install -j40 && sudo find /lib/modules/5.11.0.d2fs/ -name *.ko -exec strip --strip-unneeded {} + && sudo make install -j40
cp fs/f2fs/f2fs.ko ../experiment_script/general_resource/filesystem_module/d2fs.ko

