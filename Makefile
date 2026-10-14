# SPDX-License-Identifier: BSD-3-Clause
CC ?= cc
PYTHON ?= python3
CPPFLAGS += -Iinclude
CFLAGS ?= -O2 -g
WARNINGS = -std=c11 -Wall -Wextra -Werror -pedantic
SOURCES = src/mana.c
HEADERS = $(wildcard include/uk/*.h include/uk/mana/*.h)
ROOT := $(CURDIR)
UK_BUILD := $(ROOT)/build/unikraft-test
UK_MAKE = $(MAKE) -C $(ROOT)/build/unikraft A=$(ROOT)/tests/unikraft \
	L=$(ROOT) O=$(UK_BUILD) C=$(UK_BUILD)/.config UK_CFLAGS=-std=gnu11

.PHONY: all test test-sanitize sanitize license-check clean prepare-unikraft test-msix unikraft-test
all: test

build/test_scaffold: tests/test_scaffold.c $(SOURCES) $(HEADERS) Makefile
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(SOURCES) $< $(LDFLAGS) $(LDLIBS) -o $@

build/test_scaffold_sanitize: tests/test_scaffold.c $(SOURCES) $(HEADERS) Makefile
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -O0 -fsanitize=address,undefined \
		-fno-omit-frame-pointer $(SOURCES) $< $(LDFLAGS) \
		-fsanitize=address,undefined $(LDLIBS) -o $@

test: license-check build/test_scaffold
	./build/test_scaffold

sanitize: test-sanitize
test-sanitize: license-check build/test_scaffold_sanitize
	./build/test_scaffold_sanitize

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

unikraft-test: test-msix
	$(UK_MAKE) UK_DEFCONFIG=$(ROOT)/tests/unikraft/defconfig defconfig
	@grep -qx 'CONFIG_LIBMANA=y' $(UK_BUILD)/.config || \
		{ echo "error: MANA was not enabled by Kconfig" >&2; exit 1; }
	$(UK_MAKE) -j2

clean:
	$(RM) build/test_scaffold build/test_scaffold_sanitize build/test_msix
