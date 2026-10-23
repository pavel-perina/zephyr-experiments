ZEPHYR_PROJECT ?= $(HOME)/zephyrproject
CROSS   ?= $(HOME)/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-
UF2CONV = $(ZEPHYR_PROJECT)/zephyr/scripts/build/uf2conv.py
MDK     = $(ZEPHYR_PROJECT)/modules/hal/nordic/nrfx/bsp/stable/mdk
CMSIS   = $(ZEPHYR_PROJECT)/modules/hal/cmsis_6/CMSIS/Core/Include

CC = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
OBJDUMP = $(CROSS)objdump
SIZE = $(CROSS)size

CFLAGS = -mcpu=cortex-m4 -mthumb -O2 -g -Wall -ffreestanding \
	 -ffunction-sections -DNRF52840_XXAA -I$(MDK) -I$(CMSIS)
LDFLAGS = -nostdlib -T link.ld -Wl,--gc-sections

SRCS = startup.c main.c $(MDK)/system_nrf52840.c

all: build/blinky.uf2

build:
	mkdir -p build

build/blinky.elf: $(SRCS) link.ld | build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS)
	$(SIZE) $@

build/blinky.bin: build/blinky.elf
	$(OBJCOPY) -O binary $< $@

# 0xada52840 = nRF52840 UF2 family ID
build/blinky.uf2: build/blinky.bin
	python3 $(UF2CONV) -c -b 0x27000 -f 0xada52840 -o $@ $<

dis: build/blinky.elf
	$(OBJDUMP) -d $<

clean:
	rm -rf build

.PHONY: all dis clean
