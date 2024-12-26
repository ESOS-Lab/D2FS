#!/bin/bash

DEVGC_KIOPS=$(find ./devgc -name "kiops_sum")
FSGC_KIOPS=$(find ./fsgc -name "kiops_sum")

cp "$DEVGC_KIOPS" ./devgc_kiops_raw
cp "$FSGC_KIOPS" ./fsgc_kiops_raw

python3 parser.py devgc_kiops_raw > devgc_kiops
python3 parser.py fsgc_kiops_raw > fsgc_kiops

gnuplot plot_Fig-1.Throughput_of_FS-GC_and_Dev-GC.gpi
