As of 2025-10-26 this does not work at Raspberry Pi Pico 2 W.

On XIAO RP2040 it writes output below. Note that results are invalid for first few seconds and they are
not (reliably) updated in less than five seconds so the same values are reported repeatably. 
Internally SCD41 returns error on temperature read and must be queried if measurement is ready.

```txt
.venv) [pavel@thinkpad -=- ~/zephyrproject/zephyr-experiments/sensors/scd41]$ udisksctl mount -b /dev/sdb1 && west flash -r uf2                                                  
Mounted /dev/sdb1 at /run/media/pavel/RPI-RP2
-- west flash: rebuilding
[1/10] Performing build step for 'second_stage_bootloader'
ninja: no work to do.
[3/3] Completed 'second_stage_bootloader'
-- west flash: using runner uf2
-- runners.uf2: Copying UF2 file to '/run/media/pavel/RPI-RP2'
(.venv) [pavel@thinkpad -=- ~/zephyrproject/zephyr-experiments/sensors/scd41]$ mpremote
Connected to MicroPython at /dev/ttyACM0
Use Ctrl-] or Ctrl-x to exit this shell
ity : 0.000000
         CO2 : 0.000000
---
 Temperature : -45.000000
    Pressure : N/A
    Humidity : 0.000000
         CO2 : 0.000000
---
 Temperature : -45.000000
    Pressure : N/A
    Humidity : 0.000000
         CO2 : 0.000000
---
 Temperature : 23.483253
    Pressure : N/A
    Humidity : 46.657511
         CO2 : 1030.000000
---
 Temperature : 23.483253
    Pressure : N/A
    Humidity : 46.657511
         CO2 : 1030.000000
---
 Temperature : 23.606088
    Pressure : N/A
    Humidity : 47.130540
         CO2 : 1018.000000
---
 Temperature : 23.606088
    Pressure : N/A
    Humidity : 47.130540
         CO2 : 1018.000000
---
 Temperature : 23.606088
    Pressure : N/A
    Humidity : 47.130540
         CO2 : 1018.000000
```
