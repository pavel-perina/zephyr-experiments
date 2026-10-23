ZEPHYR_PROJECT ?= $(HOME)/zephyrproject
CROSS   ?= $(HOME)/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-
PAD_CHECKSUM = $(ZEPHYR_PROJECT)/modules/hal/rpi_pico/src/rp2040/boot_stage2/pad_checksum
UF2CONV = $(ZEPHYR_PROJECT)/zephyr/scripts/build/uf2conv.py

AS = $(CROSS)as
LD = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy
OBJDUMP = $(CROSS)objdump

all: build/blinky.uf2

build:
	mkdir -p build

# boot2 runs from SRAM, where the boot ROM copies it
build/boot2.elf: boot2.S | build
	$(AS) -o build/boot2.o $<
	$(LD) -Ttext=0x20041f00 -o $@ build/boot2.o

build/boot2.bin: build/boot2.elf
	$(OBJCOPY) -O binary $< $@

# pad to 252 bytes + CRC32, emitted as a .boot2 section in assembly
build/boot2_padded.S: build/boot2.bin
	python3 $(PAD_CHECKSUM) -s 0xffffffff $< $@

build/blinky.elf: blinky.S build/boot2_padded.S link.ld
	$(AS) -o build/blinky.o blinky.S
	$(AS) -o build/boot2_padded.o build/boot2_padded.S
	$(LD) -T link.ld -o $@ build/boot2_padded.o build/blinky.o

build/blinky.bin: build/blinky.elf
	$(OBJCOPY) -O binary $< $@

build/blinky.uf2: build/blinky.bin
	python3 $(UF2CONV) -c -b 0x10000000 -f 0xe48bff56 -o $@ $<

dis: build/blinky.elf
	$(OBJDUMP) -d $<

clean:
	rm -rf build

.PHONY: all dis clean
