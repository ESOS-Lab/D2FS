#!/bin/bash

EXP_DATA=$1
GRAPH_PATH=(../../graph/Fig-1.FS-GC_VS_Dev-GC)
OLD_DATA=$(find "$GRAPH_PATH" -name "fsgc")

if [ -s "$OLD_DATA" ]; then
	mv $OLD_DATA/* $GRAPH_PATH/fsgc_old
fi

cp -r $1 ${GRAPH_PATH}/fsgc/
