#!/bin/bash

EXP_DATA=$1
GRAPH_PATH=(../../graph/Fig-14_and_16.Macrobenchmark_throughput_and_latency)
OLD_DATA=$(find "$GRAPH_PATH" -name "zns_YCSB-F")

if [ -s "$OLD_DATA" ]; then
	mv $OLD_DATA/* $GRAPH_PATH/zns_YCSB-F_old
fi

cp -r $1 ${GRAPH_PATH}/zns_YCSB-F/



GRAPH_PATH3=(../../graph/Fig-17.GC_latency_breakdown)
OLD_DATA3=$(find "$GRAPH_PATH3" -name "zns_YCSB-F")

if [ -s "$OLD_DATA3" ]; then
	mv $OLD_DATA3/* $GRAPH_PATH3/zns_YCSB-F_old
fi

cp -r $1 ${GRAPH_PATH3}/zns_YCSB-F/






