#!/bin/bash

EXP_DATA=$1
GRAPH_PATH=(../../graph/Fig-14_and_16.Macrobenchmark_throughput_and_latency)
OLD_DATA=$(find "$GRAPH_PATH" -name "zns_TPC-C")

if [ -s "$OLD_DATA" ]; then
	cp $OLD_DATA/* $GRAPH_PATH/zns_TPC-C_old -r
	rm $OLD_DATA/* -rf
fi

cp -r $1 ${GRAPH_PATH}/zns_TPC-C/



GRAPH_PATH3=(../../graph/Fig-17.GC_latency_breakdown)
OLD_DATA3=$(find "$GRAPH_PATH3" -name "zns_TPC-C")

if [ -s "$OLD_DATA3" ]; then
	cp $OLD_DATA3/* $GRAPH_PATH3/zns_TPC-C_old -r
	rm $OLD_DATA3/* -rf
fi

cp -r $1 ${GRAPH_PATH3}/zns_TPC-C/






