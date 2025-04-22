#!/bin/bash

# Repack linux kernel debian packages with gzip compression.
# This allows to install this kernel on old distros like Lubuntu 12.04.

IN=../
OUT=$HOME/Downloads

mkdir /tmp/kernel
cp $IN/*.deb /tmp/kernel
cd /tmp/kernel
LINUX_IMAGE=$(find linux-image*.deb)
ar x $LINUX_IMAGE
zstd -d < control.tar.zst | gzip > control.tar.gz
zstd -d < data.tar.zst | gzip > data.tar.gz
ar -m -c -a sdsd $OUT/$LINUX_IMAGE debian-binary control.tar.gz data.tar.gz
rm $LINUX_IMAGE debian-binary control.tar.gz data.tar.gz control.tar.zst data.tar.zst
LINUX_HEADERS=$(find linux-headers*.deb)
ar x $LINUX_HEADERS
zstd -d < control.tar.zst | gzip > control.tar.gz
zstd -d < data.tar.zst | gzip > data.tar.gz
ar -m -c -a sdsd $OUT/$LINUX_HEADERS debian-binary control.tar.gz data.tar.gz
rm $LINUX_HEADERS debian-binary control.tar.gz data.tar.gz control.tar.zst data.tar.zst
LINUX_LIBC_DEV=$(find linux-libc-dev*.deb)
ar x $LINUX_LIBC_DEV
zstd -d < control.tar.zst | gzip > control.tar.gz
zstd -d < data.tar.zst | gzip > data.tar.gz
ar -m -c -a sdsd $OUT/$LINUX_LIBC_DEV debian-binary control.tar.gz data.tar.gz
rm -r /tmp/kernel
