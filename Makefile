WATCOM ?= /Users/thai/.local/opt/watcom
WCC    := $(WATCOM)/armo64/wcc
WLINK  := $(WATCOM)/armo64/wlink
WCL    := $(WATCOM)/armo64/wcl

CC     ?= cc
PYTHON ?= python3
CFLAGS_HOST := -std=c99 -Wall -Wextra -Wpedantic -O2 -Isrc

# 16-bit OpenWatcom DOS cross-compile.
# Memory model: small (-ms) — separate code/data segments, ≤64K each.
WCC_ENV := WATCOM=$(WATCOM) INCLUDE=$(WATCOM)/h
WCL_DOS := $(WCL) -bt=dos -ms -zq

BUILD    := build
HOST_DIR := $(BUILD)/host
DOS_DIR  := $(BUILD)/dos

LIB_SRCS := \
	src/blockdev/file_bdev.c \
	src/ext4/superblock.c \
	src/ext4/features.c \
	src/partition/mbr.c

.PHONY: all host-build dos-build host-test fixtures fixture fixture-partitioned clean

all: host-build

host-build: $(HOST_DIR)/host_cli

$(HOST_DIR)/host_cli: tools/host_cli.c $(LIB_SRCS) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR):
	mkdir -p $@

dos-build: $(DOS_DIR)/hello.exe

$(DOS_DIR)/hello.exe: tools/dos_hello.c | $(DOS_DIR)
	$(WCC_ENV) $(WCL_DOS) $< -fo=$(DOS_DIR)/hello.obj -fe=$@

$(DOS_DIR):
	mkdir -p $@

host-test: host-build
	@echo "host-test: no tests yet"

fixtures: fixture fixture-partitioned
fixture:             tests/images/small.img
fixture-partitioned: tests/images/disk.img

tests/images/small.img: scripts/mkfixture.py
	$(PYTHON) $<

tests/images/disk.img: scripts/mkfixture-partitioned.py
	$(PYTHON) $<

clean:
	rm -rf $(BUILD)
	rm -f *.obj *.err *.lst *.map
