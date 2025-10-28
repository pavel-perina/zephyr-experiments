Experiment from 2025-06-14, first Zephyr project.

Here I learned how to enable serial console over USB on RP2040-based boards.

On Raspberry Pi Pico, USB Console driver is needed.

Ways to compile:

1. Overengineered way
   ```
   west build -p -b rpi_pico . -- -DOVERLAY_CONFIG="usb.conf" -DEXTRA_DTC_OVERLAY_FILE="usb.overlay"
   ```

2. Easy way (you don't need to create overlay files and deal with `-DDTC_OVERLAY_FILE` or preferably `-DEXTRA_DTC_OVERLAY_FILE`. Uses [cdc-acm-console](https://docs.zephyrproject.org/latest/snippets/cdc-acm-console/README.html) snippet.
   ```
   west build -p -b rpi_pico -S cdc-acm-console -- -DCONFIG_USB_DEVICE_INITIALIZE_AT_BOOT=y
   west build -p -b xiao_rp2040 -S cdc-acm-console -- -DCONFIG_USB_DEVICE_INITIALIZE_AT_BOOT=y
   ```

However as of 2025-10-26 the command
   ```
   west build -p always -b rpi_pico2/rp2350a/m33/w -S  cdc-acm-console -- -DCONFIG_USB_DEVICE_INITIALIZE_AT_BOOT=y
   ```
   does not work with Raspberry Pi Pico 2 W
