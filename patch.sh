#!/bin/bash

patch -p 1 -i $HOME/Downloads/patch-6.12.$@ -r rejects
if [ -f rejects ] ; then
	echo "###### REJECTS ######"
fi
