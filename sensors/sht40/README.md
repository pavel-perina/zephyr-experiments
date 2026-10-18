SHT40 temperature and humidity, printed to the console every 2 s.
Wiring per board in `boards/<board>.overlay`.

| Board | SDA | SCL | Flash |
|---|---|---|---|
| NUCLEO-L552ZE-Q | D14 (PB9) | D15 (PB8) | `west flash -r openocd` |

```
west build -p -b nucleo_l552ze_q .
west flash -r openocd
```

Output on `/dev/ttyACM0`, 115200 8N1:

```
[0 s] T 23.07 C, RH 35.49 %
```
