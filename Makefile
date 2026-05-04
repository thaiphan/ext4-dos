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
# Medium memory model: far code (multiple 64KiB code segments), near data
# (single DGROUP). Switched from small (-ms) on 2026-05-04 because the
# TSR _TEXT was at the cap and every hardening item pushed it over.
# Far calls cost ~2 extra bytes each but unlock multiple code segments.
WCC_DOS := $(WCC) -bt=dos -mm -zq -os -i=$(WATCOM)/h -i=src
WCL_DOS := $(WCL) -bt=dos -mm -zq

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
	src/ext4/htree.c \
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
	$(DOS_DIR)/htree.obj \
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
	$(DOS_DIR)/htree.obj \
	$(DOS_DIR)/journal.obj \
	$(DOS_DIR)/mbr.obj \
	$(DOS_DIR)/crc32c.obj

vpath %.c tools src/blockdev src/ext4 src/partition src/util

.PHONY: all host-build dos-build host-test dos-test fixtures fixture fixture-partitioned clean

all: host-build

host-build: $(HOST_DIR)/host_cli $(HOST_DIR)/host_features_test $(HOST_DIR)/host_stress_test $(HOST_DIR)/host_journal_test $(HOST_DIR)/host_journal_csum_test $(HOST_DIR)/host_journal_streaming_test $(HOST_DIR)/host_checkpoint_test $(HOST_DIR)/host_orphan_recover_test $(HOST_DIR)/host_crash_recovery_test $(HOST_DIR)/host_write_test $(HOST_DIR)/host_xgroup_test $(HOST_DIR)/host_create_test $(HOST_DIR)/host_mkdir_test $(HOST_DIR)/host_rmdir_test $(HOST_DIR)/host_del_test $(HOST_DIR)/host_rename_test $(HOST_DIR)/host_rename_xdir_test $(HOST_DIR)/host_truncate_test $(HOST_DIR)/host_crc32c_test $(HOST_DIR)/host_htree_test

$(HOST_DIR)/host_cli: tools/host_cli.c $(LIB_SRCS_HOST) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_features_test: tools/host_features_test.c src/ext4/features.c | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_stress_test: tools/host_stress_test.c $(LIB_SRCS_HOST) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_htree_test: tools/host_htree_test.c $(LIB_SRCS_HOST) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_journal_test: tools/host_journal_test.c $(LIB_SRCS_HOST) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_journal_csum_test: tools/host_journal_csum_test.c $(LIB_SRCS_HOST) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_orphan_recover_test: tools/host_orphan_recover_test.c $(LIB_SRCS_HOST) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_journal_streaming_test: tools/host_journal_streaming_test.c $(LIB_SRCS_HOST) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_crash_recovery_test: tools/host_crash_recovery_test.c $(LIB_SRCS_HOST) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_checkpoint_test: tools/host_checkpoint_test.c $(LIB_SRCS_HOST) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_write_test: tools/host_write_test.c $(LIB_SRCS_HOST) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_xgroup_test: tools/host_xgroup_test.c $(LIB_SRCS_HOST) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_create_test: tools/host_create_test.c $(LIB_SRCS_HOST) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_mkdir_test: tools/host_mkdir_test.c $(LIB_SRCS_HOST) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_rmdir_test: tools/host_rmdir_test.c $(LIB_SRCS_HOST) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_del_test: tools/host_del_test.c $(LIB_SRCS_HOST) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_rename_test: tools/host_rename_test.c $(LIB_SRCS_HOST) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_rename_xdir_test: tools/host_rename_xdir_test.c $(LIB_SRCS_HOST) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_truncate_test: tools/host_truncate_test.c $(LIB_SRCS_HOST) | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR)/host_crc32c_test: tools/host_crc32c_test.c src/util/crc32c.c | $(HOST_DIR)
	$(CC) $(CFLAGS_HOST) -o $@ $^

$(HOST_DIR):
	mkdir -p $@

