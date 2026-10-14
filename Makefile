# SPDX-License-Identifier: BSD-3-Clause
CC ?= cc
PYTHON ?= python3
CPPFLAGS += -Iinclude
CFLAGS ?= -O2 -g
WARNINGS = -std=c11 -Wall -Wextra -Werror -pedantic
SOURCES = src/mana.c src/pci.c src/gdma.c
HEADERS = $(wildcard include/uk/*.h include/uk/mana/*.h src/*.h)
HOST_SUPPORT = tests/host/mock.c tests/host/bootstrap.c
HOST_HEADERS = $(wildcard tests/host/*.h)
HOST_TESTS = scaffold host bootstrap
HOST_BINARIES = $(addprefix build/test_,$(HOST_TESTS))
HOST_SANITIZERS = $(addsuffix _sanitize,$(HOST_BINARIES))
ROOT := $(CURDIR)
UK_BUILD := $(ROOT)/build/unikraft-test
UK_MAKE = $(MAKE) -C $(ROOT)/build/unikraft A=$(ROOT)/tests/unikraft \
	L=$(ROOT) O=$(UK_BUILD) C=$(UK_BUILD)/.config UK_CFLAGS=-std=gnu11

.PHONY: all test test-sanitize sanitize license-check clean prepare-unikraft test-msix unikraft-test
all: test

build/test_%: tests/test_%.c $(SOURCES) $(HEADERS) $(HOST_SUPPORT) $(HOST_HEADERS) Makefile
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(SOURCES) $(HOST_SUPPORT) $< \
		$(LDFLAGS) $(LDLIBS) -o $@

build/test_%_sanitize: tests/test_%.c $(SOURCES) $(HEADERS) $(HOST_SUPPORT) $(HOST_HEADERS) Makefile
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -O0 -fsanitize=address,undefined \
		-fno-omit-frame-pointer $(SOURCES) $(HOST_SUPPORT) $< $(LDFLAGS) \
		-fsanitize=address,undefined $(LDLIBS) -o $@

test: license-check $(HOST_BINARIES)
	@set -e; for test in $(HOST_BINARIES); do ./$$test; done

sanitize: test-sanitize
test-sanitize: license-check $(HOST_SANITIZERS)
	@set -e; for test in $(HOST_SANITIZERS); do ./$$test; done

license-check:
	$(PYTHON) scripts/check-license.py

prepare-unikraft:
	sh scripts/prepare-unikraft.sh

test-msix: prepare-unikraft license-check
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Itests/msix/include \
		-Ibuild/unikraft/drivers/ukintctlr/xpic/include \
		tests/test_msix.c build/unikraft/drivers/ukintctlr/xpic/msix.c \
		$(LDFLAGS) $(LDLIBS) -o build/test_msix
	./build/test_msix
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -fsanitize=undefined \
		-fno-sanitize-recover=undefined -Itests/msix/include \
		-Ibuild/unikraft/arch/x86/x86_64/include \
		-Ibuild/unikraft/drivers/ukintctlr/xpic \
		tests/test_pic.c build/unikraft/drivers/ukintctlr/xpic/pic.c \
		$(LDFLAGS) $(LDLIBS) -o build/test_pic
	./build/test_pic

unikraft-test: test-msix
	$(UK_MAKE) UK_DEFCONFIG=$(ROOT)/tests/unikraft/defconfig defconfig
	@grep -qx 'CONFIG_LIBMANA=y' $(UK_BUILD)/.config || \
		{ echo "error: MANA was not enabled by Kconfig" >&2; exit 1; }
	$(UK_MAKE) -j2

clean:
	$(RM) $(HOST_BINARIES) $(HOST_SANITIZERS) build/test_msix build/test_pic
