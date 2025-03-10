#!/bin/bash

KERNEL=/media/ace2nutzer/b8b4acea-417c-4c2c-a1de-059f6d035eb9/home/ace2nutzer/Kernel

cp ../*.deb /tmp/kernel
cd /tmp/kernel
ar x linux-image*
zstd -d < control.tar.zst | gzip > control.tar.gz
zstd -d < data.tar.zst | gzip > data.tar.gz
ar -m -c -a sdsd $KERNEL/linux-image.deb debian-binary control.tar.gz data.tar.gz
rm linux-image* debian-binary control.tar.gz data.tar.gz control.tar.zst data.tar.zst
ar x linux-headers*
zstd -d < control.tar.zst | gzip > control.tar.gz
zstd -d < data.tar.zst | gzip > data.tar.gz
ar -m -c -a sdsd $KERNEL/linux-headers.deb debian-binary control.tar.gz data.tar.gz
rm linux-headers* debian-binary control.tar.gz data.tar.gz control.tar.zst data.tar.zst
ar x linux-libc*
zstd -d < control.tar.zst | gzip > control.tar.gz
zstd -d < data.tar.zst | gzip > data.tar.gz
ar -m -c -a sdsd $KERNEL/linux-libc.deb debian-binary control.tar.gz data.tar.gz
rm linux-libc* debian-binary control.tar.gz data.tar.gz control.tar.zst data.tar.zst