dos-build: $(DOS_DIR)/ext4cli.exe $(DOS_DIR)/ext4.exe $(DOS_DIR)/ext4chk.exe $(DOS_DIR)/ext4dir.exe $(DOS_DIR)/ext4cnt.exe $(DOS_DIR)/ext4dmp.exe $(DOS_DIR)/ext4wr.exe $(DOS_DIR)/ext4prb.exe $(DOS_DIR)/ext4xfr.exe $(DOS_DIR)/ext4tr.exe $(DOS_DIR)/ext4mv.exe

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

$(DOS_DIR)/ext4wr.exe: $(DOS_DIR)/tsr_write.obj
	$(WCC_ENV) $(WCL_DOS) $^ -fe=$@

$(DOS_DIR)/ext4prb.exe: $(DOS_DIR)/tsr_probe.obj
	$(WCC_ENV) $(WCL_DOS) $^ -fe=$@

$(DOS_DIR)/ext4xfr.exe: $(DOS_DIR)/tsr_xfree.obj
	$(WCC_ENV) $(WCL_DOS) $^ -fe=$@

$(DOS_DIR)/ext4tr.exe: $(DOS_DIR)/tsr_truncate.obj
	$(WCC_ENV) $(WCL_DOS) $^ -fe=$@

$(DOS_DIR)/ext4mv.exe: $(DOS_DIR)/tsr_mv.obj
	$(WCC_ENV) $(WCL_DOS) $^ -fe=$@

$(DOS_DIR)/%.obj: %.c | $(DOS_DIR)
	$(WCC_ENV) $(WCC_DOS) -fo=$@ $<

$(DOS_DIR):
	mkdir -p $@

host-test: host-build tests/images/journal.img tests/images/journal-csum.img tests/images/journal-large.img tests/images/write.img tests/images/write-csum.img tests/images/xgroup.img tests/images/htree.img
	@echo "==> running host_features_test"
	@$(HOST_DIR)/host_features_test
	@echo "==> running host_crc32c_test"
	@$(HOST_DIR)/host_crc32c_test
	@echo "==> running host_journal_test (no-csum fixture)"
	@$(HOST_DIR)/host_journal_test tests/images/journal.img tests/images/journal.expect
	@echo "==> running host_journal_test (CSUM_V2 fixture)"
	@$(HOST_DIR)/host_journal_test tests/images/journal-csum.img tests/images/journal-csum.expect
	@echo "==> running host_journal_csum_test (per-tag CSUM verify catches torn data)"
	@$(HOST_DIR)/host_journal_csum_test tests/images/journal-csum.img tests/images/journal-csum.expect tests/images/journal-csum-bad-test.img
	@echo "==> running host_checkpoint_test (no-csum, mutates working copy)"
	@cp tests/images/journal.img tests/images/journal-flush.img
	@$(HOST_DIR)/host_checkpoint_test tests/images/journal-flush.img tests/images/journal.expect
	@echo "==> running host_checkpoint_test (CSUM_V2, mutates working copy)"
	@cp tests/images/journal-csum.img tests/images/journal-csum-flush.img
	@$(HOST_DIR)/host_checkpoint_test tests/images/journal-csum-flush.img tests/images/journal-csum.expect
	@echo "==> running host_orphan_recover_test (no-csum, mutates working copy)"
	@cp tests/images/write.img tests/images/orphan-recover-test.img
	@$(HOST_DIR)/host_orphan_recover_test tests/images/orphan-recover-test.img
	@echo "==> running host_orphan_recover_test (metadata_csum, mutates working copy)"
	@cp tests/images/write-csum.img tests/images/orphan-recover-csum-test.img
	@$(HOST_DIR)/host_orphan_recover_test tests/images/orphan-recover-csum-test.img
	@echo "==> running host_journal_streaming_test (>cap unique blocks, mutates working copy)"
	@cp tests/images/journal-large.img tests/images/journal-large-test.img
	@$(HOST_DIR)/host_journal_streaming_test tests/images/journal-large-test.img tests/images/journal-large.expect
	@echo "==> running host_crash_recovery_test (fault injection across write-path bdev_writes)"
	@$(HOST_DIR)/host_crash_recovery_test tests/images/write.img tests/images/crash-recovery-test.img
	@echo "==> running host_write_test (no-csum, mutates working copy)"
	@cp tests/images/write.img tests/images/write-test.img
	@$(HOST_DIR)/host_write_test tests/images/write-test.img
	@echo "==> running host_write_test (metadata_csum, mutates working copy)"
	@cp tests/images/write-csum.img tests/images/write-csum-test.img
	@$(HOST_DIR)/host_write_test tests/images/write-csum-test.img
	@echo "==> running host_xgroup_test (cross-group allocation)"
	@cp tests/images/xgroup.img tests/images/xgroup-test.img
	@$(HOST_DIR)/host_xgroup_test tests/images/xgroup-test.img
	@echo "==> running host_create_test (file creation)"
	@cp tests/images/write.img tests/images/create-test.img
	@$(HOST_DIR)/host_create_test tests/images/create-test.img
	@echo "==> running host_mkdir_test (directory creation)"
	@cp tests/images/write.img tests/images/mkdir-test.img
	@$(HOST_DIR)/host_mkdir_test tests/images/mkdir-test.img
	@echo "==> running host_rmdir_test (directory removal)"
	@cp tests/images/write.img tests/images/rmdir-test.img
	@$(HOST_DIR)/host_rmdir_test tests/images/rmdir-test.img
	@echo "==> running host_del_test (file removal)"
	@cp tests/images/write.img tests/images/del-test.img
	@$(HOST_DIR)/host_del_test tests/images/del-test.img
	@echo "==> running host_rename_test (rename)"
	@cp tests/images/write.img tests/images/rename-test.img
	@$(HOST_DIR)/host_rename_test tests/images/rename-test.img
	@echo "==> running host_rename_xdir_test (cross-dir rename)"
	@cp tests/images/write.img tests/images/rename-xdir-test.img
	@$(HOST_DIR)/host_rename_xdir_test tests/images/rename-xdir-test.img
	@echo "==> running host_truncate_test (truncate-down)"
	@cp tests/images/write.img tests/images/truncate-test.img
	@$(HOST_DIR)/host_truncate_test tests/images/truncate-test.img
	@echo "==> running host_htree_test (htree-aware CREATE into /htreedir)"
	@$(HOST_DIR)/host_htree_test tests/images/htree.img tests/images/htree-test.img

