#!/bin/bash

# Bring the board out of reset
echo 149 > /sys/class/gpio/export 2> /dev/null
echo out > /sys/class/gpio/gpio149/direction 2> /dev/null
echo 0 > /sys/class/gpio/gpio149/value 2> /dev/null
sleep 1
echo 1 > /sys/class/gpio/gpio149/value 2> /dev/null

