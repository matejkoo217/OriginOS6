CROSS ?= aarch64-linux-gnu-
CC := $(CROSS)gcc
CFLAGS := -O2 -static -nostdlib -nostartfiles \
          -ffreestanding -fno-stack-protector \
          -fno-asynchronous-unwind-tables \
          -fno-unwind-tables -Wall -Wextra
LDFLAGS := -Wl,--build-id=none \
           -Wl,-z,max-page-size=4096 \
           -Wl,-e,_start
all: init_shim
init_shim: init_shim.c _start.S
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ init_shim.c _start.S
clean:
	rm -f init_shim
.PHONY: all clean
