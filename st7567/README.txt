Build software
```sh
west build -p auto -b xiao_rp2040
```

XIAO RP2040
To enter boot mode on XIAO RP2040 press `reset` while holding `boot`.

Verify device

```sh
lsblk -o NAME,SIZE,MODEL,SERIAL,LABEL,TRAN
```

```
NAME          SIZE MODEL                      SERIAL            LABEL   TRAN
sda             0B SD/MMC                     20120501030900000         usb
sdb           128M RP2                        E0C9125B0D9B              usb
└─sdb1        128M                                              RPI-RP2 
zram0           8G                                              zram0   
nvme0n1     476.9G SAMSUNG MZVKW512HMJP-000H1 S34CNX0J652940            nvme
├─nvme0n1p1   480M                                                      nvme
├─nvme0n1p2   720M                                                      nvme
├─nvme0n1p3    70G                                                      nvme
└─nvme0n1p4 405.8G                                                      nvme
```

Mount device and flash firmware
```sh
udisksctl mount -b /dev/sdb1 && west flash -r uf2
```

```
Mounted /dev/sdb1 at /run/media/pavel/RPI-RP2
-- west flash: rebuilding
[1/10] Performing build step for 'second_stage_bootloader'
ninja: no work to do.
[3/3] Completed 'second_stage_bootloader'
-- west flash: using runner uf2
-- runners.uf2: Copying UF2 file to '/run/media/pavel/RPI-RP2'
```
