WATCOM ?= /Users/thai/.local/opt/watcom

# OpenWatcom binary subdirectory varies per host. Override WATCOM_BIN
# explicitly if your install isn't at one of the autodetected paths.
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_S),Darwin)
    ifeq ($(UNAME_M),arm64)
        WATCOM_BIN ?= $(WATCOM)/armo64
    else
        WATCOM_BIN ?= $(WATCOM)/bino64
    endif
else
    WATCOM_BIN ?= $(WATCOM)/binl64
endif

WCC    := $(WATCOM_BIN)/wcc
WLINK  := $(WATCOM_BIN)/wlink
WCL    := $(WATCOM_BIN)/wcl

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
	src/ext4/journal.c \
	src/partition/mbr.c \
	src/util/crc32c.c

DOS_CLI_OBJ := \
	$(DOS_DIR)/dos_cli.obj \
	$(DOS_DIR)/int13_bdev.obj \
	$(DOS_DIR)/superblock.obj \
	$(DOS_DIR)/features.obj \
	$(DOS_DIR)/fs.obj \
	$(DOS_DIR)/inode.obj \
	$(DOS_DIR)/extent.obj \
	$(DOS_DIR)/dir.obj \
	$(DOS_DIR)/journal.obj \
	$(DOS_DIR)/mbr.obj \
	$(DOS_DIR)/crc32c.obj

TSR_OBJ := \
	$(DOS_DIR)/tsr.obj \
	$(DOS_DIR)/int13_bdev.obj \
	$(DOS_DIR)/superblock.obj \
	$(DOS_DIR)/features.obj \
	$(DOS_DIR)/fs.obj \
	$(DOS_DIR)/inode.obj \
	$(DOS_DIR)/extent.obj \
	$(DOS_DIR)/dir.obj \
	$(DOS_DIR)/journal.obj \
	$(DOS_DIR)/mbr.obj \
	$(DOS_DIR)/crc32c.obj

vpath %.c tools src/blockdev src/ext4 src/partition src/util

.PHONY: all host-build dos-build host-test dos-test fixtures fixture fixture-partitioned clean

all: host-build

host-build: $(HOST_DIR)/host_cli $(HOST_DIR)/host_features_test $(HOST_DIR)/host_stress_test $(HOST_DIR)/host_journal_test $(HOST_DIR)/host_checkpoint_test $(HOST_DIR)/host_crc32c_test

$(HOST_DIR)/host_cli: tools/host_cli.c $(LIB_SRCS_HOST) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_features_test: tools/host_features_test.c src/ext4/features.c | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_stress_test: tools/host_stress_test.c $(LIB_SRCS_HOST) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_journal_test: tools/host_journal_test.c $(LIB_SRCS_HOST) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_checkpoint_test: tools/host_checkpoint_test.c $(LIB_SRCS_HOST) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_crc32c_test: tools/host_crc32c_test.c src/util/crc32c.c | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR):
	mkdir -p $@

dos-build: $(DOS_DIR)/ext4cli.exe $(DOS_DIR)/ext4.exe $(DOS_DIR)/ext4chk.exe $(DOS_DIR)/ext4dir.exe $(DOS_DIR)/ext4cnt.exe $(DOS_DIR)/ext4dmp.exe

$(DOS_DIR)/ext4cli.exe: $(DOS_CLI_OBJ)
	$(WCC_ENV) $(WCL_DOS) $^ -fe=$@

$(DOS_DIR)/ext4.exe: $(TSR_OBJ)
	$(WCC_ENV) $(WCL_DOS) $^ -fe=$@

$(DOS_DIR)/ext4chk.exe: $(DOS_DIR)/tsr_check.obj
	$(WCC_ENV) $(WCL_DOS) $^ -fe=$@

$(DOS_DIR)/ext4dir.exe: $(DOS_DIR)/tsr_dir.obj
	$(WCC_ENV) $(WCL_DOS) $^ -fe=$@

$(DOS_DIR)/ext4cnt.exe: $(DOS_DIR)/tsr_calls.obj
	$(WCC_ENV) $(WCL_DOS) $^ -fe=$@

$(DOS_DIR)/ext4dmp.exe: $(DOS_DIR)/tsr_dump.obj
	$(WCC_ENV) $(WCL_DOS) $^ -fe=$@

$(DOS_DIR)/%.obj: %.c | $(DOS_DIR)
	$(WCC_ENV) $(WCC_DOS) -fo=$@ $<

$(DOS_DIR):
	mkdir -p $@

host-test: host-build tests/images/journal.img tests/images/journal-csum.img
	@echo "==> running host_features_test"
	@$(HOST_DIR)/host_features_test
	@echo "==> running host_crc32c_test"
	@$(HOST_DIR)/host_crc32c_test
	@echo "==> running host_journal_test (no-csum fixture)"
	@$(HOST_DIR)/host_journal_test tests/images/journal.img tests/images/journal.expect
	@echo "==> running host_journal_test (CSUM_V2 fixture)"
	@$(HOST_DIR)/host_journal_test tests/images/journal-csum.img tests/images/journal-csum.expect
	@echo "==> running host_checkpoint_test (no-csum, mutates working copy)"
	@cp tests/images/journal.img tests/images/journal-flush.img
	@$(HOST_DIR)/host_checkpoint_test tests/images/journal-flush.img tests/images/journal.expect
	@echo "==> running host_checkpoint_test (CSUM_V2, mutates working copy)"
	@cp tests/images/journal-csum.img tests/images/journal-csum-flush.img
	@$(HOST_DIR)/host_checkpoint_test tests/images/journal-csum-flush.img tests/images/journal-csum.expect

host-stress: host-build tests/images/stress.img
	@echo "==> running host_stress_test"
	@$(HOST_DIR)/host_stress_test tests/images/stress.img

tests/images/stress.img: scripts/mkfixture-stress.py
	python3 scripts/mkfixture-stress.py

tests/images/journal.img: scripts/mkfixture-journal.py
	$(PYTHON) $<

tests/images/journal-csum.img: scripts/mkfixture-journal-csum.py
	$(PYTHON) $<

dos-test: dos-build fixture-partitioned
	@bash scripts/run-dosbox.sh

tsr-test: dos-build
	@bash scripts/run-tsr-test.sh

freedos-test: dos-build
	@bash scripts/run-freedos-test.sh

msdos4-test: dos-build
	@bash scripts/run-msdos4-test.sh

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
