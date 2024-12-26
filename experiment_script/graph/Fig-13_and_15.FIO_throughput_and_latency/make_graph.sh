#!/bin/bash

# KIOPS
D2FS_KIOPS=$(find ./d2fs_data/ -name "kiops_sum")
IPLFS_KIOPS=$(find ./iplfs_data/ -name "kiops_sum")
ZNS_KIOPS=$(find ./zns_data/ -name "kiops_sum")
F2FS_KIOPS=$(find ./f2fs_data/ -name "kiops_sum")

python3 parser.py "$D2FS_KIOPS" > d2fs_kiops_parsed
python3 parser.py "$IPLFS_KIOPS" > iplfs_kiops_parsed
python3 parser.py "$ZNS_KIOPS" > zns_kiops_parsed
python3 parser.py "$F2FS_KIOPS" > f2fs_kiops_parsed


# Latency
D2FS_RESULT=$(find ./d2fs_data/ -name "result_4k.txt")
IPLFS_RESULT=$(find ./iplfs_data/ -name "result_4k.txt")
ZNS_RESULT=$(find ./zns_data/ -name "result_4k.txt")
F2FS_RESULT=$(find ./f2fs_data/ -name "result_4k.txt")


# Average Latency
IPLFS_AVG_LAT=$(cat "$IPLFS_RESULT" | grep avg | grep clat | sed 's/=/ /g' | sed 's/,//g' | awk '{print $8}')
D2FS_AVG_LAT=$(cat "$D2FS_RESULT" | grep avg | grep clat | sed 's/=/ /g' | sed 's/,//g' | awk '{print $8}')
ZNS_AVG_LAT=$(cat "$ZNS_RESULT" | grep avg | grep clat | sed 's/=/ /g' | sed 's/,//g' | awk '{print $8}')
F2FS_AVG_LAT=$(cat "$F2FS_RESULT" | grep avg | grep clat | sed 's/=/ /g' | sed 's/,//g' | awk '{print $8}')

echo -e "steps\tIPLFS\tD2FS\tzF2FS\tF2FS\navg\t${IPLFS_AVG_LAT}\t${D2FS_AVG_LAT}\t${ZNS_AVG_LAT}\t${F2FS_AVG_LAT}" > avg_lat_data

# 99.99th Tail Latency
IPLFS_TAIL_LAT=$(cat "$IPLFS_RESULT" | grep 99.99th | sed 's/\[/ /g' | sed 's/\]/ /g' | awk '{print $3}')
D2FS_TAIL_LAT=$(cat "$D2FS_RESULT" | grep 99.99th | sed 's/\[/ /g' | sed 's/\]/ /g' | awk '{print $3}')
ZNS_TAIL_LAT=$(cat "$ZNS_RESULT" | grep 99.99th | sed 's/\[/ /g' | sed 's/\]/ /g' | awk '{print $3*1000}') # multiply by 1000 to make the unit equal with D2FS's and IPLFS's
F2FS_TAIL_LAT=$(cat "$F2FS_RESULT" | grep 99.99th | sed 's/\[/ /g' | sed 's/\]/ /g' | awk '{print $3*1000}') # multiply by 1000 to make the unit equal with D2FS's and IPLFS's
echo -e "steps\tIPLFS\tD2FS\tzF2FS\tF2FS\n99.99\t${IPLFS_TAIL_LAT}\t${D2FS_TAIL_LAT}\t${ZNS_TAIL_LAT}\t${F2FS_TAIL_LAT}" > 99.99_lat_data


# Plot graph
gnuplot plot_Fig-13.FIO_throughput.gpi
gnuplot plot_Fig-15-b.FIO_99.99_latency.gpi
gnuplot plot_Fig-15-a.FIO_avg_latency.gpi
