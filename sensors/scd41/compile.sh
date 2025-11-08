#!/bin/sh
#west build -p auto -b rpi_pico2/rp2350a/m33/w -S cdc-acm-console -- -DCONFIG_USB_DEVICE_INITIALIZE_AT_BOOT=y
west build -p -b xiao_rp2040 -S cdc-acm-console -- -DCONFIG_USB_DEVICE_INITIALIZE_AT_BOOT=y
