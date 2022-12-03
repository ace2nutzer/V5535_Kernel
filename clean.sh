#!/bin/bash

JOBS=2

make -j$JOBS clean
make -j$JOBS distclean
