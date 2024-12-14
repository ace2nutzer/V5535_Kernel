#!/bin/bash

if [ -f rejects ] ; then
	rm rejects
fi

patch -p 1 -i $HOME/Downloads/patch-6.12.$@ -r rejects
if [ -f rejects ] ; then
	echo "#####################"
	echo "###### REJECTS ######"
	echo "#####################"
fi
