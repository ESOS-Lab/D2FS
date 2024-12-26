#!/bin/bash


GRAPH_PATH2=(../../graph/Fig-13_and_15.FIO_throughput_and_latency)
OLD_DATA2=$(find "$GRAPH_PATH2" -name "f2fs_data")

if [ -s "$OLD_DATA2" ]; then
	cp -r $OLD_DATA2/* $GRAPH_PATH2/f2fs_old_data
	rm $OLD_DATA2/* -rf
fi

cp -r $1 ${GRAPH_PATH2}/f2fs_data/





