ZEPHYR_PROJECT ?= $(HOME)/zephyrproject
CROSS   ?= $(HOME)/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-
UF2CONV = $(ZEPHYR_PROJECT)/zephyr/scripts/build/uf2conv.py

AS = $(CROSS)as
LD = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy
OBJDUMP = $(CROSS)objdump

all: build/blinky.uf2

build:
	mkdir -p build

build/blinky.elf: blinky.S link.ld | build
	$(AS) -o build/blinky.o blinky.S
	$(LD) -T link.ld -o $@ build/blinky.o

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
