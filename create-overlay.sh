#!/usr/bin/env bash

if [ "$#" -eq 2 ]; then
	BASE_IMG_FILE=$1
	OVERLAY_IMG_FILE=$2
else
	BASE_IMG_FILE=stretch.img
	OVERLAY_IMG_FILE=stretch.qcow2
fi

qemu-img create -f qcow2 $OVERLAY_IMG_FILE 2G -b $BASE_IMG_FILE -F raw