host-stress: host-build tests/images/stress.img
	@echo "==> running host_stress_test"
	@$(HOST_DIR)/host_stress_test tests/images/stress.img

tests/images/stress.img: scripts/mkfixture-stress.py
	python3 scripts/mkfixture-stress.py

tests/images/journal.img: scripts/mkfixture-journal.py
	$(PYTHON) $<

tests/images/journal-csum.img: scripts/mkfixture-journal-csum.py
	$(PYTHON) $<

tests/images/journal-large.img: scripts/mkfixture-journal-large.py
	$(PYTHON) $<

tests/images/write.img: scripts/mkfixture-write.py
	$(PYTHON) $<

tests/images/write-csum.img: scripts/mkfixture-write-csum.py
	$(PYTHON) $<

tests/images/xgroup.img: scripts/mkfixture-xgroup.py
	$(PYTHON) $<

tests/images/htree.img: scripts/mkfixture-htree.py
	$(PYTHON) $<

dos-test: dos-build fixture-partitioned
	@bash scripts/run-dosbox.sh

tsr-test: dos-build
	@bash scripts/run-tsr-test.sh

FREEDOS_ZIP_URL := https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.4/FD14-LiteUSB.zip

tests/freedos/FD14LITE.img:
	@mkdir -p tests/freedos
	@echo "==> Downloading FreeDOS LiteUSB image..."
	curl -L -o tests/freedos/FD14LITE.img.zip '$(FREEDOS_ZIP_URL)'
	unzip -d tests/freedos tests/freedos/FD14LITE.img.zip FD14LITE.img
	rm -f tests/freedos/FD14LITE.img.zip

freedos-test: dos-build tests/freedos/FD14LITE.img
	@bash scripts/run-freedos-test.sh

msdos4-test: dos-build tests/freedos/FD14LITE.img
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
