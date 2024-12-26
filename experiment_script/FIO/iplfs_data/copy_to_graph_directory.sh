#!/bin/bash


GRAPH_PATH2=(../../graph/Fig-13_and_15.FIO_throughput_and_latency)
OLD_DATA2=$(find "$GRAPH_PATH2" -name "iplfs_data")

if [ -s "$OLD_DATA2" ]; then
	mv $OLD_DATA2/* $GRAPH_PATH2/iplfs_old_data
fi

cp -r $1 ${GRAPH_PATH2}/iplfs_data/


