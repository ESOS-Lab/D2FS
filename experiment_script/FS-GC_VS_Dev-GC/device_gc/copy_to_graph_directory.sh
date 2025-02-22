#!/bin/bash

EXP_DATA=$1
GRAPH_PATH=(../../graph/Fig-1.FS-GC_VS_Dev-GC)
OLD_DATA=$(find "$GRAPH_PATH" -name "devgc")

if [ -s "$OLD_DATA" ]; then
	cp -r $OLD_DATA/* $GRAPH_PATH/devgc_old
	rm $OLD_DATA/* -rf
fi

cp -r $1 ${GRAPH_PATH}/devgc/
