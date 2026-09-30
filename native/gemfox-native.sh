#!/usr/bin/env bash

log=/home/morj/projects/geminifox/log.txt

echo "===== called!" >> $log
tee >(xxd >> "$log") | /home/morj/projects/geminifox/native/build/gemfox-native 2>>$log
echo "===== exiting!" >> $log
