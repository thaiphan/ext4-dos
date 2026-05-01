WATCOM ?= /Users/thai/.local/opt/watcom
WCC    := $(WATCOM)/armo64/wcc
WLINK  := $(WATCOM)/armo64/wlink
WCL    := $(WATCOM)/armo64/wcl

CC     ?= cc
PYTHON ?= python3
CFLAGS_HOST := -std=c99 -Wall -Wextra -Wpedantic -O2 -Isrc

WCC_ENV := WATCOM=$(WATCOM) INCLUDE=$(WATCOM)/h
WCC_DOS := $(WCC) -bt=dos -ms -zq -i=$(WATCOM)/h -i=src
WCL_DOS := $(WCL) -bt=dos -ms -zq

BUILD    := build
HOST_DIR := $(BUILD)/host
DOS_DIR  := $(BUILD)/dos

LIB_SRCS_HOST := \
	src/blockdev/file_bdev.c \
	src/ext4/superblock.c \
	src/ext4/features.c \
	src/ext4/fs.c \
	src/ext4/inode.c \
	src/ext4/extent.c \
	src/ext4/dir.c \
	src/partition/mbr.c

DOS_CLI_OBJ := \
	$(DOS_DIR)/dos_cli.obj \
	$(DOS_DIR)/int13_bdev.obj \
	$(DOS_DIR)/superblock.obj \
	$(DOS_DIR)/features.obj \
	$(DOS_DIR)/fs.obj \
	$(DOS_DIR)/inode.obj \
	$(DOS_DIR)/extent.obj \
	$(DOS_DIR)/dir.obj \
	$(DOS_DIR)/mbr.obj

TSR_OBJ := \
	$(DOS_DIR)/tsr.obj \
	$(DOS_DIR)/int13_bdev.obj \
	$(DOS_DIR)/superblock.obj \
	$(DOS_DIR)/features.obj \
	$(DOS_DIR)/fs.obj \
	$(DOS_DIR)/inode.obj \
	$(DOS_DIR)/extent.obj \
	$(DOS_DIR)/dir.obj \
	$(DOS_DIR)/mbr.obj

vpath %.c tools src/blockdev src/ext4 src/partition

.PHONY: all host-build dos-build host-test dos-test fixtures fixture fixture-partitioned clean

all: host-build

host-build: $(HOST_DIR)/host_cli

$(HOST_DIR)/host_cli: tools/host_cli.c $(LIB_SRCS_HOST) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR):
	mkdir -p $@

dos-build: $(DOS_DIR)/dos_cli.exe $(DOS_DIR)/tsr.exe $(DOS_DIR)/tsr_chk.exe $(DOS_DIR)/tsr_dir.exe $(DOS_DIR)/tsr_cnt.exe $(DOS_DIR)/tsr_dmp.exe

$(DOS_DIR)/dos_cli.exe: $(DOS_CLI_OBJ)
	$(WCC_ENV) $(WCL_DOS) $^ -fe=$@

$(DOS_DIR)/tsr.exe: $(TSR_OBJ)
	$(WCC_ENV) $(WCL_DOS) $^ -fe=$@

$(DOS_DIR)/tsr_chk.exe: $(DOS_DIR)/tsr_check.obj
	$(WCC_ENV) $(WCL_DOS) $^ -fe=$@

$(DOS_DIR)/tsr_dir.exe: $(DOS_DIR)/tsr_dir.obj
	$(WCC_ENV) $(WCL_DOS) $^ -fe=$@

$(DOS_DIR)/tsr_cnt.exe: $(DOS_DIR)/tsr_calls.obj
	$(WCC_ENV) $(WCL_DOS) $^ -fe=$@

$(DOS_DIR)/tsr_dmp.exe: $(DOS_DIR)/tsr_dump.obj
	$(WCC_ENV) $(WCL_DOS) $^ -fe=$@

$(DOS_DIR)/%.obj: %.c | $(DOS_DIR)
	$(WCC_ENV) $(WCC_DOS) -fo=$@ $<

$(DOS_DIR):
	mkdir -p $@

host-test: host-build
	@echo "host-test: no tests yet"

dos-test: dos-build fixture-partitioned
	@bash scripts/run-dosbox.sh

tsr-test: dos-build
	@bash scripts/run-tsr-test.sh

freedos-test: dos-build
	@bash scripts/run-freedos-test.sh

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
