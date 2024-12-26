#!/bin/bash

EXP_DATA=$1
GRAPH_PATH=(../../graph/Fig-14_and_16.Macrobenchmark_throughput_and_latency)
OLD_DATA=$(find "$GRAPH_PATH" -name "iplfs_YCSB-F")

if [ -s "$OLD_DATA" ]; then
	mv $OLD_DATA/* $GRAPH_PATH/iplfs_YCSB-F_old
fi

cp -r $1 ${GRAPH_PATH}/iplfs_YCSB-F/






