#!/bin/bash

MG_CMD=$(find ./d2fs_data -name "migration_command_count")
MG_LOG_MEM=$(find ./d2fs_data -name "migration_record_memory_footprint")
REG_REGION_UTIL=$(find ./d2fs_data -name "section_utilization_regular_region")
GC_REGION_UTIL=$(find ./d2fs_data -name "section_utilization_gc_region")

cp "$MG_CMD" ./
cp "$MG_LOG_MEM" ./
cp "$REG_REGION_UTIL" ./
cp "$GC_REGION_UTIL" ./

python parser.py 2 $MG_CMD > migration_command_count_parsed
python parser.py 2 $MG_LOG_MEM > migration_record_memory_footprint_parsed

gnuplot plot_Fig-12.gpi
gnuplot plot_Fig-18.gpi
