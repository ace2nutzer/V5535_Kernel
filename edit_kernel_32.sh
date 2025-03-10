#!/bin/bash

N=$(nproc)

make -j $N v5535_32_defconfig
make -j $N menuconfig
cp .config arch/x86/configs/v5535_32_defconfig
make -j $N v5535_32_defconfig
cp .config arch/x86/configs/v5535_32_defconfig
