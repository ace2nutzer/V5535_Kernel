#!/bin/bash

JOBS=2
KERNEL=$(uname -r)

make -j$JOBS drivers/video/fbdev/sis/ modules
sudo cp drivers/video/fbdev/sis/sisfb.ko /lib/modules/$KERNEL/kernel/drivers/video/fbdev/sis
#cd /etc/X11
#sudo cp xorg.conf.fbdev xorg.conf
