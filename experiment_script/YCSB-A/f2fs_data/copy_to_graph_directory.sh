#!/bin/bash

EXP_DATA=$1
GRAPH_PATH=(../../graph/Fig-14_and_16.Macrobenchmark_throughput_and_latency)
OLD_DATA=$(find "$GRAPH_PATH" -name "f2fs_YCSB-A")

if [ -s "$OLD_DATA" ]; then
	mv $OLD_DATA/* $GRAPH_PATH/f2fs_YCSB-A_old
fi

cp -r $1 ${GRAPH_PATH}/f2fs_YCSB-A/






