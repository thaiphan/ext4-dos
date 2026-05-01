WATCOM ?= /Users/thai/.local/opt/watcom
WCC    := $(WATCOM)/armo64/wcc
WLINK  := $(WATCOM)/armo64/wlink
WCL    := $(WATCOM)/armo64/wcl

CC ?= cc
CFLAGS_HOST := -std=c99 -Wall -Wextra -Wpedantic -O2 -Isrc

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

dos-build:
	@echo "dos-build: not yet wired up"

host-test: host-build
	@echo "host-test: no tests yet"

fixtures: fixture fixture-partitioned
fixture:             tests/images/small.img
fixture-partitioned: tests/images/disk.img

tests/images/small.img: scripts/mkfixture.sh
	bash $<

tests/images/disk.img: scripts/mkfixture-partitioned.py
	python3 $<

clean:
	rm -rf $(BUILD)
