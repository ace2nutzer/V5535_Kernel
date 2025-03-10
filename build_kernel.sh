#!/bin/bash

N=$(nproc)

# delete old kernel packages
rm ../linux-headers*
rm ../linux-image*
rm ../linux-libc*

# build debian packages
make -j $N v5535_defconfig
make -j $N $@ bindeb-pkg

# cleanup
rm ../linux-upstream*
