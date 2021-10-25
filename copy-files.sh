#!/usr/bin/env bash
# Copyright 2021 Dokyung Song. All rights reserved.
# Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.


MOD_DIR=build/linux/modules
IMG_NAME=stretch.img
OVERLAY_NAME=stretch.qcow2
set -eux

MNT_DIR=/mnt/${IMG_NAME%.*}

sudo mkdir -p $MNT_DIR

if [ -f $OVERLAY_NAME ]; then
	sudo guestmount -a stretch.qcow2 -i /mnt/stretch
else
	sudo mount -o loop $IMG_NAME $MNT_DIR
fi

sudo mkdir -p $MNT_DIR/lib/modules
sudo cp -rd $MOD_DIR/lib/modules/* $MNT_DIR/lib/modules/
sudo mkdir -p $MNT_DIR/root/test
make -C test
sudo cp -p test/*/*.exe $MNT_DIR/root/test
sudo find $MNT_DIR/root/test -name "*.exe" |sudo xargs chmod +x

if [ -f $OVERLAY_NAME ]; then
	sudo guestunmount /mnt/stretch
else
	sudo umount $MNT_DIR
fi

sudo rmdir $MNT_DIR
