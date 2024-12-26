#!/bin/bash

EXP_DATA=$1
GRAPH_PATH=(../../graph/Fig-12_and_18.FIO_region_util_and_migration_record_size)
OLD_DATA=$(find "$GRAPH_PATH" -name "d2fs_data")

if [ -s "$OLD_DATA" ]; then
	mv $OLD_DATA/* $GRAPH_PATH/d2fs_old_data
fi

cp -r $1 ${GRAPH_PATH}/d2fs_data/


GRAPH_PATH2=(../../graph/Fig-13_and_15.FIO_throughput_and_latency)
OLD_DATA2=$(find "$GRAPH_PATH2" -name "d2fs_data")

if [ -s "$OLD_DATA2" ]; then
	mv $OLD_DATA2/* $GRAPH_PATH2/d2fs_old_data
fi

cp -r $1 ${GRAPH_PATH2}/d2fs_data/


GRAPH_PATH3=(../../graph/Fig-17.GC_latency_breakdown)
OLD_DATA3=$(find "$GRAPH_PATH3" -name "d2fs_FIO")

if [ -s "$OLD_DATA3" ]; then
	mv $OLD_DATA3/* $GRAPH_PATH3/d2fs_FIO_old
fi

cp -r $1 ${GRAPH_PATH3}/d2fs_FIO/






