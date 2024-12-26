#!/bin/bash


GRAPH_PATH2=(../../graph/Fig-13_and_15.FIO_throughput_and_latency)
OLD_DATA2=$(find "$GRAPH_PATH2" -name "zns_data")

if [ -s "$OLD_DATA2" ]; then
	mv $OLD_DATA2/* $GRAPH_PATH2/zns_old_data
fi

cp -r $1 ${GRAPH_PATH2}/zns_data/


GRAPH_PATH3=(../../graph/Fig-17.GC_latency_breakdown)
OLD_DATA3=$(find "$GRAPH_PATH3" -name "zns_FIO")

if [ -s "$OLD_DATA3" ]; then
	mv $OLD_DATA3/* $GRAPH_PATH3/zns_FIO_old
fi

cp -r $1 ${GRAPH_PATH3}/zns_FIO/






