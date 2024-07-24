#!/bin/bash

sudo dpkg -i ../*.deb
sudo apt-mark hold linux-libc-dev
