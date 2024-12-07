#!/bin/bash

JOBS=2

make -j$JOBS v5535_defconfig
make -j$JOBS menuconfig
cp .config arch/x86/configs/v5535_defconfig
make -j$JOBS v5535_defconfig
cp .config arch/x86/configs/v5535_defconfig
