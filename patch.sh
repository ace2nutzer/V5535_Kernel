#!/bin/bash

patch -p 1 -i $HOME/Downloads/patch-4.14.$@ -r rejects && if [ -f rejects ] ; then  echo "###### REJECTS ######" ; fi
