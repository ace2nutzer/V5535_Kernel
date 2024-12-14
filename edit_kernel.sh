#!/bin/bash

N=$(nproc)

make -j $N v5535_defconfig
make -j $N menuconfig
cp .config arch/x86/configs/v5535_defconfig
make -j $N v5535_defconfig
cp .config arch/x86/configs/v5535_defconfig
