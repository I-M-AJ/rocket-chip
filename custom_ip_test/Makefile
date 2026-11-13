CROSS_COMPILE ?= riscv64-unknown-elf-
CC      := $(CROSS_COMPILE)gcc
OBJDUMP := $(CROSS_COMPILE)objdump

CFLAGS  := -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -O2 -g \
           -Wall -Wextra -ffreestanding -nostdlib -nostartfiles
LDFLAGS := -T linker.ld -Wl,--no-warn-rwx-segments

TARGET  := custom_ip_test.elf

.PHONY: all clean dump

all: $(TARGET) dump

$(TARGET): start.S main.c linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ start.S main.c

dump: $(TARGET)
	$(OBJDUMP) -d $(TARGET) > custom_ip_test.dump

clean:
	rm -f $(TARGET) *.o *.dump
