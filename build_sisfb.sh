#!/bin/bash

N=$(nproc)
KERNEL=$(uname -r)

make -j $N drivers/video/fbdev/sis/ modules
sudo cp drivers/video/fbdev/sis/sisfb.ko /lib/modules/$KERNEL/kernel/drivers/video/fbdev/sis
