#!/bin/bash

N=$(nproc)

make -j $N clean
make -j $N distclean
