#!/bin/bash

JOBS=2

# delete old kernel packages
rm ../linux-headers*
rm ../linux-image*
rm ../linux-libc*

# build debian packages
make -j$JOBS $@ deb-pkg
