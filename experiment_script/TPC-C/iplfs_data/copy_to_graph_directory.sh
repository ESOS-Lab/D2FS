#!/bin/bash

EXP_DATA=$1
GRAPH_PATH=(../../graph/Fig-14_and_16.Macrobenchmark_throughput_and_latency)
OLD_DATA=$(find "$GRAPH_PATH" -name "iplfs_TPC-C")

if [ -s "$OLD_DATA" ]; then
	cp $OLD_DATA/* $GRAPH_PATH/iplfs_TPC-C_old -r
	rm $OLD_DATA/* -rf
fi

cp -r $1 ${GRAPH_PATH}/iplfs_TPC-C/






